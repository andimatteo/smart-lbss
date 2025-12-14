#!/usr/bin/env python3
import asyncio
import json
import signal
import sys
import time
from datetime import datetime

from aiocoap import Context, Message
from aiocoap.numbers.codes import GET

try:
    import cbor2  # pip install cbor2
except Exception:
    cbor2 = None

BATTERY_EP = "coap://[fd00::f6ce:362e:a297:92a7]:5683"
UGRID_EP   = "coap://[fd00::f6ce:36ac:9afa:6be2]:5683"

CONTENT_FORMAT_CBOR = 60
CONTENT_FORMAT_JSON = 50

POLL_SEC = 1.0

# --- heuristics ---
def looks_like_ram_ptr(x: int) -> bool:
    return 0x20000000 <= x <= 0x2007FFFF

def now_ts():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")

def decode_any(payload: bytes, cf):
    # try JSON first
    try:
        return ("json", json.loads(payload.decode("utf-8")))
    except Exception:
        pass
    # try CBOR
    if cbor2 is not None:
        try:
            return ("cbor", cbor2.loads(payload))
        except Exception:
            pass
    return ("raw", payload)

def decode_ugrid_compact_cbor(obj):
    """
    Compatible with the compact CBOR format used in your rca.py:
      root map:
        0: cnt
        1: load_c (centi-kW)
        2: pv_c   (centi-kW)
        3: bats   (array)
      bat entry: [idx, u_c, S_c, p_c, V_c, I_c, T_c, H_c, st]
      where *_c are scaled x100 (except if your firmware differs)
    """
    if not isinstance(obj, dict):
        return None
    if not all(isinstance(k, int) for k in obj.keys()):
        return None

    cnt = int(obj.get(0, 0) or 0)
    load_kw = (obj.get(1, 0) or 0) / 100.0
    pv_kw   = (obj.get(2, 0) or 0) / 100.0
    bats_raw = obj.get(3, []) or []

    st_map = {0: "INI", 1: "RUN", 2: "ISO"}
    bats = []
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
            "_raw_Tc": T_c,
        })
    return {"cnt": cnt, "load_kw": load_kw, "pv_kw": pv_kw, "bats": bats}

def scale_battery_dev_state(obj):
    """
    From your battery res_dev_state:
      V,I,T scaled x100
      S,H scaled x10000
    """
    if not isinstance(obj, dict):
        return None
    if all(k in obj for k in ("V","I","T","S","H","St")):
        return {
            "V": obj["V"] / 100.0,
            "I": obj["I"] / 100.0,
            "T": obj["T"] / 100.0,
            "soc": obj["S"] / 10000.0,
            "soh": obj["H"] / 10000.0,
            "St": obj["St"],
        }
    return None

def sanity_warnings(label, data_dict):
    warns = []
    if not isinstance(data_dict, dict):
        return warns

    # temperature sanity
    t = data_dict.get("T")
    if isinstance(t, (int, float)) and (t < -40 or t > 125):
        warns.append(f"T fuori range: {t}")

    # V sanity
    v = data_dict.get("V")
    if isinstance(v, (int, float)) and (v < 0 or v > 100):
        warns.append(f"V fuori range: {v}")

    # soc/soh sanity
    soc = data_dict.get("soc", data_dict.get("S"))
    if isinstance(soc, (int, float)) and (soc < 0 or soc > 1.2):
        warns.append(f"SoC fuori range: {soc}")

    soh = data_dict.get("soh", data_dict.get("H"))
    if isinstance(soh, (int, float)) and (soh < 0 or soh > 1.2):
        warns.append(f"SoH fuori range: {soh}")

    # pointer-like raw Tc (from uGrid compact)
    raw_tc = data_dict.get("_raw_Tc")
    if isinstance(raw_tc, int) and looks_like_ram_ptr(raw_tc):
        warns.append(f"T_c sembra puntatore RAM: {raw_tc} (0x{raw_tc:08x})")

    return warns

async def coap_get(ctx: Context, uri: str, accept=None, timeout=3.0):
    req = Message(code=GET, uri=uri)
    if accept is not None:
        req.opt.accept = accept
    resp = await asyncio.wait_for(ctx.request(req).response, timeout=timeout)
    payload = bytes(resp.payload)
    cf = getattr(resp.opt, "content_format", None)
    return payload, cf

async def poll_once(ctx: Context, name: str, base: str):
    uri = base + "/dev/state"

    # try CBOR accept first, then fallback
    for accept in (CONTENT_FORMAT_CBOR, None):
        try:
            payload, cf = await coap_get(ctx, uri, accept=accept, timeout=3.0)
            kind, obj = decode_any(payload, cf)
            return {
                "uri": uri,
                "accept": accept,
                "content_format": cf,
                "kind": kind,
                "obj": obj,
                "payload_len": len(payload),
            }
        except Exception as e:
            last_err = e
    return {"uri": uri, "error": str(last_err)}

async def main():
    global POLL_SEC
    if len(sys.argv) > 1:
        POLL_SEC = float(sys.argv[1])

    stop = asyncio.Event()

    def _sig(*_):
        stop.set()

    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)

    ctx = await Context.create_client_context()

    try:
        while not stop.is_set():
            ts = now_ts()
            b = await poll_once(ctx, "BATTERY", BATTERY_EP)
            u = await poll_once(ctx, "UGRID", UGRID_EP)

            print(f"\n[{ts}] --- POLL ---")

            # BATTERY
            if "error" in b:
                print(f"BATTERY ERROR: {b['error']}")
            else:
                print(f"BATTERY {b['uri']} cf={b['content_format']} kind={b['kind']} len={b['payload_len']}")
                if b["kind"] == "json":
                    scaled = scale_battery_dev_state(b["obj"])
                    if scaled:
                        w = sanity_warnings("BATTERY", scaled)
                        print("  battery_scaled:", scaled)
                        for ww in w:
                            print("  !!", ww)
                    else:
                        print("  json:", b["obj"])
                else:
                    print("  obj:", b["obj"])

            # UGRID
            if "error" in u:
                print(f"UGRID ERROR: {u['error']}")
            else:
                print(f"UGRID   {u['uri']} cf={u['content_format']} kind={u['kind']} len={u['payload_len']}")
                if u["kind"] == "cbor":
                    decoded = decode_ugrid_compact_cbor(u["obj"])
                    if decoded:
                        print(f"  cnt={decoded['cnt']} load_kw={decoded['load_kw']} pv_kw={decoded['pv_kw']}")
                        for bat in decoded["bats"]:
                            w = sanity_warnings("UGRID", bat)
                            print(f"  bat{bat['idx']}: V={bat['V']} I={bat['I']} T={bat['T']} S={bat['S']} H={bat['H']} p={bat['p']} st={bat['state']}")
                            for ww in w:
                                print("   !!", ww)
                    else:
                        print("  cbor_obj:", u["obj"])
                elif u["kind"] == "json":
                    # se uGrid manda JSON in stile aggregato
                    print("  json:", u["obj"])
                else:
                    print("  obj:", u["obj"])

            # wait
            try:
                await asyncio.wait_for(stop.wait(), timeout=POLL_SEC)
            except asyncio.TimeoutError:
                pass
    finally:
        ctx.shutdown()

if __name__ == "__main__":
    asyncio.run(main())

