#include "esp_log.h"
#include <gps.h>
#include <sys/time.h>
#include <pthread.h>
#include <math.h>
#include "gps_utils.h"
#include "station_utils.h"
#include "smartbeacon_utils.h"
#include "configuration.h"
#include "gps_math.h"

static const char* TAG = "GPS";

GpsFixLinux gpsFix = {};
bool gpsIsActive   = true;

extern Configuration Config;
extern Beacon*       currentBeacon;
extern bool          sendUpdate;
extern bool          sendStandingUpdate;
extern uint32_t      lastTxTime;
extern uint32_t      txInterval;
extern double        lastTxLat;
extern double        lastTxLng;
extern double        lastTxDistance;
extern uint32_t      lastTx;
extern SmartBeaconValues currentSmartBeaconValues;
extern bool          disableGPS;
extern bool          gpsShouldSleep;

double currentHeading  = 0;
double previousHeading = 0;
float  bearing         = 0;

// ─── gpsd thread ─────────────────────────────────────────────────────────────
static volatile bool  _gpsRunning  = false;
static pthread_t      _gpsTid;
static pthread_mutex_t _gpsMutex   = PTHREAD_MUTEX_INITIALIZER;
static bool           _newFix      = false;

static void* gpsThread(void*) {
    struct gps_data_t gpsData;
    if (gps_open("localhost", "2947", &gpsData) != 0) {
        ESP_LOGE(TAG, "gpsd not running");
        return nullptr;
    }
    gps_stream(&gpsData, WATCH_ENABLE | WATCH_JSON, nullptr);
    ESP_LOGI(TAG, "Connected to gpsd");

    while (_gpsRunning) {
        if (!gps_waiting(&gpsData, 500000)) continue;
        if (gps_read(&gpsData, nullptr, 0) == -1) {
            ESP_LOGE(TAG, "gpsd connection lost");
            break;
        }
        if (gpsData.fix.mode >= MODE_2D) {
            pthread_mutex_lock(&_gpsMutex);
            gpsFix.mode            = gpsData.fix.mode;
            gpsFix.lat             = gpsData.fix.latitude;
            gpsFix.lon             = gpsData.fix.longitude;
            gpsFix.valid_location  = true;
            if (!isnan(gpsData.fix.altitude)) {
                gpsFix.alt             = gpsData.fix.altitude;
                gpsFix.valid_altitude  = true;
            }
            if (!isnan(gpsData.fix.speed)) {
                gpsFix.speed_kph      = gpsData.fix.speed * 3.6;
                gpsFix.valid_speed    = true;
            }
            if (!isnan(gpsData.fix.track)) {
                gpsFix.heading        = (float)gpsData.fix.track;
                gpsFix.valid_heading  = true;
            }
            if (!isnan(gpsData.fix.epx) && !isnan(gpsData.fix.epy))
                gpsFix.hdop = sqrt(gpsData.fix.epx*gpsData.fix.epx + gpsData.fix.epy*gpsData.fix.epy);
            gpsFix.satellites = gpsData.satellites_used;

            // time
            struct timeval tv;
            gettimeofday(&tv, nullptr);
            struct tm* t = gmtime(&tv.tv_sec);
            gpsFix.hours   = t->tm_hour;
            gpsFix.minutes = t->tm_min;
            gpsFix.seconds = t->tm_sec;
            gpsFix.date    = t->tm_mday;
            gpsFix.month   = t->tm_mon + 1;
            gpsFix.year    = 1900 + t->tm_year;
            gpsFix.valid_time = gpsFix.valid_date = true;

            _newFix = true;
            pthread_mutex_unlock(&_gpsMutex);
        } else {
            pthread_mutex_lock(&_gpsMutex);
            gpsFix.mode           = 0;
            gpsFix.valid_location = false;
            if (gpsData.satellites_used > 0)
                gpsFix.satellites = gpsData.satellites_used;
            pthread_mutex_unlock(&_gpsMutex);
        }
    }
    gps_stream(&gpsData, WATCH_DISABLE, nullptr);
    gps_close(&gpsData);
    return nullptr;
}
// ─────────────────────────────────────────────────────────────────────────────

namespace GPS_Utils {

    void setup() {
        if (disableGPS) { ESP_LOGW(TAG, "GPS disabled"); return; }
        _gpsRunning = true;
        pthread_create(&_gpsTid, nullptr, gpsThread, nullptr);
    }

    static bool _fixConsumed = true;

    void getData() {
        if (disableGPS) return;
        pthread_mutex_lock(&_gpsMutex);
        _fixConsumed = !_newFix;
        _newFix = false;
        pthread_mutex_unlock(&_gpsMutex);
    }

    bool hasNewFix() { return !_fixConsumed; }

    void setDateFromData() {
        if (!gpsFix.valid_time || !gpsFix.valid_date) return;
        struct tm t = {};
        t.tm_year  = gpsFix.year  - 1900;
        t.tm_mon   = gpsFix.month - 1;
        t.tm_mday  = gpsFix.date;
        t.tm_hour  = gpsFix.hours;
        t.tm_min   = gpsFix.minutes;
        t.tm_sec   = gpsFix.seconds;
        struct timeval tv = { mktime(&t), 0 };
        settimeofday(&tv, nullptr);
    }

    void calculateDistanceCourse(const String& callsign, double checkLat, double checkLon) {
        double distKm = calcDist((float)gpsFix.lat, (float)gpsFix.lon, (float)checkLat, (float)checkLon) / 1000.0;
        double course = calcCourse((float)gpsFix.lat, (float)gpsFix.lon, (float)checkLat, (float)checkLon);
        STATION_Utils::deleteListenedStationsByTime();
        STATION_Utils::orderListenedStationsByDistance(callsign, (float)distKm, (float)course);
    }

    void calculateDistanceTraveled() {
        static uint32_t lastCalcMs = 0;
        uint32_t now = millis();
        if (now - lastCalcMs < 500) return;
        lastCalcMs = now;

        currentHeading = gpsFix.valid_heading ? gpsFix.heading : 0.0;

        double rawDistance = calcDist((float)gpsFix.lat, (float)gpsFix.lon, (float)lastTxLat, (float)lastTxLng);
        float  speedKmph   = gpsFix.valid_speed ? (float)gpsFix.speed_kph : 0.0f;

        if (speedKmph < 5.0 && rawDistance > 50.0 && lastTx < (uint32_t)Config.standingUpdateTime * 60 * 1000) {
            static uint32_t lastJitterLog = 0;
            if (now - lastJitterLog >= 30000) {
                lastJitterLog = now;
                ESP_LOGD(TAG, "Suppressed GPS jitter: speed %.1f km/h, jump %.1f m", speedKmph, rawDistance);
            }
            lastTxDistance = 0.0;
        } else {
            lastTxDistance = rawDistance;
        }

        if (lastTx >= txInterval) {
            if (lastTxDistance > currentSmartBeaconValues.minTxDist) {
                sendUpdate = true;
                sendStandingUpdate = false;
            }
        }
    }

    void calculateHeadingDelta(int speed) {
        uint8_t TurnMinAngle;
        double headingDelta = fabs(previousHeading - currentHeading);
        if (lastTx > (uint32_t)currentSmartBeaconValues.minDeltaBeacon * 1000) {
            if (speed == 0)
                TurnMinAngle = currentSmartBeaconValues.turnMinDeg + (currentSmartBeaconValues.turnSlope / (speed + 1));
            else
                TurnMinAngle = currentSmartBeaconValues.turnMinDeg + (currentSmartBeaconValues.turnSlope / speed);

            if (headingDelta > TurnMinAngle && lastTxDistance > currentSmartBeaconValues.minTxDist) {
                sendUpdate = true;
                sendStandingUpdate = false;
            }
        }
    }

    void checkStartUpFrames() { /* gpsd handles startup */ }

    String getCardinalDirection(float course) {
        if (gpsFix.valid_speed && gpsFix.speed_kph > 0.5) bearing = course;

        if (bearing >= 354.375 || bearing < 5.625)   return ">.NW.....(N).....NE.<";
        if (bearing >= 5.675   && bearing < 16.875)  return ">.......N.|.....NE..<";
        if (bearing >= 16.875  && bearing < 28.125)  return ">.....N...|...NE....<";
        if (bearing >= 28.125  && bearing < 39.375)  return ">...N.....|.NE......<";
        if (bearing >= 39.375  && bearing < 50.625)  return ">.N......(NE).....E.<";
        if (bearing >= 50.625  && bearing < 61.875)  return ">.......NE|.....E...<";
        if (bearing >= 61.875  && bearing < 73.125)  return ">.....NE..|...E.....<";
        if (bearing >= 73.125  && bearing < 84.375)  return ">...NE....|.E.......<";
        if (bearing >= 84.375  && bearing < 95.625)  return ">.NE.....(E).....SE.<";
        if (bearing >= 95.625  && bearing < 106.875) return ">.......E.|.....SE..<";
        if (bearing >= 106.875 && bearing < 118.125) return ">.....E...|...SE....<";
        if (bearing >= 118.125 && bearing < 129.375) return ">...E.....|.SE......<";
        if (bearing >= 129.375 && bearing < 140.625) return ">.E......(SE).....S.<";
        if (bearing >= 140.625 && bearing < 151.875) return ">.......SE|.....S...<";
        if (bearing >= 151.875 && bearing < 163.125) return ">.....SE..|...S.....<";
        if (bearing >= 163.125 && bearing < 174.375) return ">...SE....|.S.......<";
        if (bearing >= 174.375 && bearing < 185.625) return ">.SE.....(S).....SW.<";
        if (bearing >= 185.625 && bearing < 196.875) return ">.......S.|.....SW..<";
        if (bearing >= 196.875 && bearing < 208.125) return ">.....S...|...SW....<";
        if (bearing >= 208.125 && bearing < 219.375) return ">...S.....|.SW......<";
        if (bearing >= 219.375 && bearing < 230.625) return ">.S......(SW).....W.<";
        if (bearing >= 230.625 && bearing < 241.875) return ">.......SW|.....W...<";
        if (bearing >= 241.875 && bearing < 253.125) return ">.....SW..|...W.....<";
        if (bearing >= 253.125 && bearing < 264.375) return ">...SW....|.W.......<";
        if (bearing >= 264.375 && bearing < 275.625) return ">.SW.....(W).....NW.<";
        if (bearing >= 275.625 && bearing < 286.875) return ">.......W.|.....NW..<";
        if (bearing >= 286.875 && bearing < 298.125) return ">.....W...|...NW....<";
        if (bearing >= 298.125 && bearing < 309.375) return ">...W.....|.NW......<";
        if (bearing >= 309.375 && bearing < 320.625) return ">.W......(NW).....N.<";
        if (bearing >= 320.625 && bearing < 331.875) return ">.......NW|.....N...<";
        if (bearing >= 331.875 && bearing < 343.125) return ">.....NW..|...N.....<";
        if (bearing >= 343.125 && bearing < 354.375) return ">...NW....|.N.......<";
        return "";
    }
}
