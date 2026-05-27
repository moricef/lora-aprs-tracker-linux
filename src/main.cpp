// LoRa APRS Tracker — Linux port (Odroid C2 / aarch64)
// Mirrors the ESP32 firmware logic: same modules, same data flow.
#include "esp_log.h"
#include <signal.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctime>
#include <dirent.h>
#include "configuration.h"
#include "lora_utils.h"
#include "gps_utils.h"
#include "smartbeacon_utils.h"
#include "station_utils.h"
#include "storage_utils.h"
#include "msg_utils.h"
#include "aprs_is_utils.h"
#include "linux_stubs.h"
#include "FreeRTOS.h"
#include "map_state.h"
#include "webconf_httpd.h"
#include "notification_utils.h"
#include <APRSPacketLib.h>

#ifdef USE_LVGL_UI
#include "lvgl/lvgl.h"
#include "lvgl/src/drivers/display/drm/lv_linux_drm.h"
#include "lvgl/src/drivers/evdev/lv_evdev.h"
#include "ui_dashboard.h"
#include "ui_messaging.h"
#include "ui_settings.h"
#include "ui_popups.h"
#include "map_state.h"
#include "map/map_raster.h"
#include "map/map_vector.h"
#include <sys/stat.h>
#endif

static const char* TAG = "Main";

// ─── Global state (mirrors ESP32 globals) ────────────────────────────────────
Configuration   Config;
uint8_t         myBeaconsIndex  = 0;
int             myBeaconsSize   = 0;
Beacon*         currentBeacon   = nullptr;

uint8_t         loraIndex       = 0;
int             loraIndexSize   = 0;
LoraType*       currentLoRaType = nullptr;

bool     sendUpdate         = true;
bool     miceActive         = false;
bool     smartBeaconActive  = true;
bool     digipeaterActive   = false;

uint32_t lastTx             = 0;
uint32_t txInterval         = 60000UL;
uint32_t lastTxTime         = 0;
double   lastTxLat          = 0.0;
double   lastTxLng          = 0.0;
double   lastTxDistance     = 0.0;

uint32_t refreshDisplayTime = 0;
uint32_t lastGPSTime        = 0;

APRSPacket lastReceivedPacket;

// UI globals (référencées par ui_*.cpp)
String versionDate = "2026-05-26";
String versionNumber = "2.11.0-linux";
bool   WiFiConnected = false, WiFiEcoMode = false, WiFiUserDisabled = false;
bool   bluetoothActive = false, bluetoothConnected = false;
SemaphoreHandle_t spiMutex = 0;
uint32_t lastActivityTime = 0;
bool   screenDimmed = false;
uint8_t screenBrightness = 255;
bool   displayEcoMode = false;
uint32_t last_tick = 0;
int    wifiRetryCount = 0;
const char *symbolArray[] = {"/>"};
extern const int symbolArraySize = 1;
const uint8_t *symbolsAPRS[] = {nullptr};

extern bool gpsIsActive;       // defined in gps_utils.cpp
extern SmartBeaconValues currentSmartBeaconValues;

static volatile bool running = true;
static volatile bool reloadConfig = false;

static void sigHandler(int sig) {
    if (sig == SIGUSR1) {
        sendUpdate = true;
        ESP_LOGI(TAG, "SIGUSR1 → beacon triggered");
    } else if (sig == SIGHUP) {
        reloadConfig = true;
    } else {
        running = false;
    }
}

// stdin command thread (BCN, TX, QUIT)
#include <pthread.h>
static void* cmdThread(void*) {
    char buf[32];
    while (running) {
        fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 200000};
        if (select(STDIN_FILENO+1, &fds, nullptr, nullptr, &tv) > 0) {
            if (fgets(buf, sizeof(buf), stdin)) {
                if (strncmp(buf, "BCN", 3) == 0 || strncmp(buf, "TX", 2) == 0) {
                    sendUpdate = true;
                    ESP_LOGI(TAG, "CMD: beacon triggered");
                } else if (strncmp(buf, "QUIT", 4) == 0) {
                    running = false;
                }
            }
        }
    }
    return nullptr;
}

// TCP bridge thread : écoute sur 9876, lit lignes "TXPKT:<frame>" depuis
// la simu PC et les transmet via LoRa. Permet l'envoi de messages APRS
// depuis l'interface dashboard pendant le dev (avant qu'on ait l'écran
// directement sur l'Odroid en mode monolithique).
static void* txBridgeThread(void*) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { ESP_LOGE(TAG, "TXbridge: socket failed"); return nullptr; }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(9876);
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "TXbridge: bind 9876 failed: %s", strerror(errno));
        close(srv); return nullptr;
    }
    listen(srv, 1);
    ESP_LOGI(TAG, "TXbridge: listening on 9876");

    while (running) {
        int cli = accept(srv, nullptr, nullptr);
        if (cli < 0) continue;
        ESP_LOGI(TAG, "TXbridge: client connected");
        char buf[1024];
        std::string acc;
        while (running) {
            int n = recv(cli, buf, sizeof(buf), 0);
            if (n <= 0) break;
            acc.append(buf, n);
            // Process complete lines
            size_t pos;
            while ((pos = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, pos);
                acc.erase(0, pos + 1);
                if (line.rfind("TXPKT:", 0) == 0) {
                    String frame(line.substr(6).c_str());
                    LoRa_Utils::sendNewPacket(frame);
                    ESP_LOGI(TAG, "TXbridge: TX %s", frame.c_str());
                } else if (line == "BCN" || line == "TX") {
                    sendUpdate = true;
                    ESP_LOGI(TAG, "TXbridge: beacon triggered");
                }
            }
        }
        close(cli);
        ESP_LOGI(TAG, "TXbridge: client disconnected");
    }
    close(srv);
    return nullptr;
}

// ─── setup() ─────────────────────────────────────────────────────────────────
static void setup() {
    signal(SIGINT,  sigHandler);
    signal(SIGTERM, sigHandler);
    signal(SIGUSR1, sigHandler);
    signal(SIGHUP,  sigHandler);

    ESP_LOGI(TAG, "=== LoRa APRS Tracker Linux ===");

    STORAGE_Utils::setup();
    Config.init();

    myBeaconsSize  = (int)Config.beacons.size();
    loraIndexSize  = (int)Config.loraTypes.size();
    currentBeacon  = &Config.beacons[myBeaconsIndex];
    currentLoRaType= &Config.loraTypes[loraIndex];

    STATION_Utils::loadIndex(0);
    STATION_Utils::loadIndex(1);
    currentBeacon   = &Config.beacons[myBeaconsIndex];
    currentLoRaType = &Config.loraTypes[loraIndex];

    STATION_Utils::nearStationInit();
    STATION_Utils::mapStationsInit();

    STORAGE_Utils::loadStats();
    MSG_Utils::loadNumMessages();

    // Vector tiles (optional — fails silently if file absent)
    // Searches /data/LoRa_Tracker/VectorMaps/ for .pmtiles files
    // (same convention as firmware: /LoRa_Tracker/VectMaps/<region>/)
    {
        const char *vecDir = "/data/LoRa_Tracker/VectMaps";
        DIR *d = opendir(vecDir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (e->d_type != DT_DIR || e->d_name[0] == '.') continue;
                std::string path = std::string(vecDir) + "/" + e->d_name + "/" + e->d_name + ".pmtiles";
                struct stat st;
                if (stat(path.c_str(), &st) == 0) {
                    MapVector::open(path.c_str());
                    if (MapVector::isOpen()) MapVector::startWorker();
                    break;  // first region found
                }
            }
            closedir(d);
        }
    }

    GPS_Utils::setup();
    LoRa_Utils::setup();
    APRS_IS_Utils::setup();
    WEBCONF::start(8080);
    NOTIFICATION_Utils::start();

    miceActive = APRSPacketLib::validateMicE(currentBeacon->micE);

#ifdef USE_LVGL_UI
    fprintf(stderr, "[UI] lv_init...\n"); fflush(stderr);
    lv_init();
    lv_group_set_default(lv_group_create());
    lv_display_t *disp = lv_linux_drm_create();
    if (!disp) {
        ESP_LOGE(TAG, "drm_create failed — /dev/dri/card0 inaccessible?");
    } else {
        lv_result_t drm_ok = lv_linux_drm_set_file(disp, "/dev/dri/card0", -1);
        lv_display_set_default(disp);
        lv_timer_handler();  // initialise le rendu display avant création widgets
        fprintf(stderr, "[UI] evdev...\n"); fflush(stderr);
        // /dev/input/by-path/ contient le chemin stable Waveshare
        const char *touchPath = "/dev/input/by-path/platform-c9100000.usb-usb-0:1.2:1.0-event";
        // fallback event5 si by-path absent
        struct stat st;
        if (stat(touchPath, &st) != 0) touchPath = "/dev/input/event5";
        lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, touchPath);
        if (touch) {
            lv_indev_set_display(touch, disp);
            lv_indev_set_group(touch, lv_group_get_default());
            lv_evdev_set_calibration(touch, 0, 0, 1023, 599);
        }
        UIDashboard::createDashboard();
        { FILE *f = fopen("/tmp/ui_ok.txt", "w"); if (f) { fprintf(f, "DASH OK\n"); fclose(f); } }
        ESP_LOGI(TAG, "Display: fbdev 1024x600, touch=%s", touch ? "OK" : "none");
    }
#endif

    // stdout: pipe to dashboard
    // format: GPS:<lat>,<lon>,<alt>,<speed>,<hdop>,<sats>,<time>
    // LoRa RX frames: forwarded as-is via logRawFrame → frames.log

    ESP_LOGI(TAG, "Callsign: %s  Freq: %.3f MHz  Path: %s",
             currentBeacon->callsign.c_str(),
             (float)currentLoRaType->frequency / 1000000.0f,
             Config.path.c_str());

    // Start stdin command thread
    static pthread_t cmdTid;
    pthread_create(&cmdTid, nullptr, cmdThread, nullptr);
    pthread_detach(cmdTid);

    // Start TCP command listener (bridge depuis simu PC en dev)
    static pthread_t txTid;
    pthread_create(&txTid, nullptr, txBridgeThread, nullptr);
    pthread_detach(txTid);

    refreshDisplayTime = millis();
    lastTxTime         = millis();
}

// ─── loop() ──────────────────────────────────────────────────────────────────
static void loop() {
    if (reloadConfig) {
        reloadConfig = false;
        Config.reload();
        myBeaconsSize   = (int)Config.beacons.size();
        loraIndexSize   = (int)Config.loraTypes.size();
        currentBeacon   = &Config.beacons[myBeaconsIndex];
        currentLoRaType = &Config.loraTypes[loraIndex];
        miceActive = APRSPacketLib::validateMicE(currentBeacon->micE);
        LoRa_Utils::processPendingChanges();
        ESP_LOGI(TAG, "Config reloaded: %s  %.3f MHz",
                 currentBeacon->callsign.c_str(),
                 (float)currentLoRaType->frequency / 1000000.0f);
        printf("CFG:reload\n"); fflush(stdout);
    }

    currentBeacon = &Config.beacons[myBeaconsIndex];
#ifdef USE_LVGL_UI
    {
        static String lastCallsign = "";
        if (currentBeacon->callsign != lastCallsign) {
            lastCallsign = currentBeacon->callsign;
            UIDashboard::updateCallsign(lastCallsign.c_str());
        }
    }
#endif

    if (APRSPacketLib::checkNocall(currentBeacon->callsign)) {
        static uint32_t lastNocallWarn = 0;
        if (millis() - lastNocallWarn > 30000) {
            ESP_LOGE(TAG, "NOCALL detected — set callsign in %s",
                     "/data/LoRa_Tracker/tracker_conf.json");
            lastNocallWarn = millis();
        }
        usleep(2000000);
        return;
    }

    miceActive = APRSPacketLib::validateMicE(currentBeacon->micE);

    SMARTBEACON_Utils::checkSettings(currentBeacon->smartBeaconSetting);
    SMARTBEACON_Utils::checkState();

    APRS_IS_Utils::checkConnection();

    LoRa_Utils::processPendingChanges();
#ifdef USE_LVGL_UI
    UIDashboard::refreshLoRaInfo();
#endif

    // ── RX ──────────────────────────────────────────────────────────────────
    ReceivedLoRaPacket packet = LoRa_Utils::receivePacket();

    if (!packet.text.isEmpty()) {
        String rawFrame = packet.text.substring(3);

        // Path detection and digi stats
        int pathStart = rawFrame.indexOf('>');
        int pathEnd   = rawFrame.indexOf(':');
        bool isDirect = true;
        if (pathStart >= 0 && pathEnd > pathStart) {
            String path = rawFrame.substring(pathStart+1, pathEnd);
            if (path.indexOf('*') >= 0) isDirect = false;
            STORAGE_Utils::updateDigiStats(path);
        }

        STORAGE_Utils::logRawFrame(rawFrame, packet.rssi, packet.snr, isDirect);
        STORAGE_Utils::updateRxStats(packet.rssi, packet.snr);

        if (pathStart > 0) {
            String sender = rawFrame.substring(0, pathStart);
            STORAGE_Utils::updateStationStats(sender, packet.rssi, packet.snr, isDirect);
        }

        // Digipeater
        if (Config.lora.repeaterMode) {
            String digipkt = APRSPacketLib::generateDigipeatedPacket(
                packet.text, currentBeacon->callsign, Config.path);
            if (digipkt != "X") {
                usleep((rand() % 400 + 100) * 1000);
                LoRa_Utils::sendNewPacket(digipkt);
            }
        }

        // Forward raw frame to stdout for dashboard pipe
        printf("RX RSSI:%d SNR:%.1f %s\n", packet.rssi, packet.snr, rawFrame.c_str());
        fflush(stdout);

#ifdef USE_LVGL_UI
        {
            char rxLine[512];
            snprintf(rxLine, sizeof(rxLine), "RSSI:%d SNR:%.1f %s",
                     packet.rssi, packet.snr, rawFrame.c_str());
            UIDashboard::addRxLine(rxLine);
            UIMessaging::refreshFramesList();
        }
#endif
    }

    MSG_Utils::checkReceivedMessage(packet);
    MSG_Utils::processOutputBuffer();
    MSG_Utils::clean15SegBuffer();

    STATION_Utils::checkListenedStationsByTimeAndDelete();
    STATION_Utils::cleanOldMapStations();
#ifdef USE_LVGL_UI
    MapRaster::refreshStations();
#endif

    // ── GPS / SmartBeacon ────────────────────────────────────────────────────
    lastTx = millis() - lastTxTime;

    if (gpsIsActive) {
        GPS_Utils::getData();
        bool gps_loc_update  = GPS_Utils::hasNewFix() && gpsFix.valid_location;
        bool gps_time_update = GPS_Utils::hasNewFix() && gpsFix.valid_time;
        GPS_Utils::setDateFromData();

        int currentSpeed = gpsFix.valid_speed ? (int)gpsFix.speed_kph : 0;

        if (!sendUpdate && gps_loc_update && smartBeaconActive) {
            GPS_Utils::calculateDistanceTraveled();
            if (!sendUpdate) GPS_Utils::calculateHeadingDelta(currentSpeed);
            STATION_Utils::checkStandingUpdateTime();
        }
        SMARTBEACON_Utils::checkFixedBeaconTime();

        // GPS quality gate (mirrors ESP32 logic)
        bool gpsQualityOk = false;
        if (Config.gpsConfig.strict3DFix)
            gpsQualityOk = (gpsFix.satellites >= 6) && (gpsPdop() <= 5.0f);
        else
            gpsQualityOk = (gpsFix.satellites >= 6) && (gpsHdop() <= 5.0f);

        if (sendUpdate && gps_loc_update && gpsQualityOk) {
            STATION_Utils::sendBeacon();
        }

        if (gps_time_update) SMARTBEACON_Utils::checkInterval(currentSpeed);

        // Periodic GPS status to stdout — skip si pas de fix (évite le spam 0,0,0,...)
        if (millis() - refreshDisplayTime >= 1000) {
            if (gpsFix.lat != 0.0 || gpsFix.lon != 0.0) {
                printf("GPS:%.5f,%.5f,%.0f,%.1f,%.1f,%d,%02d%02d%02d\n",
                       gpsFix.lat, gpsFix.lon, gpsFix.alt, gpsFix.speed_kph,
                       gpsFix.hdop, gpsFix.satellites,
                       gpsFix.hours, gpsFix.minutes, gpsFix.seconds);
                fflush(stdout);
#ifdef USE_LVGL_UI
                UIDashboard::updateGPS(gpsFix.lat, gpsFix.lon, gpsFix.alt,
                                       gpsFix.speed_kph, gpsFix.satellites, gpsFix.hdop);
                UIDashboard::updateCallsign(currentBeacon->callsign.c_str());
#endif
            }
#ifdef USE_LVGL_UI
            // Heure locale : toujours affichée (système ou GPS)
            {
                time_t now = time(nullptr);
                struct tm* lt = localtime(&now);
                UIDashboard::updateTime(lt->tm_mday, lt->tm_mon + 1, lt->tm_year + 1900,
                                        lt->tm_hour, lt->tm_min, lt->tm_sec);
                // Heure UTC : seulement si sync GPS
                if (gpsFix.valid_time) {
                    UIDashboard::updateUtcTime(gpsFix.hours, gpsFix.minutes, gpsFix.seconds);
                }
            }
#endif
            refreshDisplayTime = millis();
        }
    } else {
        if (lastTx > txInterval) STATION_Utils::checkStandingUpdateTime();
        if (millis() - refreshDisplayTime >= 1000) {
#ifdef USE_LVGL_UI
            time_t now = time(nullptr);
            struct tm* lt = localtime(&now);
            UIDashboard::updateTime(lt->tm_mday, lt->tm_mon + 1, lt->tm_year + 1900,
                                    lt->tm_hour, lt->tm_min, lt->tm_sec);
#endif
            refreshDisplayTime = millis();
        }
    }

    STORAGE_Utils::checkStatsSave();

#ifdef USE_LVGL_UI
    {
        uint32_t d = lv_timer_handler();
        if (d == LV_NO_TIMER_READY) d = 5;
        usleep(d * 1000);
    }
#else
    usleep(10000);  // 10 ms
#endif
}

// MapState global members (needed by ui_settings / map_raster)
lv_obj_t* MapState::screen_map = nullptr;
bool MapState::blePausedForMap = false;

// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    // Override callsign from argv[1] if provided
    // (will be overwritten by Config.init() unless passed before)
    (void)argc; (void)argv;

    setup();

    // Post-setup: allow argv override of callsign
    if (argc > 1) {
        if (!Config.beacons.empty()) Config.beacons[0].callsign = argv[1];
        currentBeacon = &Config.beacons[myBeaconsIndex];
    }

    ESP_LOGI(TAG, "Loop started");
    printf("CFG:%s,%.3f,%s\n",
           currentBeacon->callsign.c_str(),
           (float)currentLoRaType->frequency / 1000000.0f,
           Config.path.c_str());
    fflush(stdout);
    while (running) loop();

    NOTIFICATION_Utils::shutDownBeep();
    LoRa_Utils::sleepRadio();
    APRS_IS_Utils::disconnect();
    WEBCONF::stop();
    ESP_LOGI(TAG, "Done.");
    return 0;
}
