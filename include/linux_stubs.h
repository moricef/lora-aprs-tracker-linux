#pragma once
// Stubs for ESP32-only subsystems not available on Linux.
#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>

// display.h stub
inline void displayShow(const char*, const char*, const char*, int) {}
inline void displayShow(const String&, const String&, const String&, int) {}
inline void displayShow(const String&, const String&, const String&, const String&, const String&, const String&, int) {}

// battery_utils.h stub
namespace BATTERY_Utils {
    inline String getBatteryInfoVoltage() { return "0.0"; }
    inline String getBatteryInfoCurrent() { return "0"; }
    inline int getPercentVoltageBattery(float) { return 0; }
    inline void monitor()          {}
    inline void initBatteryGauge() {}
}

// power_utils.h stub
namespace POWER_Utils {
    inline void setup()              {}
    inline void externalPinSetup()   {}
    inline void lowerCpuFrequency()  {}
    inline void shutdown()           { exit(0); }
}

// ble_utils.h stub
namespace BLE_Utils {
    inline void sendToPhone(const String&) {}
    inline void sendToLoRa()               {}
    inline void tryReadDeviceName()        {}
    inline void checkEcoMode()             {}
}

// bluetooth_utils.h stub (BT Classic)
namespace BLUETOOTH_Utils {
    inline void sendToPhone(const String&) {}
    inline void sendToLoRa()               {}
}

// wifi_utils.h stub
namespace WIFI_Utils {
    inline void setup()         {}
    inline void checkWiFi()     {}
    inline bool isConnected()   { return false; }
    inline bool needsWebConfig(){ return false; }
}

// telemetry_utils.h stub
namespace TELEMETRY_Utils {
    inline void sendEquationsUnitsParameters() {}
    inline String generateEncodedTelemetry()   { return ""; }
}

// gpx_writer.h stub
namespace GPXWriter {
    inline void addPoint(double, double, int, float, float) {}
}

// notification_utils.h stub
namespace NOTIFICATION_Utils {
    inline void beaconTxBeep() {}
}

// sleep_utils.h stub
namespace SLEEP_Utils {
    inline void checkIfGPSShouldSleep() {}
    inline void gpsWakeUp()             {}
}

// winlink stubs
extern bool winlinkCommentState;
extern uint8_t winlinkStatus;

// wx stubs
extern int  wxModuleType;
extern bool wxModuleFound;

// disableGPS flag
extern bool disableGPS;
extern bool gpsShouldSleep;
extern bool SleepModeActive;
