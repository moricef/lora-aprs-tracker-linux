#pragma once
#include <string>
namespace WIFI_Utils {
    inline void checkWiFi(){}
    inline void startBlockingWebConfig(){}
    inline void startStationMode(){}
    inline void stop(){}
    inline void setup(){}
    inline bool needsWebConfig(){ return false; }
    inline bool isConnected(){ return false; }
    inline std::string getStatusLine(){ return "WiFi disabled (sim)"; }
    inline bool startAPModeNonBlocking(){ return false; }
    inline std::string getAPName(){ return "LoRaTracker"; }
}
