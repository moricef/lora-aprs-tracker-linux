// LoRa APRS Tracker - Linux port
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <math.h>
#include <gps.h>
#include "linux_hal.h"
#include "Module.h"
#include "modules/SX126x/SX1262.h"

// HT-RA62 pins on Odroid C2
#define PIN_CS   622
#define PIN_DIO1 605
#define PIN_RST  609
#define PIN_BUSY 610

static volatile bool irqFlag  = false;
static volatile bool running  = true;
static volatile bool txDone   = false;

static void setFlag(void) { irqFlag = true; }
static void sigHandler(int sig) {
    if (sig == SIGUSR1) txDone = true;
    else running = false;
}

// =============================================================================
// GPS state
// =============================================================================
static volatile double gpsLat = 0, gpsLon = 0, gpsAlt = 0, gpsSpd = 0;
static volatile double gpsHdop = 99;
static volatile int gpsSats = 0, gpsFix = 0, gpsTime = 0;

// Maidenhead locator
static char locatorBuf[11];
static char letterize(int x) { return (char)x + 65; }

static const char *maidenhead(double lat, double lon, int size) {
    double LON_F[] = {20, 2.0, 0.083333, 0.008333, 0.0003472083333333333};
    double LAT_F[] = {10, 1.0, 0.0416665, 0.004166, 0.0001735833333333333};
    lon += 180; lat += 90;
    if (size <= 0 || size > 10) size = 6;
    size /= 2; size *= 2;
    int i;
    for (i = 0; i < size/2; i++) {
        if (i % 2 == 1) {
            locatorBuf[i*2]   = (char)(lon / LON_F[i] + '0');
            locatorBuf[i*2+1] = (char)(lat / LAT_F[i] + '0');
        } else {
            locatorBuf[i*2]   = letterize((int)(lon / LON_F[i]));
            locatorBuf[i*2+1] = letterize((int)(lat / LAT_F[i]));
        }
        lon = fmod(lon, LON_F[i]);
        lat = fmod(lat, LAT_F[i]);
    }
    locatorBuf[i*2] = 0;
    return locatorBuf;
}

// =============================================================================
// APRS TX beacon
// =============================================================================
static void degToDM(double deg, char dir, char *out, size_t n) {
    int d = (int)deg;
    double m = (deg - d) * 60.0;
    snprintf(out, n, "%02d%05.2f%c", d, m, dir);
}

static size_t buildPositionPacket(char *buf, size_t maxLen,
                                   const char *call, const char *lat,
                                   const char *lon, const char *comment) {
    return snprintf(buf, maxLen, "%s>APWW11,WIDE2-1:!%s/%s-%s",
                    call, lat, lon, comment);
}

static char gpsComment[64] = "Linux Tracker Odroid C2";

static void doTx(SX1262 &radio, const char *call) {
    char packet[256], latStr[16], lonStr[16];
    if (gpsFix > 0 && gpsLat != 0) {
        degToDM(gpsLat, 'N', latStr, sizeof(latStr));
        degToDM(gpsLon, 'E', lonStr, sizeof(lonStr));
    } else {
        snprintf(latStr, sizeof(latStr), "4259.61N");
        snprintf(lonStr, sizeof(lonStr), "00117.22E");
    }
    size_t len = buildPositionPacket(packet, sizeof(packet), call,
        latStr, lonStr, gpsComment);
    fprintf(stderr, "TX: %s\n", packet);

    int state = radio.startTransmit((uint8_t*)packet, len);
    if (state != RADIOLIB_ERR_NONE)
        fprintf(stderr, "startTransmit failed: %d\n", state);
    else
        fprintf(stderr, "TX done.\n");
}

// LoRa frame dedup (15-second window)
#define DEDUP_WINDOW_MS 15000
struct DedupEntry { uint32_t hash; uint32_t ms; };
static DedupEntry dedupBuf[16];
static int dedupCount = 0;

static bool isDuplicateFrame(uint8_t *data, size_t len) {
    size_t start = 0;
    while (start < len && (data[start] < 32 || data[start] > 126)) start++;
    if (start >= len) return false;
    uint32_t hash = 0;
    for (size_t i = start; i < len; i++) hash = hash * 31 + data[i];
    uint32_t now = (uint32_t)(time(NULL) * 1000);
    int w = 0;
    for (int i = 0; i < dedupCount; i++)
        if (now - dedupBuf[i].ms < DEDUP_WINDOW_MS) dedupBuf[w++] = dedupBuf[i];
    dedupCount = w;
    for (int i = 0; i < dedupCount; i++)
        if (dedupBuf[i].hash == hash) return true;
    if (dedupCount < 16) {
        dedupBuf[dedupCount].hash = hash;
        dedupBuf[dedupCount].ms = now;
        dedupCount++;
    }
    return false;
}

// SD/eMMC storage
static const char *storageBase = "/data/LoRa_Tracker";
static void storageInit() {
    mkdir(storageBase, 0755);
    char p[256];
    snprintf(p, sizeof(p), "%s/Messages", storageBase); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/Contacts", storageBase); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/Maps", storageBase);     mkdir(p, 0755);
    fprintf(stderr, "Storage: %s\n", storageBase);
}
static void storageLogFrame(const char *frame) {
    char fname[300];
    snprintf(fname, sizeof(fname), "%s/frames.log", storageBase);
    FILE *f = fopen(fname, "a");
    if (f) { fprintf(f, "%ld %s", (long)time(NULL), frame); fclose(f); }
}

// gpsd thread
static void *gpsThread(void *arg) {
    (void)arg;
    struct gps_data_t gps;
    if (gps_open("localhost", "2947", &gps) != 0) {
        fprintf(stderr, "GNSS: gpsd not running\n");
        return NULL;
    }
    gps_stream(&gps, WATCH_ENABLE | WATCH_JSON, NULL);
    fprintf(stderr, "GNSS: connected to gpsd\n");
    while (running) {
        if (!gps_waiting(&gps, 500000)) continue;
        if (gps_read(&gps, NULL, 0) == -1) { fprintf(stderr, "GNSS: gpsd lost\n"); break; }
        if (gps.fix.mode >= MODE_2D) {
            gpsFix = gps.fix.mode; gpsLat = gps.fix.latitude; gpsLon = gps.fix.longitude;
            if (!isnan(gps.fix.altitude))   gpsAlt  = gps.fix.altitude;
            if (!isnan(gps.fix.speed))      gpsSpd  = gps.fix.speed * 3.6;
            if (!isnan(gps.fix.epx) && !isnan(gps.fix.epy))
                gpsHdop = sqrt(gps.fix.epx*gps.fix.epx + gps.fix.epy*gps.fix.epy);
            gpsSats = gps.satellites_used;
            static time_t lastLog = 0;
            static double lastLat = 0, lastLon = 0;
            time_t now = time(NULL);
            if (now - lastLog >= 10 || fabs(gpsLat - lastLat) > 0.00001 || fabs(gpsLon - lastLon) > 0.00001) {
                fprintf(stderr, "GNSS: pos=%.5f,%.5f alt=%.0f loc=%s\n",
                        gpsLat, gpsLon, gpsAlt, maidenhead(gpsLat, gpsLon, 8));
                lastLog = now; lastLat = gpsLat; lastLon = gpsLon;
            }
        } else {
            gpsFix = 0;
            if (gps.satellites_used > 0) gpsSats = gps.satellites_used;
        }
        time_t now = time(NULL); struct tm *t = gmtime(&now);
        gpsTime = t->tm_hour * 10000 + t->tm_min * 100 + t->tm_sec;
    }
    gps_stream(&gps, WATCH_DISABLE, NULL);
    gps_close(&gps);
    return NULL;
}

static void *cmdThread(void *arg) {
    (void)arg;
    char buf[32];
    while (running) {
        fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 200000};
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
            if (fgets(buf, sizeof(buf), stdin)) {
                if (strncmp(buf, "BCN", 3) == 0 || strncmp(buf, "TX", 2) == 0) {
                    txDone = true;
                    fprintf(stderr, "CMD: beacon triggered\n");
                }
            }
        }
    }
    return NULL;
}

static void readConfig(const char *path, char *call, size_t cs, float *freq, char *cmt, size_t cms) {
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq++ = '\0'; char *nl = strchr(eq, '\n'); if (nl) *nl = '\0';
        if (strcmp(line, "callsign") == 0)       strncpy(call, eq, cs);
        else if (strcmp(line, "frequency") == 0)  *freq = atof(eq);
        else if (strcmp(line, "comment") == 0)    strncpy(cmt, eq, cms);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    char call[32] = "F6DEV-13", comment[64] = "Linux Tracker Odroid C2";
    float freq = 433.775f;
    readConfig("/data/LoRa_Tracker/tracker.conf", call, sizeof(call), &freq, comment, sizeof(comment));
    strncpy(gpsComment, comment, sizeof(gpsComment));
    if (argc > 1) strncpy(call, argv[1], sizeof(call));

    signal(SIGINT, sigHandler); signal(SIGUSR1, sigHandler);
    fprintf(stderr, "=== LoRa APRS Tracker Linux ===\nCall: %s  Freq: %.3f MHz\n", call, freq);
    storageInit();

    pthread_t cmdTid, gpsTid;
    pthread_create(&cmdTid, NULL, cmdThread, NULL);
    pthread_create(&gpsTid, NULL, gpsThread, NULL);

    LinuxHal hal("/dev/spidev0.0", PIN_CS, PIN_BUSY, PIN_DIO1, PIN_RST);
    Module mod(&hal, PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY);
    SX1262 radio(&mod);

    int state = radio.begin(freq, 125.0f, 9, 7, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10, 8, 3.3f);
    if (state != RADIOLIB_ERR_NONE) { fprintf(stderr, "begin() failed: %d\n", state); return 1; }
    radio.setDio1Action(setFlag);
    radio.setSpreadingFactor(12); radio.setBandwidth(125.0f); radio.setCodingRate(5);
    radio.setCRC(true); radio.setOutputPower(22);
    state = radio.startReceive();
    if (state != RADIOLIB_ERR_NONE) { fprintf(stderr, "startReceive() failed: %d\n", state); return 1; }
    fprintf(stderr, "RX started.\n");

    while (running) {
        if (txDone) { txDone = false; radio.standby(); doTx(radio, call); radio.startReceive(); }
        if (irqFlag) {
            irqFlag = false;
            size_t len = radio.getPacketLength();
            if (len > 0 && len <= 255) {
                uint8_t buf[256];
                state = radio.readData(buf, len);
                if (state == RADIOLIB_ERR_NONE) {
                    if (isDuplicateFrame(buf, len)) { radio.startReceive(); continue; }
                    char fl[512]; float rssi = radio.getRSSI(), snr = radio.getSNR();
                    int p = snprintf(fl, sizeof(fl), "RSSI:%.0f SNR:%.1f ", rssi, snr);
                    for (size_t i = 0; i < len && p < (int)sizeof(fl)-1; i++) {
                        char c = buf[i]; fl[p++] = (c >= 32 && c < 127) ? c : '.';
                    }
                    fl[p++] = '\n'; fl[p] = '\0'; fputs(fl, stdout); fflush(stdout);
                    storageLogFrame(fl);
                }
            }
            radio.startReceive();
        }
        static uint32_t lastGpsOut = 0; uint32_t now = time(NULL);
        if (now - lastGpsOut >= 1) {
            lastGpsOut = now;
            printf("GPS:%.5f,%.5f,%.0f,%.1f,%.1f,%d,%d\n",
                   gpsLat, gpsLon, gpsAlt, gpsSpd, gpsHdop, gpsSats, gpsTime);
            fflush(stdout);
        }
        usleep(10000);
    }
    radio.standby(); fprintf(stderr, "Done.\n");
    pthread_join(cmdTid, NULL); pthread_join(gpsTid, NULL);
    return 0;
}
