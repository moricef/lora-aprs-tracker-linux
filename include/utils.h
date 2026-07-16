#pragma once
#include <cmath>
#include <cstdio>
#include <cstddef>

namespace Utils {
    // Fréquence LoRa en MHz : 3 décimales si la valeur tombe pile au kHz
    // (ex 433.775), 4 décimales sinon (ex 439.9125). La valeur interne reste
    // en Hz ; seul le format d'affichage varie.
    inline const char* formatFreqMHz(long hz, char *buf, size_t n) {
        if (hz % 1000 == 0) snprintf(buf, n, "%.3f", hz / 1e6);
        else                snprintf(buf, n, "%.4f", hz / 1e6);
        return buf;
    }

    static char _locBuf[11];
    static char _letterize(int x) { return (char)x + 65; }
    inline const char* getMaidenheadLocator(double lat, double lon, int size) {
        double LON_F[] = {20, 2.0, 0.083333, 0.008333, 0.0003472083333333333};
        double LAT_F[] = {10, 1.0, 0.0416665, 0.004166, 0.0001735833333333333};
        lon += 180; lat += 90;
        if (size <= 0 || size > 10) size = 6;
        size /= 2; size *= 2;
        int i;
        for (i = 0; i < size/2; i++) {
            if (i % 2 == 1) {
                _locBuf[i*2]   = (char)(lon / LON_F[i] + '0');
                _locBuf[i*2+1] = (char)(lat / LAT_F[i] + '0');
            } else {
                _locBuf[i*2]   = _letterize((int)(lon / LON_F[i]));
                _locBuf[i*2+1] = _letterize((int)(lat / LAT_F[i]));
            }
            lon = fmod(lon, LON_F[i]);
            lat = fmod(lat, LAT_F[i]);
        }
        _locBuf[i*2] = 0;
        return _locBuf;
    }
}
