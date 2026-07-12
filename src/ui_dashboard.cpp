/* LVGL UI Dashboard Module
 * Main dashboard screen with status bar, content area, and button bar
 */

#ifdef USE_LVGL_UI

#include <esp_log.h>
static const char *TAG = "Dashboard";

#include "ui_dashboard.h"
#include "ui_common.h"
#include "map/map_view.h"
#ifdef WITH_MAPLIBRE
#include "maplibre_display.h"
#endif
#include "ui_settings.h"
#include "ui_popups.h"
#include "ui_map_manager.h"
#include "map_state.h"
#include "lvgl_ui.h"

#include <Arduino.h>

LV_FONT_DECLARE(lv_font_mono_16);
LV_FONT_DECLARE(lv_font_mono_18);
LV_FONT_DECLARE(lv_font_mono_20);
LV_FONT_DECLARE(lv_font_mono_22);
LV_FONT_DECLARE(lv_font_mono_24);
#include <WiFi.h>
#include "esp_heap_caps.h"
#include <lvgl.h>

// Définition unique partagée par tous les .cpp (déclarée extern dans ui_common.h)
namespace UIScreens { lv_obj_t *_mainScreen = nullptr; }

#include "battery_utils.h"
#include "ble_utils.h"
#include "configuration.h"
#include "custom_characters.h"
#include "storage_utils.h"
#include "utils.h"
#include <TimeLib.h>
#include <algorithm>
#include <vector>
#ifndef ARDUINO
#include <sys/stat.h>
#include <cstring>
#endif

// External configuration and state
extern Configuration Config;
extern uint8_t myBeaconsIndex;
extern int myBeaconsSize;
extern bool WiFiConnected;
extern bool WiFiEcoMode;
extern bool WiFiUserDisabled;
extern bool bluetoothActive;
extern bool bluetoothConnected;
extern bool sendUpdate;
extern uint8_t loraIndex;
extern int loraIndexSize;

// APRS symbols (defined in lvgl_ui.cpp)
extern const char *symbolArray[];
extern const int symbolArraySize;
extern const uint8_t *symbolsAPRS[];

// Screen dimensions
#if defined(CROWPANEL_ADVANCE_35)
#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 320
#elif defined(WAVESHARE_S3_TOUCH_LCD_7)
#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 480
#elif defined(LINUX_SIM) || !defined(ARDUINO)
#define SCREEN_WIDTH 1024
#define SCREEN_HEIGHT 600
#else
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#endif

// APRS symbol canvas dimensions
#define APRS_CANVAS_WIDTH SYMBOL_WIDTH
#define APRS_CANVAS_HEIGHT SYMBOL_HEIGHT

// Scalable UI sizes
#if defined(WAVESHARE_S3_TOUCH_LCD_7)
#define STATUS_BAR_H  45
#define BTN_BAR_H     60
#define BTN_W        160
#define BTN_H         44
#define CONTENT_TOP   50
#elif defined(LINUX_SIM) || !defined(ARDUINO)
#define STATUS_BAR_H  60
#define BTN_BAR_H     80
#define BTN_W        220
#define BTN_H         60
#define CONTENT_TOP   65
#elif defined(CROWPANEL_ADVANCE_35)
#define STATUS_BAR_H  35
#define BTN_BAR_H     45
#define BTN_W        100
#define BTN_H         35
#define CONTENT_TOP   40
#else
#define STATUS_BAR_H  30
#define BTN_BAR_H     40
#define BTN_W         70
#define BTN_H         30
#define CONTENT_TOP   35
#endif

namespace UIDashboard {

// Dashboard screen and labels
static lv_obj_t *screen_main = nullptr;
static lv_obj_t *label_callsign = nullptr;
static lv_obj_t *label_gps = nullptr;
static lv_obj_t *label_lora = nullptr;
static lv_obj_t *label_time = nullptr;
static lv_obj_t *label_utc = nullptr;
static lv_obj_t *aprs_symbol_canvas = nullptr;
#ifdef ARDUINO
static lv_color_t *aprs_symbol_buf = nullptr;
#endif

// Last RX stations
static lv_obj_t *label_last_rx = nullptr;

// Status bar icons
static lv_obj_t *icon_gps_strict = nullptr;
static lv_obj_t *icon_wifi = nullptr;
static lv_obj_t *icon_bluetooth = nullptr;
static lv_obj_t *icon_battery = nullptr;
static lv_obj_t *label_battery_pct = nullptr;

// Forward declarations for button callbacks
static void btn_beacon_clicked(lv_event_t *e);
static void btn_setup_clicked(lv_event_t *e);
static void btn_msg_clicked(lv_event_t *e);
static void btn_map_clicked(lv_event_t *e);

static void dashboard_gesture_cb(lv_event_t *e) {
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_LEFT) {
        btn_msg_clicked(nullptr);
    } else if (dir == LV_DIR_RIGHT) {
        btn_setup_clicked(nullptr);
    }
}

#ifndef ARDUINO
static const char *aprsSymbolsRoot() {
    static char root[256];
    if (root[0]) return root;
    const char *candidates[] = {
        "/home/fab2/Developpement/LoRa_APRS/aprs-symbols/sd_card/LoRa_Tracker/Symbols",
        "/media/fab2/TILES/LoRa_Tracker/Symbols",
        "/data/LoRa_Tracker/Symbols", NULL };
    for (int i = 0; candidates[i]; i++) {
        struct stat st;
        if (stat(candidates[i], &st) == 0) {
            strncpy(root, candidates[i], sizeof(root) - 1);
            return root;
        }
    }
    return NULL;
}

static bool aprsSymbolPath(char table, char sym, char *path, size_t pathsz) {
    const char *root = aprsSymbolsRoot();
    if (!root) return false;
    const char *tableName = (table == '/') ? "primary" : "alternate";
    snprintf(path, pathsz, "A:%s/%s/%02X.png", root, tableName, (uint8_t)sym);
    return true;
}
#endif

void init() {
    // Initialize dashboard module (nothing to do yet)
}

lv_obj_t* getMainScreen() {
    return screen_main;
}

void drawAPRSSymbol(const char *symbolStr) {
    if (!aprs_symbol_canvas || !symbolStr) return;

#ifndef ARDUINO
    char table = (strlen(symbolStr) >= 2) ? symbolStr[0] : '/';
    char sym   = (strlen(symbolStr) >= 2) ? symbolStr[1]
               : (strlen(symbolStr) == 1) ? symbolStr[0] : 0;
    if (!sym) return;
    char path[320];
    if (aprsSymbolPath(table, sym, path, sizeof(path)))
        lv_image_set_src(aprs_symbol_canvas, path);
#else
    if (!aprs_symbol_buf) return;

    char symbolChar[2] = {0, 0};
    if (strlen(symbolStr) >= 2)      symbolChar[0] = symbolStr[1];
    else if (strlen(symbolStr) == 1) symbolChar[0] = symbolStr[0];

    int symbolIndex = -1;
    for (int i = 0; i < symbolArraySize; i++) {
        if (strcmp(symbolChar, symbolArray[i]) == 0) { symbolIndex = i; break; }
    }

    lv_canvas_fill_bg(aprs_symbol_canvas, lv_color_hex(0x16213e), LV_OPA_COVER);
    if (symbolIndex < 0) return;

    const uint8_t *bitMap = symbolsAPRS[symbolIndex];
    lv_color_t white = lv_color_hex(0xffffff);
    for (int y = 0; y < SYMBOL_HEIGHT; y++) {
        for (int x = 0; x < SYMBOL_WIDTH; x++) {
            int byteIndex = (y * ((SYMBOL_WIDTH + 7) / 8)) + (x / 8);
            if (bitMap[byteIndex] & (1 << (7 - (x % 8))))
                lv_canvas_set_px(aprs_symbol_canvas, x, y, white, LV_OPA_COVER);
        }
    }
    lv_obj_invalidate(aprs_symbol_canvas);
#endif
}

// Button event callbacks
static void btn_beacon_clicked(lv_event_t *e) {
#ifdef LINUX_SIM
    system("pkill -USR1 lora_aprs_tracker");
    fprintf(stderr, "BCN: sent SIGUSR1 to lora_rx\n");
#else
    sendUpdate = true;
    ESP_LOGD(TAG, "BEACON button pressed - requesting beacon");
    UIPopups::showBeaconPending();
#endif
}

static void btn_setup_clicked(lv_event_t *e) {
    ESP_LOGD(TAG, "SETUP button pressed");
    UIPopups::closeAll();
    UISettings::openSetup();
}

static void btn_msg_clicked(lv_event_t *e) {
    ESP_LOGI(TAG, "Before MSG - DRAM: %u  PSRAM: %u  Largest DRAM block: %u",
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    UIPopups::closeAll();
    LVGL_UI::openMessagesScreen();
}

static void btn_map_clicked(lv_event_t *e) {
    ESP_LOGD(TAG, "MAP button pressed");

    // Pause BLE if active — map needs DRAM that BLE occupies (~65 KB)
    if (bluetoothActive) {
        ESP_LOGI(TAG, "Pausing BLE for map entry");
        BLE_Utils::stop();
        bluetoothActive = false;
        MapState::blePausedForMap = true;
    }

    UIPopups::closeAll();

    if (MapState::screen_map) {
        lv_obj_del(MapState::screen_map);
        MapState::screen_map = nullptr;
    }
#ifdef WITH_MAPLIBRE
    if (MaplibreDisplay::isActive())
        MapState::screen_map = MapView::createGpuOverlay(NULL);
    else
#endif
        MapState::screen_map = MapView::create(NULL);
    lv_screen_load(MapState::screen_map);
    ESP_LOGD(TAG, "btn_map_clicked DONE");
}

// Public button action functions
void onBeaconClicked() { btn_beacon_clicked(nullptr); }
void onMsgClicked() { btn_msg_clicked(nullptr); }
void onMapClicked() { btn_map_clicked(nullptr); }
void onSetupClicked() { btn_setup_clicked(nullptr); }

void createDashboard() {
    // Create main screen — utiliser lv_screen_active() plutot que
    // lv_obj_create(NULL) qui peut creer un screen sans display valide sur ARM
    screen_main = lv_screen_active();
    lv_obj_set_style_bg_color(screen_main, lv_color_hex(0x1a1a2e), 0);

    // Status bar at top
    lv_obj_t *status_bar = lv_obj_create(screen_main);
    lv_obj_set_size(status_bar, SCREEN_WIDTH, STATUS_BAR_H);
    lv_obj_set_pos(status_bar, 0, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_set_style_pad_all(status_bar, 5, 0);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Callsign label (left)
    label_callsign = lv_label_create(status_bar);
    lv_label_set_text(label_callsign, "NOCALL");
    lv_obj_set_style_text_color(label_callsign, lv_color_hex(0xffffff), 0);
#if defined(WAVESHARE_S3_TOUCH_LCD_7) || !defined(ARDUINO)
    lv_obj_set_style_text_font(label_callsign, &lv_font_montserrat_24, 0);
#else
    lv_obj_set_style_text_font(label_callsign, &lv_font_montserrat_14, 0);
#endif

    // APRS symbol icon
#ifndef ARDUINO
    aprs_symbol_canvas = lv_image_create(status_bar);
    lv_obj_set_size(aprs_symbol_canvas, 32, 32);
    lv_obj_clear_flag(aprs_symbol_canvas, LV_OBJ_FLAG_CLICKABLE);
    if (!Config.beacons.empty()) {
        Beacon *b = &Config.beacons[myBeaconsIndex];
        String fullSymbol = b->overlay + b->symbol;
        drawAPRSSymbol(fullSymbol.c_str());
    }
#else
    aprs_symbol_buf = (lv_color_t *)lv_malloc(
        APRS_CANVAS_WIDTH * APRS_CANVAS_HEIGHT * sizeof(lv_color_t));
    if (aprs_symbol_buf) {
        aprs_symbol_canvas = lv_canvas_create(status_bar);
        lv_canvas_set_buffer(aprs_symbol_canvas, aprs_symbol_buf, APRS_CANVAS_WIDTH,
                             APRS_CANVAS_HEIGHT, LV_COLOR_FORMAT_NATIVE);
        lv_obj_set_size(aprs_symbol_canvas, APRS_CANVAS_WIDTH, APRS_CANVAS_HEIGHT);
        if (!Config.beacons.empty()) {
            Beacon *b = &Config.beacons[myBeaconsIndex];
            String fullSymbol = b->overlay + b->symbol;
            drawAPRSSymbol(fullSymbol.c_str());
        }
    }
#endif

    // UTC time (GPS reference)
    label_utc = lv_label_create(status_bar);
    lv_label_set_text(label_utc, "UTC --:--:--");
    lv_obj_set_style_text_color(label_utc, lv_color_hex(0xffffff), 0);
#if defined(WAVESHARE_S3_TOUCH_LCD_7) || !defined(ARDUINO)
    lv_obj_set_style_text_font(label_utc, &lv_font_mono_20, 0);
#else
    lv_obj_set_style_text_font(label_utc, &lv_font_mono_16, 0);
#endif

    // Local time
    label_time = lv_label_create(status_bar);
    lv_label_set_text(label_time, "--/-- --:--");
    lv_obj_set_style_text_color(label_time, lv_color_hex(0x888888), 0);
#if defined(WAVESHARE_S3_TOUCH_LCD_7) || !defined(ARDUINO)
    lv_obj_set_style_text_font(label_time, &lv_font_mono_20, 0);
#else
    lv_obj_set_style_text_font(label_time, &lv_font_mono_16, 0);
#endif

    // GPS Strict 3D icon (hidden by default, shown when active)
    icon_gps_strict = lv_label_create(status_bar);
    lv_label_set_text(icon_gps_strict, LV_SYMBOL_GPS " 3D");
    lv_obj_set_style_text_color(icon_gps_strict, lv_color_hex(0xffd700), 0); // Gold/Yellow
    if (!Config.gpsConfig.strict3DFix) lv_obj_add_flag(icon_gps_strict, LV_OBJ_FLAG_HIDDEN);

    // WiFi icon (hidden by default, shown when connected)
    icon_wifi = lv_label_create(status_bar);
    lv_label_set_text(icon_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(icon_wifi, lv_color_hex(0x00ff00), 0);
    lv_obj_add_flag(icon_wifi, LV_OBJ_FLAG_HIDDEN);

    // Bluetooth icon (hidden by default, shown when connected)
    icon_bluetooth = lv_label_create(status_bar);
    lv_label_set_text(icon_bluetooth, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_color(icon_bluetooth, lv_color_hex(0x00ff00), 0);
    lv_obj_add_flag(icon_bluetooth, LV_OBJ_FLAG_HIDDEN);

    // Battery icon + percentage
    icon_battery = lv_label_create(status_bar);
    lv_label_set_text(icon_battery, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(icon_battery, lv_color_hex(0x00ff00), 0);

    label_battery_pct = lv_label_create(status_bar);
    lv_label_set_text(label_battery_pct, "--%");
    lv_obj_set_style_text_color(label_battery_pct, lv_color_hex(0xffffff), 0);
#if defined(WAVESHARE_S3_TOUCH_LCD_7) || !defined(ARDUINO)
    lv_obj_set_style_text_font(label_battery_pct, &lv_font_montserrat_16, 0);
#else
    lv_obj_set_style_text_font(label_battery_pct, &lv_font_montserrat_12, 0);
#endif

    // Main content area
    lv_obj_t *content = lv_obj_create(screen_main);
    lv_obj_set_size(content, SCREEN_WIDTH - 10, SCREEN_HEIGHT - STATUS_BAR_H - BTN_BAR_H - 10);
    lv_obj_set_pos(content, 5, CONTENT_TOP);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x0f0f23), 0);
    lv_obj_set_style_border_color(content, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_radius(content, 8, 0);
    lv_obj_set_style_pad_all(content, 14, 0);
    lv_obj_set_style_pad_row(content, 8, 0);

    // GNSS info
    label_gps = lv_label_create(content);
    lv_label_set_text(label_gps, "GNSS: -- sat  Loc: --------\n"
                                 "Lat:      --.----   Lon:   --.----\n"
                                 "Alt:    ---- m      Spd:    --- km/h");
    lv_obj_set_style_text_color(label_gps, lv_color_hex(0x759a9e), 0);
#if defined(WAVESHARE_S3_TOUCH_LCD_7) || !defined(ARDUINO)
    lv_obj_set_style_text_font(label_gps, &lv_font_mono_20, 0);
#else
    lv_obj_set_style_text_font(label_gps, &lv_font_mono_16, 0);
#endif
    lv_obj_set_pos(label_gps, 0, 0);

    // LoRa info
    label_lora = lv_label_create(content);
    char lora_init[64];
    float freq = Config.loraTypes[loraIndex].frequency / 1000000.0;
    int rate = Config.loraTypes[loraIndex].dataRate;
    snprintf(lora_init, sizeof(lora_init), "LoRa: %.3f MHz  %d bps", freq, rate);
    lv_label_set_text(label_lora, lora_init);
    lv_obj_set_style_text_color(label_lora, lv_color_hex(0xff6b6b), 0);
#if defined(WAVESHARE_S3_TOUCH_LCD_7) || !defined(ARDUINO)
    lv_obj_set_style_text_font(label_lora, &lv_font_montserrat_22, 0);
#else
    lv_obj_set_style_text_font(label_lora, &lv_font_montserrat_16, 0);
#endif
#if defined(WAVESHARE_S3_TOUCH_LCD_7) || !defined(ARDUINO)
    lv_obj_set_pos(label_lora, 0, 105);
#else
    lv_obj_set_pos(label_lora, 0, 55);
#endif

    // Last RX stations
    label_last_rx = lv_label_create(content);
    lv_label_set_recolor(label_last_rx, true);
    lv_label_set_long_mode(label_last_rx, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label_last_rx, "Last RX:\n---");
    lv_obj_set_style_text_color(label_last_rx, lv_color_hex(0xffcc00), 0);
#if defined(WAVESHARE_S3_TOUCH_LCD_7) || !defined(ARDUINO)
    lv_obj_set_style_text_font(label_last_rx, &lv_font_mono_18, 0);
#else
    lv_obj_set_style_text_font(label_last_rx, &lv_font_mono_16, 0);
#endif
#if defined(WAVESHARE_S3_TOUCH_LCD_7) || !defined(ARDUINO)
    lv_obj_set_pos(label_last_rx, 0, 145);
#else
    lv_obj_set_pos(label_last_rx, 0, 80);
#endif

    // Bottom button bar
    lv_obj_t *btn_bar = lv_obj_create(screen_main);
    lv_obj_set_size(btn_bar, SCREEN_WIDTH, BTN_BAR_H);
    lv_obj_set_pos(btn_bar, 0, SCREEN_HEIGHT - BTN_BAR_H);
    lv_obj_set_style_bg_color(btn_bar, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_border_width(btn_bar, 0, 0);
    lv_obj_set_style_radius(btn_bar, 0, 0);
    lv_obj_set_style_pad_all(btn_bar, 5, 0);
    lv_obj_set_flex_flow(btn_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_bar, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
#if defined(WAVESHARE_S3_TOUCH_LCD_7) || defined(LINUX_SIM)
    lv_obj_set_style_text_font(btn_bar, &lv_font_montserrat_18, 0);
#endif

    // Beacon button (APRS red)
    lv_obj_t *btn_beacon = lv_btn_create(btn_bar);
    lv_obj_set_size(btn_beacon, BTN_W, BTN_H);
    lv_obj_set_style_bg_color(btn_beacon, lv_color_hex(0xcc0000), 0);
    lv_obj_add_event_cb(btn_beacon, btn_beacon_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_beacon = lv_label_create(btn_beacon);
    lv_label_set_text(lbl_beacon, "BEACON");
    lv_obj_center(lbl_beacon);
    lv_obj_set_style_text_color(lbl_beacon, lv_color_hex(0xffffff), 0);

    // Messages button (APRS blue)
    lv_obj_t *btn_msg = lv_btn_create(btn_bar);
    lv_obj_set_size(btn_msg, BTN_W, BTN_H);
    lv_obj_set_style_bg_color(btn_msg, lv_color_hex(0x0066cc), 0);
    lv_obj_add_event_cb(btn_msg, btn_msg_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_msg = lv_label_create(btn_msg);
    lv_label_set_text(lbl_msg, "MESSAGES");
    lv_obj_center(lbl_msg);
    lv_obj_set_style_text_color(lbl_msg, lv_color_hex(0xffffff), 0);

    // Map button (green)
    lv_obj_t *btn_map = lv_btn_create(btn_bar);
    lv_obj_set_size(btn_map, BTN_W, BTN_H);
    lv_obj_set_style_bg_color(btn_map, lv_color_hex(0x009933), 0);
    lv_obj_add_event_cb(btn_map, btn_map_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_map = lv_label_create(btn_map);
    lv_label_set_text(lbl_map, "MAP");
    lv_obj_center(lbl_map);
    lv_obj_set_style_text_color(lbl_map, lv_color_hex(0xffffff), 0);

    // Settings button
    lv_obj_t *btn_settings = lv_btn_create(btn_bar);
    lv_obj_set_size(btn_settings, BTN_W, BTN_H);
    lv_obj_set_style_bg_color(btn_settings, lv_color_hex(0xc792ea), 0);
    lv_obj_add_event_cb(btn_settings, btn_setup_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_settings = lv_label_create(btn_settings);
    lv_label_set_text(lbl_settings, "SETTINGS");
    lv_obj_center(lbl_settings);
    lv_obj_set_style_text_color(lbl_settings, lv_color_hex(0x000000), 0);

    // Swipe gesture navigation
    lv_obj_add_event_cb(screen_main, dashboard_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_clear_flag(screen_main, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // Load the screen
    lv_scr_load(screen_main);
    UIScreens::setMainScreen(screen_main);
}

// Update functions
void updateGPS(double lat, double lng, double alt, double speed, int sats, double hdop) {
    if (label_gps) {
        char buf[128];
        const char *locator = Utils::getMaidenheadLocator(lat, lng, 8);

        // Determine HDOP quality indicator
        const char *hdopState = "";
        if (hdop > 5.0) {
            hdopState = "X"; // Bad precision
        } else if (hdop > 2.0 && hdop < 5.0) {
            hdopState = "-"; // Medium precision
        } else if (hdop <= 2.0) {
            hdopState = "+"; // Good precision
        }

        char c1[32], c2[32];
        snprintf(c1, sizeof(c1), "GNSS: %d%s sat", sats, hdopState);
        snprintf(c2, sizeof(c2), "Loc: %s", locator);
        snprintf(buf, sizeof(buf), "%-20s%s\n", c1, c2);

        snprintf(c1, sizeof(c1), "Lat:  %.4f", lat);
        snprintf(c2, sizeof(c2), "Lon: %.4f", lng);
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%-20s%s\n", c1, c2);

        snprintf(c1, sizeof(c1), "Alt:  %.0f m", alt);
        snprintf(c2, sizeof(c2), "Spd: %.0f km/h", speed);
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%-20s%s", c1, c2);
        lv_label_set_text(label_gps, buf);
    }
}

void updateBattery(int percent, float voltage) {
    // Update battery icon
    if (icon_battery) {
        // Select icon based on level
        if (percent > 85) {
            lv_label_set_text(icon_battery, LV_SYMBOL_BATTERY_FULL);
        } else if (percent > 60) {
            lv_label_set_text(icon_battery, LV_SYMBOL_BATTERY_3);
        } else if (percent > 35) {
            lv_label_set_text(icon_battery, LV_SYMBOL_BATTERY_2);
        } else if (percent > 10) {
            lv_label_set_text(icon_battery, LV_SYMBOL_BATTERY_1);
        } else {
            lv_label_set_text(icon_battery, LV_SYMBOL_BATTERY_EMPTY);
        }

        // Change color based on level
        if (percent > 50) {
            lv_obj_set_style_text_color(icon_battery, lv_color_hex(0x00ff00), 0); // Green
        } else if (percent > 20) {
            lv_obj_set_style_text_color(icon_battery, lv_color_hex(0xffa500), 0); // Orange
        } else {
            lv_obj_set_style_text_color(icon_battery, lv_color_hex(0xff6b6b), 0); // Red
        }
    }

    // Update battery percentage
    if (label_battery_pct) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", percent);
        lv_label_set_text(label_battery_pct, buf);
    }
}

void updateLoRa(const char *lastRx, int rssi) {
    (void)lastRx; // Now handled by updateLastRx
    (void)rssi;
    // Just refresh LoRa freq/rate info
    refreshLoRaInfo();
}

void refreshLoRaInfo() {
    if (label_lora) {
        char buf[64];
        float freq = Config.loraTypes[loraIndex].frequency / 1000000.0;
        int rate = Config.loraTypes[loraIndex].dataRate;
        snprintf(buf, sizeof(buf), "LoRa: %.3f MHz  %d bps", freq, rate);
        lv_label_set_text(label_lora, buf);
    }
}

void updateLastRx() {
    if (!label_last_rx) return;
    const std::vector<DashboardRxEntry> &entries = STORAGE_Utils::getDashboardLastRx();
    if (entries.empty()) {
        lv_label_set_text(label_last_rx, "Last RX:\n---");
        return;
    }

    String text = "Last RX:";
    char line[128];

    for (size_t i = 0; i < entries.size() && i < 8; i++) {
        const DashboardRxEntry &e = entries[i];

        // No timestamp - details available in MSG > Frames
        snprintf(line, sizeof(line), "\n#00ff00 %-9.9s  RSSI:%-4d  SNR:%-3.0f#",
                 e.callsign.c_str(), e.rssi, e.snr);
        text += line;
    }
    lv_label_set_text(label_last_rx, text.c_str());
    }

    void updateGPSStrictIcon() {
    if (icon_gps_strict) {
        if (Config.gpsConfig.strict3DFix) {
            lv_obj_clear_flag(icon_gps_strict, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(icon_gps_strict, LV_OBJ_FLAG_HIDDEN);
        }
    }
    }

    void updateWiFi(bool connected, int rssi) {
    if (icon_wifi) {
        // Show icon only if WiFi is connected
        if (connected && !WiFiUserDisabled && !WiFiEcoMode) {
            lv_obj_clear_flag(icon_wifi, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(icon_wifi, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void updateCallsign(const char *callsign) {
    if (label_callsign) {
        lv_label_set_text(label_callsign, callsign);
    }
}

void updateTime(int day, int month, int year, int hour, int minute, int second) {
    (void)year;   // Unused
    (void)second; // Unused
    if (label_time) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d", day, month, hour, minute);
        lv_label_set_text(label_time, buf);
    }
}

void updateUtcTime(int hour, int minute, int second) {
    if (label_utc) {
        char buf[16];
        snprintf(buf, sizeof(buf), "UTC %02d:%02d:%02d", hour, minute, second);
        lv_label_set_text(label_utc, buf);
    }
}

void updateBluetooth() {
    if (icon_bluetooth) {
        // Show icon only if BT is connected
        if (bluetoothActive && !BLE_Utils::isSleeping() && bluetoothConnected) {
            lv_obj_clear_flag(icon_bluetooth, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(icon_bluetooth, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void addRxLine(const char *frame) {
    if (!frame || !*frame) return;

    int rssi = 0;
    float snr = 0;
    const char *payload = frame;
    // Optional prefix: "RSSI:-105 SNR:8.5 "
    if (strncmp(payload, "RSSI:", 5) == 0) {
        rssi = atoi(payload + 5);
        payload = strchr(payload, ' ');
        if (!payload) return;
        if (strncmp(payload + 1, "SNR:", 4) == 0) {
            snr = atof(payload + 5);
            payload = strchr(payload + 1, ' ');
            if (!payload) return;
            payload++;
        }
    }
    // Skip AX.25 binary header / TNC2 garbage until callsign start
    while (*payload && !((*payload >= 'A' && *payload <= 'Z') ||
                         (*payload >= 'a' && *payload <= 'z') ||
                         (*payload >= '0' && *payload <= '9')))
        payload++;
    // Parse APRS frame: CALLSIGN>PATH:PAYLOAD
    const char *gt = strchr(payload, '>');
    if (!gt) { fprintf(stderr, "FRAME_NO_GT: [%s]\n", frame); return; }
    std::string callsign(payload, gt - payload);
    if (callsign.empty()) { fprintf(stderr, "FRAME_EMPTY: [%s]\n", frame); return; }

    // Store as "MM-DD HH:MM RSSI:x SNR:y.z APRS_frame"
    time_t now = time(nullptr);
    struct tm *tm = localtime(&now);
    char stored[512];
    snprintf(stored, sizeof(stored), "%02d-%02d %02d:%02d RSSI:%d SNR:%.1f %s",
             tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min,
             rssi, snr, payload);
    STORAGE_Utils::appendFrame(String(stored));

    DashboardRxEntry entry;
    entry.callsign = callsign;
    entry.rssi = rssi;
    entry.snr = snr;
    STORAGE_Utils::addRxEntry(entry);
    STORAGE_Utils::updateRxStats(rssi, snr);

    // Stats par station + digi stats (direct = pas de '*' dans le path)
    const char *colon = strchr(gt, ':');
    bool isDirect = true;
    if (colon && colon > gt + 1) {
        std::string path(gt + 1, colon - gt - 1);
        if (path.find('*') != std::string::npos) isDirect = false;
        STORAGE_Utils::updateDigiStats(String(path.c_str()));
    }
    STORAGE_Utils::updateStationStats(String(callsign.c_str()), rssi, snr, isDirect);

    updateLastRx();
    UIMessaging::refreshFramesList();
}

void returnToDashboard() {
    if (screen_main) {
#ifdef LINUX_SIM
        lv_screen_load(screen_main);
#else
        UI_SCR_LOAD_ANIM(screen_main, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, false);
#endif
    }
}

// Provide label access for UISettings (callsign, wifi labels)
lv_obj_t* getLabelCallsign() { return label_callsign; }
lv_obj_t* getLabelWifi() { return nullptr; } // Removed from dashboard, now icon only

} // namespace UIDashboard

#endif // USE_LVGL_UI
