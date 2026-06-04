#include "gpx_writer.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <esp_log.h>

static const char *TAG = "GPX";

namespace GPXWriter {

static bool recording = false;
static char currentFilePath[256] = "";

bool isRecording() {
    return recording;
}

bool startRecording(int year, int month, int day, int hour, int minute) {
    if (recording) return true;

    mkdir("/data/LoRa_Tracker/gpx", 0755);

    char filename[256];
    snprintf(filename, sizeof(filename),
             "/data/LoRa_Tracker/gpx/track_%04d-%02d-%02d_%02d%02d.gpx",
             year, month, day, hour, minute);

    FILE *f = fopen(filename, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create: %s", filename);
        return false;
    }

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<gpx version=\"1.1\" creator=\"LoRa_APRS_Tracker\"\n");
    fprintf(f, "  xmlns=\"http://www.topografix.com/GPX/1/1\">\n");
    fprintf(f, "  <trk>\n");
    fprintf(f, "    <name>LoRa APRS Track</name>\n");
    fprintf(f, "    <trkseg>\n");
    fclose(f);

    strncpy(currentFilePath, filename, sizeof(currentFilePath) - 1);
    recording = true;
    ESP_LOGI(TAG, "Recording started: %s", filename);
    return true;
}

void stopRecording() {
    if (!recording) return;

    FILE *f = fopen(currentFilePath, "a");
    if (f) {
        fprintf(f, "    </trkseg>\n");
        fprintf(f, "  </trk>\n");
        fprintf(f, "</gpx>\n");
        fclose(f);
    }

    ESP_LOGI(TAG, "Recording stopped: %s", currentFilePath);
    recording = false;
    currentFilePath[0] = '\0';
}

void addPoint(float lat, float lon, float alt, float hdop, float speed,
              int year, int month, int day, int hour, int minute, int second) {
    if (!recording) return;

    char timestamp[32] = "";
    if (year > 0)
        snprintf(timestamp, sizeof(timestamp),
                 "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 year, month, day, hour, minute, second);

    FILE *f = fopen(currentFilePath, "a");
    if (!f) return;

    fprintf(f, "      <trkpt lat=\"%.6f\" lon=\"%.6f\">\n", lat, lon);
    fprintf(f, "        <ele>%.1f</ele>\n", alt);
    if (timestamp[0])
        fprintf(f, "        <time>%s</time>\n", timestamp);
    fprintf(f, "        <hdop>%.1f</hdop>\n", hdop);
    fprintf(f, "        <speed>%.1f</speed>\n", speed);
    fprintf(f, "      </trkpt>\n");
    fclose(f);
}

} // namespace GPXWriter
