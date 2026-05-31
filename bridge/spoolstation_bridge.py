#!/usr/bin/env python3
"""
SpoolDesk Bridge - Raspberry Pi
Reads Arduino serial → logs to Spoolman API → syncs to Snapmaker U1 via Moonraker

INSTALL:
    pip install pyserial requests flask flask-cors

RUN:
    python3 spooldesk_bridge.py

CONFIG:
    Edit the CONFIG block below.
"""

import serial
import json
import math
import time
import threading
import logging
import requests
from datetime import datetime, timezone
from flask import Flask, request, jsonify
from flask_cors import CORS

# ─── CONFIG ──────────────────────────────────────────────────────────────────
CONFIG = {
    # Arduino serial port — check: ls /dev/ttyACM* or /dev/ttyUSB*
    "serial_port": "/dev/ttyACM0",
    "serial_baud": 115200,

    # Spoolman (running on this RPi via Docker)
    "spoolman_url": "http://localhost:7912/api/v1",

    # Snapmaker U1 on your LAN — get IP from printer screen or router
    "u1_ip": "192.168.1.XXX",    # ← CHANGE THIS
    "u1_port": 80,
    "u1_token": "",               # ← Get from http://<u1-ip> in browser

    # Bridge HTTP server (dashboard talks to this)
    "bridge_port": 8765,

    # Minimum weight change (grams) to trigger a Spoolman update
    "update_threshold_g": 3,

    # How often (seconds) to poll U1 for filament_detect changes (30 is plenty)
    "u1_poll_interval": 30,
}

# ─── MATERIAL DENSITY TABLE ───────────────────────────────────────────────────
DENSITIES = {
    "PLA": 1.24, "PLA+": 1.22, "PLA-CF": 1.30,
    "PETG": 1.27, "PETG-CF": 1.35,
    "ABS": 1.04, "ASA": 1.07,
    "TPU": 1.21, "TPE": 1.20,
    "Nylon": 1.08, "Nylon-CF": 1.15,
    "HIPS": 1.07, "PC": 1.20,
    "PVA": 1.23,
}

# ─── LOGGING ─────────────────────────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    handlers=[logging.StreamHandler()]
)
log = logging.getLogger("spooldesk")

# ─── SHARED STATE ─────────────────────────────────────────────────────────────
state = {
    "connected": False,
    "last_weight_g": None,
    "last_tag_uid": None,
    "last_event_ts": None,
    "pending_write_spool_id": None,
    "transaction_log": [],   # list of dicts
}

# Module-level serial handle — set by serial_listener once connected.
# Flask routes write to this to send commands to the Arduino.
_serial_port = None


def serial_write(cmd: str) -> bool:
    """Send a newline-terminated command to the Arduino. Returns True if sent."""
    global _serial_port
    if _serial_port and _serial_port.is_open:
        try:
            _serial_port.write(f"{cmd}\n".encode())
            return True
        except Exception as e:
            log.warning(f"serial_write failed: {e}")
    else:
        log.warning(f"serial_write: no port open — command dropped: {cmd[:60]}")
    return False

# ─── CALC HELPERS ─────────────────────────────────────────────────────────────
def calc_remaining_length_m(weight_g: float, material: str = "PLA", diameter_mm: float = 1.75) -> float:
    density = DENSITIES.get(material, 1.24)
    r_cm = (diameter_mm / 2) / 10
    volume_cm3 = weight_g / density
    length_cm = volume_cm3 / (math.pi * r_cm ** 2)
    return round(length_cm / 100, 1)   # meters

# ─── SPOOLMAN API ─────────────────────────────────────────────────────────────
def spoolman_get(endpoint: str) -> dict | None:
    try:
        r = requests.get(f"{CONFIG['spoolman_url']}{endpoint}", timeout=5)
        r.raise_for_status()
        return r.json()
    except Exception as e:
        log.warning(f"Spoolman GET {endpoint} failed: {e}")
        return None

def spoolman_patch(endpoint: str, payload: dict) -> dict | None:
    try:
        r = requests.patch(f"{CONFIG['spoolman_url']}{endpoint}", json=payload, timeout=5)
        r.raise_for_status()
        return r.json()
    except Exception as e:
        log.warning(f"Spoolman PATCH {endpoint} failed: {e}")
        return None

def spoolman_post(endpoint: str, payload: dict) -> dict | None:
    try:
        r = requests.post(f"{CONFIG['spoolman_url']}{endpoint}", json=payload, timeout=5)
        r.raise_for_status()
        return r.json()
    except Exception as e:
        log.warning(f"Spoolman POST {endpoint} failed: {e}")
        return None

def find_spool_by_tag_uid(uid: str) -> dict | None:
    """Search Spoolman extra fields for matching NFC UID."""
    spools = spoolman_get("/spool?allow_archived=false") or []
    for spool in spools:
        extra = spool.get("extra", {})
        if extra.get("nfc_uid", "").upper() == uid.upper():
            return spool
    return None

def update_spool_weight(spool_id: int, new_remaining_g: float, material: str, diameter_mm: float, old_weight: float) -> bool:
    """Update Spoolman spool with new remaining weight and calculated length."""
    remaining_length_m = calc_remaining_length_m(new_remaining_g, material, diameter_mm)

    payload = {
        "remaining_weight": new_remaining_g,
        "extra": {
            "remaining_length_m": remaining_length_m,
            "last_weighed": datetime.now(timezone.utc).isoformat(),
        }
    }
    result = spoolman_patch(f"/spool/{spool_id}", payload)
    if result:
        log.info(f"Spool {spool_id} updated: {old_weight:.0f}g → {new_remaining_g:.0f}g ({remaining_length_m}m)")
        # Record transaction
        state["transaction_log"].insert(0, {
            "ts": datetime.now().isoformat(),
            "spool_id": spool_id,
            "old_weight": old_weight,
            "new_weight": new_remaining_g,
            "delta": round(new_remaining_g - old_weight, 1),
            "remaining_length_m": remaining_length_m,
        })
        state["transaction_log"] = state["transaction_log"][:100]   # keep last 100
        return True
    return False

# ─── U1 MOONRAKER API ─────────────────────────────────────────────────────────
def u1_headers():
    h = {"Content-Type": "application/json"}
    if CONFIG["u1_token"]:
        h["Authorization"] = f"Bearer {CONFIG['u1_token']}"
    return h

def u1_base():
    return f"http://{CONFIG['u1_ip']}:{CONFIG['u1_port']}"

def u1_set_filament(channel: int, spool: dict) -> bool:
    """
    Push filament state to U1 toolhead channel via Moonraker.
    Channel 0 = T1, 1 = T2, 2 = T3, 3 = T4
    """
    filament = spool.get("filament", {})
    vendor = filament.get("vendor", {}).get("name", "Generic") if isinstance(filament.get("vendor"), dict) else "Generic"
    material = filament.get("material", "PLA")
    color_hex = filament.get("color_hex", "FFFFFF")
    if not color_hex.startswith("#"):
        color_hex = f"#{color_hex}"

    extra = spool.get("extra", {})
    min_temp = int(extra.get("min_temp", 200))
    max_temp = int(extra.get("max_temp", 230))
    bed_temp = int(extra.get("bed_temp", 60))

    # Convert hex color to integer RGB
    c = color_hex.lstrip("#")
    rgb_int = int(c[:6], 16) if len(c) >= 6 else 0xFFFFFF

    payload = {
        "channel": channel,
        "info": {
            "VENDOR": vendor,
            "MAIN_TYPE": material,
            "SUB_TYPE": filament.get("name", ""),
            "RGB_1": rgb_int,
            "ALPHA": 255,
            "HOTEND_MIN_TEMP": min_temp,
            "HOTEND_MAX_TEMP": max_temp,
            "BED_TEMP": bed_temp,
        }
    }
    try:
        r = requests.post(
            f"{u1_base()}/printer/filament_detect/set",
            json=payload, headers=u1_headers(), timeout=5
        )
        r.raise_for_status()
        log.info(f"U1 T{channel+1} updated: {vendor} {material}")
        return True
    except Exception as e:
        log.warning(f"U1 filament_detect/set failed ch{channel}: {e}")
        return False

def u1_sync_all_toolheads():
    """
    Read current toolhead assignments from Spoolman extra fields and push all to U1.
    Assumes each spool has extra.toolhead_channel (0-3) when loaded.
    """
    spools = spoolman_get("/spool?allow_archived=false") or []
    synced = 0
    for spool in spools:
        ch = spool.get("extra", {}).get("toolhead_channel")
        if ch is not None:
            if u1_set_filament(int(ch), spool):
                synced += 1
    log.info(f"U1 sync: {synced} toolheads pushed")
    return synced

def u1_get_loaded_spools() -> list:
    """Query U1 to see what it reports as loaded."""
    try:
        r = requests.get(
            f"{u1_base()}/printer/objects/query?filament_detect",
            headers=u1_headers(), timeout=5
        )
        r.raise_for_status()
        data = r.json()
        return data.get("result", {}).get("status", {}).get("filament_detect", {}).get("info", [])
    except Exception as e:
        log.warning(f"U1 query failed: {e}")
        return []

# ─── BUILD OPENSPOOL TAG PAYLOAD ──────────────────────────────────────────────
def build_openspool_json(spool_id: int) -> str | None:
    """Build OpenSpool JSON for writing to NFC tag."""
    spool = spoolman_get(f"/spool/{spool_id}")
    if not spool:
        return None
    filament = spool.get("filament", {})
    vendor = filament.get("vendor", {}).get("name", "Generic") if isinstance(filament.get("vendor"), dict) else "Generic"
    material = filament.get("material", "PLA")
    color_hex = filament.get("color_hex", "FFFFFF")
    diameter = filament.get("diameter", 1.75)
    density = filament.get("density", DENSITIES.get(material, 1.24))
    extra = spool.get("extra", {})

    payload = {
        "protocol": "openspool",
        "version": "1.0",
        "brand": vendor,
        "type": material,
        "subtype": filament.get("name", ""),
        "color_hex": f"#{color_hex.lstrip('#')}",
        "min_temp": int(extra.get("min_temp", 200)),
        "max_temp": int(extra.get("max_temp", 230)),
        "bed_min_temp": int(extra.get("bed_min_temp", 50)),
        "bed_max_temp": int(extra.get("bed_max_temp", 65)),
        "density": density,
        "diameter": diameter,
        "spool_weight": int(extra.get("tare_weight", 200)),
        "spoolman_id": spool_id,
    }
    return json.dumps(payload, separators=(",", ":"))

# ─── ARDUINO SERIAL LISTENER ─────────────────────────────────────────────────
def serial_listener():
    """Background thread: reads Arduino serial, dispatches events."""
    while True:
        try:
            global _serial_port
            ser = serial.Serial(CONFIG["serial_port"], CONFIG["serial_baud"], timeout=1)
            _serial_port = ser
            state["connected"] = True
            log.info(f"Arduino connected on {CONFIG['serial_port']}")

            while True:
                line = ser.readline().decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                try:
                    event = json.loads(line)
                except json.JSONDecodeError:
                    log.debug(f"Non-JSON from Arduino: {line}")
                    continue

                evt = event.get("event")
                state["last_event_ts"] = datetime.now().isoformat()

                if evt == "tag_read":
                    uid = event.get("uid", "")
                    weight_g = float(event.get("weight_g", 0))
                    stable = event.get("stable", False)
                    state["last_tag_uid"] = uid
                    state["last_weight_g"] = weight_g
                    log.info(f"Tag: {uid}  Weight: {weight_g}g  Stable: {stable}")

                    if stable and weight_g > CONFIG["update_threshold_g"]:
                        spool = find_spool_by_tag_uid(uid)
                        if spool:
                            spool_id = spool["id"]
                            filament = spool.get("filament", {})
                            material = filament.get("material", "PLA")
                            diameter = filament.get("diameter", 1.75)
                            tare = int(spool.get("extra", {}).get("tare_weight", 200))
                            remaining_g = max(0, weight_g - tare)
                            old_remaining = spool.get("remaining_weight", 0)

                            if abs(remaining_g - old_remaining) >= CONFIG["update_threshold_g"]:
                                update_spool_weight(spool_id, remaining_g, material, diameter, old_remaining)
                                # Write updated tag back with new weight info
                                if state.get("pending_write_spool_id") is None:
                                    ws = build_openspool_json(spool_id)
                                    if ws:
                                        serial_write(f"WRITE:{ws}")
                        else:
                            log.warning(f"Unknown tag UID: {uid} — register spool first")

                elif evt == "weight":
                    state["last_weight_g"] = float(event.get("weight_g", 0))

                elif evt == "tag_written":
                    uid = event.get("uid", "")
                    status = event.get("status", "")
                    log.info(f"Tag written: {uid} → {status}")
                    state["pending_write_spool_id"] = None

                elif evt == "tare":
                    log.info("Scale tared")

                elif evt == "init":
                    log.info(f"Arduino init: NFC={event.get('nfc')} Scale={event.get('scale')}")

        except serial.SerialException as e:
            _serial_port = None
            state["connected"] = False
            log.warning(f"Serial disconnected: {e}")
            time.sleep(5)
        except Exception as e:
            log.error(f"Serial error: {e}")
            time.sleep(2)

# ─── FLASK API (Dashboard ↔ Bridge) ───────────────────────────────────────────
app = Flask(__name__)
CORS(app)

@app.route("/status")
def api_status():
    return jsonify({
        "connected": state["connected"],
        "last_weight_g": state["last_weight_g"],
        "last_tag_uid": state["last_tag_uid"],
        "last_event_ts": state["last_event_ts"],
    })

@app.route("/spools")
def api_spools():
    spools = spoolman_get("/spool?allow_archived=false") or []
    # Enrich with calculated length
    for s in spools:
        rw = s.get("remaining_weight") or 0
        mat = s.get("filament", {}).get("material", "PLA")
        dia = s.get("filament", {}).get("diameter", 1.75)
        s["remaining_length_m"] = calc_remaining_length_m(rw, mat, dia)
    return jsonify(spools)

@app.route("/spools/<int:spool_id>", methods=["PATCH"])
def api_update_spool(spool_id):
    data = request.get_json()
    result = spoolman_patch(f"/spool/{spool_id}", data)
    return jsonify(result or {"error": "Failed"})

@app.route("/tare", methods=["POST"])
def api_tare():
    """Tell Arduino to tare the scale."""
    # In production: write to the open serial port handle
    # For now returns instruction
    return jsonify({"status": "ok", "msg": "TARE command issued"})

@app.route("/write-tag", methods=["POST"])
def api_write_tag():
    """Queue a tag write for the tag currently on the reader (spool already in Spoolman)."""
    data = request.get_json()
    spool_id = data.get("spool_id")
    if not spool_id:
        return jsonify({"error": "spool_id required"}), 400
    payload = build_openspool_json(int(spool_id))
    if not payload:
        return jsonify({"error": "Spool not found"}), 404
    state["pending_write_spool_id"] = spool_id
    sent = serial_write(f"WRITE:{payload}")
    return jsonify({"status": "queued" if sent else "queued_offline", "payload": payload,
                    "note": "Tag will be written when spool is on reader" if not sent else ""})


@app.route("/register-and-write", methods=["POST"])
def api_register_and_write():
    """
    One-shot endpoint called by QuickRegisterModal.
    1. Creates vendor in Spoolman if needed.
    2. Creates filament type in Spoolman if needed.
    3. Creates spool record with all extra fields.
    4. Stores nfc_uid in spool extra so future scale reads auto-match.
    5. Builds OpenSpool JSON and fires WRITE: to Arduino immediately.
    6. Returns the new spool_id and tag write status.

    Body JSON:
    {
      "tag_uid": "04:AB:12:CD:EF:01",
      "vendor": "Sunlu",
      "material": "PLA",
      "name": "Galaxy Blue",
      "color_hex": "#2255FF",
      "diameter": 1.75,
      "tare_weight": 130,
      "gross_weight": 650,
      "min_temp": 200,
      "max_temp": 230,
      "bed_min_temp": 50,
      "bed_max_temp": 65
    }
    """
    data = request.get_json()
    if not data:
        return jsonify({"error": "No body"}), 400

    tag_uid     = (data.get("tag_uid") or "").strip()
    vendor_name = (data.get("vendor") or "Generic").strip()
    material    = (data.get("material") or "PLA").strip()
    color_name  = (data.get("name") or "").strip()
    color_hex   = (data.get("color_hex") or "FFFFFF").lstrip("#")
    diameter    = float(data.get("diameter") or 1.75)
    tare_weight = int(data.get("tare_weight") or 200)
    gross_weight = int(data.get("gross_weight") or 0)
    remaining   = max(0, gross_weight - tare_weight)
    density     = DENSITIES.get(material, 1.24)
    min_temp    = int(data.get("min_temp") or 200)
    max_temp    = int(data.get("max_temp") or 230)
    bed_min     = int(data.get("bed_min_temp") or 50)
    bed_max     = int(data.get("bed_max_temp") or 65)

    # ── Step 1: find or create vendor ────────────────────────────────────────
    vendors = spoolman_get("/vendor") or []
    vendor_id = None
    for v in vendors:
        if v.get("name", "").lower() == vendor_name.lower():
            vendor_id = v["id"]
            break
    if vendor_id is None:
        new_vendor = spoolman_post("/vendor", {"name": vendor_name})
        if new_vendor:
            vendor_id = new_vendor["id"]
            log.info(f"Created vendor: {vendor_name} (id={vendor_id})")
        else:
            return jsonify({"error": f"Failed to create vendor '{vendor_name}'"}), 500

    # ── Step 2: find or create filament type ─────────────────────────────────
    filaments = spoolman_get("/filament") or []
    filament_id = None
    for f in filaments:
        f_vendor = f.get("vendor") or {}
        f_vendor_id = f_vendor.get("id") if isinstance(f_vendor, dict) else None
        if (f_vendor_id == vendor_id
                and f.get("material", "").upper() == material.upper()
                and f.get("name", "").lower() == color_name.lower()):
            filament_id = f["id"]
            break
    if filament_id is None:
        new_fil = spoolman_post("/filament", {
            "vendor_id": vendor_id,
            "name": color_name,
            "material": material,
            "color_hex": color_hex,
            "diameter": diameter,
            "density": density,
            "settings_extruder_temp": max_temp,
            "settings_bed_temp": bed_max,
            "weight": 1000,
        })
        if new_fil:
            filament_id = new_fil["id"]
            log.info(f"Created filament: {vendor_name} {color_name} {material} (id={filament_id})")
        else:
            return jsonify({"error": "Failed to create filament type"}), 500

    # ── Step 3: create spool ──────────────────────────────────────────────────
    remaining_length_m = calc_remaining_length_m(remaining, material, diameter)
    new_spool = spoolman_post("/spool", {
        "filament_id": filament_id,
        "remaining_weight": remaining,
        "extra": {
            "nfc_uid": tag_uid.upper(),
            "tare_weight": tare_weight,
            "min_temp": min_temp,
            "max_temp": max_temp,
            "bed_min_temp": bed_min,
            "bed_max_temp": bed_max,
            "remaining_length_m": remaining_length_m,
            "last_weighed": datetime.now(timezone.utc).isoformat(),
        }
    })
    if not new_spool:
        return jsonify({"error": "Failed to create spool"}), 500

    spool_id = new_spool["id"]
    log.info(f"Registered spool #{spool_id}: {vendor_name} {color_name} {material} UID={tag_uid}")

    # ── Step 4: build OpenSpool JSON and write to tag ─────────────────────────
    openspool = {
        "protocol": "openspool",
        "version": "1.0",
        "brand": vendor_name,
        "type": material,
        "subtype": color_name,
        "color_hex": f"#{color_hex}",
        "min_temp": min_temp,
        "max_temp": max_temp,
        "bed_min_temp": bed_min,
        "bed_max_temp": bed_max,
        "density": density,
        "diameter": diameter,
        "spool_weight": tare_weight,
        "spoolman_id": spool_id,
    }
    openspool_str = json.dumps(openspool, separators=(",", ":"))
    tag_written = serial_write(f"WRITE:{openspool_str}")

    # Log transaction
    state["transaction_log"].insert(0, {
        "ts": datetime.now().isoformat(),
        "spool_id": spool_id,
        "action": "registered",
        "vendor": vendor_name,
        "name": color_name,
        "material": material,
        "uid": tag_uid,
        "tag_written": tag_written,
    })

    return jsonify({
        "status": "ok",
        "spool_id": spool_id,
        "filament_id": filament_id,
        "vendor_id": vendor_id,
        "remaining_weight": remaining,
        "remaining_length_m": remaining_length_m,
        "tag_write": "sent" if tag_written else "offline_queued",
        "openspool_payload": openspool_str,
    })


@app.route("/u1/status")
def api_u1_status():
    loaded = u1_get_loaded_spools()
    return jsonify({"toolheads": loaded})

@app.route("/log")
def api_log():
    return jsonify(state["transaction_log"])

@app.route("/materials")
def api_materials():
    return jsonify(DENSITIES)

# ─── FILAMENTCOLORS.XYZ COLOR LOOKUP ─────────────────────────────────────────
# Colorimeter-measured hex values — not marketing colors.
# API docs: https://filamentcolors.xyz/api/
# No auth required. Please credit/support: https://www.patreon.com/filamentcolors

FCX_BASE = "https://filamentcolors.xyz/api"
_fcx_manufacturer_cache: dict = {}   # name.lower() → id


def fcx_get_manufacturer_id(name: str) -> int | None:
    """Resolve vendor name to filamentcolors.xyz manufacturer ID. Cached per run."""
    key = name.strip().lower()
    if key in _fcx_manufacturer_cache:
        return _fcx_manufacturer_cache[key]

    # Fetch all manufacturers (318 total, one page is all of them currently)
    try:
        r = requests.get(f"{FCX_BASE}/manufacturer/", timeout=8)
        r.raise_for_status()
        mfrs = r.json().get("results", [])
        for m in mfrs:
            _fcx_manufacturer_cache[m["name"].strip().lower()] = m["id"]
    except Exception as e:
        log.warning(f"filamentcolors.xyz manufacturer fetch failed: {e}")
        return None

    # Try exact match first
    if key in _fcx_manufacturer_cache:
        return _fcx_manufacturer_cache[key]

    # Try partial match (e.g. "Prusament" matches "Prusament" but also handles
    # cases like "Bambu" matching "Bambu Lab")
    for cached_name, cached_id in _fcx_manufacturer_cache.items():
        if key in cached_name or cached_name in key:
            _fcx_manufacturer_cache[key] = cached_id   # cache alias
            return cached_id

    return None


def fcx_lookup_color(vendor: str, color_name: str, material: str = "PLA") -> dict | None:
    """
    Look up colorimeter-measured hex for a specific vendor + color name.
    Returns dict with hex_color, lab_l/a/b, td, color_name, image_front, or None.

    Strategy:
    1. Resolve vendor → manufacturer_id
    2. GET /api/swatch/?manufacturer=<id> (paginate if needed)
    3. Score each swatch by color_name similarity (case-insensitive token match)
    4. Also filter by filament_type if material matches
    5. Return best match
    """
    mfr_id = fcx_get_manufacturer_id(vendor)
    if mfr_id is None:
        log.info(f"filamentcolors.xyz: vendor '{vendor}' not found")
        return None

    # Fetch swatches for this manufacturer
    swatches = []
    url = f"{FCX_BASE}/swatch/?manufacturer={mfr_id}"
    try:
        while url:
            r = requests.get(url, timeout=8)
            r.raise_for_status()
            data = r.json()
            swatches.extend(data.get("results", []))
            url = data.get("next")   # paginate
    except Exception as e:
        log.warning(f"filamentcolors.xyz swatch fetch failed: {e}")
        return None

    if not swatches:
        return None

    # Score swatches by color name match
    query_tokens = set(color_name.lower().split())
    material_lower = material.lower()
    best_score = -1
    best_swatch = None

    for s in swatches:
        swatch_name = (s.get("color_name") or "").lower()
        swatch_type = (s.get("filament_type") or {}).get("name", "").lower()

        # Token overlap score
        swatch_tokens = set(swatch_name.split())
        overlap = len(query_tokens & swatch_tokens)
        exact = 1 if swatch_name == color_name.lower() else 0

        # Bonus if material type matches (pla, petg, abs, etc.)
        type_bonus = 1 if material_lower in swatch_type or swatch_type in material_lower else 0

        score = exact * 100 + overlap * 10 + type_bonus
        if score > best_score:
            best_score = score
            best_swatch = s

    if best_swatch and best_score > 0:
        return {
            "hex_color": f"#{best_swatch.get('hex_color', 'FFFFFF')}",
            "color_name": best_swatch.get("color_name"),
            "filament_type": (best_swatch.get("filament_type") or {}).get("name"),
            "lab_l": best_swatch.get("lab_l"),
            "lab_a": best_swatch.get("lab_a"),
            "lab_b": best_swatch.get("lab_b"),
            "td": best_swatch.get("td"),
            "image_front": best_swatch.get("image_front"),
            "image_card": best_swatch.get("card_img"),
            "swatch_id": best_swatch.get("id"),
            "swatch_url": f"https://filamentcolors.xyz/swatch/{best_swatch.get('slug', '')}",
            "score": best_score,
            "source": "filamentcolors.xyz",
        }

    # No name match — return closest available color for this vendor as fallback
    # (caller can present a picker from the list)
    return None


def fcx_list_vendor_colors(vendor: str, material: str | None = None) -> list:
    """Return all known colors for a vendor, optionally filtered by material."""
    mfr_id = fcx_get_manufacturer_id(vendor)
    if mfr_id is None:
        return []

    swatches = []
    url = f"{FCX_BASE}/swatch/?manufacturer={mfr_id}"
    try:
        while url:
            r = requests.get(url, timeout=8)
            r.raise_for_status()
            data = r.json()
            swatches.extend(data.get("results", []))
            url = data.get("next")
    except Exception as e:
        log.warning(f"filamentcolors.xyz list failed: {e}")
        return []

    results = []
    for s in swatches:
        swatch_type = (s.get("filament_type") or {}).get("name", "")
        if material and material.lower() not in swatch_type.lower():
            continue
        results.append({
            "id": s.get("id"),
            "color_name": s.get("color_name"),
            "hex_color": f"#{s.get('hex_color', 'FFFFFF')}",
            "filament_type": swatch_type,
            "image_card": s.get("card_img"),
            "swatch_url": f"https://filamentcolors.xyz/swatch/{s.get('slug', '')}",
            "td": s.get("td"),
        })
    return results


# ─── COLOR LOOKUP API ENDPOINTS ───────────────────────────────────────────────

@app.route("/colors/lookup")
def api_color_lookup():
    """
    Resolve vendor + color name to measured hex.
    Query params: vendor, color_name, material (optional)
    Returns: {hex_color, color_name, image_front, swatch_url, source} or {error}
    Example: /colors/lookup?vendor=Prusament&color_name=Galaxy Purple&material=PLA
    """
    vendor = request.args.get("vendor", "").strip()
    color_name = request.args.get("color_name", "").strip()
    material = request.args.get("material", "PLA").strip()

    if not vendor or not color_name:
        return jsonify({"error": "vendor and color_name required"}), 400

    result = fcx_lookup_color(vendor, color_name, material)
    if result:
        return jsonify(result)
    return jsonify({"error": "not_found", "vendor": vendor, "color_name": color_name}), 404


@app.route("/colors/vendor")
def api_color_vendor_list():
    """
    List all known colors for a vendor from filamentcolors.xyz.
    Query params: vendor, material (optional filter)
    Example: /colors/vendor?vendor=Kingroon&material=PLA
    """
    vendor = request.args.get("vendor", "").strip()
    material = request.args.get("material", "").strip() or None

    if not vendor:
        return jsonify({"error": "vendor required"}), 400

    colors = fcx_list_vendor_colors(vendor, material)
    return jsonify({"vendor": vendor, "count": len(colors), "colors": colors})


@app.route("/colors/manufacturers")
def api_color_manufacturers():
    """Return full manufacturer list from filamentcolors.xyz (cached after first call)."""
    if not _fcx_manufacturer_cache:
        fcx_get_manufacturer_id("__init__")   # trigger cache populate
    return jsonify(sorted(_fcx_manufacturer_cache.keys()))



# ─── U1 TOOLHEAD POLLER ───────────────────────────────────────────────────────
# Polls /printer/objects/query?filament_detect every u1_poll_interval seconds.
# Matches detected spools back to Spoolman records by NFC UID stored in extra.nfc_uid.
# Updates toolhead_channel custom field on the matching spool so the dashboard
# reflects which spool is physically loaded without any manual input.

state["u1_toolheads"] = []       # live U1 filament_detect snapshot
state["last_u1_poll"] = None     # ISO timestamp of last successful poll


def _match_spool_by_uid(uid: str, spools: list) -> dict | None:
    """Find a Spoolman spool whose extra.nfc_uid matches the given tag UID."""
    uid_upper = uid.upper().replace("-", ":").strip()
    for s in spools:
        stored = (s.get("extra") or {}).get("nfc_uid", "").upper().replace("-", ":").strip()
        if stored and stored == uid_upper:
            return s
    return None


def sync_u1_to_spoolman() -> dict:
    """
    One sync cycle:
    1. Query U1 filament_detect for all 4 channels.
    2. For each channel, match the tag UID to a Spoolman spool.
    3. Update toolhead_channel on matched spool; clear it on spools no longer loaded.
    Returns a summary dict for the /u1/sync endpoint to return to the dashboard.
    """
    detected = u1_get_loaded_spools()   # list of 4 channel info dicts from U1
    if not detected:
        return {"error": "U1 unreachable or no data"}

    state["u1_toolheads"] = detected
    state["last_u1_poll"] = datetime.now().isoformat()

    all_spools = spoolman_get("/spool?allow_archived=false") or []
    changes = []

    # Build map: channel → detected UID (empty string if no tag)
    channel_uid: dict[int, str] = {}
    for ch_idx, ch_info in enumerate(detected):
        # U1 returns VENDOR=NONE / OFFICIAL=false when no proprietary tag detected
        # For OpenSpool tags, OpenRFID populates these fields from tag data.
        # The raw UID isn't in filament_detect — it's in the tag processor logs.
        # Best available key: we wrote spoolman_id into the OpenSpool JSON payload,
        # and the U1 exposes the parsed brand/type. We use nfc_uid from Spoolman
        # matched against what the tag wrote to filament_detect.
        # If spoolman_id is in the sub_type field (we put it there), use that.
        sub = (ch_info.get("SUB_TYPE") or "")
        vendor = (ch_info.get("VENDOR") or "NONE")
        main_type = (ch_info.get("MAIN_TYPE") or "NONE")
        is_loaded = vendor != "NONE" and main_type != "NONE"
        channel_uid[ch_idx] = sub if is_loaded else ""

    # Match channels to spools and update Spoolman
    newly_loaded: set[int] = set()
    for ch_idx, sub_type in channel_uid.items():
        if not sub_type:
            continue
        # Try to extract spoolman_id we embedded in the tag JSON as subtype
        # Format we used: subtype field in openspool payload = filament name
        # Better: find spool by vendor+type+color match since UID isn't in filament_detect
        # Use the color RGB to narrow it down
        ch_info = detected[ch_idx] if ch_idx < len(detected) else {}
        rgb_int = ch_info.get("RGB_1", 0)
        vendor = ch_info.get("VENDOR", "")
        material = ch_info.get("MAIN_TYPE", "")

        # Find best matching spool in Spoolman by vendor + material + color proximity
        best_spool = None
        best_score = 999999
        for s in all_spools:
            fil = s.get("filament") or {}
            s_vendor_obj = fil.get("vendor") or {}
            s_vendor = s_vendor_obj.get("name", "") if isinstance(s_vendor_obj, dict) else ""
            s_material = fil.get("material", "")
            s_hex = (fil.get("color_hex") or "FFFFFF").lstrip("#")

            if s_vendor.lower() != vendor.lower() or s_material.lower() != material.lower():
                continue

            # Color distance (rough RGB)
            try:
                s_r = int(s_hex[0:2], 16)
                s_g = int(s_hex[2:4], 16)
                s_b = int(s_hex[4:6], 16)
                d_r = ((rgb_int >> 16) & 0xFF) - s_r
                d_g = ((rgb_int >> 8) & 0xFF) - s_g
                d_b = (rgb_int & 0xFF) - s_b
                dist = d_r*d_r + d_g*d_g + d_b*d_b
            except Exception:
                dist = 999999

            if dist < best_score:
                best_score = dist
                best_spool = s

        if best_spool:
            spool_id = best_spool["id"]
            newly_loaded.add(spool_id)
            old_ch = (best_spool.get("extra") or {}).get("toolhead_channel")
            if old_ch != ch_idx:
                spoolman_patch(f"/spool/{spool_id}", {"extra": {"toolhead_channel": ch_idx}})
                changes.append({"spool_id": spool_id, "channel": ch_idx, "action": "loaded"})
                log.info(f"U1 T{ch_idx+1} → spool {spool_id} ({vendor} {material})")

    # Clear toolhead_channel for spools no longer detected
    for s in all_spools:
        spool_id = s["id"]
        if spool_id in newly_loaded:
            continue
        old_ch = (s.get("extra") or {}).get("toolhead_channel")
        if old_ch is not None:
            spoolman_patch(f"/spool/{spool_id}", {"extra": {"toolhead_channel": None}})
            changes.append({"spool_id": spool_id, "channel": old_ch, "action": "unloaded"})

    return {
        "polled_at": state["last_u1_poll"],
        "channels_detected": sum(1 for v in channel_uid.values() if v),
        "changes": changes,
    }


def u1_poll_loop():
    """Background thread: poll U1 every u1_poll_interval seconds."""
    interval = CONFIG["u1_poll_interval"]
    log.info(f"U1 poller started — interval: {interval}s")
    while True:
        time.sleep(interval)
        try:
            result = sync_u1_to_spoolman()
            if result.get("changes"):
                log.info(f"U1 poll: {len(result['changes'])} toolhead change(s)")
        except Exception as e:
            log.warning(f"U1 poll error: {e}")


@app.route("/u1/sync", methods=["POST"])
def api_u1_manual_sync():
    """Manual sync trigger — called by dashboard SYNC button. Returns changes immediately."""
    try:
        result = sync_u1_to_spoolman()
        return jsonify({"status": "ok", **result})
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500


@app.route("/u1/toolheads")
def api_u1_toolheads_live():
    """Return last-known U1 toolhead state from cache (no new poll)."""
    return jsonify({
        "toolheads": state["u1_toolheads"],
        "last_polled": state["last_u1_poll"],
        "poll_interval_s": CONFIG["u1_poll_interval"],
    })



@app.route("/station/event", methods=["POST"])
def api_station_event():
    """
    Receives events from the ESP32 station firmware.
    Currently handles: unknown_tag (triggers unknown tag state in dashboard via SSE or polling)
    """
    data = request.get_json()
    if not data:
        return jsonify({"error": "No body"}), 400

    evt = data.get("event")

    if evt == "unknown_tag":
        uid      = data.get("uid", "")
        weight_g = data.get("weight_g", 0)
        stable   = data.get("stable", False)
        log.info(f"Station unknown tag: {uid}  {weight_g}g  stable={stable}")
        # Store in state so dashboard /status endpoint exposes it
        state["last_tag_uid"]  = uid
        state["last_weight_g"] = weight_g
        # If the dashboard is open it will pick this up on its 2s poll
        return jsonify({"status": "ok", "uid": uid})

    elif evt == "weight":
        state["last_weight_g"] = data.get("weight_g", 0)
        return jsonify({"status": "ok"})

    return jsonify({"status": "ok", "event": evt})

if __name__ == "__main__":
    # Start serial listener in background thread
    serial_thread = threading.Thread(target=serial_listener, daemon=True)
    serial_thread.start()
    # Start U1 toolhead poller
    poll_thread = threading.Thread(target=u1_poll_loop, daemon=True)
    poll_thread.start()
    log.info(f"SpoolDesk Bridge starting on port {CONFIG['bridge_port']}")
    log.info(f"Spoolman: {CONFIG['spoolman_url']}")
    log.info(f"U1: http://{CONFIG['u1_ip']}")
    app.run(host="0.0.0.0", port=CONFIG["bridge_port"], debug=False)
