#pragma once
#include <string>
namespace BLE_Utils {
    inline void stop(){}
    inline void setup(){}
    inline bool isSleeping(){ return false; }
    inline void wake(){}
    inline void checkEcoMode(){}
    inline void sendToLoRa(){}
    inline void sendToPhone(const std::string&){}
    inline void tryReadDeviceName(){}
    inline std::string getConnectedDeviceAddress(){ return ""; }
    inline std::string getConnectedDeviceName(){ return ""; }
}
