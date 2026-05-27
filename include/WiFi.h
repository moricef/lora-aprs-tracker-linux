#pragma once
#include <string>
struct IPAddress {
    std::string toString() const { return "0.0.0.0"; }
};
static IPAddress wifiLocalIP;
class WiFiClass {
public:
    int status() { return 0; }
    IPAddress& localIP() { return wifiLocalIP; }
    int RSSI() { return -100; }
    void disconnect() {}
    void mode(int) {}
    void softAP(const char*) {}
    int getMode() { return 0; }
};
extern WiFiClass WiFi;
