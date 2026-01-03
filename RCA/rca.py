import json
import logging
import signal
import sys
import threading
import time
from urllib.parse import urlparse
from datetime import datetime
from typing import Optional, Dict, Any, Tuple, Union

from flask import Flask, jsonify, request, abort
import mysql.connector
import paho.mqtt.client as mqtt
from coapthon.client.helperclient import HelperClient
from coapthon import defines
import cbor2

# ---------------------------------------------------------------------------
# CONFIGURAZIONE
# ---------------------------------------------------------------------------

# MySQL
MYSQL_CONFIG = {
    "host": "localhost",
    "user": "root",
    "password": "",
    "port": 3306,
}
DB_NAME = "ugrid"
DROP_SCHEMA_ON_STARTUP = False 

# MQTT
MQTT_BROKER_HOST = "localhost"
MQTT_BROKER_PORT = 1883
MQTT_ALERT_TOPIC_BASE = "ugrid/alerts"

UGRIDS = {
    "ug1": {
        "coap_state_uri": "coap://[fd00::f6ce:36ac:9afa:6be2]/dev/state",
    },
}

# Polling
POLL_INTERVAL_SEC = 5.0
CONTENT_FORMAT_CBOR = 60  
CONTENT_FORMAT_JSON = 50  
ENERGY_PRICE_EUR_PER_KWH = 0.25
SOC_LOW_WARNING = 0.15      
SOH_LOW_CRITICAL = 0.80     
TEMP_HIGH_CRITICAL = 50.0  
MAX_CHARGE_POWER_KW = 5.0   
MAX_DISCH_POWER_KW  = -5.0  

logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] %(levelname)s %(name)s: %(message)s",
)
logger = logging.getLogger("RCA")

app = Flask(__name__)

# ---------------------------------------------------------------------------
# HELPERS MYSQL
# ---------------------------------------------------------------------------

def get_mysql_connection(database=None):
    cfg = dict(MYSQL_CONFIG)
    if database:
        cfg["database"] = database
    return mysql.connector.connect(**cfg)


def init_database():
    conn = get_mysql_connection()
    conn.autocommit = True
    cur = conn.cursor()

    cur.execute(f"CREATE DATABASE IF NOT EXISTS {DB_NAME}")
    cur.close()
    conn.close()

    conn = get_mysql_connection(DB_NAME)
    conn.autocommit = True
    cur = conn.cursor()

    if DROP_SCHEMA_ON_STARTUP:
        logger.warning("Dropping existing tables...")
        cur.execute("DROP TABLE IF EXISTS alerts")
        cur.execute("DROP TABLE IF EXISTS objectives")
        cur.execute("DROP TABLE IF EXISTS mpc_params")
        cur.execute("DROP TABLE IF EXISTS telemetry")

    cur.execute("""
        CREATE TABLE IF NOT EXISTS telemetry (
            id BIGINT AUTO_INCREMENT PRIMARY KEY,
            ugrid_id      VARCHAR(64) NOT NULL,
            battery_index INT         NOT NULL,
            ts            TIMESTAMP   DEFAULT CURRENT_TIMESTAMP,
            soc           FLOAT,
            soh           FLOAT,
            voltage       FLOAT,
            temperature   FLOAT,
            current       FLOAT,
            power_kw      FLOAT,
            optimal_u_kw  FLOAT,
            grid_power_kw FLOAT,
            load_kw       FLOAT,
            pv_kw         FLOAT,
            profit_eur    FLOAT
        ) ENGINE=InnoDB
    """)

    cur.execute("""
        CREATE TABLE IF NOT EXISTS objectives (
            ugrid_id      VARCHAR(64) NOT NULL,
            battery_index INT         NOT NULL,
            mode          VARCHAR(32) NOT NULL,
            target_soc    FLOAT,
            created_at    TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            PRIMARY KEY (ugrid_id, battery_index)
        ) ENGINE=InnoDB
    """)

    cur.execute("""
        CREATE TABLE IF NOT EXISTS mpc_params (
            ugrid_id   VARCHAR(64) PRIMARY KEY,
            alpha      FLOAT NOT NULL,
            beta       FLOAT NOT NULL,
            gamma      FLOAT NOT NULL,
            price      FLOAT NOT NULL,
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        ) ENGINE=InnoDB
    """)

    cur.execute("""
        CREATE TABLE IF NOT EXISTS alerts (
            id            BIGINT AUTO_INCREMENT PRIMARY KEY,
            level         VARCHAR(16) NOT NULL,
            ugrid_id      VARCHAR(64),
            battery_index INT,
            ts            TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            message       TEXT,
            payload       JSON
        ) ENGINE=InnoDB
    """)

    cur.close()
    conn.close()
    logger.info("Database inizializzato")


# ---------------------------------------------------------------------------
# HELPERS COAP (CoAPthon3 implementation)
# ---------------------------------------------------------------------------

def _parse_coap_uri(uri: str) -> Tuple[str, int, str]:
    parsed = urlparse(uri)
    host = parsed.hostname
    port = parsed.port or 5683
    path = parsed.path
    if path.startswith("/"):
        path = path[1:]
    return host, port, path

def coap_get(uri: str, timeout: float = 5.0) -> Tuple[bytes, Optional[int]]:
    host, port, path = _parse_coap_uri(uri)
    client = None
    try:
        client = HelperClient(server=(host, port))
        response = client.get(path, timeout=timeout)
        
        if response:
            payload = response.payload
            if isinstance(payload, str):
                payload = payload.encode('utf-8')
            
            cf = response.content_type
            return payload, cf
        else:
            raise IOError("Nessuna risposta dal server CoAP (timeout o errore)")
            
    except Exception as e:
        logger.error(f"Errore coap_get su {uri}: {e}")
        raise e
    finally:
        if client:
            client.stop()

def coap_put(uri: str, payload: bytes, timeout: float = 5.0) -> bytes:
    host, port, path = _parse_coap_uri(uri)
    client = None
    try:
        client = HelperClient(server=(host, port))
        response = client.put(path, payload, timeout=timeout)
        
        if response:
            p_out = response.payload
            if isinstance(p_out, str):
                p_out = p_out.encode('utf-8')
            return p_out
        else:
            raise IOError("Nessuna risposta dal server CoAP (timeout PUT)")
            
    except Exception as e:
        logger.error(f"Errore coap_put su {uri}: {e}")
        raise e
    finally:
        if client:
            client.stop()

# ---------------------------------------------------------------------------
# HELPERS Logica uGrid
# ---------------------------------------------------------------------------

def ugrid_obj_uri(ugrid_id: str) -> str:
    cfg = UGRIDS[ugrid_id]
    state_uri = cfg["coap_state_uri"]
    host, port, _ = _parse_coap_uri(state_uri)
    
    if ":" in host and not host.startswith("["):
        host_str = f"[{host}]"
    else:
        host_str = host
        
    return f"coap://{host_str}:{port}/ctrl/obj"

def send_ugrid_objective(ugrid_id: str, battery_index: int, power_kw: float):
    uri = ugrid_obj_uri(ugrid_id)
    body = {"idx": battery_index, "power_kw": int(power_kw * 100), "clear": 0}
    payload = json.dumps(body).encode("utf-8")
    coap_put(uri, payload)

def clear_ugrid_objective(ugrid_id: str, battery_index: int):
    uri = ugrid_obj_uri(ugrid_id)
    body = {"idx": battery_index, "power_kw": 0, "clear": 1}
    payload = json.dumps(body).encode("utf-8")
    coap_put(uri, payload)

# ---------------------------------------------------------------------------
# DECODIFICA /dev/state (JSON o CBOR)
# ---------------------------------------------------------------------------

def _decode_state_from_cbor(obj: Any) -> Dict[str, Any]:
    if not isinstance(obj, dict):
        raise ValueError("CBOR root non è una mappa")
    if any(isinstance(k, str) for k in obj.keys()):
        return obj

    cnt = int(obj.get(0, 0))
    load_kw = (obj.get(1, 0) or 0) / 100.0
    pv_kw = (obj.get(2, 0) or 0) / 100.0
    bats_raw = obj.get(3, []) or []
    st_map = {0: "INI", 1: "RUN", 2: "ISO"}
    bats: list[dict] = []
    for entry in bats_raw:
        if not isinstance(entry, (list, tuple)) or len(entry) < 9:
            continue
        idx, u_c, S_c, p_c, V_c, I_c, T_c, H_c, st = entry[:9]
        bats.append({
            "idx": int(idx),
            "u": (u_c or 0) / 100.0,
            "S": (S_c or 0) / 100.0,
            "p": (p_c or 0) / 100.0,
            "V": (V_c or 0) / 100.0,
            "I": (I_c or 0) / 100.0,
            "T": (T_c or 0) / 100.0,
            "H": (H_c or 0) / 100.0,
            "state": st_map.get(int(st), str(st)),
        })
    return {
        "cnt": cnt, "load_kw": load_kw, "pv_kw": pv_kw, "bats": bats,
    }

def decode_ugrid_state(payload: bytes, content_format: Optional[int]) -> Dict[str, Any]:
    if content_format == CONTENT_FORMAT_CBOR:
        if cbor2 is None:
            raise RuntimeError("Risposta CBOR ma 'cbor2' mancante")
        obj = cbor2.loads(payload)
        return _decode_state_from_cbor(obj)
    
    try:
        return json.loads(payload.decode("utf-8"))
    except Exception:
        if cbor2 is not None:
            try:
                obj = cbor2.loads(payload)
                return _decode_state_from_cbor(obj)
            except Exception:
                pass
        raise ValueError("Impossibile decodificare stato (ne JSON ne CBOR valido)")

# ---------------------------------------------------------------------------
# MQTT 
# ---------------------------------------------------------------------------

class MqttPublisher:
    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self.client = mqtt.Client()
        self.client.on_connect = self.on_connect
        self.client.on_disconnect = self.on_disconnect
        self._connected = False

    def start(self):
        try:
            self.client.connect(self.host, self.port, keepalive=60)
            self.client.loop_start()
        except Exception as e:
            logger.error(f"Errore connessione MQTT broker: {e}")

    def stop(self):
        try:
            self.client.loop_stop()
            self.client.disconnect()
        except Exception:
            pass

    def on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self._connected = True
            logger.info("Connesso al broker MQTT")
        else:
            logger.error(f"Connessione MQTT fallita, rc={rc}")

    def on_disconnect(self, client, userdata, rc):
        self._connected = False
        logger.warning("Disconnesso dal broker MQTT")

    def publish_alert(self, level, ugrid_id, battery_index, message, payload):
        topic_parts = [MQTT_ALERT_TOPIC_BASE, level, ugrid_id]
        if battery_index is not None:
            topic_parts.append(str(battery_index))
        topic = "/".join(topic_parts)
        msg = {
            "level": level, "ugrid_id": ugrid_id, "battery_index": battery_index,
            "message": message, "timestamp": datetime.utcnow().isoformat() + "Z",
        }
        if payload: msg["data"] = payload
        data_json = json.dumps(msg)
        logger.info(f"[ALERT MQTT] {topic} -> {data_json}")
        try:
            self.client.publish(topic, payload=data_json, qos=0)
        except Exception as e:
            logger.error(f"Errore pubblicando alert MQTT: {e}")

# ---------------------------------------------------------------------------
# CORE RCA
# ---------------------------------------------------------------------------

class RCA:
    def __init__(self):
        self.stop_event = threading.Event()
        self.mqtt_pub = MqttPublisher(MQTT_BROKER_HOST, MQTT_BROKER_PORT)
        self.db_lock = threading.Lock()
        self.logger = logger
        self.ugrid_price: Dict[str, float] = {
            ugrid_id: ENERGY_PRICE_EUR_PER_KWH for ugrid_id in UGRIDS.keys()
        }
        self.latest_batt_extra: Dict[Tuple[str, int], Dict[str, Any]] = {}

    # --- DB Helpers ------------------------------------------------
    def insert_telemetry(self, ugrid_id, battery_index, row):
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            try:
                cur = conn.cursor()
                cur.execute("""
                    INSERT INTO telemetry (
                        ugrid_id, battery_index, soc, soh, voltage, temperature,
                        current, power_kw, optimal_u_kw, grid_power_kw,
                        load_kw, pv_kw, profit_eur
                    ) VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)
                """, (ugrid_id, battery_index, row.get("soc"), row.get("soh"), row.get("voltage"),
                      row.get("temperature"), row.get("current"), row.get("power_kw"), row.get("optimal_u_kw"),
                      row.get("grid_power_kw"), row.get("load_kw"), row.get("pv_kw"), row.get("profit_eur")))
                conn.commit()
            finally:
                conn.close()

    def insert_alert(self, level, ugrid_id, battery_index, message, payload):
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            try:
                cur = conn.cursor()
                cur.execute("""
                    INSERT INTO alerts (level, ugrid_id, battery_index, message, payload)
                    VALUES (%s,%s,%s,%s,%s)
                """, (level, ugrid_id, battery_index, message, json.dumps(payload) if payload else None))
                conn.commit()
            finally:
                conn.close()
        self.mqtt_pub.publish_alert(level, ugrid_id, battery_index, message, payload)

    def upsert_objective(self, ugrid_id, battery_index, mode, target_soc):
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            try:
                cur = conn.cursor()
                cur.execute("""
                    INSERT INTO objectives (ugrid_id, battery_index, mode, target_soc)
                    VALUES (%s,%s,%s,%s)
                    ON DUPLICATE KEY UPDATE mode=VALUES(mode), target_soc=VALUES(target_soc)
                """, (ugrid_id, battery_index, mode, target_soc))
                conn.commit()
            finally:
                conn.close()

    def delete_objective(self, ugrid_id, battery_index):
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            try:
                cur = conn.cursor()
                cur.execute("DELETE FROM objectives WHERE ugrid_id=%s AND battery_index=%s", 
                            (ugrid_id, battery_index))
                conn.commit()
            finally:
                conn.close()

    def get_objectives_for_ugrid(self, ugrid_id):
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            try:
                cur = conn.cursor()
                cur.execute("SELECT battery_index, mode, target_soc FROM objectives WHERE ugrid_id=%s", (ugrid_id,))
                rows = cur.fetchall()
            finally:
                conn.close()
        out = {}
        for idx, mode, target_soc in rows:
            out[int(idx)] = (mode, float(target_soc) if target_soc is not None else None)
        return out

    def get_latest_status(self):
        # (Logica identica all'originale per aggregare dati DB)
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            try:
                cur = conn.cursor(dictionary=True)
                cur.execute("""
                    SELECT t.* FROM telemetry t
                    JOIN (SELECT ugrid_id, battery_index, MAX(ts) AS ts FROM telemetry GROUP BY ugrid_id, battery_index) last
                    ON t.ugrid_id = last.ugrid_id AND t.battery_index = last.battery_index AND t.ts = last.ts
                    ORDER BY t.ugrid_id, t.battery_index
                """)
                rows = cur.fetchall()
            finally:
                conn.close()

        res = {}
        profit_totals = {}
        objectives_all = {ug: self.get_objectives_for_ugrid(ug) for ug in UGRIDS.keys()}

        for r in rows:
            ugrid_id = r["ugrid_id"]
            idx = r["battery_index"]
            if ugrid_id not in res:
                res[ugrid_id] = {
                    "load_kw": r["load_kw"], "pv_kw": r["pv_kw"], "grid_power_kw": r["grid_power_kw"],
                    "price_eur_per_kwh": self.ugrid_price.get(ugrid_id, ENERGY_PRICE_EUR_PER_KWH),
                    "batteries": []
                }
                profit_totals[ugrid_id] = 0.0
            else:
                if r["grid_power_kw"] is not None: res[ugrid_id]["grid_power_kw"] = r["grid_power_kw"]
            
            if r["profit_eur"] is not None: profit_totals[ugrid_id] += float(r["profit_eur"])

            extra = self.latest_batt_extra.get((ugrid_id, idx), {})
            obj_mode, obj_tgt = None, None
            if idx in objectives_all.get(ugrid_id, {}):
                obj_mode, obj_tgt = objectives_all[ugrid_id][idx]

            res[ugrid_id]["batteries"].append({
                "index": idx, "soc": r["soc"], "soh": r["soh"], "voltage": r["voltage"],
                "temperature": r["temperature"], "current": r["current"], "power_kw": r["power_kw"],
                "optimal_u_kw": r["optimal_u_kw"], "grid_power_kw": r["grid_power_kw"],
                "profit_eur": r["profit_eur"], "state": extra.get("state"), "ip": extra.get("ip"),
                "objective_mode": obj_mode, "objective_target_soc": obj_tgt, "ts": r["ts"].isoformat()
            })

        for ugrid_id, info in res.items():
            grid_p = info.get("grid_power_kw")
            if grid_p is not None:
                info["profit_eur_per_hour"] = -grid_p * info["price_eur_per_kwh"]
            else:
                info["profit_eur_per_hour"] = None
            info["profit_eur_interval"] = profit_totals.get(ugrid_id)

        return res

    # --- Logica Controllo (sincrona) ---
    def apply_objective(self, ugrid_id, battery_index, battery_state, objective):
        mode, target_soc = objective
        soc = battery_state.get("soc") or battery_state.get("S")
        if soc is None:
            return

        # FULL DISCHARGE
        if mode == "full_discharge":
            if soc > 0.05:
                power_kw = MAX_DISCH_POWER_KW
                try:
                    send_ugrid_objective(ugrid_id, battery_index, power_kw)
                except Exception as e:
                    self.logger.error(f"Errore CoAP full_discharge: {e}")
                return
            
            # Completato
            try:
                clear_ugrid_objective(ugrid_id, battery_index)
            except Exception: pass
            self.delete_objective(ugrid_id, battery_index)
            self.insert_alert("info", ugrid_id, battery_index, "Scarica completa terminata", {"soc": soc})
            return

        # TARGET SOC
        if mode == "target_soc":
            if target_soc is None: return
            if abs(soc - target_soc) <= 0.02:
                try:
                    clear_ugrid_objective(ugrid_id, battery_index)
                except Exception: pass
                self.delete_objective(ugrid_id, battery_index)
                self.insert_alert("info", ugrid_id, battery_index, "Target SoC raggiunto", {"soc": soc})
                return

            error = target_soc - soc
            if error > 0:
                power_kw = min(MAX_CHARGE_POWER_KW, MAX_CHARGE_POWER_KW * error * 5.0)
            else:
                power_kw = max(MAX_DISCH_POWER_KW, MAX_DISCH_POWER_KW * (-error) * 5.0)
            
            try:
                send_ugrid_objective(ugrid_id, battery_index, power_kw)
            except Exception as e:
                self.logger.error(f"Errore CoAP target_soc: {e}")
            return

        # DETACH
        if mode == "detach":
            try:
                send_ugrid_objective(ugrid_id, battery_index, 0.0)
            except Exception: pass
            self.delete_objective(ugrid_id, battery_index)
            self.insert_alert("info", ugrid_id, battery_index, "Batteria staccata (detach)", {})
            return

    # --- Polling Loop ---
    def _handle_ugrid_state(self, ugrid_id, state, dt_hours):
        # (Logica identica per calcoli e parsing)
        bats = state.get("bats", []) or []
        load_kw = state.get("load_kw")
        pv_kw = state.get("pv_kw")
        price = self.ugrid_price.get(ugrid_id, ENERGY_PRICE_EUR_PER_KWH)
        
        grid_power_kw = None
        if load_kw is not None and pv_kw is not None:
            total_p = sum(b.get("p", 0.0) or 0.0 for b in bats)
            grid_power_kw = load_kw + total_p - pv_kw
        
        profit_eur_total = None
        if grid_power_kw is not None and dt_hours > 0:
            profit_eur_total = -price * grid_power_kw * dt_hours
        
        total_abs_power = sum(abs(b.get("p", 0.0) or 0.0) for b in bats) or 1.0
        objectives = self.get_objectives_for_ugrid(ugrid_id)

        for b in bats:
            idx = int(b.get("idx", 0))
            power_kw = b.get("p")
            soc = b.get("S")
            soh = b.get("H")
            temp = b.get("T")
            
            self.latest_batt_extra[(ugrid_id, idx)] = {
                "state": b.get("state"), "ip": b.get("ip")
            }
            
            profit_eur = None
            if profit_eur_total is not None and power_kw is not None:
                 profit_eur = profit_eur_total * (abs(power_kw) / total_abs_power)

            row = {
                "soc": soc, "soh": soh, "voltage": b.get("V"), "temperature": temp,
                "current": b.get("I"), "power_kw": power_kw, "optimal_u_kw": b.get("u"),
                "grid_power_kw": grid_power_kw, "load_kw": load_kw, "pv_kw": pv_kw,
                "profit_eur": profit_eur
            }
            self.insert_telemetry(ugrid_id, idx, row)

            # Alerts
            if soh is not None and soh < SOH_LOW_CRITICAL:
                self.insert_alert("critical", ugrid_id, idx, f"SoH critico {soh*100:.1f}%", {"soh": soh})
            if temp is not None and temp > TEMP_HIGH_CRITICAL:
                self.insert_alert("critical", ugrid_id, idx, f"Temp alta {temp:.1f}°C", {"temp": temp})
            if soc is not None and soc < SOC_LOW_WARNING:
                self.insert_alert("warning", ugrid_id, idx, f"SoC basso {soc*100:.1f}%", {"soc": soc})

            if idx in objectives:
                self.apply_objective(ugrid_id, idx, b, objectives[idx])

    def poll_loop(self):
        logger.info("Poll loop avviato (CoAPthon sync)")
        last_ts = {ugrid_id: time.time() for ugrid_id in UGRIDS.keys()}

        while not self.stop_event.is_set():
            start_t = time.time()
            for ugrid_id, cfg in UGRIDS.items():
                uri = cfg["coap_state_uri"]
                try:
                    # Chiamata sincrona, non c'è await
                    payload, cf = coap_get(uri, timeout=3.0)
                    state = decode_ugrid_state(payload, cf)
                    
                    # Normalizzazione numerica
                    if isinstance(state.get("load_kw"), str): state["load_kw"] = float(state["load_kw"])
                    if isinstance(state.get("pv_kw"), str): state["pv_kw"] = float(state["pv_kw"])
                    
                    now = time.time()
                    dt = (now - last_ts.get(ugrid_id, now)) / 3600.0
                    last_ts[ugrid_id] = now
                    
                    self._handle_ugrid_state(ugrid_id, state, max(dt, POLL_INTERVAL_SEC/3600.0))
                    
                except Exception as e:
                    logger.error(f"Errore poll ugrid {ugrid_id}: {e}")

            elapsed = time.time() - start_t
            wait_for = max(0.0, POLL_INTERVAL_SEC - elapsed)
            # wait gestito con Event per uscire puliti se richiesto stop
            self.stop_event.wait(wait_for)
        logger.info("Poll loop terminato")

    def set_mpc_params(self, ugrid_id, alpha, beta, gamma, price):
        if price is None: price = ENERGY_PRICE_EUR_PER_KWH
        self.ugrid_price[ugrid_id] = price
        
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            try:
                cur = conn.cursor()
                cur.execute("""
                    INSERT INTO mpc_params (ugrid_id, alpha, beta, gamma, price)
                    VALUES (%s,%s,%s,%s,%s)
                    ON DUPLICATE KEY UPDATE alpha=VALUES(alpha), beta=VALUES(beta), 
                    gamma=VALUES(gamma), price=VALUES(price)
                """, (ugrid_id, alpha, beta, gamma, price))
                conn.commit()
            finally:
                conn.close()
        
        # CoAP PUT
        uconf = UGRIDS.get(ugrid_id)
        if not uconf: return
        host, port, _ = _parse_coap_uri(uconf["coap_state_uri"])
        
        # Ricostruzione path mpc
        if ":" in host and not host.startswith("["): host = f"[{host}]"
        mpc_uri = f"coap://{host}:{port}/ctrl/mpc"
        
        payload = json.dumps(
            {"a": int(alpha*100), "b": int(beta*100), "g": int(gamma*100), "p": int(price*100)},
            separators=(",", ":")
        ).encode("utf-8")
        
        try:
            coap_put(mpc_uri, payload, timeout=3.0)
        except Exception as e:
            logger.error(f"Errore set_mpc_params CoAP: {e}")

    def start(self):
        self.mqtt_pub.start()
        # Thread per il loop di polling (che ora usa chiamate bloccanti)
        t = threading.Thread(target=self.poll_loop, daemon=True)
        t.start()

    def stop(self):
        self.stop_event.set()
        self.mqtt_pub.stop()

rca = RCA()

# ---------------------------------------------------------------------------
# API HTTP (Flask)                                              
# ---------------------------------------------------------------------------

@app.route("/api/status", methods=["GET"])
def api_status():
    return jsonify(rca.get_latest_status())

@app.route("/api/batteries/<ugrid_id>/<int:bat_idx>/objective", methods=["POST", "DELETE"])
def api_battery_objective(ugrid_id, bat_idx):
    if ugrid_id not in UGRIDS: abort(404, "uGrid sconosciuto")
    if request.method == "DELETE":
        try:
            clear_ugrid_objective(ugrid_id, bat_idx)
        except Exception as e:
            logger.error(f"Errore clear objective: {e}")
        rca.delete_objective(ugrid_id, bat_idx)
        return jsonify({"status": "ok"})

    data = request.get_json(force=True, silent=True) or {}
    mode = data.get("mode")
    target_soc = data.get("target_soc")
    if mode not in {"full_discharge", "target_soc", "detach"}: abort(400, "mode invalido")
    if mode == "target_soc":
        if target_soc is None: abort(400, "target_soc mancante")
        target_soc = float(target_soc)
    else:
        target_soc = None
    rca.upsert_objective(ugrid_id, bat_idx, mode, target_soc)
    return jsonify({"status": "ok", "mode": mode})

@app.route("/api/batteries/<ugrid_id>/<int:bat_idx>/history", methods=["GET"])
def api_battery_history(ugrid_id, bat_idx):
    limit = int(request.args.get("limit", 100))
    with rca.db_lock:
        conn = get_mysql_connection(DB_NAME)
        try:
            cur = conn.cursor(dictionary=True)
            cur.execute("SELECT * FROM telemetry WHERE ugrid_id=%s AND battery_index=%s ORDER BY ts DESC LIMIT %s", 
                        (ugrid_id, bat_idx, limit))
            rows = cur.fetchall()
        finally:
            conn.close()
    for r in rows: r["ts"] = r["ts"].isoformat()
    return jsonify(rows)

@app.route("/api/ugrids/<ugrid_id>/mpc_params", methods=["POST", "GET"])
def api_mpc_params(ugrid_id):
    if ugrid_id not in UGRIDS: abort(404, "uGrid sconosciuto")
    if request.method == "GET":
        with rca.db_lock:
            conn = get_mysql_connection(DB_NAME)
            try:
                cur = conn.cursor(dictionary=True)
                cur.execute("SELECT * FROM mpc_params WHERE ugrid_id=%s", (ugrid_id,))
                row = cur.fetchone()
            finally:
                conn.close()
        if not row: abort(404)
        row["updated_at"] = row["updated_at"].isoformat()
        return jsonify(row)

    data = request.get_json(force=True, silent=True) or {}
    try:
        a = float(data["alpha"])
        b = float(data["beta"])
        g = float(data["gamma"])
        p = float(data.get("price", ENERGY_PRICE_EUR_PER_KWH))
    except: abort(400)
    rca.set_mpc_params(ugrid_id, a, b, g, p)
    return jsonify({"status": "ok"})

@app.route("/api/alerts", methods=["GET"])
def api_alerts():
    limit = int(request.args.get("limit", 50))
    with rca.db_lock:
        conn = get_mysql_connection(DB_NAME)
        try:
            cur = conn.cursor(dictionary=True)
            cur.execute("SELECT * FROM alerts ORDER BY ts DESC LIMIT %s", (limit,))
            rows = cur.fetchall()
        finally:
            conn.close()
    for r in rows: r["ts"] = r["ts"].isoformat()
    return jsonify(rows)

# ---------------------------------------------------------------------------
# MAIN
# ---------------------------------------------------------------------------
def main():
    init_database()
    rca.start()
    
    # ctrl+C handler
    def handle_sig(sig, frame):
        logger.info("Arresto RCA...")
        rca.stop()
        sys.exit(0)
    
    signal.signal(signal.SIGINT, handle_sig)
    signal.signal(signal.SIGTERM, handle_sig)
    
    # Flask gestito nel main thread
    app.run(host="0.0.0.0", port=3000, debug=False, threaded=True)

if __name__ == "__main__":
    main()
