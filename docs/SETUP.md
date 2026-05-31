# SpoolStation — Complete Setup Guide

## Parts List

| Part | Qty | ASIN | ~Price |
|------|-----|------|--------|
| ESP32-3248S035C (3.5" capacitive touch display) | 1 | B0C4KSKW96 | $24 |
| 5kg Load Cell + HX711 Combo | 1 | B075317R45 | $15 |
| HiLetgo PN532 NFC V3 Module | 1 | B01I1J17LC | $9 |
| NTAG215 NFC Stickers 50-pack | 1 | B087NFCQM3 | $12 |
| Dupont wire kit | 1 | B01EV70C78 | $6 |
| Raspberry Pi 3B+ (you have one) | 1 | — | $0 |
| USB-A to Micro-USB cable (short, internal) | 1 | — | $3 |

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

---

## Arduino IDE Setup

### Board Settings
- Board: ESP32 Dev Module
- CPU Frequency: 240MHz
- Flash Size: 4MB
- Partition Scheme: Default 4MB with spiffs
- Upload Speed: 921600

### Required Libraries (Library Manager)
- TFT_eSPI by Bodmer
- LVGL by kisvegabor — **v8.3.x ONLY (not v9)**
- HX711 Arduino Library by bogde
- Adafruit PN532 by Adafruit
- ArduinoJson by Benoit Blanchon

### TFT_eSPI User_Setup.h
Replace contents of `Arduino/libraries/TFT_eSPI/User_Setup.h` with:
```
#define ST7796_DRIVER
#define TFT_WIDTH  320
#define TFT_HEIGHT 480
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

---

## Raspberry Pi — Spoolman (Docker)

```bash
curl -sSL https://get.docker.com | sh
sudo usermod -aG docker $USER && newgrp docker

sudo mkdir -p /opt/spoolman/data
sudo chown 1000:1000 /opt/spoolman/data

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
#   u1_ip: your Snapmaker U1 IP
#   bridge_port: 8765

python3 bridge/spoolstation_bridge.py
```

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
3. Place known 200g weight on mushroom mount
4. Note raw value printed
5. Set `CALIBRATION_FACTOR = raw_value / 200.0`
6. Re-flash and verify

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

UNKNOWN SPOOL:
  1. Hang spool — UNKNOWN SPOOL screen appears
  2. Open web dashboard on laptop — QuickRegister modal fires
  3. Fill vendor/color/material — color auto-looks up from filamentcolors.xyz
  4. Click Register + Write Tag — spool registered and tag written simultaneously
```
