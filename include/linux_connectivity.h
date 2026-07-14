#pragma once

#include <string>

namespace LinuxConnectivity {

struct WifiStatus {
    bool enabled = false;
    bool connected = false;
    std::string ip;
    int rssi = -100;
};

struct BluetoothStatus {
    bool powered = false;
    bool connected = false;
    std::string deviceName;
    std::string deviceAddress;
};

WifiStatus getWifiStatus();
bool setWifiEnabled(bool enabled);

BluetoothStatus getBluetoothStatus();
bool setBluetoothEnabled(bool enabled);

} // namespace LinuxConnectivity
