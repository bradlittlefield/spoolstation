# SpoolStation — Session Context Handoff

This document exists so a fresh Claude instance can take over debugging and
development without losing context. Read this before touching any code.

---

## Project identity

**Name:** SpoolStation (was SpoolDesk earlier in development — some file names
still say spooldesk, that is intentional for now)
**Repo:** bradlittlefield/spoolstation
**Owner:** Brad Littlefield, Lead Electrical Designer, A&E firm, Andover MA
**Surface Pro 8, 16GB. ADHD. Pragmatic. No over-engineering.**

---

## What it is

A physical filament inventory and weighing station for 3D printing. A spool hangs
on a mushroom-shaped NFC/scale mount. The station reads the tag, weighs the spool,
shows info on a touchscreen, lets you assign it to a printer toolhead, and syncs
everything to a local database and the Snapmaker U1 printer.

---

## Hardware (all purchased and in hand)

| Part | Detail |
|---|---|
| ESP32-3248S035C | 3.5" 320x480 capacitive IPS display, ST7796 driver, GT911 touch, ESP32-WROOM-32, micro-USB |
| HX711 + 5kg load cell | Straight bar TAL220B type, 75x12.7x12.7mm |
| PN532 NFC V3 | 13.56MHz, UART/HSU mode (SEL0=LOW, SEL1=HIGH) |
| NTAG215 tags | 504 byte, OpenSpool JSON format |
| Raspberry Pi 3B | Micro-USB power, runs Spoolman + bridge |
| Elegoo Uno R3 | Not used in this build — superseded by ESP32 display |

---

## Pin assignments (ESP32-3248S035C) — FINAL

| Function | GPIO | Notes |
|---|---|---|
| HX711 SCK | 18 | Clock |
| HX711 DT | 19 | Data |
| PN532 TX → ESP32 | 22 | UART2 RX |
| PN532 RX ← ESP32 | 17 | UART2 TX |
| Display CLK | 14 | RESERVED |
| Display MOSI | 13 | RESERVED |
| Display CS | 15 | RESERVED |
| Display DC | 2 | RESERVED |
| Backlight | 27 | RESERVED |
| Touch SDA | 33 | RESERVED |
| Touch SCL | 32 | RESERVED |
| Touch RST | 25 | RESERVED |
| Touch IRQ | 21 | RESERVED |

GPIO 25 is taken by GT911 touch reset. This is why HX711 is on 18/19 not 25/26
as you might see in older notes.

---

## Software architecture

### ESP32 firmware (`firmware/spoolstation_esp32.ino`)

Arduino IDE. Libraries required:
- TFT_eSPI by Bodmer — configure User_Setup.h (config block at top of .ino)
- LVGL v8.3.x by kisvegabor — NOT v9, breaking API changes
- HX711 Arduino Library by bogde
- Adafruit PN532 by Adafruit
- ArduinoJson by Benoit Blanchon

Board settings: ESP32 Dev Module, 240MHz, 4MB, Default spiffs, 921600 baud

TFT_eSPI User_Setup.h: ST7796_DRIVER, CS=15, DC=2, RST=-1, MOSI=13, SCLK=14,
MISO=12, BL=27, SPI_FREQUENCY=65000000

Station state machine:
- IDLE → tag detected → TAG_READ → Spoolman lookup
- Known spool → SPOOL_FOUND → shows color swatch, weight, length, delta,
  T1/T2/T3/T4/Storage buttons, SAVE+SYNC button
- Tap SAVE+SYNC → SAVING → updates Spoolman weight + toolhead_channel,
  pushes to U1 Moonraker → SAVED (3s confirmation) → IDLE
- Unknown tag → UNKNOWN_TAG → posts to bridge /station/event,
  shows UID + weight + dismiss button
- WiFi fail → WIFI_ERROR screen

Weight stability: 6-sample rolling buffer, stable when max-min <= 2g

### RPi bridge (`bridge/spoolstation_bridge.py`)

Flask on port 8765. Key endpoints:

| Endpoint | Method | Purpose |
|---|---|---|
| /status | GET | Live weight + last tag UID — polled every 2s by dashboard |
| /station/event | POST | Receives unknown_tag events from ESP32 |
| /register-and-write | POST | Creates vendor/filament/spool in Spoolman + writes NFC tag |
| /write-tag | POST | Writes OpenSpool JSON to tag for existing spool |
| /colors/lookup | GET | filamentcolors.xyz color lookup by vendor+name |
| /colors/vendor | GET | All colors for a vendor |
| /u1/sync | POST | Manual sync: Spoolman toolhead data → U1 Moonraker |
| /u1/toolheads | GET | Cached U1 filament state |
| /spools | GET | All Spoolman spools enriched with calculated length |

Background threads: serial_listener (reads ESP32 serial), u1_poll_loop (30s interval)

### Spoolman (Docker on RPi, port 7912)

Self-hosted filament database. Docker compose at ~/spoolman/docker-compose.yml.

Custom extra fields required in Spoolman Settings → Custom Fields:

| Field | Type |
|---|---|
| nfc_uid | text |
| tare_weight | integer |
| min_temp | integer |
| max_temp | integer |
| bed_temp | integer |
| toolhead_channel | integer |
| remaining_length_m | float |
| last_weighed | text |

### Web dashboard (`dashboard/dashboard.jsx`)

React. Served separately or via Flask static. Config at top of file:
- SPOOLMAN_BASE = "http://raspberrypi.local:7912"
- BRIDGE_BASE = "http://raspberrypi.local:8765"

Tabs: Inventory, Station, Toolheads, Activity
Features: filamentcolors.xyz color lookup, QuickRegisterModal for unknown tags,
U1 sync button, 30s auto-poll U1, 2s bridge status poll.

---

## Enclosure design (pending Fusion 360)

Brad is designing the enclosure himself. Key geometry:

**Base box** — houses RPi 3B + ESP32 display board + HX711 + PN532 + power.
ESP32 display mounts angled upward (~15-20 deg) on front face.
RPi powered by 5V 2.5A micro-USB wall adapter.
ESP32 powered from RPi USB-A port (single micro-USB cable = power + serial).

**Tower/riser** — rises vertically from top center of base box.
PN532 embedded flush in the riser face at spool hub height.
Load cell exits horizontally from back of riser.

**Mushroom mount** (separate 3D printed piece, PETG):
- Stem OD 48mm, length 60mm — spool slides over this
- Cap OD 68mm, thickness 8mm — spool hub face rests on this
- PN532 recess 43x41mm, 4mm deep, centered in cap face
- 6mm wire channel through stem axis
- Base plate 20x20mm, M5 holes 10mm apart matching load cell free end

**Spool orientation** — hangs face-on like a clock (hub toward viewer).
Load cell is horizontal, fixed end bolted to riser back wall,
free end pointing toward viewer, mushroom mount on free end.

**Component dimensions for Fusion:**

RPi 3B: 85.6 x 56.5mm, M2.5 mount holes at (3.5,3.5), (3.5,52.5), (61.5,3.5), (61.5,52.5)
ESP32 display: 101.5 x 54.9mm, no mounting holes — mount via bezel lip/rail
HX711: 33.5 x 20.3mm, no mount holes
PN532: 42.7 x 40.4mm, 4x 3mm holes ~2.5mm from corners
Load cell: 75 x 12.7 x 12.7mm, 2x M4 fixed end, 2x M5 free end, holes 10mm apart

---

## U1 integration

Station does NOT plug into U1 directly. All comms over LAN via Moonraker.
U1 built-in RFID reads only Snapmaker proprietary Mifare tags (encrypted).
Custom NTAG215 tags read by station PN532 only.

Workflow:
1. Hang spool on station
2. Station reads tag + weight
3. Tap toolhead assignment + SAVE+SYNC
4. Bridge updates Spoolman weight + toolhead_channel
5. Bridge calls POST /printer/filament_detect/set on U1 Moonraker
6. U1 and Orca show correct filament profile as if they read a tag

Extended firmware required for OpenRFID:
https://github.com/paxx12/SnapmakerU1-Extended-Firmware

---

## filamentcolors.xyz integration

Free colorimeter-measured hex values, 3000+ spools, 318+ manufacturers.
API: https://filamentcolors.xyz/api/ — no auth required.
Bridge proxies lookups to avoid CORS from dashboard.
Confirmed vendors in DB: Prusament, Bambu, eSun, Polymaker, Sunlu, Hatchbox, Kingroon.

---

## Paint chip cards (future feature — not built yet)

Idea: 3D print credit-card-sized filament sample cards with NFC tag embedded.
Pause print at layer 6 (1.2mm height), insert NTAG215 tag, resume.
Top cover: 14 layers at 0.2mm = 2.8mm over tag.
Phone tap → opens mobile spool page via NDEF URL in tag.
Color swatch square printed in actual filament color via filament swap mid-print.
Card dimensions: 85x55mm, 4mm total thickness.
Not yet implemented in firmware or dashboard.

---

## Mobile spool page (future feature — not built yet)

Plan: NDEF URL in tag points to http://[rpi-ip]:8765/spool/[id]
Bridge serves a mobile-optimized HTML page showing spool data.
Not yet implemented.

---

## HX711 calibration (pending — hardware not yet tested)

1. Flash firmware with CALIBRATION_FACTOR = 1.0
2. Serial Monitor 115200 baud
3. Place known 200g weight on mushroom mount
4. Note raw value printed
5. CALIBRATION_FACTOR = raw_value / 200.0
6. Re-flash, verify with second known weight
7. Expected range: 400–800 for this load cell

---

## PN532 jumper (CRITICAL — must do before wiring)

SEL0 = LOW (0), SEL1 = HIGH (1) for UART/HSU mode.
Verify with multimeter before connecting.
Default from factory is usually HSU but confirm — wrong mode = no communication.

---

## What still needs to happen

1. Physical enclosure — Brad designing in Fusion 360
2. Mushroom mount print
3. First flash — Arduino IDE setup, library install, TFT_eSPI User_Setup.h config
4. HX711 calibration
5. Spoolman setup on RPi — Docker install, custom fields, initial spool population
6. PN532 jumper verify + wiring
7. Bridge setup on RPi — pip install, edit CONFIG block (WiFi, RPi IP, U1 IP)
8. First end-to-end test: hang known spool, verify weight + tag read, verify Spoolman update
9. U1 extended firmware install + OpenRFID config
10. Mobile spool page (bridge endpoint + HTML template)
11. Paint chip card STL + Orca pause workflow

---

## Known issues / things to watch for in debug

- The firmware header still says "SpoolDesk" in some label strings — cosmetic only,
  update to "SPOOLSTATION" when doing first flash cleanup
- GPIO 25 conflict: GT911 touch uses GPIO 25 for reset. HX711 was moved to 18/19
  specifically because of this. Do not move it back.
- LVGL v9 is NOT compatible with this firmware. Library Manager default may offer v9.
  Install v8.3.x specifically.
- Weight stability check requires STABLE_SAMPLES (6) readings within 2g range.
  If scale is noisy, increase STABLE_THRESHOLD_G or STABLE_SAMPLES.
- Spoolman spool lookup fetches ALL spools on every tag read (no pagination yet).
  Fine for personal use (<100 spools) but will slow down at scale.
- U1 sync uses RGB color proximity matching to identify which Spoolman spool
  corresponds to which U1 toolhead. This is imprecise — the correct fix is to
  use the station assignment (toolhead_channel in Spoolman extra fields) as the
  source of truth rather than trying to match against U1 filament_detect output.
- Bridge serial port defaults to /dev/ttyACM0. Check with `ls /dev/ttyACM*` after
  connecting ESP32 to RPi USB.

---

## Files in repo

| Path | Status | Notes |
|---|---|---|
| firmware/spoolstation_esp32.ino | Complete | Full LVGL firmware |
| bridge/spoolstation_bridge.py | Complete | Full Flask bridge |
| dashboard/dashboard.jsx | Complete | Full React dashboard |
| docs/SETUP.md | Complete | Hardware + software setup guide |
| klipper/spoolstation.cfg | Complete | Klipper macro |
| klipper/spoolstation_sync.sh | Complete | Print-start sync script |
| README.md | Complete | Project overview |
| docs/CONTEXT.md | This file | Session handoff context |