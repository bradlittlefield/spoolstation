/*
 * SpoolDesk Station Firmware
 * Target: ESP32-3248S035C (ST7796 display + GT911 capacitive touch)
 *
 * LIBRARIES REQUIRED (Arduino Library Manager):
 *   - TFT_eSPI by Bodmer (configure User_Setup.h — see below)
 *   - LVGL by kisvegabor (v8.3.x — NOT v9, incompatible with this config)
 *   - HX711 Arduino Library by bogde
 *   - Adafruit PN532 by Adafruit
 *   - ArduinoJson by Benoit Blanchon
 *   - GT911 by Luca Dentella (or use bb_captouch)
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
 *   Partition Scheme: Default 4MB with spiffs
 *   Upload Speed: 921600
 *
 * PIN ASSIGNMENTS:
 *   HX711 SCK    -> GPIO 18
 *   HX711 DT     -> GPIO 19
 *   PN532 TX     -> GPIO 22 (ESP32 UART2 RX)
 *   PN532 RX     -> GPIO 17 (ESP32 UART2 TX)
 *   Display SPI  -> GPIO 13,14,15,2 (reserved, do not touch)
 *   Touch I2C    -> GPIO 33(SDA), 32(SCL), 21(IRQ), 25(RST)
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

// ─── USER CONFIG ─────────────────────────────────────────────────────────────
// Edit these before flashing

const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASS     = "YOUR_WIFI_PASSWORD";
const char* SPOOLMAN_URL  = "http://192.168.1.XXX:7912/api/v1";   // RPi IP
const char* BRIDGE_URL    = "http://192.168.1.XXX:8765";           // RPi bridge

// HX711 calibration — run with 1.0, note raw/known_weight, set correct value
float CALIBRATION_FACTOR  = -96650.0;

// ─── PIN DEFINITIONS ─────────────────────────────────────────────────────────
#define HX711_SCK_PIN   18
#define HX711_DT_PIN    19
#define PN532_RX_PIN    22    // ESP32 UART2 RX — PN532 TX connects here
#define PN532_TX_PIN    17    // ESP32 UART2 TX — PN532 RX connects here

// Touch GT911 I2C (used internally by touch driver)
#define GT911_SDA_PIN   33
#define GT911_SCL_PIN   32
#define GT911_IRQ_PIN   21
#define GT911_RST_PIN   25

// Display backlight (TFT_eSPI handles SPI pins)
#define TFT_BL_PIN      27

// ─── DISPLAY / LVGL ──────────────────────────────────────────────────────────
#define SCREEN_W        320
#define SCREEN_H        480
#define LV_BUF_SIZE     (SCREEN_W * 20)

TFT_eSPi tft = TFT_eSPI();
static lv_disp_draw_buf_t draw_buf;
static lv_color_t         buf[LV_BUF_SIZE];

// ─── TOUCH GT911 ─────────────────────────────────────────────────────────────
// Minimal GT911 driver inline (avoids library version conflicts)
#define GT911_ADDR      0x5D
#define GT911_COORD_REG 0x814E

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
                Wire.read(); // skip track ID
                uint8_t xl = Wire.read(); uint8_t xh = Wire.read();
                uint8_t yl = Wire.read(); uint8_t yh = Wire.read();
                tp.x = ((xh << 8) | xl);
                tp.y = ((yh << 8) | yl);
                // clear status register
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

// ─── HARDWARE INSTANCES ───────────────────────────────────────────────────────
HX711       scale;
HardwareSerial pn532Serial(2);
Adafruit_PN532 nfc(pn532Serial);

// ─── STATE ────────────────────────────────────────────────────────────────────
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

StationState  g_state        = StationState::IDLE;
SpoolData     g_spool        = {};
String        g_last_uid     = "";
float         g_weight_g     = 0;
bool          g_weight_stable = false;
unsigned long g_last_tag_ms  = 0;
unsigned long g_stable_since = 0;
int           g_selected_toolhead = 0;
bool          g_saving       = false;

#define STABLE_THRESHOLD_G   2.0
#define STABLE_SAMPLES       6
float g_weight_buf[STABLE_SAMPLES] = {0};
int   g_weight_idx = 0;
bool  g_weight_buf_full = false;

// LVGL flush
void lv_disp_flush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t*)color_p, w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(disp);
}

// LVGL touch read
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

// Color utilities
lv_color_t hex_to_lv_color(const String& hex) {
    String h = hex; h.replace("#", "");
    if (h.length() < 6) return lv_color_white();
    long c = strtol(h.c_str(), nullptr, 16);
    return lv_color_make((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

// Weight stability
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

// HTTP helpers
String http_get(const String& url) {
    HTTPClient http; http.begin(url); http.setTimeout(5000);
    int code = http.GET();
    String body = "";
    if (code == 200) body = http.getString();
    http.end(); return body;
}
bool http_patch(const String& url, const String& json) {
    HTTPClient http; http.begin(url);
    http.addHeader("Content-Type", "application/json"); http.setTimeout(5000);
    int code = http.PATCH(json); http.end();
    return (code >= 200 && code < 300);
}
bool http_post(const String& url, const String& json) {
    HTTPClient http; http.begin(url);
    http.addHeader("Content-Type", "application/json"); http.setTimeout(5000);
    int code = http.POST(json); http.end();
    return (code >= 200 && code < 300);
}

// Spoolman lookup by UID
SpoolData find_spool_by_uid(const String& uid) {
    SpoolData s = {}; s.valid = false;
    String body = http_get(String(SPOOLMAN_URL) + "/spool?allow_archived=false");
    if (body.length() == 0) return s;
    DynamicJsonDocument doc(32768);
    if (deserializeJson(doc, body) != DeserializationError::Ok) return s;
    JsonArray spools = doc.as<JsonArray>();
    for (JsonObject sp : spools) {
        String stored = "";
        if (sp["extra"].containsKey("nfc_uid")) stored = sp["extra"]["nfc_uid"].as<String>();
        stored.toUpperCase();
        String search = uid; search.toUpperCase();
        if (stored == search) {
            s.valid = true; s.id = sp["id"];
            s.remaining_weight = sp["remaining_weight"] | 0;
            s.tare_weight = sp["extra"]["tare_weight"] | 200;
            s.min_temp = sp["extra"]["min_temp"] | 200;
            s.max_temp = sp["extra"]["max_temp"] | 230;
            s.bed_temp = sp["extra"]["bed_temp"] | 60;
            s.toolhead = sp["extra"]["toolhead_channel"] | 0;
            s.remaining_length_m = sp["extra"]["remaining_length_m"] | 0;
            s.tag_uid = uid;
            JsonObject fil = sp["filament"];
            if (!fil.isNull()) {
                s.material = fil["material"].as<String>();
                s.name = fil["name"].as<String>();
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

bool update_spool_weight(int spool_id, float remaining_g, float len_m, int toolhead_ch) {
    DynamicJsonDocument doc(512);
    doc["remaining_weight"] = (int)remaining_g;
    JsonObject extra = doc.createNestedObject("extra");
    extra["remaining_length_m"] = (int)len_m;
    extra["last_weighed"] = "now";
    if (toolhead_ch >= 0) extra["toolhead_channel"] = toolhead_ch;
    else extra["toolhead_channel"] = nullptr;
    String json; serializeJson(doc, json);
    return http_patch(String(SPOOLMAN_URL) + "/spool/" + spool_id, json);
}

bool push_to_u1(const SpoolData& s, int channel) {
    String c = s.color_hex; c.replace("#", "");
    long rgb = strtol(c.c_str(), nullptr, 16);
    DynamicJsonDocument doc(512);
    doc["channel"] = channel;
    JsonObject info = doc.createNestedObject("info");
    info["VENDOR"] = s.vendor; info["MAIN_TYPE"] = s.material;
    info["SUB_TYPE"] = s.name; info["RGB_1"] = (long)rgb;
    info["ALPHA"] = 255; info["HOTEND_MIN_TEMP"] = s.min_temp;
    info["HOTEND_MAX_TEMP"] = s.max_temp; info["BED_TEMP"] = s.bed_temp;
    String json; serializeJson(doc, json);
    return http_post(String(BRIDGE_URL) + "/u1/toolhead/" + channel, json);
}

float calc_length_m(float weight_g, const String& material, float diameter_mm = 1.75) {
    float density = 1.24;
    if (material == "PETG" || material == "PETGCF") density = 1.27;
    else if (material == "ABS") density = 1.04;
    else if (material == "ASA") density = 1.07;
    else if (material == "TPU") density = 1.21;
    else if (material == "Nylon") density = 1.08;
    else if (material == "PC") density = 1.20;
    else if (material == "PLA+") density = 1.22;
    float r = (diameter_mm / 2.0) / 10.0;
    float vol = weight_g / density;
    return (vol / (PI * r * r)) / 100.0;
}

// UI globals
lv_obj_t* scr_idle = nullptr, *scr_spool = nullptr, *scr_unknown = nullptr;
lv_obj_t* scr_saving = nullptr, *scr_saved = nullptr, *scr_error = nullptr;
lv_obj_t* lbl_vendor = nullptr, *lbl_material = nullptr, *lbl_name = nullptr;
lv_obj_t* lbl_weight = nullptr, *lbl_length = nullptr, *lbl_weight_delta = nullptr;
lv_obj_t* bar_filament = nullptr, *color_swatch = nullptr, *lbl_uid = nullptr;
lv_obj_t* btn_t[4] = {}, *btn_storage = nullptr, *btn_save = nullptr;
lv_obj_t* lbl_uid_unk = nullptr, *lbl_weight_unk = nullptr, *lbl_saved_msg = nullptr;
static lv_style_t style_bg, style_btn_active, style_btn_inactive, style_btn_save;

"void setup_styles() {
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, lv_color_make(13, 13, 25));
    lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);
    lv_style_set_border_width(&style_bg, 0);
    lv_style_set_pad_all(&style_bg, 0);
    lv_style_init(&style_btn_inactive);
    lv_style_set_bg_color(&style_btn_inactive, lv_color_make(20, 28, 50));
    lv_style_set_border_color(&style_btn_inactive, lv_color_make(40, 80, 130));
    lv_style_set_border_width(&style_btn_inactive, 1);
    lv_style_set_radius(&style_btn_inactive, 6);
    lv_style_set_text_color(&style_btn_inactive, lv_color_make(100, 140, 200));
    lv_style_init(&style_btn_active);
    lv_style_set_bg_color(&style_btn_active, lv_color_make(0, 60, 100));
    lv_style_set_border_color(&style_btn_active, lv_color_make(0, 180, 255));
    lv_style_set_border_width(&style_btn_active, 2);
    lv_style_set_radius(&style_btn_active, 6);
    lv_style_set_text_color(&style_btn_active, lv_color_make(0, 200, 255));
    lv_style_init(&style_btn_save);
    lv_style_set_bg_color(&style_btn_save, lv_color_make(0, 80, 30));
    lv_style_set_border_color(&style_btn_save, lv_color_make(0, 200, 80));
    lv_style_set_border_width(&style_btn_save, 1);
    lv_style_set_radius(&style_btn_save, 8);
    lv_style_set_text_color(&style_btn_save, lv_color_make(80, 255, 120));
}

static void cb_toolhead(lv_event_t* e) {
    int ch = (int)(intptr_t)lv_event_get_user_data(e);
    g_selected_toolhead = ch;
    for (int i = 0; i < 4; i++) {
        lv_obj_remove_style(btn_t[i], nullptr, LV_PART_MAIN);
        lv_obj_add_style(btn_t[i], i == ch - 1 ? &style_btn_active : &style_btn_inactive, LV_PART_MAIN);
    }
    lv_obj_remove_style(btn_storage, nullptr, LV_PART_MAIN);
    lv_obj_add_style(btn_storage, ch == 0 ? &style_btn_active : &style_btn_inactive, LV_PART_MAIN);
}

static void cb_save(lv_event_t* e) {
    if (g_saving) return;
    g_saving = true; g_state = StationState::SAVING;
    lv_scr_load(scr_saving);
}

// Screen builders -- condensed for size
void build_screen_idle() {
    scr_idle = lv_obj_create(nullptr); lv_obj_add_style(scr_idle, &style_bg, LV_PART_MAIN);
    lv_obj_t* hdr = lv_obj_create(scr_idle); lv_obj_set_size(hdr, 320, 48);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_make(0, 40, 80), LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hdr, 0, LV_PART_MAIN);
    lv_obj_t* title = lv_label_create(hdr);
    lv_label_set_text(title, "SPOOLSTATION");
    lv_obj_set_style_text_color(title, lv_color_make(0, 200, 255), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_t* icon = lv_label_create(scr_idle);
    lv_label_set_text(icon, LV_SYMBOL_UPLOAD);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_make(30, 50, 90), LV_PART_MAIN);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -40);
    lv_obj_t* lbl = lv_label_create(scr_idle);
    lv_label_set_text(lbl, "Hang spool to begin");
    lv_obj_set_style_text_color(lbl, lv_color_make(60, 90, 140), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 20);
    lv_obj_t* sub = lv_label_create(scr_idle);
    lv_label_set_text(sub, "NFC tag read automatically");
    lv_obj_set_style_text_color(sub, lv_color_make(40, 60, 100), LV_PART_MAIN);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 44);
}

void build_screen_spool() {
    scr_spool = lv_obj_create(nullptr); lv_obj_add_style(scr_spool, &style_bg, LV_PART_MAIN);
    lv_obj_t* hdr = lv_obj_create(scr_spool); lv_obj_set_size(hdr, 320, 44);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_make(0, 40, 80), LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hdr, 0, LV_PART_MAIN);
    lv_obj_t* hlbl = lv_label_create(hdr);
    lv_label_set_text(hlbl, "SPOOL IDENTIFIED");
    lv_obj_set_style_text_color(hlbl, lv_color_make(0, 200, 255), LV_PART_MAIN);
    lv_obj_set_style_text_font(hlbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(hlbl, LV_ALIGN_LEFT_MID, 12, 0);
    color_swatch = lv_obj_create(scr_spool); lv_obj_set_size(color_swatch, 56, 56);
    lv_obj_align(color_swatch, LV_ALIGN_TOP_LEFT, 12, 54);
    lv_obj_set_style_radius(color_swatch, 28, LV_PART_MAIN);
    lv_obj_set_style_border_color(color_swatch, lv_color_make(60, 80, 120), LV_PART_MAIN);
    lv_obj_set_style_border_width(color_swatch, 2, LV_PART_MAIN);
    lbl_vendor = lv_label_create(scr_spool); lv_label_set_text(lbl_vendor, "---");
    lv_obj_set_style_text_color(lbl_vendor, lv_color_make(0, 180, 255), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_vendor, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(lbl_vendor, LV_ALIGN_TOP_LEFT,  82, 56);
    lbl_material = lv_label_create(scr_spool); lv_label_set_text(lbl_material, "---");
    lv_obj_set_style_text_color(lbl_material, lv_color_make(140, 170, 220), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_material, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(lbl_material, LV_ALIGN_TOP_LEFT, 82, 78);
    lbl_name = lv_label_create(scr_spool); lv_label_set_text(lbl_name, "---");
    lv_obj_set_style_text_color(lbl_name, lv_color_make(180, 200, 240), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(lbl_name, LV_ALIGN_TOP_LEFT, 82, 94);
    lbl_weight = lv_label_create(scr_spool); lv_label_set_text(lbl_weight, "---g");
    lv_obj_set_style_text_color(lbl_weight, lv_color_make(0, 200, 255), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_weight, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_align(lbl_weight, LV_ALIGN_TOP_LEFT, 12, 144);
    lbl_length = lv_label_create(scr_spool); lv_label_set_text(lbl_length, "~---m");
    lv_obj_set_style_text_color(lbl_length, lv_color_make(100, 140, 200), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_length, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(lbl_length, LV_ALIGN_TOP_LEFT, 12, 172);
    lbl_weight_delta = lv_label_create(scr_spool); lv_label_set_text(lbl_weight_delta, "");
    lv_obj_set_style_text_color(lbl_weight_delta, lv_color_make(255, 160, 40), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_weight_delta, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(lbl_weight_delta, LV_ALIGN_TOP_RIGHT, -12, 152);
    bar_filament = lv_bar_create(scr_spool); lv_obj_set_size(bar_filament, 296, 8);
    lv_obj_align(bar_filament, LV_ALIGN_TOP_MID, 0, 194);
    lv_obj_set_style_bg_color(bar_filament, lv_color_make(20, 30, 55), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_filament, lv_color_make(0, 180, 255), LV_PART_INDICATOR);
    lv_bar_set_range(bar_filament, 0, 100); lv_bar_set_value(bar_filament, 50, LV_ANIM_OFF);
    lbl_uid = lv_label_create(scr_spool); lv_label_set_text(lbl_uid, "UID: ---");
    lv_obj_set_style_text_color(lbl_uid, lv_color_make(40, 60, 100), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_uid, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_align(lbl_uid, LV_ALIGN_TOP_MID, 0, 208);
    lv_obj_t* lab_sec = lv_label_create(scr_spool);
    lv_label_set_text(lab_sec, "ASSIGN TO:");
    lv_obj_set_style_text_color(lab_sec, lv_color_make(60, 90, 140), LV_PART_MAIN);
    lv_obj_set_style_text_font(lab_sec, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_align(lab_sec, LV_ALIGN_TOP_LEFT, 12, 236);
    const char* tl[] = {"T1", "T2", "T3", "T4"};
    for (int i = 0; i < 4; i++) {
        btn_t[i] = lv_btn_create(scr_spool); lv_obj_set_size(btn_t[i], 60, 42);
        lv_obj_align(btn_t[i], LV_ALIGN_TOP_LEFT, 12 + i * 66, 252);
        lv_obj_add_style(btn_t[i], &style_btn_inactive, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn_t[i], 0, LV_PART_MAIN);
        lv_obj_add_event_cb(btn_t[i], cb_toolhead, LV_EVENT_CLICKED, (void*)(intptr_t)(i+1));
        lv_obj_t* l = lv_label_create(btn_t[i]); lv_label_set_text(l, tl[i]); lv_obj_center(l);
    }
    btn_storage = lv_btn_create(scr_spool); lv_obj_set_size(btn_storage, 80, 42);
    lv_obj_align(btn_storage, LV_ALIGN_TOP_RIGHT, -12, 252);
    lv_obj_add_style(btn_storage, &style_btn_active, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn_storage, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_storage, cb_toolhead, LV_EVENT_CLICKED, (void*)(intptr_t)0);
    lv_obj_t* ls = lv_label_create(btn_storage); lv_label_set_text(ls, "Storage"); lv_obj_center(ls);
    btn_save = lv_btn_create(scr_spool); lv_obj_set_size(btn_save, 296, 50);
    lv_obj_align(btn_save, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_style(btn_save, &style_btn_save, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn_save, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_save, cb_save, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lsave = lv_label_create(btn_save);
    lv_label_set_text(lsave, "SAVE + SYNC");
    lv_obj_set_style_text_font(lsave, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(lsave);
}

void build_screen_saving() {
    scr_saving = lv_obj_create(nullptr); lv_obj_add_style(scr_saving, &style_bg, LV_PART_MAIN);
    lv_obj_t* sp = lv_spinner_create(scr_saving, 1000, 60);
    lv_obj_set_size(sp, 60, 60); lv_obj_align(sp, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_arc_color(sp, lv_color_make(0, 180, 255), LV_PART_INDICATOR);
    lv_obj_t* l = lv_label_create(scr_saving);
    lv_label_set_text(l, "Saving to Spoolman...");
    lv_obj_set_style_text_color(l, lv_color_make(0, 180, 255), LV_PART_MAIN);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 40);
}

void build_screen_saved() {
    scr_saved = lv_obj_create(nullptr); lv_obj_add_style(scr_saved, &style_bg, LV_PART_MAIN);
    lv_obj_t* icon = lv_label_create(scr_saved);
    lv_label_set_text(icon, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_make(0, 200, 80), LV_PART_MAIN);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -40);
    lbl_saved_msg = lv_label_create(scr_saved); lv_label_set_text(lbl_saved_msg, "Saved");
    lv_obj_set_style_text_color(lbl_saved_msg, lv_color_make(0, 200, 80), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_saved_msg, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(lbl_saved_msg, LV_ALIGN_CENTER, 0, 20);
}

void build_screen_unknown() {
    scr_unknown = lv_obj_create(nullptr); lv_obj_add_style(scr_unknown, &style_bg, LV_PART_MAIN);
    lv_obj_t* hdr = lv_obj_create(scr_unknown); lv_obj_set_size(hdr, 320, 44);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_make(80, 40, 0), LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hdr, 0, LV_PART_MAIN);
    lv_obj_t* hl = lv_label_create(hdr); lv_label_set_text(hl, "UNKNOWN SPOOL");
    lv_obj_set_style_text_color(hl, lv_color_make(255, 180, 0), LV_PART_MAIN);
    lv_obj_set_style_text_font(hl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(hl, LV_ALIGN_LEFT_MID, 12, 0);
    lbl_uid_unk = lv_label_create(scr_unknown); lv_label_set_text(lbl_uid_unk, "UID: ---");
    lv_obj_set_style_text_color(lbl_uid_unk, lv_color_make(255, 160, 40), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_uid_unk, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_align(lbl_uid_unk, LV_ALIGN_TOP_LEFT, 12, 54);
    lbl_weight_unk = lv_label_create(scr_unknown); lv_label_set_text(lbl_weight_unk, "Weight: ---g");
    lv_obj_set_style_text_color(lbl_weight_unk, lv_color_make(0, 200, 255), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_weight_unk, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(lbl_weight_unk, LV_ALIGN_TOP_LEFT, 12, 70);
    lv_obj_t* inst = lv_label_create(scr_unknown);
    lv_label_set_text(inst, "Open dashboard to register.\nUID + weight sent to bridge.");
    lv_label_set_long_mode(inst, LV_LABEL_LONG_WRAP); lv_obj_set_width(inst, 296);
    lv_obj_set_style_text_color(inst, lv_color_make(140, 160, 200), LV_PART_MAIN);
    lv_obj_set_style_text_font(inst, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(inst, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_t* db = lv_btn_create(scr_unknown); lv_obj_set_size(db, 200, 44);
    lv_obj_align(db, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(db, lv_color_make(40, 30, 0), LV_PART_MAIN);
    lv_obj_set_style_border_color(db, lv_color_make(200, 120, 0), LV_PART_MAIN);
    lv_obj_set_style_border_width(db, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(db, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(db, [](lv_event_t*) {
        g_state = StationState::IDLE ; g_last_uid = ""; lv_scr_load(scr_idle);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* dl = lv_label_create(db); lv_label_set_text(dl, "Dismiss");
    lv_obj_set_style_text_color(dl, lv_color_make(255, 180, 0), LV_PART_MAIN); lv_obj_center(dl);
}

void build_screen_error() {
    scr_error = lv_obj_create(nullptr); lv_obj_add_style(scr_error, &style_bg, LV_PART_MAIN);
    lv_obj_t* l = lv_label_create(scr_error);
    lv_label_set_text(l, LV_SYMBOL_WARNING " WiFi / Spoolman\nnot reachable");
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP); lv_obj_set_width(l, 280);
    lv_obj_set_style_text_color(l, lv_color_make(255, 80, 80), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
}

void update_spool_screen(const SpoolData& s, float new_w, float old_w) {
    lv_obj_set_style_bg_color(color_swatch, hex_to_lv_color(s.color_hex), LV_PART_MAIN);
    lv_label_set_text(lbl_vendor, s.vendor.c_str());
    lv_label_set_text(lbl_material, s.material.c_str());
    lv_label_set_text(lbl_name, s.name.c_str());
    char buf[64];
    snprintf(buf, sizeof(buf), "%dg", (int)new_w); lv_label_set_text(lbl_weight, buf);
    float len = calc_length_m(new_w, s.material);
    snprintf(buf, sizeof(buf), "~-%dm", (int)len); lv_label_set_text(lbl_length, buf);
    float delta = new_w - old_w;
    if (abs(delta) > 2) { snprintf(buf, sizeof(buf), "%+.0fg", delta); lv_label_set_text(lbl_weight_delta, buf); }
    else lv_label_set_text(lbl_weight_delta, "");
    int total = max(1000, (int)new_w + s.tare_weight);
    int pct = max(0, min(100, (int)((new_w / (float)(total - s.tare_weight)) * 100.0)));
    lv_bar_set_value(bar_filament, pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_filament,
        pct < 15 ? lv_color_make(200,40,40) : pct < 30 ? lv_color_make(220,140,0) : hex_to_lv_color(s.color_hex),
        LV_PART_INDICATOR);
    String ss = s.tag_uid.length() > 17 ? s.tag_uid.substring(0,17) + "..." : s.tag_uid;
    lv_label_set_text(lbl_uid, ("UID: " + ss).c_str());
    g_selected_toolhead = 0;
    for (int i = 0; i < 4; i++) lv_obj_add_style(btn_t[i], &style_btn_inactive, LV_PART_MAIN);
    lv_obj_add_style(btn_storage, &style_btn_active, LV_PART_MAIN);
}

void do_save() {
    float remaining = max(0.0f, g_weight_g - g_spool.tare_weight);
    float len = calc_length_m(remaining, g_spool.material);
    int ch = g_selected_toolhead;
    bool ok = update_spool_weight(g_spool.id, remaining, len, ch > 0 ? ch-1 : -1);
    if (ok && ch > 0) push_to_u1(g_spool, ch-1);
    String msg = ok ?
        String("Saved: ") + g_spool.vendor + " " + g_spool.name + "\n" +
        String((int)remaining) + "g ~" + String((int)len) + "m " + (ch>0?"T"+String(ch):"Storage")
        : "Save failed - check WiFi";
    lv_label_set_text(lbl_saved_msg, msg.c_str());
    lv_obj_set_style_text_color(lbl_saved_msg, ok?lv_color_make(0,200,80):lv_color_make(255,80,80), LV_PART_MAIN);
    g_saving = false; g_state = StationState::SAVED; lv_scr_load(scr_saved);
}

void setup() {
    Serial.begin(115200); Serial.println("SpoolStation booting...");
    pinMode(TFT_BL_PIN, OUTPUT); digitalWrite(TFT_BL_PIN, HIGH);
    tft.begin(); tft.setRotation(0); tft.fillScreen(TFT_BLACK);
    lv_init(); lv_disp_draw_buf_init(&draw_buf, buf, nullptr, LV_BUF_SIZE);
    static lv_disp_drv_t dd; lv_disp_drv_init(&dd);
    dd.hor_res = SCREEN_W; dd.ver_res = SCREEN_H;
    dd.flush_cb = lv_disp_flush; dd.draw_buf = &draw_buf;
    lv_disp_drv_register(&dd);
    gt911_init();
    static lv_indev_drv_t id; lv_indev_drv_init(&id);
    id.type = LV_INDEV_TYPE_POINTER; id.read_cb = lv_touch_read;
    lv_indev_drv_register(&id);
    setup_styles();
    build_screen_idle(); build_screen_spool(); build_screen_saving();
    build_screen_saved(); build_screen_unknown(); build_screen_error();
    lv_scr_load(scr_idle);
    scale.begin(HX711_DT_PIN, HX711_SCK_PIN);
    scale.set_scale(CALIBRATION_FACTOR); scale.tare();
    Serial.println("HX711 ready");
    pn532Serial.begin(115200, SERIAL_8N1, PN532_RX_PIN, PN532_TX_PIN);
    nfc.begin();
    uint32_t fw = nfc.getFirmwareVersion();
    if (fw) {
        Serial.printf("PN532 FW: %d.%d\n", (fw >> 16) & 0xFF, (fw >> 8) & 0xFF);
        nfc.setPassiveActivationRetries(0x01); nfc.SAMConfig();
    } else Serial.println("PN532 not found - check wiring (HSU mode)");
    Serial.printf("Connecting to %s...\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int att = 0;
    while (WiFi.status() != WL_CONNECTED && att < 20) { delay(500); lv_timer_handler(); att++; }
    if (WiFi.status() == WL_CONNECTED) Serial.printf("WiFi OK: %s\n", WiFi.localIP().toString().c_str());
    else { Serial.println("WiFi FAILED"); g_state = StationState::WIFI_ERROR; lv_scr_load(scr_error); }
}

unsigned long g_saved_ts = 0;

void loop() {
    lv_timer_handler();
    if (scale.is_ready()) {
        float raw = scale.get_units(1);
        if (raw < 0) raw = 0;
        g_weight_buf[g_weight_idx % STABLE_SAMPLES] = raw;
        g_weight_idx++;
        if (g_weight_idx >= STABLE_SAMPLES) g_weight_buf_full = true;
        g_weight_g = get_avg_weight(); g_weight_stable = is_weight_stable();
    }
    uint8_t uid[7]; uint8_t uid_len;
    bool tf = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uid_len, 50);
    if (tf) {
        String uid_str = format_uid(uid, uid_len);
        g_last_tag_ms = millis();
        if (uid_str != g_last_uid) {
            g_last_uid = uid_str;
            if (g_state == StationState::IDLE || g_state == StationState::SAVED) {
                g_state = StationState::TAG_READ;
                SpoolData found = find_spool_by_uid(uid_str);
                if (found.valid) {
                    float nr = max(0.0f, g_weight_g - found.tare_weight);
                    g_spool = found; g_state = StationState::SPOOL_FOUND;
                    update_spool_screen(found, nr, found.remaining_weight);
                    lv_scr_load(scr_spool);
                } else {
                    g_state = StationState::UNKNOWN_TAG;
                    if (lbl_uid_unk) lv_label_set_text(lbl_uid_unk, ("UID: "+uid_str).c_str());
                    if (lbl_weight_unk) lv_label_set_text(lbl_weight_unk, (String((int)g_weight_g)+"g").c_str());
                    DynamicJsonDocument doc(256);
                    doc["event"]="unknown_tag"; doc["uid"]=uid_str;
                    doc["weight_g"]=(int)g_weight_g; doc["stable"]=g_weight_stable;
                    String json; serializeJson(doc, json);
                    http_post(String(BRIDGE_URL)+"/station/event", jzson);
                    lv_scr_load(scr_unknown);
                }
            }
        }
    } else {
        if (g_last_uid != "" && millis() - g_last_tag_ms > 2000) {
            g_last_uid = "";
            if (g_state == StationState::SPOOL_FOUND ||
                g_state == StationState::UNKNOWN_TAG ||
                g_state == StationState::TAG_READ) {
                g_state = StationState::IDLE; lv_scr_load(scr_idle);
            }
        }
    }
    if (g_state == StationState::SAVING && g_saving) do_save();
    if (g_state == StationState::SAVED) {
        if (g_saved_ts == 0) g_saved_ts = millis();
        if (millis() - g_saved_ts > 3000) {
            g_saved_ts = 0; g_state = StationState::IDLE ;
            g_last_uid = ""; lv_scr_load(scr_idle);
        }
    } else g_saved_ts = 0;
    delay(5);
}
