#pragma once

#include <Arduino.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

struct AprsPositionInfo {
    char payloadType = 0;
    char symbolTable = 0;
    char symbol = 0;
    bool hasPosition = false;
    double lat = 0.0;
    double lon = 0.0;
};

static inline bool aprsParseUncompressedCoord(const char *latp, const char *lonp,
                                              double &lat, double &lon) {
    if (!latp || !lonp) return false;
    char hemiLat = latp[7];
    char hemiLon = lonp[8];
    if ((hemiLat != 'N' && hemiLat != 'S') || (hemiLon != 'E' && hemiLon != 'W')) return false;

    int latDeg = (latp[0] - '0') * 10 + (latp[1] - '0');
    int lonDeg = (lonp[0] - '0') * 100 + (lonp[1] - '0') * 10 + (lonp[2] - '0');
    double latMin = atof(latp + 2);
    double lonMin = atof(lonp + 3);
    if (latDeg < 0 || latDeg > 90 || lonDeg < 0 || lonDeg > 180 ||
        !isfinite(latMin) || !isfinite(lonMin))
        return false;

    lat = latDeg + latMin / 60.0;
    lon = lonDeg + lonMin / 60.0;
    if (hemiLat == 'S') lat = -lat;
    if (hemiLon == 'W') lon = -lon;
    return true;
}

static inline int aprsBase91Value(const char *p, size_t len) {
    int value = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)p[i];
        if (c < 33 || c > 123) return -1;
        value = value * 91 + (c - 33);
    }
    return value;
}

static inline bool aprsParseCompressedCoord(const char *p, double &lat, double &lon) {
    if (!p) return false;
    int latVal = aprsBase91Value(p, 4);
    int lonVal = aprsBase91Value(p + 4, 4);
    if (latVal < 0 || lonVal < 0) return false;
    lat = 90.0 - ((double)latVal / 380926.0);
    lon = -180.0 + ((double)lonVal / 190463.0);
    return lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

static inline AprsPositionInfo aprsParsePositionInfo(const char *body) {
    AprsPositionInfo info;
    if (!body || !*body) return info;

    info.payloadType = body[0];
    size_t len = strlen(body);
    bool posType = (info.payloadType == '!' || info.payloadType == '=' ||
                    info.payloadType == '/' || info.payloadType == '@');
    if (!posType) {
        if (info.payloadType == '_') info.symbol = '_';
        return info;
    }

    size_t offset = (info.payloadType == '/' || info.payloadType == '@') ? 8 : 1;
    if (len <= offset + 9) return info;

    if ((body[offset] == '/' || body[offset] == '\\') && len > offset + 9) {
        info.symbolTable = body[offset];
        info.symbol = body[offset + 9];
        info.hasPosition = aprsParseCompressedCoord(body + offset + 1, info.lat, info.lon);
        return info;
    }

    if (len > offset + 18) {
        info.symbolTable = body[offset + 8];
        info.symbol = body[offset + 18];
        char latBuf[9] = {};
        char lonBuf[10] = {};
        memcpy(latBuf, body + offset, 8);
        memcpy(lonBuf, body + offset + 9, 9);
        info.hasPosition = aprsParseUncompressedCoord(latBuf, lonBuf, info.lat, info.lon);
    }
    return info;
}
