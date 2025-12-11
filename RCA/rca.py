#!/usr/bin/env python3
"""
Remote Control Application (RCA) – Smart LBSS Demo

- HTTP REST API (Flask)
- CoAP client (aiocoap) verso uGridController e BatteryController
- Persistenza dati in MySQL
- Logica di alto livello (obiettivi: scarica completa, porta a % di SoC, detach)
- Pubblica alert asincroni su MQTT (Paho)
"""

import asyncio
import json
import logging
import signal
import sys
import threading
import time
from datetime import datetime
from typing import Optional, Dict, Any, Tuple

from flask import Flask, jsonify, request, abort

import mysql.connector

import paho.mqtt.client as mqtt

from aiocoap import Context, Message
from aiocoap.numbers.codes import GET, PUT

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
DROP_SCHEMA_ON_STARTUP = False  # metti True se vuoi droppare le tabelle a ogni avvio

# MQTT
MQTT_BROKER_HOST = "localhost"
MQTT_BROKER_PORT = 1883
MQTT_ALERT_TOPIC_BASE = "ugrid/alerts"  # p.es. ugrid/alerts/critical/ug1/0

# CoAP / uGrid
# Mappa logica: ID uGrid -> config
UGRIDS = {
    "ug1": {
        # "coap_state_uri": "coap://[fd00::202:2:2:2]/dev/state",
        "coap_state_uri": "coap://[fd00::f6ce:36ac:9afa:6be2]/dev/state",
    },
}

# Polling
POLL_INTERVAL_SEC = 5.0

# Prezzo energia (€/kWh) – usato per calcolo guadagno/perdita
ENERGY_PRICE_EUR_PER_KWH = 0.25

# Soglie per alert (puoi regolarle)
SOC_LOW_WARNING = 0.15     # 15%
SOH_LOW_CRITICAL = 0.7     # 70%
TEMP_HIGH_CRITICAL = 55.0  # °C

# Potenze di default (non usate direttamente ora ma lasciate per completezza)
DEFAULT_DISCHARGE_POWER_KW = -2.0  # scarica
DEFAULT_CHARGE_POWER_KW = 2.0      # carica

# Limiti hard per gli obiettivi (kW)
MAX_CHARGE_POWER_KW = 5.0   # carica max
MAX_DISCH_POWER_KW  = -5.0  # scarica max (negativo)


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
    """Crea DB e tabelle, opzionalmente droppa lo schema esistente."""
    conn = get_mysql_connection()
    conn.autocommit = True
    cur = conn.cursor()

    # Crea DB se non esiste
    cur.execute(f"CREATE DATABASE IF NOT EXISTS {DB_NAME}")
    cur.close()
    conn.close()

    conn = get_mysql_connection(DB_NAME)
    conn.autocommit = True
    cur = conn.cursor()

    if DROP_SCHEMA_ON_STARTUP:
        logger.warning("Dropping existing tables (DROP_SCHEMA_ON_STARTUP=True)")
        cur.execute("DROP TABLE IF EXISTS alerts")
        cur.execute("DROP TABLE IF EXISTS objectives")
        cur.execute("DROP TABLE IF EXISTS mpc_params")
        cur.execute("DROP TABLE IF EXISTS telemetry")

    # Telemetry
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

    # Objectives
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

    # MPC params
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

    # Alerts
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
# HELPERS COAP / uGrid objectives
# ---------------------------------------------------------------------------

def ugrid_obj_uri(ugrid_id: str) -> str:
    """
    Ritorna la URI CoAP della risorsa ctrl/obj per una certa uGrid,
    derivando l'host dalla coap_state_uri (es. coap://[fd00::...]/dev/state).
    """
    cfg = UGRIDS[ugrid_id]
    state_uri = cfg["coap_state_uri"]  # es: "coap://[fd00::...]/dev/state"

    prefix = "coap://"
    if not state_uri.startswith(prefix):
        raise ValueError(f"URI CoAP non valida per ugrid {ugrid_id}: {state_uri}")

    rest = state_uri[len(prefix):]           # "[fd00::...]/dev/state"
    slash_idx = rest.find("/")
    if slash_idx == -1:
        host = rest
    else:
        host = rest[:slash_idx]              # "[fd00::...]"

    if not host.startswith("["):
        host = f"[{host}]"

    return f"coap://{host}/ctrl/obj"


def send_ugrid_objective(ugrid_id: str, battery_index: int, power_kw: float):
    """
    Imposta un obiettivo di potenza (in kW) per una batteria sull'uGrid.
    """
    uri = ugrid_obj_uri(ugrid_id)
    body = {"idx": battery_index, "power_kw": float(power_kw)}
    payload = json.dumps(body).encode("utf-8")
    coap_put(uri, payload)


def clear_ugrid_objective(ugrid_id: str, battery_index: int):
    """
    Cancella l'obiettivo per una batteria sull'uGrid.
    """
    uri = ugrid_obj_uri(ugrid_id)
    body = {"idx": battery_index, "clear": 1}
    payload = json.dumps(body).encode("utf-8")
    coap_put(uri, payload)


# ---------------------------------------------------------------------------
# HELPERS COAP (uso sincrono via asyncio.run per semplicità)
# ---------------------------------------------------------------------------

async def _coap_get(uri: str, timeout: float = 5.0) -> bytes:
    protocol = await Context.create_client_context()
    try:
        req = Message(code=GET, uri=uri)
        resp = await asyncio.wait_for(protocol.request(req).response, timeout=timeout)
        return bytes(resp.payload)
    finally:
        protocol.shutdown()


async def _coap_put(uri: str, payload: bytes, timeout: float = 5.0) -> bytes:
    protocol = await Context.create_client_context()
    try:
        req = Message(code=PUT, uri=uri, payload=payload)
        resp = await asyncio.wait_for(protocol.request(req).response, timeout=timeout)
        return bytes(resp.payload)
    finally:
        protocol.shutdown()


def coap_get(uri: str, timeout: float = 5.0) -> bytes:
    return asyncio.run(_coap_get(uri, timeout))


def coap_put(uri: str, payload: bytes, timeout: float = 5.0) -> bytes:
    return asyncio.run(_coap_put(uri, payload, timeout))


# ---------------------------------------------------------------------------
# MQTT – publisher per alert
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

    def publish_alert(
        self,
        level: str,
        ugrid_id: str,
        battery_index: Optional[int] = None,
        message: str = "",
        payload: Optional[dict] = None,
    ):
        topic_parts = [MQTT_ALERT_TOPIC_BASE, level, ugrid_id]
        if battery_index is not None:
            topic_parts.append(str(battery_index))
        topic = "/".join(topic_parts)

        msg = {
            "level": level,
            "ugrid_id": ugrid_id,
            "battery_index": battery_index,
            "message": message,
            "timestamp": datetime.utcnow().isoformat() + "Z",
        }
        if payload is not None:
            msg["data"] = payload

        data = json.dumps(msg)
        logger.info(f"[ALERT MQTT] {topic} -> {data}")
        try:
            self.client.publish(topic, payload=data, qos=0, retain=False)
        except Exception as e:
            logger.error(f"Errore pubblicando alert MQTT: {e}")


# ---------------------------------------------------------------------------
# CORE RCA
# ---------------------------------------------------------------------------

class RCA:
    def __init__(self):
        self.stop_event = threading.Event()
        self.mqtt_pub = MqttPublisher(MQTT_BROKER_HOST, MQTT_BROKER_PORT)
        self.db_lock = threading.Lock()  # semplicità: un lock per tutte le operazioni
        self.logger = logger            # usa il logger globale all'interno della classe

        # Prezzo energia per ogni uGrid (di default ENERGY_PRICE_EUR_PER_KWH,
        # aggiornato quando chiami /api/ugrids/<id>/mpc_params)
        self.ugrid_price: Dict[str, float] = {
            ugrid_id: ENERGY_PRICE_EUR_PER_KWH for ugrid_id in UGRIDS.keys()
        }

        # Cache in memoria di info extra per batteria (state, ip, ecc.)
        # chiave: (ugrid_id, battery_index) -> dict con "state", "ip", ...
        self.latest_batt_extra: Dict[Tuple[str, int], Dict[str, Any]] = {}

    # ---------- DB helpers ----------

    def insert_telemetry(self, ugrid_id: str, battery_index: int, row: dict):
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            cur = conn.cursor()
            cur.execute("""
                INSERT INTO telemetry (
                    ugrid_id, battery_index, soc, soh, voltage, temperature,
                    current, power_kw, optimal_u_kw, grid_power_kw,
                    load_kw, pv_kw, profit_eur
                ) VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)
            """, (
                ugrid_id, battery_index,
                row.get("soc"), row.get("soh"), row.get("voltage"),
                row.get("temperature"), row.get("current"),
                row.get("power_kw"), row.get("optimal_u_kw"),
                row.get("grid_power_kw"), row.get("load_kw"),
                row.get("pv_kw"), row.get("profit_eur"),
            ))
            conn.commit()
            cur.close()
            conn.close()

    def insert_alert(
        self,
        level: str,
        ugrid_id: str,
        battery_index: Optional[int],
        message: str,
        payload: Optional[dict] = None,
    ):
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            cur = conn.cursor()
            cur.execute("""
                INSERT INTO alerts (level, ugrid_id, battery_index, message, payload)
                VALUES (%s,%s,%s,%s,%s)
            """, (
                level, ugrid_id, battery_index, message,
                json.dumps(payload) if payload else None,
            ))
            conn.commit()
            cur.close()
            conn.close()

        self.mqtt_pub.publish_alert(level, ugrid_id, battery_index, message, payload)

    def upsert_objective(
        self,
        ugrid_id: str,
        battery_index: int,
        mode: str,
        target_soc: Optional[float],
    ):
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            cur = conn.cursor()
            cur.execute("""
                INSERT INTO objectives (ugrid_id, battery_index, mode, target_soc)
                VALUES (%s,%s,%s,%s)
                ON DUPLICATE KEY UPDATE mode=VALUES(mode), target_soc=VALUES(target_soc)
            """, (ugrid_id, battery_index, mode, target_soc))
            conn.commit()
            cur.close()
            conn.close()

    def delete_objective(self, ugrid_id: str, battery_index: int):
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            cur = conn.cursor()
            cur.execute("""
                DELETE FROM objectives WHERE ugrid_id=%s AND battery_index=%s
            """, (ugrid_id, battery_index))
            conn.commit()
            cur.close()
            conn.close()

    def get_objectives_for_ugrid(self, ugrid_id: str) -> dict:
        """Ritorna dict battery_index -> (mode, target_soc)"""
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            cur = conn.cursor()
            cur.execute("""
                SELECT battery_index, mode, target_soc
                FROM objectives WHERE ugrid_id=%s
            """, (ugrid_id,))
            rows = cur.fetchall()
            cur.close()
            conn.close()

        out = {}
        for idx, mode, target_soc in rows:
            out[int(idx)] = (mode, float(target_soc) if target_soc is not None else None)
        return out

    def get_latest_status(self) -> Dict[str, Dict[str, Any]]:
        """Ritorna lo stato più recente per ogni batteria (per API /status)."""
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            cur = conn.cursor(dictionary=True)

            cur.execute("""
                SELECT t.*
                FROM telemetry t
                JOIN (
                    SELECT ugrid_id, battery_index, MAX(ts) AS ts
                    FROM telemetry
                    GROUP BY ugrid_id, battery_index
                ) last
                ON t.ugrid_id = last.ugrid_id
                AND t.battery_index = last.battery_index
                AND t.ts = last.ts
                ORDER BY t.ugrid_id, t.battery_index
            """)
            rows = cur.fetchall()
            cur.close()
            conn.close()

        res: Dict[str, Dict[str, Any]] = {}
        profit_totals: Dict[str, float] = {}

        # mappa ugrid_id -> {idx: (mode, target_soc)}
        objectives_all: Dict[str, Dict[int, Tuple[str, Optional[float]]]] = {
            ugrid_id: self.get_objectives_for_ugrid(ugrid_id)
            for ugrid_id in UGRIDS.keys()
        }

        for r in rows:
            ugrid_id = r["ugrid_id"]
            idx = r["battery_index"]

            if ugrid_id not in res:
                res[ugrid_id] = {
                    "load_kw": r["load_kw"],
                    "pv_kw": r["pv_kw"],
                    "grid_power_kw": r["grid_power_kw"],
                    "price_eur_per_kwh": self.ugrid_price.get(
                        ugrid_id, ENERGY_PRICE_EUR_PER_KWH
                    ),
                    "batteries": [],
                }
                profit_totals[ugrid_id] = 0.0
            else:
                # eventuale grid_power più recente
                if r["grid_power_kw"] is not None:
                    res[ugrid_id]["grid_power_kw"] = r["grid_power_kw"]

            profit = r["profit_eur"]
            if profit is not None:
                profit_totals[ugrid_id] += float(profit)

            key = (ugrid_id, idx)
            extra = self.latest_batt_extra.get(key, {})
            state_str = extra.get("state")
            ip_str = extra.get("ip")

            obj_mode = None
            obj_tgt = None
            obj_for_ug = objectives_all.get(ugrid_id, {})
            if idx in obj_for_ug:
                obj_mode, obj_tgt = obj_for_ug[idx]

            res[ugrid_id]["batteries"].append({
                "index": idx,
                "soc": r["soc"],
                "soh": r["soh"],
                "voltage": r["voltage"],
                "temperature": r["temperature"],
                "power_kw": r["power_kw"],
                "optimal_u_kw": r["optimal_u_kw"],
                "grid_power_kw": r["grid_power_kw"],
                "profit_eur": r["profit_eur"],
                "state": state_str,
                "ip": ip_str,
                "objective_mode": obj_mode,
                "objective_target_soc": obj_tgt,
                "ts": r["ts"].isoformat(),
            })

        # aggiungi stima €/h per uGrid
        for ugrid_id, info in res.items():
            price = info["price_eur_per_kwh"]
            grid_p = info.get("grid_power_kw")
            interval_profit = profit_totals.get(ugrid_id)
            info["profit_eur_interval"] = interval_profit
            if grid_p is not None and price is not None:
                # grid_p > 0 → import → costo (negativo)
                info["profit_eur_per_hour"] = -grid_p * price
            else:
                info["profit_eur_per_hour"] = None

        return res

    # ---------- Logica di controllo alto livello ----------

    def apply_objective(
        self,
        ugrid_id: str,
        battery_index: int,
        battery_state: dict,
        objective: Tuple[str, Optional[float]],
    ):
        """
        Applica un obiettivo ad una batteria usando la risorsa ctrl/obj dell'uGrid.
        objective = (mode, target_soc | None)
        mode = "full_discharge" | "target_soc" | "detach"
        """
        mode, target_soc = objective
        soc = battery_state.get("soc")

        if soc is None:
            self.insert_alert(
                "warning",
                ugrid_id,
                battery_index,
                f"Impossibile applicare obiettivo {mode}: SoC sconosciuto",
                {},
            )
            return

        # Obiettivo: scarica completamente la batteria (per rimozione)
        if mode == "full_discharge":
            # Finché sopra ~5% scarichiamo a potenza costante
            if soc > 0.05:
                power_kw = MAX_DISCH_POWER_KW
                self.logger.info(
                    "[OBJ] %s bat#%d full_discharge: SoC=%.1f%% → cmd=%.2f kW",
                    ugrid_id, battery_index, soc * 100.0, power_kw
                )
                try:
                    send_ugrid_objective(ugrid_id, battery_index, power_kw)
                except Exception as e:
                    self.logger.error("Errore CoAP full_discharge: %s", e)
                return

            # Sotto la soglia → stop, azzera obiettivo
            self.logger.info(
                "[OBJ] %s bat#%d full_discharge COMPLETATA (SoC=%.1f%%), clear obj",
                ugrid_id, battery_index, soc * 100.0
            )
            try:
                clear_ugrid_objective(ugrid_id, battery_index)
            except Exception as e:
                self.logger.error("Errore CoAP clear objective: %s", e)

            self.delete_objective(ugrid_id, battery_index)
            self.insert_alert(
                "info",
                ugrid_id,
                battery_index,
                "Batteria scaricata (SoC basso), obiettivo rimosso",
                {"soc": soc},
            )
            return

        # Obiettivo: portare batteria ad un certo SoC
        if mode == "target_soc":
            if target_soc is None:
                self.logger.warning(
                    "[OBJ] %s bat#%d target_soc senza valore target", ugrid_id, battery_index
                )
                return

            # Se siamo già vicini al target → clear
            if abs(soc - target_soc) <= 0.02:
                self.logger.info(
                    "[OBJ] %s bat#%d target_soc raggiunto: SoC=%.1f%% ≈ %.1f%%",
                    ugrid_id, battery_index, soc * 100.0, target_soc * 100.0
                )
                try:
                    clear_ugrid_objective(ugrid_id, battery_index)
                except Exception as e:
                    self.logger.error("Errore CoAP clear objective: %s", e)

                self.delete_objective(ugrid_id, battery_index)
                self.insert_alert(
                    "info",
                    ugrid_id,
                    battery_index,
                    "Target SoC raggiunto, obiettivo rimosso",
                    {"soc": soc, "target_soc": target_soc},
                )
                return

            # Semplice P-controller su SoC
            error = float(target_soc - soc)
            # segno: positivo → carica, negativo → scarica
            if error > 0:
                # carica
                power_kw = min(
                    MAX_CHARGE_POWER_KW,
                    MAX_CHARGE_POWER_KW * error * 5.0
                )
            else:
                # scarica
                power_kw = max(
                    MAX_DISCH_POWER_KW,
                    MAX_DISCH_POWER_KW * (-error) * 5.0
                )

            self.logger.info(
                "[OBJ] %s bat#%d target_soc: SoC=%.1f%% → target=%.1f%%, cmd=%.2f kW",
                ugrid_id, battery_index, soc * 100.0, target_soc * 100.0, power_kw
            )
            try:
                send_ugrid_objective(ugrid_id, battery_index, power_kw)
            except Exception as e:
                self.logger.error("Errore CoAP target_soc: %s", e)
            return

        # Obiettivo: detach (stacca la batteria, niente potenza)
        if mode == "detach":
            self.logger.info(
                "[OBJ] %s bat#%d detach: forza potenza=0kW e rimuove obiettivo",
                ugrid_id, battery_index
            )
            try:
                send_ugrid_objective(ugrid_id, battery_index, 0.0)
            except Exception as e:
                self.logger.error("Errore CoAP detach: %s", e)

            self.delete_objective(ugrid_id, battery_index)
            self.insert_alert(
                "info",
                ugrid_id,
                battery_index,
                "Batteria staccata (detach)",
                {},
            )
            return

        # Modalità sconosciuta
        self.logger.warning(
            "[OBJ] %s bat#%d: mode sconosciuto '%s'", ugrid_id, battery_index, mode
        )

    # ---------- Poll uGrid / parsing stato / alert ----------

    def _handle_ugrid_state(self, ugrid_id: str, state: dict, dt_hours: float):
        """
        `state` è il JSON ricevuto da /dev/state dell'uGridController.
        dt_hours è il tempo passato dall'ultima lettura (in ore), per calcolare profitto.
        """
        bats = state.get("bats", [])
        load_kw = state.get("load_kw")
        pv_kw = state.get("pv_kw")
        price = self.ugrid_price.get(ugrid_id, ENERGY_PRICE_EUR_PER_KWH)


        # compute grid power 
        grid_power_kw = None
        if load_kw is not None and pv_kw is not None:
            total_p = sum(b.get("p", 0.0) for b in bats)
            # p > 0  → batteria in CARICA (assorbe dalla microgrid)
            # p < 0  → batteria in SCARICA (fornisce potenza alla microgrid)
            # Bilancio: grid = load + Σ(p) - pv
            grid_power_kw = load_kw + total_p - pv_kw

        # profitto/perdita per intervallo dt
        profit_eur_total = None
        if grid_power_kw is not None and dt_hours > 0:
            energy_kwh = grid_power_kw * dt_hours
            # grid_power_kw > 0 (import) → costo (profit negativo)
            profit_eur_total = -price * energy_kwh

        # divido profit per batteria in proporzione di |power|
        total_abs_power = sum(abs(b.get("p", 0.0)) for b in bats) or 1.0

        objectives = self.get_objectives_for_ugrid(ugrid_id)

        for b in bats:
            idx = int(b.get("idx", bats.index(b)))
            soc = b.get("soc")
            soh = b.get("soh")
            temp = b.get("temp")

            # compatibilità con chiavi "V"/"I" o "voltage"/"current"
            if "V" in b:
                volt = b["V"]
            else:
                volt = b.get("voltage")

            if "I" in b:
                curr = b["I"]
            else:
                curr = b.get("current")

            power_kw = b.get("p")
            optimal_u_kw = b.get("u")

            # cache in memoria info extra (state, ip) per /api/status
            self.latest_batt_extra[(ugrid_id, idx)] = {
                "state": b.get("state"),
                "ip": b.get("ip"),
            }

            # quota di profitto
            profit_eur = None
            if profit_eur_total is not None and power_kw is not None and total_abs_power > 0:
                profit_eur = profit_eur_total * (abs(power_kw) / total_abs_power)

            row = {
                "soc": soc,
                "soh": soh,
                "voltage": volt,
                "temperature": temp,
                "current": curr,
                "power_kw": power_kw,
                "optimal_u_kw": optimal_u_kw,
                "grid_power_kw": grid_power_kw,
                "load_kw": load_kw,
                "pv_kw": pv_kw,
                "profit_eur": profit_eur,
            }
            self.insert_telemetry(ugrid_id, idx, row)

            # ALERT: condizioni di sicurezza
            if soh is not None and soh < SOH_LOW_CRITICAL:
                self.insert_alert(
                    "critical",
                    ugrid_id,
                    idx,
                    f"SoH basso ({soh*100:.1f}%) – possibile manutenzione",
                    {"soh": soh, "soc": soc}
                )
            if temp is not None and temp > TEMP_HIGH_CRITICAL:
                self.insert_alert(
                    "critical",
                    ugrid_id,
                    idx,
                    f"Temperatura alta ({temp:.1f}°C)",
                    {"temp": temp, "soc": soc}
                )
            if soc is not None and soc < SOC_LOW_WARNING:
                self.insert_alert(
                    "warning",
                    ugrid_id,
                    idx,
                    f"SoC basso ({soc*100:.1f}%)",
                    {"soc": soc}
                )

            # Applica eventuale obiettivo per questa batteria
            if idx in objectives:
                self.apply_objective(ugrid_id, idx, b, objectives[idx])

    def poll_loop(self):
        """Thread che periodicamente fa GET /dev/state dagli uGridController."""
        logger.info("Poll loop avviato")
        last_ts = {ugrid_id: time.time() for ugrid_id in UGRIDS.keys()}

        while not self.stop_event.is_set():
            start = time.time()
            for ugrid_id, cfg in UGRIDS.items():
                uri = cfg["coap_state_uri"]
                try:
                    payload = coap_get(uri, timeout=3.0)
                    state = json.loads(payload.decode("utf-8"))
                except Exception as e:
                    logger.error(f"Errore CoAP state ugrid {ugrid_id}: {e}")
                    continue

                now = time.time()
                dt_hours = (now - last_ts.get(ugrid_id, now)) / 3600.0
                last_ts[ugrid_id] = now

                self._handle_ugrid_state(
                    ugrid_id,
                    state,
                    max(dt_hours, POLL_INTERVAL_SEC / 3600.0),
                )

            elapsed = time.time() - start
            wait_for = max(0.0, POLL_INTERVAL_SEC - elapsed)
            self.stop_event.wait(wait_for)

        logger.info("Poll loop terminato")

    # ---------- MPC params ----------

    def set_mpc_params(
        self,
        ugrid_id: str,
        alpha: float,
        beta: float,
        gamma: float,
        price: Optional[float] = None,
    ):
        """
        Aggiorna i parametri MPC lato DB e (se implementato) via CoAP verso uGridController.
        Si assume che tu abbia aggiunto una risorsa CoAP tipo /ctrl/mpc.
        """

        if price is None:
            price = ENERGY_PRICE_EUR_PER_KWH
        
        # aggiorna il prezzo usato per i calcoli di profitto
        self.ugrid_price[ugrid_id] = price

        # DB
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            cur = conn.cursor()
            cur.execute("""
                INSERT INTO mpc_params (ugrid_id, alpha, beta, gamma, price)
                VALUES (%s,%s,%s,%s,%s)
                ON DUPLICATE KEY UPDATE
                    alpha=VALUES(alpha),
                    beta=VALUES(beta),
                    gamma=VALUES(gamma),
                    price=VALUES(price)
            """, (ugrid_id, alpha, beta, gamma, price))
            conn.commit()
            cur.close()
            conn.close()

        # CoAP (richiede risorsa aggiunta sul firmware)
        uconf = UGRIDS.get(ugrid_id)
        if not uconf:
            raise ValueError(f"uGrid {ugrid_id} non configurato")

        state_uri = uconf["coap_state_uri"]
        base = state_uri.split("/dev/state")[0]
        mpc_uri = base + "/ctrl/mpc"
        payload = json.dumps({
            "alpha": alpha,
            "beta": beta,
            "gamma": gamma,
            "price": price
        }).encode("utf-8")

        try:
            coap_put(mpc_uri, payload, timeout=3.0)
        except Exception as e:
            logger.error(f"Errore CoAP impostando MPC params su {ugrid_id}: {e}")

    # ---------- Avvio/stop ----------

    def start(self):
        self.mqtt_pub.start()
        t = threading.Thread(target=self.poll_loop, daemon=True)
        t.start()

    def stop(self):
        self.stop_event.set()
        self.mqtt_pub.stop()


# Istanza globale RCA
rca = RCA()


# ---------------------------------------------------------------------------
# API HTTP (Flask)
# ---------------------------------------------------------------------------

@app.route("/api/status", methods=["GET"])
def api_status():
    """
    Stato corrente di tutti gli uGrid e batterie.
    Usato dalla CLI per mostrare riepilogo.
    """
    data = rca.get_latest_status()
    return jsonify(data)


@app.route("/api/batteries/<ugrid_id>/<int:bat_idx>/objective", methods=["POST", "DELETE"])
def api_battery_objective(ugrid_id, bat_idx):
    """
    POST: imposta un obiettivo di controllo high-level per una batteria.
        body JSON:
          - mode: 'full_discharge' | 'target_soc' | 'detach'
          - target_soc: float [0,1] (solo per 'target_soc')
    DELETE: rimuove l'obiettivo corrente.
    """
    if ugrid_id not in UGRIDS:
        abort(404, description="uGrid sconosciuto")

    if request.method == "DELETE":
        rca.delete_objective(ugrid_id, bat_idx)
        return jsonify({"status": "ok", "message": "objective removed"})

    data = request.get_json(force=True, silent=True) or {}
    mode = data.get("mode")
    target_soc = data.get("target_soc")

    if mode not in {"full_discharge", "target_soc", "detach"}:
        abort(400, description="mode non valido")

    if mode == "target_soc":
        if target_soc is None:
            abort(400, description="target_soc richiesto per mode=target_soc")
        if not (0.0 <= float(target_soc) <= 1.0):
            abort(400, description="target_soc deve essere tra 0 e 1")
        target_soc = float(target_soc)
    else:
        target_soc = None

    rca.upsert_objective(ugrid_id, bat_idx, mode, target_soc)
    return jsonify({"status": "ok", "mode": mode, "target_soc": target_soc})


@app.route("/api/batteries/<ugrid_id>/<int:bat_idx>/history", methods=["GET"])
def api_battery_history(ugrid_id, bat_idx):
    """
    Restituisce gli ultimi N punti di telemetria per una batteria.
    Query param: ?limit=100
    """
    limit = int(request.args.get("limit", 100))
    with rca.db_lock:
        conn = get_mysql_connection(DB_NAME)
        cur = conn.cursor(dictionary=True)
        cur.execute("""
            SELECT * FROM telemetry
            WHERE ugrid_id=%s AND battery_index=%s
            ORDER BY ts DESC
            LIMIT %s
        """, (ugrid_id, bat_idx, limit))
        rows = cur.fetchall()
        cur.close()
        conn.close()

    for r in rows:
        r["ts"] = r["ts"].isoformat()
    return jsonify(rows)


@app.route("/api/ugrids/<ugrid_id>/mpc_params", methods=["POST", "GET"])
def api_mpc_params(ugrid_id):
    """
    POST: cambia alfa, beta, gamma (e opzionalmente price) su un uGrid.
    GET: legge i parametri memorizzati a DB.
    """
    if ugrid_id not in UGRIDS:
        abort(404, description="uGrid sconosciuto")

    if request.method == "GET":
        with rca.db_lock:
            conn = get_mysql_connection(DB_NAME)
            cur = conn.cursor(dictionary=True)
            cur.execute("SELECT * FROM mpc_params WHERE ugrid_id=%s", (ugrid_id,))
            row = cur.fetchone()
            cur.close()
            conn.close()
        if not row:
            abort(404, description="Parametri MPC non definiti")
        row["updated_at"] = row["updated_at"].isoformat()
        return jsonify(row)

    data = request.get_json(force=True, silent=True) or {}
    try:
        alpha = float(data["alpha"])
        beta = float(data["beta"])
        gamma = float(data["gamma"])
        price = float(data.get("price", ENERGY_PRICE_EUR_PER_KWH))
    except (KeyError, ValueError):
        abort(400, description="alpha, beta, gamma (e opzionalmente price) sono richiesti come numeri")

    rca.set_mpc_params(ugrid_id, alpha, beta, gamma, price)
    return jsonify({"status": "ok", "alpha": alpha, "beta": beta, "gamma": gamma, "price": price})


@app.route("/api/alerts", methods=["GET"])
def api_alerts():
    """
    Restituisce gli ultimi N alert (per eventuale consultazione da CLI).
    """
    limit = int(request.args.get("limit", 50))
    with rca.db_lock:
        conn = get_mysql_connection(DB_NAME)
        cur = conn.cursor(dictionary=True)
        cur.execute("""
            SELECT * FROM alerts
            ORDER BY ts DESC
            LIMIT %s
        """, (limit,))
        rows = cur.fetchall()
        cur.close()
        conn.close()

    for r in rows:
        r["ts"] = r["ts"].isoformat()
    return jsonify(rows)


# ---------------------------------------------------------------------------
# ENTRYPOINT
# ---------------------------------------------------------------------------

def main():
    init_database()
    rca.start()

    def handle_sig(sig, frame):
        logger.info("Ricevuto segnale, arresto RCA...")
        rca.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, handle_sig)
    signal.signal(signal.SIGTERM, handle_sig)

    app.run(host="0.0.0.0", port=3000, debug=False, threaded=True)


if __name__ == "__main__":
    main()
