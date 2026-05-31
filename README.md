# SpoolStation

NFC-based filament inventory and weighing station for multi-material FDM printing. Designed for the Snapmaker U1 but compatible with any Moonraker-based printer.

## What it does

- Hang a spool on the mushroom mount
- PN532 reads the NFC tag, HX711 reads the weight
- Touchscreen shows: color swatch, vendor, material, remaining weight, estimated length, delta from last weigh
- Tap T1/T2/T3/T4 or Storage to assign
- Tap SAVE + SYNC — updates Spoolman, pushes filament data to U1 via Moonraker

## Hardware

| Component | Role |
|---|---|
| ESP32-3248S035C | Main controller + 3.5" capacitive touchscreen |
| HX711 + 5kg load cell | Weighing |
| PN532 NFC module | Tag read/write (NTAG215, OpenSpool protocol) |
| Raspberry Pi 3B | Runs Spoolman (Docker) + bridge server |
| 3D printed enclosure | Tower/riser + mushroom spool mount |

## Repo structure

```
firmware/     ESP32 Arduino sketch (LVGL UI, HX711, PN532, WiFi, Spoolman API)
bridge/       RPi Python bridge (Flask API, Spoolman, Moonraker sync, filamentcolors.xyz)
dashboard/    React web dashboard (inventory, station monitor, toolheads, activity log)
klipper/      Klipper macro + shell script for U1 print-start sync
docs/         Setup guide, wiring diagram HTML, blueprint HTML
```

## Key integrations

- **Spoolman** — filament inventory database (self-hosted, Docker)
- **Snapmaker U1 Moonraker** — filament state pushed via `/printer/filament_detect/set`
- **filamentcolors.xyz** — colorimeter-measured hex values by vendor + color name
- **OpenSpool** — NFC tag format for cross-platform filament data
- **Extended Firmware** — enables U1 to read custom NTAG tags via OpenRFID

## Quick start

See [docs/SETUP.md](docs/SETUP.md) for full setup instructions.

## Status

Active development. Hardware in hand. Firmware compiling.
