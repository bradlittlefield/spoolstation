#!/bin/bash
# spoolstation_sync.sh
# Called by Klipper shell command on print start.
# Queries Spoolman for loaded spools and pushes to U1 via Moonraker.
#
# Place at: /home/lava/printer_data/config/extended/spoolstation_sync.sh
# chmod +x spoolstation_sync.sh

SPOOLMAN="http://localhost:7912/api/v1"
# or if Spoolman runs on RPi: SPOOLMAN="http://YOUR_RPI_IP:7912/api/v1"
MOONRAKER="http://localhost:7125"

SPOOLS=$(curl -s "$SPOOLMAN/spool?allow_archived=false")

echo "$SPOOLS" | python3 - <<'EOF'
import json, sys, requests

spools_raw = sys.stdin.read()
try:
    spools = json.loads(spools_raw)
except:
    print("SpoolStation: Failed to parse Spoolman response")
    sys.exit(1)

moonraker = "http://localhost:7125"

for spool in spools:
    extra = spool.get("extra", {})
    ch = extra.get("toolhead_channel")
    if ch is None:
        continue

    filament = spool.get("filament", {})
    vendor_obj = filament.get("vendor", {})
    vendor = vendor_obj.get("name", "Generic") if isinstance(vendor_obj, dict) else "Generic"
    material = filament.get("material", "PLA")
    color_hex = (filament.get("color_hex") or "FFFFFF").lstrip("#")

    try:
        rgb_int = int(color_hex[:6], 16)
    except:
        rgb_int = 0xFFFFFF

    payload = {
        "channel": int(ch),
        "info": {
            "VENDOR": vendor,
            "MAIN_TYPE": material,
            "SUB_TYPE": filament.get("name", ""),
            "RGB_1": rgb_int,
            "ALPHA": 255,
            "HOTEND_MIN_TEMP": int(extra.get("min_temp", 200)),
            "HOTEND_MAX_TEMP": int(extra.get("max_temp", 230)),
            "BED_TEMP": int(extra.get("bed_temp", 60)),
        }
    }
    try:
        r = requests.post(f"{moonraker}/printer/filament_detect/set", json=payload, timeout=5)
        print(f"SpoolStation: T{int(ch)+1} -> {vendor} {material}: HTTP {r.status_code}")
    except Exception as e:
        print(f"SpoolStation: T{int(ch)+1} failed: {e}")

print("SpoolStation: sync complete")
EOF
