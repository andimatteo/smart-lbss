import json
import threading
import time
from collections import deque
from typing import Optional, Dict, Any, Tuple

import curses
import requests
import paho.mqtt.client as mqtt

# ---------------------------------------------------------------------------
# CONFIG
# ---------------------------------------------------------------------------

RCA_BASE_URL = "http://localhost:3000"
MQTT_BROKER_HOST = "localhost"
MQTT_BROKER_PORT = 1883
MQTT_ALERT_TOPIC = "ugrid/alerts/#"
STATUS_REFRESH_INTERVAL = 2.0
BATTERY_ENERGY_KWH = 13.5

# ---------------------------------------------------------------------------
# STATO CONDIVISO
# ---------------------------------------------------------------------------

status_data_lock = threading.Lock()
status_data: Dict[str, Any] = {}  # ultimo /api/status

alerts_lock = threading.Lock()
alerts = deque(maxlen=100)  # (level, text)

stop_event = threading.Event()

# ---------------------------------------------------------------------------
# HTTP helpers
# ---------------------------------------------------------------------------

def rca_get(path: str, **kwargs):
    url = RCA_BASE_URL + path
    r = requests.get(url, timeout=5, **kwargs)
    r.raise_for_status()
    return r.json()

def rca_post(path: str, json_body: Optional[dict] = None):
    url = RCA_BASE_URL + path
    r = requests.post(url, json=json_body, timeout=5)
    r.raise_for_status()
    return r.json()

def rca_delete(path: str):
    url = RCA_BASE_URL + path
    r = requests.delete(url, timeout=5)
    r.raise_for_status()
    return r.json()

# ---------------------------------------------------------------------------
# MQTT (alert asincroni)
# ---------------------------------------------------------------------------

def on_mqtt_connect(client, userdata, flags, rc):
    if rc == 0:
        with alerts_lock:
            alerts.append(("INFO", "[MQTT] Connesso, sottoscrizione agli alert..."))
        client.subscribe(MQTT_ALERT_TOPIC)
    else:
        with alerts_lock:
            alerts.append(("ERROR", f"[MQTT] Errore connessione (rc={rc})"))

def on_mqtt_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
    except Exception:
        payload = {"raw": msg.payload.decode("utf-8", errors="ignore")}

    level = payload.get("level", "info").upper()
    ugrid_id = payload.get("ugrid_id", "?")
    bat = payload.get("battery_index", "?")
    message = payload.get("message", "")
    ts = payload.get("timestamp", "")

    text = f"{ts}  {ugrid_id}/bat{bat}: {message}"
    with alerts_lock:
        alerts.append((level, text))

def start_mqtt_listener():
    client = mqtt.Client()
    client.on_connect = on_mqtt_connect
    client.on_message = on_mqtt_message

    try:
        client.connect(MQTT_BROKER_HOST, MQTT_BROKER_PORT, keepalive=60)
    except Exception as e:
        with alerts_lock:
            alerts.append(("ERROR", f"[MQTT] Impossibile connettersi al broker: {e}"))
        return None

    t = threading.Thread(target=client.loop_forever, daemon=True)
    t.start()
    return client

# ---------------------------------------------------------------------------
# THREAD DI POLLING STATO
# ---------------------------------------------------------------------------

def poll_status_loop():
    while not stop_event.is_set():
        try:
            data = rca_get("/api/status")
            with status_data_lock:
                global status_data
                status_data = data
        except Exception as e:
            with alerts_lock:
                alerts.append(("ERROR", f"[RCA] Errore lettura /api/status: {e}"))
        time.sleep(STATUS_REFRESH_INTERVAL)

# ---------------------------------------------------------------------------
# UTILITY PER COLORI / ETA
# ---------------------------------------------------------------------------

def init_colors():
    curses.start_color()
    curses.use_default_colors()

    curses.init_pair(1, curses.COLOR_GREEN, -1)   # OK
    curses.init_pair(2, curses.COLOR_YELLOW, -1)  # WARNING
    curses.init_pair(3, curses.COLOR_RED, -1)     # CRITICAL
    curses.init_pair(4, curses.COLOR_CYAN, -1)    # TITLE / LABEL
    curses.init_pair(5, curses.COLOR_MAGENTA, -1) # COMMAND / STATUS
    curses.init_pair(6, curses.COLOR_BLUE, -1)    # LOAD/PV/Grid

def draw_header(stdscr, max_x):
    title = "Smart LBSS"
    stdscr.attron(curses.color_pair(4) | curses.A_BOLD)
    stdscr.addstr(0, max_x // 2 - len(title) // 2, title)
    stdscr.attroff(curses.color_pair(4) | curses.A_BOLD)

def soc_color_pair(soc: Optional[float]) -> int:
    if soc is None:
        return 0
    if soc >= 0.7:
        return 1
    if soc >= 0.3:
        return 2
    return 3

def temp_color_pair(temp: Optional[float]) -> int:
    if temp is None:
        return 0
    if temp > 55.0:
        return 3
    if temp > 45.0:
        return 2
    return 1

def soh_color_pair(soh: Optional[float]) -> int:
    if soh is None:
        return 0
    if soh >= 0.9:
        return 1
    if soh >= 0.75:
        return 2
    return 3

def power_flow_symbol(p: Optional[float]) -> str:
    if p is None:
        return "?"
    if p > 0.05:
        return "←"  # batteria assorbe (carica)
    if p < -0.05:
        return "→"  # batteria eroga (scarica)
    return "·"

def estimate_eta_seconds(
    soc: Optional[float],
    power_kw: Optional[float],
    obj_mode: Optional[str],
    obj_tgt: Optional[float],
    energy_kwh: float = BATTERY_ENERGY_KWH,
) -> Optional[int]:
    if soc is None or power_kw is None or abs(power_kw) < 0.01:
        return None

    if obj_mode == "full_discharge":
        target_soc = 0.05
    elif obj_mode == "target_soc" and obj_tgt is not None:
        target_soc = obj_tgt
    else:
        return None

    delta = target_soc - soc
    if abs(delta) < 1e-3:
        return 0

    # power_kw > 0 → carica, power_kw < 0 → scarica
    if (delta > 0 and power_kw <= 0) or (delta < 0 and power_kw >= 0):
        return None

    energy_needed_kwh = abs(delta) * energy_kwh
    time_h = energy_needed_kwh / max(0.01, abs(power_kw))
    return int(time_h * 3600)

def format_eta(eta_sec: Optional[int]) -> str:
    if eta_sec is None:
        return "n/a"
    if eta_sec <= 0:
        return "done"
    if eta_sec < 3600:
        m = eta_sec // 60
        return f"{int(m)}m"
    h = eta_sec // 3600
    m = (eta_sec % 3600) // 60
    if h < 10:
        return f"{int(h)}h{int(m):02d}"
    return f"{int(h)}h+"

# ---------------------------------------------------------------------------
# TABLE LAYOUT (fixed widths)
# ---------------------------------------------------------------------------

TABLE_HEADER = " idx |  SoC% |  SoH% | Temp |   P[kW] |  u*[kW] |  St |   Obj    |  ETA  | ts"
TABLE_SEP = "-" * len(TABLE_HEADER)

OFF_SOC  = 6    # start of SoC cell
OFF_SOH  = 14   # start of SoH cell
OFF_TEMP = 22   # start of Temp cell

ROW_FMT = (
    "{idx:>4} |"
    "{soc:>6} |"
    "{soh:>6} |"
    "{temp:>5} |"
    "{p:>7} {sym} |"
    "{u:>7} |"
    "{st:>4} |"
    "{obj:>8} |"
    "{eta:>5} |"
    " {ts}"
)

# ---------------------------------------------------------------------------
# RENDERING TUI
# ---------------------------------------------------------------------------

def draw_status_panel(stdscr, start_y, max_y, max_x):
    y = start_y

    with status_data_lock:
        data = dict(status_data) if status_data else {}

    if not data:
        stdscr.addstr(y, 2, "Nessun dato disponibile (in attesa che la RCA acquisisca).")
        return y + 2

    for ugrid_id, info in sorted(data.items()):
        if y >= max_y - 10:
            break

        load_kw = info.get("load_kw")
        pv_kw = info.get("pv_kw")
        grid_kw = info.get("grid_power_kw")
        profit_rate = info.get("profit_eur_per_hour")
        price = info.get("price_eur_per_kwh")

        # header uGrid
        title = f"uGrid {ugrid_id}"
        stdscr.attron(curses.color_pair(4) | curses.A_BOLD)
        stdscr.addstr(y, 2, title)
        stdscr.attroff(curses.color_pair(4) | curses.A_BOLD)

        x_info = 2 + len(title) + 2
        line_used = 1

        # PV / Load / Grid line (aligned after title)
        if load_kw is not None and pv_kw is not None and x_info < max_x - 2:
            stdscr.attron(curses.color_pair(6))
            s = f"☀ PV: {pv_kw:.2f} kW   ⚡ Load: {load_kw:.2f} kW"
            if grid_kw is not None:
                if grid_kw > 0:
                    direction = "import"
                    arrow = "⇐"
                elif grid_kw < 0:
                    direction = "export"
                    arrow = "⇒"
                else:
                    direction = "eq"
                    arrow = "·"
                s += f"   {arrow} Grid: {grid_kw:+.2f} kW ({direction})"
            stdscr.addstr(y, x_info, s[: max_x - x_info - 1])
            stdscr.attroff(curses.color_pair(6))
            line_used += 1

        # €/h line (aligned under the PV/Load/Grid line)
        if grid_kw is not None and profit_rate is not None and price is not None:
            py = y + 1
            if py < max_y - 5 and x_info < max_x - 2:
                if profit_rate < 0:
                    cp = 3
                elif profit_rate > 0:
                    cp = 1
                else:
                    cp = 2
                stdscr.attron(curses.color_pair(cp))
                s2 = f"€/h: {profit_rate:+.3f}   (price ≈ {price:.3f} €/kWh)"
                stdscr.addstr(py, x_info, s2[: max_x - x_info - 1])
                stdscr.attroff(curses.color_pair(cp))
                line_used += 1

        y += line_used

        # Battery table header
        stdscr.addstr(y, 4, TABLE_HEADER[: max_x - 8])
        y += 1
        stdscr.addstr(y, 4, TABLE_SEP[: max_x - 8])
        y += 1

        bats = sorted(info.get("batteries", []), key=lambda x: x.get("index", 0))
        for b in bats:
            if y >= max_y - 5:
                break

            idx = int(b.get("index", 0))
            soc = b.get("soc")
            soh = b.get("soh")
            temp = b.get("temperature")
            power = b.get("power_kw")
            u_opt = b.get("optimal_u_kw")
            state = (b.get("state") or "???")[:4]
            ts = (b.get("ts", "") or "")[:19]

            obj_mode = b.get("objective_mode")
            obj_tgt = b.get("objective_target_soc")

            if obj_mode == "full_discharge":
                obj_str = "FD"
            elif obj_mode == "target_soc" and obj_tgt is not None:
                obj_str = f"TS{obj_tgt*100:.0f}%"
            elif obj_mode == "detach":
                obj_str = "DET"
            elif obj_mode:
                obj_str = obj_mode[:8].upper()
            else:
                obj_str = ""

            eta_sec = estimate_eta_seconds(soc, power, obj_mode, obj_tgt)
            eta_str = format_eta(eta_sec)

            soc_str = "n/a" if soc is None else f"{soc*100:5.1f}"
            soh_str = "n/a" if soh is None else f"{soh*100:5.1f}"
            temp_str = "n/a" if temp is None else f"{temp:4.1f}"
            p_str = "n/a" if power is None else f"{power:7.2f}"
            u_str = "n/a" if u_opt is None else f"{u_opt:7.2f}"
            sym = power_flow_symbol(power)

            line = ROW_FMT.format(
                idx=idx,
                soc=soc_str,
                soh=soh_str,
                temp=temp_str,
                p=p_str,
                sym=sym,
                u=u_str,
                st=state,
                obj=obj_str[:8],
                eta=eta_str[:5],
                ts=ts,
            )

            # Print base line
            stdscr.addstr(y, 4, line[: max_x - 8])

            # Re-apply colors on SoC/SoH/Temp cells (fixed offsets)
            # SoC
            scp = soc_color_pair(soc)
            if scp:
                stdscr.attron(curses.color_pair(scp) | curses.A_BOLD)
                stdscr.addstr(y, 4 + OFF_SOC, f"{soc_str:>6}"[:6])
                stdscr.attroff(curses.color_pair(scp) | curses.A_BOLD)

            # SoH
            shp = soh_color_pair(soh)
            if shp:
                stdscr.attron(curses.color_pair(shp))
                stdscr.addstr(y, 4 + OFF_SOH, f"{soh_str:>6}"[:6])
                stdscr.attroff(curses.color_pair(shp))

            # Temp
            tp = temp_color_pair(temp)
            if tp:
                stdscr.attron(curses.color_pair(tp))
                stdscr.addstr(y, 4 + OFF_TEMP, f"{temp_str:>5}"[:5])
                stdscr.attroff(curses.color_pair(tp))

            y += 1

        y += 1  # spazio tra ugrid

    return y

def draw_alerts_panel(stdscr, start_y, max_y, max_x):
    y = start_y
    if y >= max_y - 4:
        return y

    stdscr.attron(curses.color_pair(4) | curses.A_BOLD)
    stdscr.addstr(y, 2, "Alert (MQTT + RCA):")
    stdscr.attroff(curses.color_pair(4) | curses.A_BOLD)
    y += 1

    avail_lines = max_y - y - 4
    if avail_lines <= 0:
        return y

    with alerts_lock:
        last_alerts = list(alerts)[-avail_lines:]

    for level, text in last_alerts:
        if y >= max_y - 4:
            break
        lvl = level.upper()
        if lvl == "CRITICAL":
            cp = 3
        elif lvl == "WARNING":
            cp = 2
        elif lvl == "ERROR":
            cp = 3
        else:
            cp = 1

        prefix = f"[{lvl}] "
        stdscr.attron(curses.color_pair(cp))
        stdscr.addstr(y, 2, prefix)
        stdscr.attroff(curses.color_pair(cp))
        stdscr.addstr(y, 2 + len(prefix), text[: max_x - 4 - len(prefix)])
        y += 1

    return y

def draw_command_line(stdscr, cmd_buffer: str, status_msg: str):
    max_y, max_x = stdscr.getmaxyx()
    y_status = max_y - 3
    y_cmd = max_y - 2

    stdscr.hline(y_status - 1, 0, ord("-"), max_x)

    if status_msg:
        stdscr.attron(curses.color_pair(5))
        stdscr.addstr(y_status, 2, status_msg[: max_x - 4])
        stdscr.attroff(curses.color_pair(5))
    else:
        stdscr.addstr(y_status, 2, " " * (max_x - 4))

    prompt = "Command (type 'help' for help): "
    stdscr.attron(curses.color_pair(5) | curses.A_BOLD)
    stdscr.addstr(y_cmd, 2, prompt)
    stdscr.attroff(curses.color_pair(5) | curses.A_BOLD)

    max_input_len = max_x - 4 - len(prompt)
    visible = cmd_buffer[-max_input_len:]
    stdscr.addstr(y_cmd, 2 + len(prompt), " " * max_input_len)
    stdscr.addstr(y_cmd, 2 + len(prompt), visible)
    stdscr.move(y_cmd, 2 + len(prompt) + len(visible))

# ---------------------------------------------------------------------------
# PARSING COMANDI
# ---------------------------------------------------------------------------

HELP_TEXT = (
    "Comandi:\n"
    "  help                      mostra questo help\n"
    "  fd <ugrid> <bat>          scarica completamente batteria (full_discharge)\n"
    "  setsoc <ugrid> <bat> <p>  porta batteria a SoC p (percentuale 0-100)\n"
    "  detach <ugrid> <bat>      stacca batteria (alta impedenza)\n"
    "  clear <ugrid> <bat>       rimuove obiettivo per batteria\n"
    "  setmpc <ugrid> a b g [p]  cambia alpha, beta, gamma, price opzionale\n"
    "  pullalerts [N]            recupera ultimi N alert da /api/alerts\n"
    "  quit / exit               esce\n"
)

def run_command(cmd: str) -> Tuple[str, bool]:
    parts = cmd.strip().split()
    if not parts:
        return "", False

    c = parts[0].lower()

    if c in ("quit", "exit"):
        return "Uscita...", True

    if c == "help":
        for line in HELP_TEXT.splitlines():
            if not line.strip():
                continue
            with alerts_lock:
                alerts.append(("INFO", line))
        return "Help inviato nel pannello alert.", False

    try:
        if c == "fd":
            if len(parts) != 3:
                return "Uso: fd <ugrid> <bat>", False
            ugrid = parts[1]
            bat = int(parts[2])
            res = rca_post(f"/api/batteries/{ugrid}/{bat}/objective",
                           {"mode": "full_discharge"})
            return f"full_discharge impostato su {ugrid}/bat{bat}: {res}", False

        if c == "setsoc":
            if len(parts) != 4:
                return "Uso: setsoc <ugrid> <bat> <percent>", False
            ugrid = parts[1]
            bat = int(parts[2])
            perc = float(parts[3])
            target_soc = perc / 100.0
            res = rca_post(f"/api/batteries/{ugrid}/{bat}/objective",
                           {"mode": "target_soc", "target_soc": target_soc})
            return f"target SoC {perc:.1f}% impostato su {ugrid}/bat{bat}: {res}", False

        if c == "detach":
            if len(parts) != 3:
                return "Uso: detach <ugrid> <bat>", False
            ugrid = parts[1]
            bat = int(parts[2])
            res = rca_post(f"/api/batteries/{ugrid}/{bat}/objective",
                           {"mode": "detach"})
            return f"detach impostato su {ugrid}/bat{bat}: {res}", False

        if c == "clear":
            if len(parts) != 3:
                return "Uso: clear <ugrid> <bat>", False
            ugrid = parts[1]
            bat = int(parts[2])
            res = rca_delete(f"/api/batteries/{ugrid}/{bat}/objective")
            return f"Obiettivo rimosso per {ugrid}/bat{bat}: {res}", False

        if c == "setmpc":
            if len(parts) not in (5, 6):
                return "Uso: setmpc <ugrid> alpha beta gamma [price]", False
            ugrid = parts[1]
            alpha = float(parts[2])
            beta = float(parts[3])
            gamma = float(parts[4])
            body = {"alpha": alpha, "beta": beta, "gamma": gamma}
            if len(parts) == 6:
                body["price"] = float(parts[5])
            res = rca_post(f"/api/ugrids/{ugrid}/mpc_params", body)
            return f"MPC params aggiornati per {ugrid}: {res}", False

        if c == "pullalerts":
            limit = 20
            if len(parts) >= 2:
                limit = int(parts[1])
            data = rca_get("/api/alerts", params={"limit": limit})
            if not data:
                return "Nessun alert nel DB.", False
            for a in reversed(data):
                level = str(a.get("level", "info")).upper()
                text = f"{a.get('ts','')} {a.get('ugrid_id','?')}/bat{a.get('battery_index','?')}: {a.get('message','')}"
                with alerts_lock:
                    alerts.append((level, text))
            return f"{len(data)} alert caricati dal DB.", False

        return f"Comando sconosciuto: {c}. Digita 'help' per la lista.", False

    except Exception as e:
        return f"Errore eseguendo '{c}': {e}", False

# ---------------------------------------------------------------------------
# MAIN CURSES LOOP
# ---------------------------------------------------------------------------

def tui_main(stdscr):
    curses.curs_set(1)
    stdscr.nodelay(True)
    stdscr.timeout(200)

    init_colors()

    cmd_buffer = ""
    status_msg = ""

    while True:
        stdscr.erase()
        max_y, max_x = stdscr.getmaxyx()

        draw_header(stdscr, max_x)

        y = 3
        y = draw_status_panel(stdscr, y, max_y, max_x)

        y = draw_alerts_panel(stdscr, y, max_y, max_x)

        draw_command_line(stdscr, cmd_buffer, status_msg)

        stdscr.refresh()

        ch = stdscr.getch()
        if ch == -1:
            continue

        if ch in (curses.KEY_ENTER, 10, 13):
            cmd = cmd_buffer.strip()
            cmd_buffer = ""
            status_msg, should_exit = run_command(cmd)
            if should_exit:
                break
        elif ch in (curses.KEY_BACKSPACE, 127, 8):
            cmd_buffer = cmd_buffer[:-1]
        elif ch == 27:
            cmd_buffer = ""
        else:
            if 32 <= ch <= 126:
                cmd_buffer += chr(ch)

    stop_event.set()

def main():
    poller = threading.Thread(target=poll_status_loop, daemon=True)
    poller.start()

    start_mqtt_listener()

    curses.wrapper(tui_main)

if __name__ == "__main__":
    main()
