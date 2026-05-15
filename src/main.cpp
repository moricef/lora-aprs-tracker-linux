// LoRa APRS Tracker — Linux port (Odroid C2 / aarch64)
// Mirrors the ESP32 firmware logic: same modules, same data flow.
#include "esp_log.h"
#include <signal.h>
#include <unistd.h>
#include <sys/select.h>
#include <stdio.h>
#include <stdlib.h>
#include "configuration.h"
#include "lora_utils.h"
#include "gps_utils.h"
#include "smartbeacon_utils.h"
#include "station_utils.h"
#include "storage_utils.h"
#include "msg_utils.h"
#include "aprs_is_utils.h"
#include "linux_stubs.h"
#include <APRSPacketLib.h>

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

extern bool gpsIsActive;       // defined in gps_utils.cpp
extern SmartBeaconValues currentSmartBeaconValues;

static volatile bool running = true;

static void sigHandler(int sig) {
    if (sig == SIGUSR1) {
        // Manual beacon trigger
        sendUpdate = true;
        ESP_LOGI(TAG, "SIGUSR1 → beacon triggered");
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

// ─── setup() ─────────────────────────────────────────────────────────────────
static void setup() {
    signal(SIGINT,  sigHandler);
    signal(SIGTERM, sigHandler);
    signal(SIGUSR1, sigHandler);

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

    GPS_Utils::setup();
    LoRa_Utils::setup();
    APRS_IS_Utils::setup();

    miceActive = APRSPacketLib::validateMicE(currentBeacon->micE);

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

    refreshDisplayTime = millis();
    lastTxTime         = millis();
}

// ─── loop() ──────────────────────────────────────────────────────────────────
static void loop() {
    currentBeacon = &Config.beacons[myBeaconsIndex];

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
    }

    MSG_Utils::checkReceivedMessage(packet);
    MSG_Utils::processOutputBuffer();
    MSG_Utils::clean15SegBuffer();

    STATION_Utils::checkListenedStationsByTimeAndDelete();
    STATION_Utils::cleanOldMapStations();

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

        // Periodic GPS status to stdout
        if (millis() - refreshDisplayTime >= 1000) {
            printf("GPS:%.5f,%.5f,%.0f,%.1f,%.1f,%d,%02d%02d%02d\n",
                   gpsFix.lat, gpsFix.lon, gpsFix.alt, gpsFix.speed_kph,
                   gpsFix.hdop, gpsFix.satellites,
                   gpsFix.hours, gpsFix.minutes, gpsFix.seconds);
            fflush(stdout);
            refreshDisplayTime = millis();
        }
    } else {
        if (lastTx > txInterval) STATION_Utils::checkStandingUpdateTime();
        if (millis() - refreshDisplayTime >= 1000) refreshDisplayTime = millis();
    }

    STORAGE_Utils::checkStatsSave();

    usleep(10000);  // 10 ms
}

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
    while (running) loop();

    LoRa_Utils::sleepRadio();
    APRS_IS_Utils::disconnect();
    ESP_LOGI(TAG, "Done.");
    return 0;
}
