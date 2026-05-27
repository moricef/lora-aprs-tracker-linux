#include <cstdio>
#include <ctime>
#include <deque>
#include <string>
#include <mutex>
#include <pthread.h>
#include <unistd.h>
#include "storage_utils.h"

#include <sys/stat.h>

// Arduino.h provides millis/delay/String etc.
// LVGL
#include "lvgl/lvgl.h"
#ifdef LINUX_SIM
#include "lvgl/src/drivers/sdl/lv_sdl_keyboard.h"
#include "lvgl/src/drivers/sdl/lv_sdl_mouse.h"
#include "lvgl/src/drivers/sdl/lv_sdl_mousewheel.h"
#include "lvgl/src/drivers/sdl/lv_sdl_window.h"
#endif
#include "lvgl/src/drivers/display/fb/lv_linux_fbdev.h"
#include "lvgl/src/drivers/evdev/lv_evdev.h"
#include "FreeRTOS.h"
#include <APRSPacketLib.h>
#include "ui_dashboard.h"
#include "ui_messaging.h"
#include "ui_settings.h"
#include "ui_popups.h"
#include "configuration.h"
#include "map_state.h"
#include "notification_utils.h"
#include "msg_utils.h"
#include "lora_utils.h"
#include "map/map_raster.h"
#include "map/map_vector.h"

// Globals needed by ui_dashboard and ui_messaging
bool WiFiConnected=false, WiFiEcoMode=false, WiFiUserDisabled=false;
bool bluetoothActive=false, bluetoothConnected=false, sendUpdate=false;
uint8_t myBeaconsIndex=0, loraIndex=0;
int myBeaconsSize=1, loraIndexSize=1;
const char *symbolArray[] = {"/>"};
extern const int symbolArraySize;
const int symbolArraySize = 1;
const uint8_t *symbolsAPRS[] = {nullptr};
Configuration Config;
SemaphoreHandle_t spiMutex = 0;
uint32_t lastActivityTime = 0;
bool screenDimmed = false;
uint8_t screenBrightness = 255;
bool displayEcoMode = false;
String versionDate = "2026-05-13";
String versionNumber = "2.11.0-linux";
uint32_t last_tick = 0;
int wifiRetryCount = 0;
// Globals required by real modules (msg_utils, station_utils)
Beacon* currentBeacon = nullptr;
bool digipeaterActive = false;
uint32_t lastTxTime = 0;
uint32_t lastTx = 0;
uint32_t txInterval = 60000UL;
double lastTxLat = 0.0, lastTxLng = 0.0, lastTxDistance = 0.0;
bool miceActive = false;
bool smartBeaconActive = false;
APRSPacket lastReceivedPacket;

// Stdin → dashboard bridge
static std::mutex rxMutex;
static std::deque<std::string> rxQueue;
static volatile bool stdinRunning = true;

static void *stdinThread(void *) {
    char buf[512];
    while (stdinRunning) {
        if (fgets(buf, sizeof(buf), stdin)) {
            std::lock_guard<std::mutex> lock(rxMutex);
            rxQueue.push_back(std::string(buf));
        } else {
            usleep(100000); // EOF or error, wait before retry
        }
    }
    return nullptr;
}

static void sysTimeCallback(lv_timer_t *) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    UIDashboard::updateTime(t->tm_mday, t->tm_mon + 1, t->tm_year + 1900,
                            t->tm_hour, t->tm_min, t->tm_sec);
}

static void stdinTimerCallback(lv_timer_t *) {
    std::lock_guard<std::mutex> lock(rxMutex);
    while (!rxQueue.empty()) {
        const std::string &line = rxQueue.front();
        if (strncmp(line.c_str(), "CFG:", 4) == 0) {
            char call[32], path[32]; float freq;
            if (sscanf(line.c_str(), "CFG:%31[^,],%f,%31s", call, &freq, path) == 3)
                UIDashboard::updateCallsign(call);
        } else if (strncmp(line.c_str(), "GPS:", 4) == 0) {
            double lat, lon, alt, spd, hdop;
            int sats, timev = 0;
            if (sscanf(line.c_str(), "GPS:%lf,%lf,%lf,%lf,%lf,%d,%d",
                       &lat, &lon, &alt, &spd, &hdop, &sats, &timev) >= 3) {
                UIDashboard::updateGPS(lat, lon, alt, spd, sats, hdop);
                if (timev > 0) {
                    int h = timev / 10000, m = (timev / 100) % 100, s = timev % 100;
                    UIDashboard::updateUtcTime(h, m, s);
                }
            }
        } else if (strncmp(line.c_str(), "TX:", 3) == 0) {
            UIPopups::showTxPacket(line.substr(3));
        } else if (strncmp(line.c_str(), "MSG:", 4) == 0) {
            // Format: MSG:sender:message
            std::string rest = line.substr(4);
            size_t sep = rest.find(':');
            std::string popup = (sep != std::string::npos)
                ? "De: " + rest.substr(0, sep) + "\n" + rest.substr(sep + 1)
                : rest;
            UIPopups::showRxPacket(popup);
        } else if (strncmp(line.c_str(), "BEEP:", 5) == 0) {
            const char *b = line.c_str() + 5;
            if (strncmp(b, "BOOT",     4) == 0) NOTIFICATION_Utils::start();
            else if (strncmp(b, "TX",  2) == 0) NOTIFICATION_Utils::beaconTxBeep();
            else if (strncmp(b, "MSG", 3) == 0) NOTIFICATION_Utils::messageBeep();
            else if (strncmp(b, "STATION",  7) == 0) NOTIFICATION_Utils::stationHeardBeep();
            else if (strncmp(b, "SHUTDOWN", 8) == 0) NOTIFICATION_Utils::shutDownBeep();
            else if (strncmp(b, "LOWBAT",   6) == 0) NOTIFICATION_Utils::lowBatteryBeep();
        } else {
            const char *p = line.c_str();
            if (strncmp(p, "RX ", 3) == 0) {
                p += 3;
                // Reconstruire le paquet pour peupler mapStations[]
                int rssi = 0; float snr = 0.0f;
                const char *frameStart = p;
                if (sscanf(p, "RSSI:%d SNR:%f ", &rssi, &snr) == 2) {
                    const char *s = strchr(p, ' ');        // après RSSI:xxx
                    if (s) s = strchr(s + 1, ' ');         // après SNR:x.x
                    if (s) frameStart = s + 1;
                }
                std::string frame(frameStart);
                while (!frame.empty() && (frame.back() == '\n' || frame.back() == '\r'))
                    frame.pop_back();
                ReceivedLoRaPacket pkt;
                pkt.text = String("\x3c\xff\x01") + frame.c_str();
                pkt.rssi = rssi;
                pkt.snr  = snr;
                pkt.freqError = 0;
                MSG_Utils::checkReceivedMessage(pkt);
                MapRaster::refreshStations();
            }
            UIDashboard::addRxLine(p);
        }
        rxQueue.pop_front();
    }
}

static lv_display_t *hal_init(void) {
    lv_group_set_default(lv_group_create());

    // Dual backend : fbdev/evdev sur Odroid (si /dev/fb0 existe),
    // SDL2 sur PC (dev).
    bool use_fbdev = false;
    const char *fbdevFile = getenv("LV_FBDEV_FILE");
    struct stat fbSt;
    if ((fbdevFile && fbdevFile[0]) || stat("/dev/fb0", &fbSt) == 0) {
        use_fbdev = true;
    }

    if (use_fbdev) {
        lv_display_t *disp = lv_linux_fbdev_create();
        if (fbdevFile && fbdevFile[0])
            lv_linux_fbdev_set_file(disp, fbdevFile);
        lv_display_set_default(disp);

        const char *touchPath = getenv("LV_EVDEV_TOUCH");
        if (!touchPath || !touchPath[0]) touchPath = "/dev/input/event0";
        lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, touchPath);
        if (touch) {
            lv_indev_set_display(touch, disp);
            lv_indev_set_group(touch, lv_group_get_default());
            lv_evdev_set_calibration(touch, 0, 0, 1023, 599);
        }
        fprintf(stderr, "Display: fbdev 1024x600, touch=%s (%s)\n",
                touchPath, touch ? "OK" : "FAILED");
        return disp;
    }

    // SDL2 fallback
    lv_display_t *disp = lv_sdl_window_create(1024, 600);
    lv_indev_t *mouse = lv_sdl_mouse_create();
    lv_indev_set_group(mouse, lv_group_get_default());
    lv_indev_set_display(mouse, disp);
    lv_display_set_default(disp);
    LV_IMAGE_DECLARE(mouse_cursor_icon);
    lv_obj_t *cursor = lv_image_create(lv_screen_active());
    lv_image_set_src(cursor, &mouse_cursor_icon);
    lv_indev_set_cursor(mouse, cursor);
    lv_indev_t *mw = lv_sdl_mousewheel_create();
    lv_indev_set_display(mw, disp);
    lv_indev_t *kb = lv_sdl_keyboard_create();
    lv_indev_set_display(kb, disp);
    lv_indev_set_group(kb, lv_group_get_default());
    fprintf(stderr, "Display: SDL2 1024x600\n");
    return disp;
}

int main(void) {
    STORAGE_Utils::setup();
    // Charge la config partagée avant toute écriture depuis l'UI :
    // sinon writeFile() depuis l'écran Son écraserait le fichier avec les valeurs par défaut.
    Config.init();
    // Synchronise les compteurs avec la config chargée (sinon figés à 1 → l'UI n'affiche qu'un profil)
    loraIndexSize = (int)Config.loraTypes.size();
    myBeaconsSize = (int)Config.beacons.size();
    STORAGE_Utils::loadStats();
    lv_init();
    hal_init();
    // Ouvre les tuiles vectorielles (sans bloquer si fichier absent)
    MapVector::open("/home/fab2/Developpement/LoRa_APRS/sud-france-aprs.pmtiles");
    UIDashboard::createDashboard();
    fprintf(stderr, "Real ESP32 dashboard running (1024x600)\n");

    // System time updated every second
    lv_timer_create(sysTimeCallback, 1000, nullptr);
    // Timer to drain stdin queue into dashboard
    lv_timer_create(stdinTimerCallback, 100, nullptr);
    // Timer to refresh stats tab when dirty
    lv_timer_create([](lv_timer_t*){ UIMessaging::refreshStatsIfActive(); }, 2000, nullptr);

    // Background thread reading stdin
    pthread_t tid;
    pthread_create(&tid, nullptr, stdinThread, nullptr);

    while (1) {
        UISettings::checkPendingWebConf();
        uint32_t d = lv_timer_handler();
        if (d == LV_NO_TIMER_READY) d = 5;
        lv_delay_ms(d);
    }

    stdinRunning = false;
    pthread_join(tid, nullptr);
    return 0;
}

lv_obj_t* MapState::screen_map = nullptr;
bool MapState::blePausedForMap = false;
