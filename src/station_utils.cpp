#include "esp_log.h"
#include <APRSPacketLib.h>
#include <math.h>
#include <algorithm>
#include "station_utils.h"
#include "gps_utils.h"
#include "configuration.h"
#include "lora_utils.h"
#include "aprs_is_utils.h"
#include "storage_utils.h"
#include "smartbeacon_utils.h"
#include "linux_stubs.h"
#ifdef USE_LVGL_UI
#include "ui_popups.h"
#endif

static const char* TAG = "Station";

extern Configuration Config;
extern Beacon*       currentBeacon;
extern uint8_t       myBeaconsIndex;
extern uint8_t       loraIndex;

extern uint32_t lastTx;
extern uint32_t lastTxTime;
extern bool     sendUpdate;
extern double   currentHeading;
extern double   previousHeading;
extern double   lastTxLat;
extern double   lastTxLng;
extern double   lastTxDistance;
extern bool     miceActive;
extern bool     smartBeaconActive;
extern bool     winlinkCommentState;
extern bool     wxModuleFound;
extern bool     gpsShouldSleep;

bool     sendStandingUpdate = false;
uint8_t  updateCounter      = 100;

static uint32_t lastDeleteListenedStation = 0;
static const int nearbyStationsSize = 4;

struct nearStation {
    String   callsign;
    float    distance;
    int      course;
    uint32_t lastTime;
};

static nearStation nearbyStations[nearbyStationsSize];

MapStation mapStations[MAP_STATIONS_MAX];
int        mapStationsCount = 0;

namespace STATION_Utils {

    void nearStationInit() {
        for (int i = 0; i < nearbyStationsSize; i++) {
            nearbyStations[i] = {"", 0.0f, 0, 0};
        }
    }

    void mapStationsInit() {
        for (int i = 0; i < MAP_STATIONS_MAX; i++) {
            mapStations[i] = {};
            mapStations[i].valid = false;
        }
        mapStationsCount = 0;
    }

    float perpendicularDistance(float px, float py, float x1, float y1, float x2, float y2) {
        float dx = x2-x1, dy = y2-y1;
        float lenSq = dx*dx + dy*dy;
        if (lenSq == 0.0f) {
            float ddx = px-x1, ddy = py-y1;
            return sqrtf(ddx*ddx+ddy*ddy);
        }
        float t = ((px-x1)*dx+(py-y1)*dy)/lenSq;
        t = t<0?0:t>1?1:t;
        float ddx = px-(x1+t*dx), ddy = py-(y1+t*dy);
        return sqrtf(ddx*ddx+ddy*ddy);
    }

    void douglasPeuckerSimplify(TracePoint* trace, int start, int end, bool* keep, float epsilon) {
        if (end <= start+1) return;
        float maxDist = 0; int maxIdx = start;
        for (int i = start+1; i < end; i++) {
            float d = perpendicularDistance(trace[i].lat, trace[i].lon,
                                            trace[start].lat, trace[start].lon,
                                            trace[end].lat,   trace[end].lon);
            if (d > maxDist) { maxDist = d; maxIdx = i; }
        }
        if (maxDist > epsilon) {
            keep[maxIdx] = true;
            douglasPeuckerSimplify(trace, start, maxIdx, keep, epsilon);
            douglasPeuckerSimplify(trace, maxIdx, end, keep, epsilon);
        }
    }

    static void simplifyTrace(MapStation* st) {
        if (st->traceCount < TRACE_MAX_POINTS) return;
        TracePoint linear[TRACE_MAX_POINTS];
        for (int i = 0; i < st->traceCount; i++) {
            int idx = (st->traceHead - st->traceCount + i + TRACE_MAX_POINTS) % TRACE_MAX_POINTS;
            linear[i] = st->trace[idx];
        }
        bool keep[TRACE_MAX_POINTS] = {};
        keep[0] = keep[st->traceCount-1] = true;
        douglasPeuckerSimplify(linear, 0, st->traceCount-1, keep, 0.00008f);
        int newCount = 0;
        for (int i = 0; i < st->traceCount; i++)
            if (keep[i]) linear[newCount++] = linear[i];
        st->traceCount = newCount;
        st->traceHead  = newCount % TRACE_MAX_POINTS;
        for (int i = 0; i < newCount; i++) st->trace[i] = linear[i];
    }

    void addMapStation(const String& callsign, float lat, float lon, const String& symbol, const String& overlay, int rssi) {
        if (lat == 0.0f && lon == 0.0f) return;


        // Our own beacon can come back through a digipeater or APRS-IS. It is
        // already represented by the GNSS marker and own trace, so adding the
        // echoed frame to mapStations[] creates a duplicate marker (visible when
        // spiderfied), an incorrect station count and a second trace.
        String stationCall = callsign;
        stationCall.trim();
        if (currentBeacon) {
            String ownCall = currentBeacon->callsign;
            ownCall.trim();
            if (!ownCall.isEmpty() && stationCall == ownCall) return;
        }

        for (int i = 0; i < MAP_STATIONS_MAX; i++) {
            if (mapStations[i].valid && mapStations[i].callsign == callsign) {
                float dlat = fabsf(mapStations[i].latitude - lat);
                float dlon = fabsf(mapStations[i].longitude - lon);
                if (dlat > 0.0001f || dlon > 0.0001f) {
                    if (mapStations[i].traceCount >= TRACE_MAX_POINTS) simplifyTrace(&mapStations[i]);
                    mapStations[i].trace[mapStations[i].traceHead] = {mapStations[i].latitude, mapStations[i].longitude, millis()};
                    mapStations[i].traceHead = (mapStations[i].traceHead+1) % TRACE_MAX_POINTS;
                    if (mapStations[i].traceCount < TRACE_MAX_POINTS) mapStations[i].traceCount++;
                }
                mapStations[i].latitude  = lat;
                mapStations[i].longitude = lon;
                mapStations[i].symbol    = symbol;
                mapStations[i].overlay   = overlay;
                mapStations[i].rssi      = rssi;
                mapStations[i].lastTime  = millis();
                return;
            }
        }
        int oldestIdx = 0; uint32_t oldestTime = UINT32_MAX;
        for (int i = 0; i < MAP_STATIONS_MAX; i++) {
            if (!mapStations[i].valid) {
                mapStations[i] = {callsign, lat, lon, symbol, overlay, rssi, millis(), true};
                mapStations[i].traceCount = mapStations[i].traceHead = 0;
                mapStationsCount++;
                return;
            }
            if (mapStations[i].lastTime < oldestTime) { oldestTime = mapStations[i].lastTime; oldestIdx = i; }
        }
        mapStations[oldestIdx] = {callsign, lat, lon, symbol, overlay, rssi, millis(), true};
        mapStations[oldestIdx].traceCount = mapStations[oldestIdx].traceHead = 0;
    }

    void cleanOldMapStations() {
        uint32_t timeout = Config.rememberStationTime * 60 * 1000;
        for (int i = 0; i < MAP_STATIONS_MAX; i++) {
            if (mapStations[i].valid && (millis() - mapStations[i].lastTime > timeout)) {
                mapStations[i].valid    = false;
                mapStations[i].callsign = "";
                mapStations[i].traceCount = mapStations[i].traceHead = 0;
                mapStationsCount--;
            }
        }
    }

    MapStation* getMapStation(int index) {
        if (index < 0 || index >= MAP_STATIONS_MAX || !mapStations[index].valid) return nullptr;
        return &mapStations[index];
    }

    MapStation* findMapStation(const String& callsign) {
        for (int i = 0; i < MAP_STATIONS_MAX; i++)
            if (mapStations[i].valid && mapStations[i].callsign == callsign) return &mapStations[i];
        return nullptr;
    }

    String getNearStation(uint8_t position) {
        if (nearbyStations[position].callsign.isEmpty()) return "";
        return nearbyStations[position].callsign + "> " +
               String(nearbyStations[position].distance, 2) + "km " +
               String(nearbyStations[position].course);
    }

    void deleteListenedStationsByTime() {
        uint32_t limit = Config.rememberStationTime * 60 * 1000;
        for (int a = 0; a < nearbyStationsSize; a++) {
            if (!nearbyStations[a].callsign.isEmpty() && (millis() - nearbyStations[a].lastTime > limit))
                nearbyStations[a] = {"", 0.0f, 0, 0};
        }
        for (int b = 0; b < nearbyStationsSize-1; b++)
            for (int c = 0; c < nearbyStationsSize-b-1; c++)
                if (nearbyStations[c].callsign.isEmpty()) std::swap(nearbyStations[c], nearbyStations[c+1]);
        lastDeleteListenedStation = millis();
    }

    void checkListenedStationsByTimeAndDelete() {
        if (millis() - lastDeleteListenedStation > (uint32_t)Config.rememberStationTime * 60 * 1000)
            deleteListenedStationsByTime();
    }

    void orderListenedStationsByDistance(const String& callsign, float distance, float course) {
        bool shouldSort = false, found = false;
        for (int a = 0; a < nearbyStationsSize; a++) {
            if (nearbyStations[a].callsign == callsign) {
                found = true;
                nearbyStations[a].lastTime = millis();
                if (nearbyStations[a].distance != distance) { nearbyStations[a].distance = distance; shouldSort = true; }
                break;
            }
        }
        if (!found) {
            for (int b = 0; b < nearbyStationsSize; b++) {
                if (nearbyStations[b].callsign.isEmpty()) {
                    nearbyStations[b] = {callsign, distance, (int)course, millis()};
                    shouldSort = true; break;
                }
            }
            if (!shouldSort) {
                for (int c = 0; c < nearbyStationsSize; c++) {
                    if (nearbyStations[c].distance > distance) {
                        for (int d = nearbyStationsSize-1; d > c; d--) nearbyStations[d] = nearbyStations[d-1];
                        nearbyStations[c] = {callsign, distance, (int)course, millis()};
                        break;
                    }
                }
            }
        }
        if (shouldSort) {
            for (int e = 0; e < nearbyStationsSize-1; e++)
                for (int f = 0; f < nearbyStationsSize-e-1; f++)
                    if (!nearbyStations[f].callsign.isEmpty() && !nearbyStations[f+1].callsign.isEmpty())
                        if (nearbyStations[f].distance > nearbyStations[f+1].distance)
                            std::swap(nearbyStations[f], nearbyStations[f+1]);
        }
    }

    void checkStandingUpdateTime() {
        if (!sendUpdate && lastTx >= (uint32_t)Config.standingUpdateTime * 60 * 1000) {
            sendUpdate = true;
            sendStandingUpdate = true;
        }
    }

    void sendBeacon() {
        String path = Config.path;
        if (gpsSpeedKnots() * 1.852f > 200 || gpsFix.alt > 9000) path = "";

        String packet;
        if (miceActive) {
            packet = APRSPacketLib::generateMiceGPSBeaconPacket(
                currentBeacon->micE, currentBeacon->callsign,
                currentBeacon->symbol, currentBeacon->overlay, path,
                (float)gpsFix.lat, (float)gpsFix.lon,
                gpsFix.heading, gpsSpeedKnots(), (int)gpsFix.alt);
        } else {
            packet = APRSPacketLib::generateBase91GPSBeaconPacket(
                currentBeacon->callsign, "APLRT1", path, currentBeacon->overlay,
                APRSPacketLib::encodeGPSIntoBase91(
                    (float)gpsFix.lat, (float)gpsFix.lon,
                    gpsFix.heading, gpsSpeedKnots(),
                    currentBeacon->symbol, Config.sendAltitude, gpsAltFeet(),
                    sendStandingUpdate));
        }

        // comment
        String comment = (winlinkCommentState ? "winlink" : currentBeacon->comment);
        if (comment.isEmpty()) comment = "LoRa APRS Tracker";
        int sendCommentAfterX = (winlinkCommentState ? 1 : Config.sendCommentAfterXBeacons);

        if (Config.lora.sendInfo) {
            comment += " ";
            comment += String((float)(Config.loraTypes[loraIndex].frequency / 1000000.0), 3);
            comment += "MHz ";
            comment += String(Config.loraTypes[loraIndex].dataRate);
            comment += "bps";
        }

        updateCounter++;
        if (!comment.isEmpty() && updateCounter >= (uint8_t)sendCommentAfterX) {
            packet += comment;
            updateCounter = 0;
        }

        ESP_LOGI(TAG, "TX: %s", packet.c_str());
        printf("TX:%s\n", packet.c_str()); fflush(stdout);
#ifdef USE_LVGL_UI
        UIPopups::showTxPacket(packet.c_str());
#endif
        LoRa_Utils::sendNewPacket(packet);

        if (APRS_IS_Utils::isConnected()) APRS_IS_Utils::upload(packet);

        if (smartBeaconActive) {
            lastTxLat      = gpsFix.lat;
            lastTxLng      = gpsFix.lon;
            previousHeading= currentHeading;
            lastTxDistance = 0.0;
        }
        lastTxTime  = millis();
        sendUpdate  = false;
        if (currentBeacon->gpsEcoMode) gpsShouldSleep = true;

    }

    void saveIndex(uint8_t type, uint8_t index) {
        // On Linux: persist to a small file under /data/LoRa_Tracker/
        char path[64];
        const char* name = nullptr;
        switch (type) {
            case 0: name = "callsignIndex.txt"; break;
            case 1: name = "freqIndex.txt";     break;
            default: return;
        }
        snprintf(path, sizeof(path), "/data/LoRa_Tracker/%s", name);
        FILE* f = fopen(path, "w");
        if (f) { fprintf(f, "%d\n", index); fclose(f); }
    }

    void loadIndex(uint8_t type) {
        char path[64];
        const char* name = nullptr;
        switch (type) {
            case 0: name = "callsignIndex.txt"; break;
            case 1: name = "freqIndex.txt";     break;
            default: return;
        }
        snprintf(path, sizeof(path), "/data/LoRa_Tracker/%s", name);
        FILE* f = fopen(path, "r");
        if (!f) return;
        int idx = 0;
        if (fscanf(f, "%d", &idx) == 1) {
            if (type == 0) myBeaconsIndex = (uint8_t)idx;
            else if (type == 1) loraIndex = (uint8_t)idx;
        }
        fclose(f);
    }
}
