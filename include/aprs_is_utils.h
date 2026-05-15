#pragma once
#include <Arduino.h>

namespace APRS_IS_Utils {
    void setup();
    void connect();
    void upload(const String& packet);
    bool isConnected();
    void checkConnection();
    void disconnect();
}
