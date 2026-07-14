#pragma once

#include <string>

namespace BluetoothBLE {
bool start(const std::string &deviceName, bool useKiss);
void stop();
bool isRunning();
bool isConnected();
void sendToClient(const std::string &tnc2Frame);
}
