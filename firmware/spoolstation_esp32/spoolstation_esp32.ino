/*
 * SpoolStation Firmware
 * Target: ESP32-3248S035C (ST7796 display + GT911 capacitive touch)
 * Orientation: LANDSCAPE (480x320)
 *
 * LIBRARIES REQUIRED (Arduino Library Manager):
 *   - TFT_eSPI by Bodmer (configure User_Setup.h — see below)
 *   - LVGL by kisvegabor (v8.4.x — NOT v9, incompatible with this config)
 *   - HX711 Arduino Library by bogde
 *   - Adafruit PN532 by Adafruit
 *   - ArduinoJson by Benoit Blanchon
 *
 * TFT_eSPI User_Setup.h config — replace contents with:
 * -------------------------------------------------------
 * #define ST7796_DRIVER
 * #define TFT_WIDTH  320
 * #define TFT_HEIGHT 480
 * #define TFT_CS   15
 * #define TFT_DC    2
 * #define TFT_RST  -1
 * #define TFT_MOSI 13
 * #define TFT_SCLK 14
 * #define TFT_MISO 12
 * #define TFT_BL   27
 * #define TFT_BACKLIGHT_ON HIGH
 * #define LOAD_GLCD
 * #define LOAD_FONT2
 * #define LOAD_FONT4
 * #define LOAD_FONT6
 * #define LOAD_FONT7
 * #define LOAD_FONT8
 * #define LOAD_GFXFF
 * #define SMOOTH_FONT
 * #define SPI_FREQUENCY  65000000
 * #define SPI_READ_FREQUENCY  20000000
 * -------------------------------------------------------
 *
 * Arduino IDE Board Settings:
 *   Board: ESP32 Dev Module
 *   CPU Freq: 240MHz
 *   Flash Size: 4MB
 *   Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)
 *   Upload Speed: 921600
 *
 * PIN ASSIGNMENTS:
 *   HX711 SCK    -> GPIO 18
 *   HX711 DT     -> GPIO 19
 *   PN532 TXD    -> GPIO 35  (Header B IO35 — ESP32 RX, input-only pin)
 *   PN532 RXD    -> GPIO 22  (Header B IO22 — ESP32 TX)
 *   Display SPI  -> GPIO 13,14,15,2 (reserved, do not touch)
 *   Touch I2C    -> GPIO 33(SDA), 32(SCL), 21(IRQ), 25(RST)
 *
 * PN532 WIRING:
 *   Header B IO35  <-- PN532 TXD   (signal only, no power on this header)
 *   Header B IO22  --> PN532 RXD   (signal only)
 *   Header B GND   --- PN532 GND   (shared ground reference — REQUIRED)
 *   Cut USB cable red  --> PN532 VCC  (5V from RPi USB port)
 *   Cut USB cable black -> PN532 GND  (tie to ESP32 GND as well)
 *
 * POWER:
 *   ESP32 powered via RPi USB-A port (micro-USB cable)
 *   PN532 powered via separate RPi USB-A port (cut USB cable, 5V+GND only)
 *   Shared ground between ESP32 and PN532 via Header B GND — critical for UART
 *
 * Serial Monitor is fully functional (GPIO1/3 are free).
 * No need to disconnect anything before flashing.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <HX711.h>
#include <Wire.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <Adafruit_PN532.h>

struct SpoolData {
    int     id;
    String  vendor;
    String  material;
    String  name;
    String  color_hex;
    int     remaining_weight;
    int     remaining_length_m;
    int     tare_weight;
    int     min_temp;
    int     max_temp;
    int     bed_temp;
    String  tag_uid;
    int     toolhead;
    bool    valid;
};

// ─── USER CONFIG ─────────────────────────────────────────────────────────────
const char* WIFI_SSID    = "YOUR_WIFI_SSID";
const char* WIFI_PASS    = "";
const char* SPOOLMAN_URL = "http://192.168.1.XXX:7912/api/v1";
const char* BRIDGE_URL   = "http://192.168.1.XXX:8765";

float CALIBRATION_FACTOR = -96650.0;

// ─── PIN DEFINITIONS ─────────────────────────────────────────────────────────
#define HX711_SCK_PIN  18
#define HX711_DT_PIN   19

// PN532 on UART2 via Header B
// GPIO35 is input-only — ideal for RX. GPIO22 is TX.
// Serial Monitor on UART0 (GPIO1/3) remains fully functional.
#define PN532_RX_PIN   35   // Header B IO35 — PN532 TXD connects here
#define PN532_TX_PIN   22   // Header B IO22 — PN532 RXD connects here

// Touch GT911 I2C
#define GT911_SDA_PIN  33
#define GT911_SCL_PIN  32
#define GT911_IRQ_PIN  21
#define GT911_RST_PIN  25

#define TFT_BL_PIN     27

// ─── DISPLAY / LVGL ──────────────────────────────────────────────────────────
#define SCREEN_W       480
#define SCREEN_H       320
#define LV_BUF_SIZE    (SCREEN_W * 20)

TFT_eSPI tft = TFT_eSPI();
static lv_disp_draw_buf_t draw_buf;
static lv_color_t         buf[LV_BUF_SIZE];

// ─── TOUCH GT911 ─────────────────────────────────────────────────────────────
// GT911 native resolution is portrait (480 tall x 320 wide internally mapped to
// the physical 320x480 panel). For landscape rotation=1:
//   screen_x = raw_y
//   screen_y = GT911_MAX_X - raw_x
#define GT911_ADDR      0x5D
#define GT911_COORD_REG 0x814E
#define GT911_MAX_X     479
#define GT911_MAX_Y     319

struct TouchPoint { int16_t x; int16_t y; bool pressed; };

void gt911_init() {
    Wire.begin(GT911_SDA_PIN, GT911_SCL_PIN);
    pinMode(GT911_RST_PIN, OUTPUT);
    pinMode(GT911_IRQ_PIN, INPUT);
    digitalWrite(GT911_RST_PIN, LOW);
    delay(10);
    digitalWrite(GT911_RST_PIN, HIGH);
    delay(100);
}

TouchPoint gt911_read() {
    TouchPoint tp = {0, 0, false};
    Wire.beginTransmission(GT911_ADDR);
    Wire.write(GT911_COORD_REG >> 8);
    Wire.write(GT911_COORD_REG & 0xFF);
    Wire.endTransmission();
    Wire.requestFrom(GT911_ADDR, 7);
    if (Wire.available() >= 7) {
        uint8_t status = Wire.read();
        if (status & 0x80) {
            tp.pressed = (status & 0x0F) > 0;
            if (tp.pressed) {
                Wire.read(); // track id
                uint8_t xl = Wire.read(); uint8_t xh = Wire.read();
                uint8_t yl = Wire.read(); uint8_t yh = Wire.read();
                int16_t raw_x = (xh << 8) | xl;
                int16_t raw_y = (yh << 8) | yl;
                // Remap portrait GT911 coords to landscape screen coords
                tp.x = raw_y;
                tp.y = GT911_MAX_X - raw_x;
                // Clear status register
                Wire.beginTransmission(GT911_ADDR);
                Wire.write(GT911_COORD_REG >> 8);
                Wire.write(GT911_COORD_REG & 0xFF);
                Wire.write(0x00);
                Wire.endTransmission();
            }
        }
    }
    return tp;
}

// ─── HARDWARE INSTANCES ──────────────────────────────────────────────────────
HX711          scale;
HardwareSerial pn532Serial(2);   // UART2 — GPIO35(RX)/GPIO22(TX)
Adafruit_PN532 nfc(pn532Serial);

// ─── STATE ───────────────────────────────────────────────────────────────────
enum class StationState {
    IDLE,
    TAG_READ,
    SPOOL_FOUND,
    UNKNOWN_TAG,
    WEIGHING,
    SAVING,
    SAVED,
    WIFI_ERROR,
    NO_TAG_WEIGHT
};

StationState  g_state             = StationState::IDLE;
SpoolData     g_spool             = {};
String        g_last_uid          = "";
float         g_weight_g          = 0;
bool          g_weight_stable     = false;
unsigned long g_last_tag_ms       = 0;
unsigned long g_stable_since      = 0;
int           g_selected_toolhead = 0;
bool          g_saving            = false;
bool          g_wifi_ok           = false;

#define STABLE_THRESHOLD_G  2.0
#define STABLE_SAMPLES      6
float g_weight_buf[STABLE_SAMPLES];
int   g_weight_idx      = 0;
bool  g_weight_buf_full = false;

// ─── LVGL DISPLAY FLUSH ──────────────────────────────────────────────────────
void lv_disp_flush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t*)color_p, w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(disp);
}

// ─── LVGL TOUCH READ ─────────────────────────────────────────────────────────
void lv_touch_read(lv_indev_drv_t* drv, lv_indev_data_t* data) {
    TouchPoint tp = gt911_read();
    if (tp.pressed) {
        data->state   = LV_INDEV_STATE_PR;
        data->point.x = tp.x;
        data->point.y = tp.y;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

// ─── COLOR UTILITIES ─────────────────────────────────────────────────────────
lv_color_t hex_to_lv_color(const String& hex) {
    String h = hex;
    h.replace("#", "");
    if (h.length() < 6) return lv_color_white();
    long c = strtol(h.c_str(), nullptr, 16);
    return lv_color_make((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

// ─── WEIGHT HELPERS ──────────────────────────────────────────────────────────
bool is_weight_stable() {
    if (!g_weight_buf_full) return false;
    float mn = g_weight_buf[0], mx = g_weight_buf[0];
    for (int i = 1; i < STABLE_SAMPLES; i++) {
        mn = min(mn, g_weight_buf[i]);
        mx = max(mx, g_weight_buf[i]);
    }
    return (mx - mn) <= STABLE_THRESHOLD_G;
}

float get_avg_weight() {
    int count = g_weight_buf_full ? STABLE_SAMPLES : g_weight_idx;
    if (count == 0) return 0;
    float sum = 0;
    for (int i = 0; i < count; i++) sum += g_weight_buf[i];
    return sum / count;
}

// ─── UID FORMATTING ──────────────────────────────────────────────────────────
String format_uid(uint8_t* uid, uint8_t len) {
    String s = "";
    for (uint8_t i = 0; i < len; i++) {
        if (uid[i] < 0x10) s += "0";
        s += String(uid[i], HEX);
        if (i < len - 1) s += ":";
    }
    s.toUpperCase();
    return s;
}

// ─── HTTP HELPERS ────────────────────────────────────────────────────────────
String http_get(const String& url) {
    HTTPClient http;
    http.begin(url);
    http.setTimeout(5000);
    int code = http.GET();
    String body = "";
    if (code == 200) body = http.getString();
    http.end();
    return body;
}

bool http_patch(const String& url, const String& json) {
    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);
    int code = http.PATCH(json);
    http.end();
    return (code >= 200 && code < 300);
}

bool http_post(const String& url, const String& json) {
    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);
    int code = http.POST(json);
    http.end();
    return (code >= 200 && code < 300);
}

// ─── SPOOLMAN API ────────────────────────────────────────────────────────────
SpoolData find_spool_by_uid(const String& uid) {
    SpoolData s = {};
    s.valid = false;
    String body = http_get(String(SPOOLMAN_URL) + "/spool?allow_archived=false");
    if (body.length() == 0) return s;

    DynamicJsonDocument doc(32768);
    if (deserializeJson(doc, body) != DeserializationError::Ok) return s;

    JsonArray spools = doc.as<JsonArray>();
    for (JsonObject sp : spools) {
        String stored_uid = "";
        if (sp["extra"].containsKey("nfc_uid"))
            stored_uid = sp["extra"]["nfc_uid"].as<String>();
        stored_uid.toUpperCase();
        String search_uid = uid;
        search_uid.toUpperCase();
        if (stored_uid == search_uid) {
            s.valid              = true;
            s.id                 = sp["id"];
            s.remaining_weight   = sp["remaining_weight"] | 0;
            s.tare_weight        = sp["extra"]["tare_weight"] | 200;
            s.min_temp           = sp["extra"]["min_temp"] | 200;
            s.max_temp           = sp["extra"]["max_temp"] | 230;
            s.bed_temp           = sp["extra"]["bed_temp"] | 60;
            s.toolhead           = sp["extra"]["toolhead_channel"] | 0;
            s.remaining_length_m = sp["extra"]["remaining_length_m"] | 0;
            s.tag_uid            = uid;
            JsonObject fil = sp["filament"];
            if (!fil.isNull()) {
                s.material  = fil["material"].as<String>();
                s.name      = fil["name"].as<String>();
                s.color_hex = fil["color_hex"].as<String>();
                if (!s.color_hex.startsWith("#")) s.color_hex = "#" + s.color_hex;
                JsonObject ven = fil["vendor"];
                if (!ven.isNull()) s.vendor = ven["name"].as<String>();
            }
            break;
        }
    }
    return s;
}

bool update_spool_weight(int spool_id, float remaining_g, float remaining_length_m, int toolhead_ch) {
    DynamicJsonDocument doc(512);
    doc["remaining_weight"] = (int)remaining_g;
    JsonObject extra = doc.createNestedObject("extra");
    extra["remaining_length_m"] = (int)remaining_length_m;
    extra["last_weighed"] = "now";
    if (toolhead_ch >= 0) extra["toolhead_channel"] = toolhead_ch;
    else                  extra["toolhead_channel"]  = nullptr;
    String json;
    serializeJson(doc, json);
    return http_patch(String(SPOOLMAN_URL) + "/spool/" + spool_id, json);
}

bool push_to_u1(const SpoolData& s, int channel) {
    String c = s.color_hex;
    c.replace("#", "");
    long rgb = strtol(c.c_str(), nullptr, 16);
    DynamicJsonDocument doc(512);
    doc["channel"] = channel;
    JsonObject info = doc.createNestedObject("info");
    info["VENDOR"]          = s.vendor;
    info["MAIN_TYPE"]       = s.material;
    info["SUB_TYPE"]        = s.name;
    info["RGB_1"]           = (long)rgb;
    info["ALPHA"]           = 255;
    info["HOTEND_MIN_TEMP"] = s.min_temp;
    info["HOTEND_MAX_TEMP"] = s.max_temp;
    info["BED_TEMP"]        = s.bed_temp;
    String json;
    serializeJson(doc, json);
    return http_post(String(BRIDGE_URL) + "/u1/toolhead/" + channel, json);
}

// ─── LENGTH CALCULATION ──────────────────────────────────────────────────────
float calc_length_m(float weight_g, const String& material, float diameter_mm = 1.75) {
    float density = 1.24;
    if      (material == "PETG" || material == "PETG-CF") density = 1.27;
    else if (material == "ABS")   density = 1.04;
    else if (material == "ASA")   density = 1.07;
    else if (material == "TPU")   density = 1.21;
    else if (material == "Nylon") density = 1.08;
    else if (material == "PC")    density = 1.20;
    else if (material == "PLA+")  density = 1.22;
    else if (material == "HIPS")  density = 1.07;
    float r      = (diameter_mm / 2.0) / 10.0;
    float vol    = weight_g / density;
    float len_cm = vol / (PI * r * r);
    return len_cm / 100.0;
}

// ─── UI GLOBALS ──────────────────────────────────────────────────────────────
lv_obj_t* scr_idle    = nullptr;
lv_obj_t* scr_spool   = nullptr;
lv_obj_t* scr_unknown = nullptr;
lv_obj_t* scr_saving  = nullptr;
lv_obj_t* scr_saved   = nullptr;
lv_obj_t* scr_error   = nullptr;
lv_obj_t* scr_wifi    = nullptr;

lv_obj_t* lbl_vendor       = nullptr;
lv_obj_t* lbl_material     = nullptr;
lv_obj_t* lbl_name         = nullptr;
lv_obj_t* lbl_weight       = nullptr;
lv_obj_t* lbl_length       = nullptr;
lv_obj_t* lbl_weight_delta = nullptr;
lv_obj_t* bar_filament     = nullptr;
lv_obj_t* color_swatch     = nullptr;
lv_obj_t* lbl_uid          = nullptr;
lv_obj_t* lbl_wifi_icon    = nullptr;

lv_obj_t* btn_t[4]     = {};
lv_obj_t* btn_storage  = nullptr;
lv_obj_t* btn_save     = nullptr;

lv_obj_t* lbl_uid_unk    = nullptr;
lv_obj_t* lbl_weight_unk = nullptr;
lv_obj_t* lbl_saved_msg  = nullptr;

lv_obj_t* lbl_wifi_ssid   = nullptr;
lv_obj_t* lbl_wifi_ip     = nullptr;
lv_obj_t* lbl_wifi_status = nullptr;

static lv_style_t style_bg;
static lv_style_t style_card;
static lv_style_t style_btn_active;
static lv_style_t style_btn_inactive;
static lv_style_t style_btn_save;
static lv_style_t style_title;
static lv_style_t style_value;
static lv_style_t style_dim;

// ─── STYLE SETUP ─────────────────────────────────────────────────────────────
void setup_styles() {
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, lv_color_make(13, 13, 25));
    lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);
    lv_style_set_border_width(&style_bg, 0);
    lv_style_set_pad_all(&style_bg, 0);

    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, lv_color_make(20, 28, 50));
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_border_color(&style_card, lv_color_make(40, 80, 130));
    lv_style_set_border_width(&style_card, 1);
    lv_style_set_radius(&style_card, 8);
    lv_style_set_pad_all(&style_card, 12);

    lv_style_init(&style_btn_inactive);
    lv_style_set_bg_color(&style_btn_inactive, lv_color_make(20, 28, 50));
    lv_style_set_bg_opa(&style_btn_inactive, LV_OPA_COVER);
    lv_style_set_border_color(&style_btn_inactive, lv_color_make(40, 80, 130));
    lv_style_set_border_width(&style_btn_inactive, 1);
    lv_style_set_radius(&style_btn_inactive, 6);
    lv_style_set_text_color(&style_btn_inactive, lv_color_make(100, 140, 200));

    lv_style_init(&style_btn_active);
    lv_style_set_bg_color(&style_btn_active, lv_color_make(0, 60, 100));
    lv_style_set_bg_opa(&style_btn_active, LV_OPA_COVER);
    lv_style_set_border_color(&style_btn_active, lv_color_make(0, 180, 255));
    lv_style_set_border_width(&style_btn_active, 2);
    lv_style_set_radius(&style_btn_active, 6);
    lv_style_set_text_color(&style_btn_active, lv_color_make(0, 200, 255));

    lv_style_init(&style_btn_save);
    lv_style_set_bg_color(&style_btn_save, lv_color_make(0, 80, 30));
    lv_style_set_bg_opa(&style_btn_save, LV_OPA_COVER);
    lv_style_set_border_color(&style_btn_save, lv_color_make(0, 200, 80));
    lv_style_set_border_width(&style_btn_save, 1);
    lv_style_set_radius(&style_btn_save, 8);
    lv_style_set_text_color(&style_btn_save, lv_color_make(80, 255, 120));

    lv_style_init(&style_title);
    lv_style_set_text_color(&style_title, lv_color_make(0, 200, 255));
    lv_style_set_text_font(&style_title, &lv_font_montserrat_14);

    lv_style_init(&style_value);
    lv_style_set_text_color(&style_value, lv_color_make(220, 235, 255));
    lv_style_set_text_font(&style_value, &lv_font_montserrat_14);

    lv_style_init(&style_dim);
    lv_style_set_text_color(&style_dim, lv_color_make(80, 100, 140));
    lv_style_set_text_font(&style_dim, &lv_font_montserrat_12);
}

// ─── WIFI ICON UPDATE ────────────────────────────────────────────────────────
void update_wifi_icon() {
    if (lbl_wifi_icon == nullptr) return;
    lv_color_t col = g_wifi_ok
        ? lv_color_make(0, 200, 80)
        : lv_color_make(200, 40, 40);
    lv_obj_set_style_text_color(lbl_wifi_icon, col, LV_PART_MAIN);
}

// ─── WIFI POPUP ──────────────────────────────────────────────────────────────
void build_screen_wifi() {
    scr_wifi = lv_obj_create(nullptr);
    lv_obj_add_style(scr_wifi, &style_bg, LV_PART_MAIN);

    lv_obj_t* card = lv_obj_create(scr_wifi);
    lv_obj_set_size(card, 360, 200);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_style(card, &style_card, LV_PART_MAIN);

    lv_obj_t* lbl_title = lv_label_create(card);
    lv_label_set_text(lbl_title, LV_SYMBOL_WIFI "  WiFi Status");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_title, lv_color_make(0, 200, 255), LV_PART_MAIN);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_wifi_status = lv_label_create(card);
    lv_label_set_text(lbl_wifi_status, "---");
    lv_obj_set_style_text_font(lbl_wifi_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(lbl_wifi_status, LV_ALIGN_TOP_LEFT, 0, 30);

    lbl_wifi_ssid = lv_label_create(card);
    lv_label_set_text(lbl_wifi_ssid, "SSID: ---");
    lv_obj_set_style_text_font(lbl_wifi_ssid, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_wifi_ssid, lv_color_make(140, 170, 220), LV_PART_MAIN);
    lv_obj_align(lbl_wifi_ssid, LV_ALIGN_TOP_LEFT, 0, 56);

    lbl_wifi_ip = lv_label_create(card);
    lv_label_set_text(lbl_wifi_ip, "IP: ---");
    lv_obj_set_style_text_font(lbl_wifi_ip, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_wifi_ip, lv_color_make(140, 170, 220), LV_PART_MAIN);
    lv_obj_align(lbl_wifi_ip, LV_ALIGN_TOP_LEFT, 0, 76);

    lv_obj_t* btn_close = lv_btn_create(card);
    lv_obj_set_size(btn_close, 100, 36);
    lv_obj_align(btn_close, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_close, lv_color_make(0, 40, 80), LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_close, lv_color_make(0, 120, 200), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_close, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn_close, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_close, [](lv_event_t*) {
        lv_scr_load(scr_idle);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "Close");
    lv_obj_set_style_text_color(lbl_close, lv_color_make(0, 200, 255), LV_PART_MAIN);
    lv_obj_center(lbl_close);
}

void show_wifi_popup() {
    if (lbl_wifi_status) {
        if (g_wifi_ok) {
            lv_label_set_text(lbl_wifi_status, "Connected");
            lv_obj_set_style_text_color(lbl_wifi_status, lv_color_make(0, 200, 80), LV_PART_MAIN);
        } else {
            lv_label_set_text(lbl_wifi_status, "Not connected");
            lv_obj_set_style_text_color(lbl_wifi_status, lv_color_make(200, 40, 40), LV_PART_MAIN);
        }
    }
    if (lbl_wifi_ssid)
        lv_label_set_text(lbl_wifi_ssid, ("SSID: " + String(WIFI_SSID)).c_str());
    if (lbl_wifi_ip) {
        String ip = g_wifi_ok ? WiFi.localIP().toString() : "---";
        lv_label_set_text(lbl_wifi_ip, ("IP: " + ip).c_str());
    }
    lv_scr_load(scr_wifi);
}

// ─── TOOLHEAD BUTTON CALLBACK ────────────────────────────────────────────────
static void cb_toolhead(lv_event_t* e) {
    int ch = (int)(intptr_t)lv_event_get_user_data(e);
    g_selected_toolhead = ch;
    for (int i = 0; i < 4; i++) {
        lv_obj_remove_style(btn_t[i], nullptr, LV_PART_MAIN);
        if (i == ch - 1) lv_obj_add_style(btn_t[i], &style_btn_active,   LV_PART_MAIN);
        else              lv_obj_add_style(btn_t[i], &style_btn_inactive, LV_PART_MAIN);
    }
    lv_obj_remove_style(btn_storage, nullptr, LV_PART_MAIN);
    if (ch == 0) lv_obj_add_style(btn_storage, &style_btn_active,   LV_PART_MAIN);
    else         lv_obj_add_style(btn_storage, &style_btn_inactive, LV_PART_MAIN);
}

// ─── SAVE BUTTON CALLBACK ────────────────────────────────────────────────────
static void cb_save(lv_event_t* e) {
    if (g_saving) return;
    g_saving = true;
    g_state  = StationState::SAVING;
    lv_scr_load(scr_saving);
}

// ─── BUILD IDLE SCREEN ───────────────────────────────────────────────────────
void build_screen_idle() {
    scr_idle = lv_obj_create(nullptr);
    lv_obj_add_style(scr_idle, &style_bg, LV_PART_MAIN);

    lv_obj_t* hdr = lv_obj_create(scr_idle);
    lv_obj_set_size(hdr, 480, 44);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_make(0, 40, 80), LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hdr, 0, LV_PART_MAIN);

    lv_obj_t* lbl_title = lv_label_create(hdr);
    lv_label_set_text(lbl_title, "SPOOLSTATION");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_title, lv_color_make(0, 200, 255), LV_PART_MAIN);
    lv_obj_align(lbl_title, LV_ALIGN_LEFT_MID, 16, 0);

    // WiFi icon — tappable, red/green based on actual connection state
    lbl_wifi_icon = lv_label_create(hdr);
    lv_label_set_text(lbl_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(lbl_wifi_icon, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_wifi_icon, lv_color_make(200, 40, 40), LV_PART_MAIN);
    lv_obj_align(lbl_wifi_icon, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_flag(lbl_wifi_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lbl_wifi_icon, [](lv_event_t*) {
        show_wifi_popup();
    }, LV_EVENT_CLICKED, nullptr);

    // Centered idle content
    lv_obj_t* icon = lv_label_create(scr_idle);
    lv_label_set_text(icon, LV_SYMBOL_UPLOAD);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_make(30, 50, 90), LV_PART_MAIN);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t* lbl_hang = lv_label_create(scr_idle);
    lv_label_set_text(lbl_hang, "Hang spool to begin");
    lv_obj_set_style_text_color(lbl_hang, lv_color_make(60, 90, 140), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_hang, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(lbl_hang, LV_ALIGN_CENTER, 0, 46);

    lv_obj_t* lbl_sub = lv_label_create(scr_idle);
    lv_label_set_text(lbl_sub, "NFC tag will be read automatically");
    lv_obj_set_style_text_color(lbl_sub, lv_color_make(40, 60, 100), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_sub, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(lbl_sub, LV_ALIGN_CENTER, 0, 66);
}

// ─── BUILD SPOOL SCREEN ──────────────────────────────────────────────────────
void build_screen_spool() {
    scr_spool = lv_obj_create(nullptr);
    lv_obj_add_style(scr_spool, &style_bg, LV_PART_MAIN);

    lv_obj_t* hdr = lv_obj_create(scr_spool);
    lv_obj_set_size(hdr, 480, 40);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_make(0, 40, 80), LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hdr, 0, LV_PART_MAIN);

    lv_obj_t* lbl_hdr = lv_label_create(hdr);
    lv_label_set_text(lbl_hdr, "SPOOL IDENTIFIED");
    lv_obj_set_style_text_color(lbl_hdr, lv_color_make(0, 200, 255), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_hdr, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(lbl_hdr, LV_ALIGN_LEFT_MID, 12, 0);

    // Left column
    color_swatch = lv_obj_create(scr_spool);
    lv_obj_set_size(color_swatch, 52, 52);
    lv_obj_set_pos(color_swatch, 12, 48);
    lv_obj_set_style_radius(color_swatch, 26, LV_PART_MAIN);
    lv_obj_set_style_border_color(color_swatch, lv_color_make(60, 80, 120), LV_PART_MAIN);
    lv_obj_set_style_border_width(color_swatch, 2, LV_PART_MAIN);

    lbl_vendor = lv_label_create(scr_spool);
    lv_label_set_text(lbl_vendor, "---");
    lv_obj_set_style_text_color(lbl_vendor, lv_color_make(0, 180, 255), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_vendor, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_pos(lbl_vendor, 74, 50);

    lbl_material = lv_label_create(scr_spool);
    lv_label_set_text(lbl_material, "---");
    lv_obj_set_style_text_color(lbl_material, lv_color_make(140, 170, 220), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_material, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_pos(lbl_material, 74, 72);

    lbl_name = lv_label_create(scr_spool);
    lv_label_set_text(lbl_name, "---");
    lv_obj_set_style_text_color(lbl_name, lv_color_make(180, 200, 240), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_pos(lbl_name, 74, 88);

    lbl_uid = lv_label_create(scr_spool);
    lv_label_set_text(lbl_uid, "UID: ---");
    lv_obj_set_style_text_color(lbl_uid, lv_color_make(40, 60, 100), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_uid, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_pos(lbl_uid, 12, 108);

    // Divider
    lv_obj_t* vdiv = lv_obj_create(scr_spool);
    lv_obj_set_size(vdiv, 1, 268);
    lv_obj_set_pos(vdiv, 228, 44);
    lv_obj_set_style_bg_color(vdiv, lv_color_make(30, 50, 90), LV_PART_MAIN);
    lv_obj_set_style_border_width(vdiv, 0, LV_PART_MAIN);

    // Right column
    lv_obj_t* lbl_w_title = lv_label_create(scr_spool);
    lv_label_set_text(lbl_w_title, "REMAINING");
    lv_obj_set_style_text_color(lbl_w_title, lv_color_make(60, 90, 140), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_w_title, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_pos(lbl_w_title, 238, 50);

    lbl_weight = lv_label_create(scr_spool);
    lv_label_set_text(lbl_weight, "---g");
    lv_obj_set_style_text_color(lbl_weight, lv_color_make(0, 200, 255), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_weight, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_pos(lbl_weight, 238, 63);

    lbl_length = lv_label_create(scr_spool);
    lv_label_set_text(lbl_length, "~---m");
    lv_obj_set_style_text_color(lbl_length, lv_color_make(100, 140, 200), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_length, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_pos(lbl_length, 238, 90);

    lbl_weight_delta = lv_label_create(scr_spool);
    lv_label_set_text(lbl_weight_delta, "");
    lv_obj_set_style_text_color(lbl_weight_delta, lv_color_make(255, 160, 40), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_weight_delta, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_pos(lbl_weight_delta, 380, 70);

    bar_filament = lv_bar_create(scr_spool);
    lv_obj_set_size(bar_filament, 228, 8);
    lv_obj_set_pos(bar_filament, 238, 108);
    lv_obj_set_style_bg_color(bar_filament, lv_color_make(20, 30, 55), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_filament, lv_color_make(0, 180, 255), LV_PART_INDICATOR);
    lv_bar_set_range(bar_filament, 0, 100);
    lv_bar_set_value(bar_filament, 50, LV_ANIM_OFF);

    lv_obj_t* hdiv = lv_obj_create(scr_spool);
    lv_obj_set_size(hdiv, 228, 1);
    lv_obj_set_pos(hdiv, 238, 124);
    lv_obj_set_style_bg_color(hdiv, lv_color_make(30, 50, 90), LV_PART_MAIN);
    lv_obj_set_style_border_width(hdiv, 0, LV_PART_MAIN);

    lv_obj_t* lbl_assign = lv_label_create(scr_spool);
    lv_label_set_text(lbl_assign, "ASSIGN TO:");
    lv_obj_set_style_text_color(lbl_assign, lv_color_make(60, 90, 140), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_assign, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_pos(lbl_assign, 238, 130);

    const char* t_labels[4] = {"T1", "T2", "T3", "T4"};
    int btn_w = 40, btn_h = 36;
    for (int i = 0; i < 4; i++) {
        btn_t[i] = lv_btn_create(scr_spool);
        lv_obj_set_size(btn_t[i], btn_w, btn_h);
        lv_obj_set_pos(btn_t[i], 238 + i * (btn_w + 5), 146);
        lv_obj_add_style(btn_t[i], &style_btn_inactive, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn_t[i], 0, LV_PART_MAIN);
        lv_obj_add_event_cb(btn_t[i], cb_toolhead, LV_EVENT_CLICKED, (void*)(intptr_t)(i + 1));
        lv_obj_t* lbl = lv_label_create(btn_t[i]);
        lv_label_set_text(lbl, t_labels[i]);
        lv_obj_center(lbl);
    }

    btn_storage = lv_btn_create(scr_spool);
    lv_obj_set_size(btn_storage, 55, 36);
    lv_obj_set_pos(btn_storage, 411, 146);
    lv_obj_add_style(btn_storage, &style_btn_active, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn_storage, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_storage, cb_toolhead, LV_EVENT_CLICKED, (void*)(intptr_t)0);
    lv_obj_t* lbl_stor = lv_label_create(btn_storage);
    lv_label_set_text(lbl_stor, "Stor");
    lv_obj_center(lbl_stor);

    btn_save = lv_btn_create(scr_spool);
    lv_obj_set_size(btn_save, 228, 48);
    lv_obj_set_pos(btn_save, 238, 264);
    lv_obj_add_style(btn_save, &style_btn_save, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn_save, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_save, cb_save, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl_save = lv_label_create(btn_save);
    lv_label_set_text(lbl_save, "SAVE + SYNC");
    lv_obj_set_style_text_font(lbl_save, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(lbl_save);
}

// ─── BUILD SAVING SCREEN ─────────────────────────────────────────────────────
void build_screen_saving() {
    scr_saving = lv_obj_create(nullptr);
    lv_obj_add_style(scr_saving, &style_bg, LV_PART_MAIN);

    lv_obj_t* spinner = lv_spinner_create(scr_saving, 1000, 60);
    lv_obj_set_size(spinner, 60, 60);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_arc_color(spinner, lv_color_make(0, 180, 255), LV_PART_INDICATOR);

    lv_obj_t* lbl = lv_label_create(scr_saving);
    lv_label_set_text(lbl, "Saving to Spoolman...");
    lv_obj_set_style_text_color(lbl, lv_color_make(0, 180, 255), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 36);
}

// ─── BUILD SAVED SCREEN ──────────────────────────────────────────────────────
void build_screen_saved() {
    scr_saved = lv_obj_create(nullptr);
    lv_obj_add_style(scr_saved, &style_bg, LV_PART_MAIN);

    lv_obj_t* icon = lv_label_create(scr_saved);
    lv_label_set_text(icon, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_make(0, 200, 80), LV_PART_MAIN);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -30);

    lbl_saved_msg = lv_label_create(scr_saved);
    lv_label_set_text(lbl_saved_msg, "Saved");
    lv_label_set_long_mode(lbl_saved_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_saved_msg, 440);
    lv_obj_set_style_text_color(lbl_saved_msg, lv_color_make(0, 200, 80), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_saved_msg, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(lbl_saved_msg, LV_ALIGN_CENTER, 0, 24);
}

// ─── BUILD UNKNOWN TAG SCREEN ────────────────────────────────────────────────
void build_screen_unknown() {
    scr_unknown = lv_obj_create(nullptr);
    lv_obj_add_style(scr_unknown, &style_bg, LV_PART_MAIN);

    lv_obj_t* hdr = lv_obj_create(scr_unknown);
    lv_obj_set_size(hdr, 480, 40);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_make(80, 40, 0), LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hdr, 0, LV_PART_MAIN);

    lv_obj_t* lbl_hdr = lv_label_create(hdr);
    lv_label_set_text(lbl_hdr, "UNKNOWN SPOOL");
    lv_obj_set_style_text_color(lbl_hdr, lv_color_make(255, 180, 0), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_hdr, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(lbl_hdr, LV_ALIGN_LEFT_MID, 12, 0);

    lbl_uid_unk = lv_label_create(scr_unknown);
    lv_label_set_text(lbl_uid_unk, "UID: ---");
    lv_obj_set_style_text_color(lbl_uid_unk, lv_color_make(255, 160, 40), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_uid_unk, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_pos(lbl_uid_unk, 12, 50);

    lbl_weight_unk = lv_label_create(scr_unknown);
    lv_label_set_text(lbl_weight_unk, "Weight: ---g");
    lv_obj_set_style_text_color(lbl_weight_unk, lv_color_make(0, 200, 255), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_weight_unk, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(lbl_weight_unk, 12, 66);

    lv_obj_t* lbl_inst = lv_label_create(scr_unknown);
    lv_label_set_text(lbl_inst, "Open dashboard to register this spool.\nUID and weight have been sent to bridge.");
    lv_label_set_long_mode(lbl_inst, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_inst, 456);
    lv_obj_set_style_text_color(lbl_inst, lv_color_make(140, 160, 200), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_inst, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_pos(lbl_inst, 12, 100);

    lv_obj_t* btn_dismiss = lv_btn_create(scr_unknown);
    lv_obj_set_size(btn_dismiss, 200, 44);
    lv_obj_align(btn_dismiss, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(btn_dismiss, lv_color_make(40, 30, 0), LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_dismiss, lv_color_make(200, 120, 0), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_dismiss, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn_dismiss, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_dismiss, [](lv_event_t*) {
        g_state    = StationState::IDLE;
        g_last_uid = "";
        lv_scr_load(scr_idle);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl_d = lv_label_create(btn_dismiss);
    lv_label_set_text(lbl_d, "Dismiss");
    lv_obj_set_style_text_color(lbl_d, lv_color_make(255, 180, 0), LV_PART_MAIN);
    lv_obj_center(lbl_d);
}

// ─── BUILD ERROR SCREEN ──────────────────────────────────────────────────────
void build_screen_error() {
    scr_error = lv_obj_create(nullptr);
    lv_obj_add_style(scr_error, &style_bg, LV_PART_MAIN);

    lv_obj_t* lbl = lv_label_create(scr_error);
    lv_label_set_text(lbl, LV_SYMBOL_WARNING "  WiFi / Spoolman not reachable");
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, 440);
    lv_obj_set_style_text_color(lbl, lv_color_make(255, 80, 80), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
}

// ─── UPDATE SPOOL SCREEN ─────────────────────────────────────────────────────
void update_spool_screen(const SpoolData& s, float new_weight, float old_weight) {
    lv_obj_set_style_bg_color(color_swatch, hex_to_lv_color(s.color_hex), LV_PART_MAIN);
    lv_label_set_text(lbl_vendor,   s.vendor.c_str());
    lv_label_set_text(lbl_material, s.material.c_str());
    lv_label_set_text(lbl_name,     s.name.c_str());

    char cbuf[64];
    snprintf(cbuf, sizeof(cbuf), "%dg", (int)new_weight);
    lv_label_set_text(lbl_weight, cbuf);

    float len = calc_length_m(new_weight, s.material);
    snprintf(cbuf, sizeof(cbuf), "~%dm", (int)len);
    lv_label_set_text(lbl_length, cbuf);

    float delta = new_weight - old_weight;
    if (abs(delta) > 2) {
        snprintf(cbuf, sizeof(cbuf), "%+.0fg", delta);
        lv_label_set_text(lbl_weight_delta, cbuf);
    } else {
        lv_label_set_text(lbl_weight_delta, "");
    }

    int total = max(1000, (int)new_weight + s.tare_weight);
    int pct   = (int)((new_weight / (float)(total - s.tare_weight)) * 100.0);
    pct = max(0, min(100, pct));
    lv_bar_set_value(bar_filament, pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_filament,
        pct < 15 ? lv_color_make(200, 40, 40) :
        pct < 30 ? lv_color_make(220, 140, 0) :
                   hex_to_lv_color(s.color_hex),
        LV_PART_INDICATOR);

    String uid_short = s.tag_uid.length() > 20
        ? s.tag_uid.substring(0, 20) + "..." : s.tag_uid;
    lv_label_set_text(lbl_uid, ("UID: " + uid_short).c_str());

    g_selected_toolhead = 0;
    for (int i = 0; i < 4; i++)
        lv_obj_add_style(btn_t[i], &style_btn_inactive, LV_PART_MAIN);
    lv_obj_add_style(btn_storage, &style_btn_active, LV_PART_MAIN);
}

// ─── SAVE TRANSACTION ────────────────────────────────────────────────────────
void do_save() {
    float remaining = g_weight_g - g_spool.tare_weight;
    if (remaining < 0) remaining = 0;
    float len = calc_length_m(remaining, g_spool.material);
    int   ch  = g_selected_toolhead;

    bool ok = update_spool_weight(g_spool.id, remaining, len, ch > 0 ? ch - 1 : -1);
    if (ok && ch > 0) push_to_u1(g_spool, ch - 1);

    String msg = ok
        ? String("Saved: ") + g_spool.vendor + " " + g_spool.name + "\n"
          + String((int)remaining) + "g  ~" + String((int)len) + "m  "
          + (ch > 0 ? "T" + String(ch) : "Storage")
        : "Save failed - check WiFi";

    lv_label_set_text(lbl_saved_msg, msg.c_str());
    lv_obj_set_style_text_color(lbl_saved_msg,
        ok ? lv_color_make(0, 200, 80) : lv_color_make(255, 80, 80),
        LV_PART_MAIN);

    g_saving = false;
    g_state  = StationState::SAVED;
    lv_scr_load(scr_saved);
}

// ─── SETUP ───────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("SpoolStation booting...");

    pinMode(TFT_BL_PIN, OUTPUT);
    digitalWrite(TFT_BL_PIN, HIGH);

    tft.begin();
    tft.setRotation(1);  // Landscape
    tft.fillScreen(TFT_BLACK);

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf, nullptr, LV_BUF_SIZE);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = SCREEN_W;
    disp_drv.ver_res  = SCREEN_H;
    disp_drv.flush_cb = lv_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    gt911_init();
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lv_touch_read;
    lv_indev_drv_register(&indev_drv);

    setup_styles();
    build_screen_idle();
    build_screen_spool();
    build_screen_saving();
    build_screen_saved();
    build_screen_unknown();
    build_screen_error();
    build_screen_wifi();
    lv_scr_load(scr_idle);

    Serial.println("HX711 init...");
    scale.begin(HX711_DT_PIN, HX711_SCK_PIN);
    scale.set_scale(CALIBRATION_FACTOR);
    // scale.tare(); // uncomment after HX711 is wired
    Serial.println("HX711 ready.");

    Serial.println("PN532 init...");
    pn532Serial.begin(115200, SERIAL_8N1, PN532_RX_PIN, PN532_TX_PIN);
    nfc.begin();
    uint32_t fw = nfc.getFirmwareVersion();
    if (fw) {
        Serial.printf("PN532 FW: %d.%d\n", (fw >> 16) & 0xFF, (fw >> 8) & 0xFF);
        nfc.setPassiveActivationRetries(0x01);
        nfc.SAMConfig();
    } else {
        Serial.println("PN532 not found - check wiring and DIP switches (HSU: both DOWN)");
    }

    Serial.printf("Connecting to %s...\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        lv_timer_handler();
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        g_wifi_ok = true;
        Serial.printf("WiFi OK: %s\n", WiFi.localIP().toString().c_str());
    } else {
        g_wifi_ok = false;
        g_state   = StationState::WIFI_ERROR;
        lv_scr_load(scr_error);
        Serial.println("WiFi FAILED");
    }

    update_wifi_icon();
}

// ─── MAIN LOOP ───────────────────────────────────────────────────────────────
unsigned long g_saved_ts = 0;

void loop() {
    lv_timer_handler();

    // Read HX711
    if (scale.is_ready()) {
        float raw = scale.get_units(1);
        if (raw < 0) raw = 0;
        g_weight_buf[g_weight_idx % STABLE_SAMPLES] = raw;
        g_weight_idx++;
        if (g_weight_idx >= STABLE_SAMPLES) g_weight_buf_full = true;
        g_weight_g      = get_avg_weight();
        g_weight_stable = is_weight_stable();
    }

    // Poll PN532
    uint8_t uid[7];
    uint8_t uid_len;
    bool tag_found = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uid_len, 50);

    if (tag_found) {
        String uid_str = format_uid(uid, uid_len);
        g_last_tag_ms  = millis();

        if (uid_str != g_last_uid) {
            g_last_uid = uid_str;
            Serial.printf("Tag: %s  Weight: %.1fg  Stable: %s\n",
                uid_str.c_str(), g_weight_g, g_weight_stable ? "yes" : "no");

            if (g_state == StationState::IDLE || g_state == StationState::SAVED) {
                g_state = StationState::TAG_READ;
                SpoolData found = find_spool_by_uid(uid_str);
                if (found.valid) {
                    float old_weight    = found.remaining_weight;
                    float new_remaining = g_weight_g - found.tare_weight;
                    if (new_remaining < 0) new_remaining = 0;
                    g_spool = found;
                    g_state = StationState::SPOOL_FOUND;
                    update_spool_screen(found, new_remaining, old_weight);
                    lv_scr_load(scr_spool);
                } else {
                    g_state = StationState::UNKNOWN_TAG;
                    if (lbl_uid_unk)    lv_label_set_text(lbl_uid_unk,    ("UID: " + uid_str).c_str());
                    if (lbl_weight_unk) lv_label_set_text(lbl_weight_unk, (String((int)g_weight_g) + "g on scale").c_str());
                    DynamicJsonDocument doc(256);
                    doc["event"]    = "unknown_tag";
                    doc["uid"]      = uid_str;
                    doc["weight_g"] = (int)g_weight_g;
                    doc["stable"]   = g_weight_stable;
                    String json; serializeJson(doc, json);
                    http_post(String(BRIDGE_URL) + "/station/event", json);
                    lv_scr_load(scr_unknown);
                }
            }
        }
    } else {
        if (g_last_uid != "" && millis() - g_last_tag_ms > 2000) {
            g_last_uid = "";
            if (g_state == StationState::SPOOL_FOUND ||
                g_state == StationState::UNKNOWN_TAG  ||
                g_state == StationState::TAG_READ) {
                g_state = StationState::IDLE;
                lv_scr_load(scr_idle);
            }
        }
    }

    // Handle save
    if (g_state == StationState::SAVING && g_saving) do_save();

    // Auto-return from saved screen after 3s
    if (g_state == StationState::SAVED) {
        if (g_saved_ts == 0) g_saved_ts = millis();
        if (millis() - g_saved_ts > 3000) {
            g_saved_ts = 0;
            g_state    = StationState::IDLE;
            g_last_uid = "";
            lv_scr_load(scr_idle);
        }
    } else {
        g_saved_ts = 0;
    }

    delay(5);
}
