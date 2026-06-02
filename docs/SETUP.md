# SpoolStation — Complete Setup Guide

## Parts List

| Part | Qty | ASIN | ~Price |
|------|-----|------|--------|
| ESP32-3248S035C (3.5" capacitive touch display) | 1 | B0C4KSKW96 | $24 |
| 5kg Load Cell + HX711 Combo | 1 | B075317R45 | $15 |
| HiLetGo PN532 NFC V3 Module | 1 | B01I1J17LC | $9 |
| NTAG215 NFC Stickers 50-pack | 1 | B087NFCQM3 | $12 |
| Jumper wire kit (male-to-male) | 1 | B01EV70C78 | $6 |
| Half-size breadboard | 1 | — | $4 |
| Raspberry Pi 3B (you have one) | 1 | — | $0 |
| USB-A to Micro-USB cable (short, internal) | 1 | — | $3 |
| USB-C to Micro-USB cable (RPi power) | 1 | — | $3 |

---

## Pin Assignments (ESP32-3248S035C)

| Function | GPIO | Notes |
|---|---|---|
| HX711 SCK | 18 | Clock |
| HX711 DT | 19 | Data |
| PN532 TX (UART2 RX) | 22 | PN532 sends to ESP32 |
| PN532 RX (UART2 TX) | 17 | ESP32 sends to PN532 |
| Display SPI | 13,14,15,2 | RESERVED — do not use |
| Touch I2C | 33(SDA),32(SCL) | RESERVED — do not use |
| Touch RST | 25 | RESERVED — do not use |
| Touch IRQ | 21 | RESERVED — do not use |
| Backlight | 27 | RESERVED — do not use |

### PN532 Wiring (UART/HSU mode)

The ESP32-3248S035C exposes PN532 connections via a 4-pin JST header labeled
**GND / IO22 / IO21 / 3.3V** (Option 3 on the board back).

| PN532 Pin | ESP32 Header Pin |
|---|---|
| GND | GND |
| TXD | IO22 (UART2 RX) |
| RXD | IO17 — **see note below** |
| VCC | 3.3V |

**Note on GPIO 17:** GPIO 17 is not exposed on any header on the ESP32-3248S035C.
For bench testing, the PN532 RXD line can be left disconnected — the station will
read tags (RX only) but cannot write to tags without TX. Full TX requires either
soldering a wire to the GPIO 17 pad on the ESP32 module or using a different ESP32
dev board with full pin breakout.

### PN532 DIP Switch (UART/HSU mode)
- Switch 1: DOWN (0)
- Switch 2: DOWN (0)

Verify with multimeter before connecting. Wrong mode = no UART communication.

---

## Arduino IDE Setup

### Board Settings
- Board: ESP32 Dev Module
- CPU Frequency: 240MHz
- Flash Size: 4MB
- **Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)**
- Upload Speed: 921600

### Required Libraries (Library Manager)
- TFT_eSPI by Bodmer (latest)
- **LVGL by kisvegabor — v8.4.x (NOT v9, breaking API changes)**
- HX711 Arduino Library by bogde
- Adafruit PN532 by Adafruit
- ArduinoJson by Benoit Blanchon

### lv_conf.h Setup (required after LVGL install)

1. Navigate to `Arduino/libraries/lvgl/`
2. Copy `lv_conf_template.h` one level up to `Arduino/libraries/`
3. Rename it to `lv_conf.h`
4. Open `lv_conf.h` and change the first `#if 0` near the top to `#if 1`
5. Enable required fonts by changing `0` to `1`:

```c
#define LV_FONT_MONTSERRAT_10  1
#define LV_FONT_MONTSERRAT_12  1
#define LV_FONT_MONTSERRAT_14  1  // usually already 1
#define LV_FONT_MONTSERRAT_16  1
#define LV_FONT_MONTSERRAT_22  1
#define LV_FONT_MONTSERRAT_48  1
```

### TFT_eSPI User_Setup.h
Replace contents of `Arduino/libraries/TFT_eSPI/User_Setup.h` with the provided
`User_Setup.h` file from the repo root, or manually configure:

```
#define ST7796_DRIVER
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_MISO 12
#define TFT_BL   27
#define TFT_BACKLIGHT_ON HIGH
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
#define SPI_FREQUENCY  65000000
#define SPI_READ_FREQUENCY  20000000
```

Also verify `Arduino/libraries/TFT_eSPI/User_Setup_Select.h` has only this line active:
```
#include <User_Setup.h>
```

---

## USB / Serial Port (Windows)

The ESP32 requires the CP210x USB driver on Windows.
Download: https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers

After installing, plug in the ESP32 via micro-USB. A new COM port will appear under
Tools → Port in Arduino IDE. If multiple COM ports exist, unplug the ESP32 and note
which port disappears — that is the correct port.

---

## Raspberry Pi — First Setup

### Flash RPi OS
1. Download Raspberry Pi Imager: https://www.raspberrypi.com/software/
2. Flash **Raspberry Pi OS Lite (64-bit)** to microSD
3. Enable SSH in Imager advanced options
4. Insert card, power on RPi, SSH in: `ssh pi@raspberrypi.local`

```bash
sudo apt update && sudo apt upgrade -y
sudo raspi-config  # set timezone
```

---

## Raspberry Pi — Spoolman (Docker)

```bash
curl -sSL https://get.docker.com | sh
sudo usermod -aG docker $USER && newgrp docker

sudo mkdir -p /opt/spoolman/data
sudo chown 1000:1000 /opt/spoolman/data

mkdir -p ~/spoolman
cat > ~/spoolman/docker-compose.yml << 'EOF'
version: "3.8"
services:
  spoolman:
    image: ghcr.io/donkie/spoolman:latest
    restart: unless-stopped
    volumes:
      - /opt/spoolman/data:/home/app/.local/share/spoolman
    ports:
      - "7912:8000"
    environment:
      - TZ=America/New_York
EOF

cd ~/spoolman && docker compose up -d
```

Spoolman UI: http://raspberrypi.local:7912

---

## Spoolman Custom Fields

In Spoolman Settings → Custom Fields, add:

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

---

## Bridge Service

```bash
pip install pyserial requests flask flask-cors

# Edit CONFIG block in spoolstation_bridge.py:
#   SPOOLMAN_URL: http://raspberrypi.local:7912
#   BRIDGE_PORT: 8765
#   U1_IP: your Snapmaker U1 IP

python3 bridge/spoolstation_bridge.py
```

Verify bridge is running: http://raspberrypi.local:8765/status

Systemd service (`/etc/systemd/system/spoolstation.service`):
```ini
[Unit]
Description=SpoolStation Bridge
After=network.target

[Service]
ExecStart=/usr/bin/python3 /home/pi/spoolstation/bridge/spoolstation_bridge.py
WorkingDirectory=/home/pi/spoolstation
Restart=always
User=pi

[Install]
WantedBy=multi-user.target
```

---

## HX711 Calibration

1. Flash firmware with `CALIBRATION_FACTOR = 1.0`
2. Open Serial Monitor at 115200 baud
3. Uncomment `scale.tare()` in setup(), re-flash
4. Place known 200g weight on mushroom mount
5. Note raw value printed to serial
6. Set `CALIBRATION_FACTOR = raw_value / 200.0`
7. Re-flash and verify with a second known weight
8. Expected range: 400–800 for this load cell

---

## Firmware Notes

### Display Orientation
Firmware runs in **landscape mode (480x320)**. `tft.setRotation(1)` is set in setup().
The LVGL canvas is 480 wide × 320 tall. Touch coordinates are remapped in `gt911_read()`
to match landscape orientation.

### WiFi Dot
The WiFi status dot in the header starts **red** on boot and turns **green** only after
a successful WiFi connection. It reflects actual state, not assumed state.

### HX711 Tare
`scale.tare()` is commented out in setup() until the HX711 is physically wired.
Uncomment it after wiring is complete. Without it, weight readings will be offset
by the tare value but the station will boot normally.

### Serial Debug
Serial output at 115200 baud reports boot progress, tag reads, and WiFi status.
Connect via USB and open Serial Monitor to debug.

---

## Mushroom Mount Dimensions

| Feature | Dimension |
|---|---|
| Stem OD | 48mm |
| Stem length | 60mm |
| Cap OD | 68mm |
| Cap thickness | 8mm |
| PN532 recess | 43x41mm, 4mm deep |
| Wire channel | 6mm dia through stem axis |
| Base plate | 20x20mm, M5 holes 10mm apart |

---

## U1 Extended Firmware

1. Download: https://github.com/paxx12/SnapmakerU1-Extended-Firmware/releases
2. Copy .bin to USB root
3. U1 touchscreen: Settings → Update → select file
4. After reboot: http://u1-ip/firmware-config
5. Snapmaker Components → RFID Detection System → openrfid-generic

---

## Workflow

```
HANG SPOOL:
  1. Hang spool on mushroom mount
  2. PN532 reads NFC tag, HX711 reads weight
  3. Display shows: color swatch, vendor, material, weight, length, delta
  4. Tap T1/T2/T3/T4 or Storage
  5. Tap SAVE + SYNC
  6. Spoolman updated, U1 Moonraker pushed, confirmation shown
  7. Auto-returns to IDLE after 3 seconds

UNKNOWN SPOOL:
  1. Hang spool — UNKNOWN SPOOL screen appears
  2. Open web dashboard on laptop — QuickRegister modal fires
  3. Fill vendor/color/material — color auto-looks up from filamentcolors.xyz
  4. Click Register + Write Tag — spool registered and tag written simultaneously
```
