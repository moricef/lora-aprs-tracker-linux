#pragma once
#include <Arduino.h>
#include <stdint.h>

// GPS fix from gpsd — replaces NeoGPS gps_fix on Linux
struct GpsFixLinux {
    double   lat, lon, alt;   // degrees / metres
    double   speed_kph;       // km/h
    float    heading;         // degrees 0-360
    double   hdop, vdop, pdop;
    int      satellites;
    int      mode;            // 0=no fix, 2=2D, 3=3D
    // validity flags
    bool valid_location;
    bool valid_speed;
    bool valid_altitude;
    bool valid_heading;
    bool valid_time;
    bool valid_date;
    // UTC time
    int hours, minutes, seconds;
    int date, month, year;    // year = full (e.g. 2025)

    double latitude()  const { return lat; }
    double longitude() const { return lon; }
    double speed_kph_val() const { return speed_kph; }
    int    sats()      const { return satellites; }
};

extern GpsFixLinux gpsFix;

inline float gpsSpeedKnots() { return gpsFix.valid_speed   ? (float)(gpsFix.speed_kph / 1.852) : 0.0f; }
inline int32_t gpsAltFeet()  { return gpsFix.valid_altitude ? (int32_t)(gpsFix.alt * 3.28084)  : 0; }
inline float gpsHdop()       { return gpsFix.valid_location ? (float)gpsFix.hdop : 99.0f; }
inline float gpsVdop()       { return (float)gpsFix.vdop; }
inline float gpsPdop()       { return (float)gpsFix.pdop; }

namespace GPS_Utils {
    void   setup();
    void   calculateDistanceCourse(const String& callsign, double checkpointLat, double checkpointLon);
    void   getData();
    bool   hasNewFix();
    void   setDateFromData();
    void   calculateDistanceTraveled();
    void   calculateHeadingDelta(int speed);
    void   checkStartUpFrames();
    String getCardinalDirection(float course);
}
