#pragma once
#include <stdint.h>

namespace NOTIFICATION_Utils {
    void start();
    void playTone(int freq, uint8_t duration_ms);
    void beaconTxBeep();
    void messageBeep();
    void stationHeardBeep();
    void shutDownBeep();
    void lowBatteryBeep();
}
