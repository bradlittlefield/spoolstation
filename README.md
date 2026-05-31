# SpoolStation

NFC-based filament inventory and weighing station for multi-material FDM printing.
Designed around the Snapmaker U1 but works with any Moonraker/Klipper printer.

## What it does

Hang a filament spool on the mushroom mount. The PN532 reads the NFC tag and the
HX711 reads the weight simultaneously. The 3.5" touchscreen shows the spool's color
swatch, vendor, material, remaining weight, estimated length, and delta from the last
weighing. Tap T1/T2/T3/T4 or Storage to assign the spool, then tap SAVE + SYNC.
The station updates Spoolman, pushes filament data to the U1 via Moonraker, and
returns to idle in 3 seconds.

Unknown tags show a prompt on the station display and trigger the QuickRegister modal
in the web dashboard simultaneously.

## Hardware

| Component | Role |
|---|---|
| ESP32-3248S035C | Main controller + 3.5" 320x480 capacitive touchscreen (ST7796 + GT911) |
| HX711 + 5kg straight bar load cell | Weighing |
| PN532 NFC V3 module | NTAG215 tag read/write (OpenSpool protocol, UART/HSU mode) |
| Raspberry Pi 3B | Runs Spoolman (Docker, port 7912) + bridge server (port 8765) |
| NTAG215 sticker tags | One per spool, 504 byte capacity |

## Enclosure concept

Tower/riser rises from a base electronics box. Load cell exits the riser horizontally
from the back. A 3D printed mushroom mount bolts to the free end of the load cell.
The spool slides over the mushroom stem (48mm OD) and hangs face-on like a clock,
hub face resting on the cap. The PN532 is embedded in the riser face at hub height.
The ESP32 display mounts angled upward on the front face of the base box.

Mushroom mount dimensions:
- Stem OD: 48mm, length: 60mm
- Cap OD: 68mm, thickness: 8mm
- PN532 recess: 43x41mm, 4mm deep, centered in cap face
- Wire channel: 6mm diameter through stem axis
- Base plate: 20x20mm, M5 holes 10mm apart (matches load cell free end)

## Pin assignments (ESP32-3248S035C)

| Function | GPIO |
|---|---|
| HX711 SCK | 18 |
| HX711 DT | 19 |
| PN532 TX → ESP32 RX | 22 (UART2 RX) |
| PN532 RX → ESP32 TX | 17 (UART2 TX) |
| Display SPI | 13, 14, 15, 2 — RESERVED |
| Touch I2C | 33 (SDA), 32 (SCL) — RESERVED |
| Touch RST | 25 — RESERVED |
| Touch IRQ | 21 — RESERVED |
| Backlight | 27 — RESERVED |

## Repo structure

firmware/    ESP32 Arduino sketch — LVGL UI, HX711, PN532, WiFi, Spoolman API, U1 sync
bridge/      RPi Flask server — Spoolman API, Moonraker sync, filamentcolors.xyz, serial
dashboard/   React web dashboard — inventory, station monitor, toolheads, activity log
klipper/     Klipper macro + shell script for U1 print-start filament sync
docs/        Setup guide, wiring reference HTML, blueprint HTML

## Key integrations

- **Spoolman** — self-hosted filament database (Docker on RPi, port 7912)
- **Snapmaker U1 Moonraker** — filament state pushed via `/printer/filament_detect/set`
- **filamentcolors.xyz** — colorimeter-measured hex values by vendor + color name, no auth
- **OpenSpool** — open NFC tag JSON format for cross-platform filament data
- **Extended Firmware** — enables U1 to read custom NTAG tags via OpenRFID

## U1 integration note

The station does not plug into the U1 directly. All communication is over LAN via
Moonraker REST API. The U1's built-in RFID readers only read Snapmaker proprietary
Mifare tags. Custom NTAG215 tags are read by the station's PN532 only. Filament
data is pushed to U1 via Moonraker so Orca and the U1 display show correct profiles.

Extended firmware: https://github.com/paxx12/SnapmakerU1-Extended-Firmware

## Quick start

See [docs/SETUP.md](docs/SETUP.md) for full setup, wiring, calibration, and Spoolman
custom fields.

## Status

Hardware in hand. Firmware written. Physical enclosure pending Fusion 360 design.
First flash and HX711 calibration are the next steps.
