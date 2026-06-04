#pragma once

namespace GPXWriter {
    bool isRecording();
    bool startRecording(int year, int month, int day, int hour, int minute);
    void stopRecording();
    void addPoint(float lat, float lon, float alt, float hdop, float speed,
                  int year, int month, int day, int hour, int minute, int second);
}
