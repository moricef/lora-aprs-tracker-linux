#include "linux_connectivity.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>

namespace LinuxConnectivity {
namespace {

std::string commandOutput(const char *command) {
    std::array<char, 256> buffer{};
    std::string output;
    FILE *pipe = popen(command, "r");
    if (!pipe) return output;
    while (fgets(buffer.data(), buffer.size(), pipe)) output += buffer.data();
    pclose(pipe);
    while (!output.empty() &&
           (output.back() == '\n' || output.back() == '\r' || output.back() == ' '))
        output.pop_back();
    return output;
}

bool commandOk(const char *command) {
    int status = std::system(command);
    return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

WifiStatus getWifiStatus() {
    WifiStatus status;
    status.enabled = commandOutput(
        "nmcli -g WIFI general 2>/dev/null") == "enabled";

    const std::string state = commandOutput(
        "nmcli -g GENERAL.STATE device show wlan0 2>/dev/null");
    status.connected = status.enabled && state.rfind("100", 0) == 0;
    if (!status.connected) return status;

    status.ip = commandOutput(
        "nmcli -g IP4.ADDRESS device show wlan0 2>/dev/null | head -n1");
    const std::string signal = commandOutput(
        "nmcli -t -f IN-USE,SIGNAL device wifi list ifname wlan0 2>/dev/null | "
        "sed -n 's/^\\*://p' | head -n1");
    if (!signal.empty()) {
        // NetworkManager reports quality (0..100), while the UI historically
        // displayed dBm. This standard approximation is sufficient for status.
        const int quality = std::atoi(signal.c_str());
        status.rssi = quality / 2 - 100;
    }
    return status;
}

bool setWifiEnabled(bool enabled) {
    return commandOk(enabled
        ? "sudo -n nmcli radio wifi on >/dev/null 2>&1"
        : "sudo -n nmcli radio wifi off >/dev/null 2>&1");
}

BluetoothStatus getBluetoothStatus() {
    BluetoothStatus status;
    const std::string controller = commandOutput("bluetoothctl show 2>/dev/null");
    status.powered = controller.find("Powered: yes") != std::string::npos;

    const std::string device = commandOutput(
        "bluetoothctl devices Connected 2>/dev/null | head -n1");
    if (device.rfind("Device ", 0) == 0 && device.size() > 24) {
        status.connected = true;
        status.deviceAddress = device.substr(7, 17);
        status.deviceName = device.substr(24);
    }
    return status;
}

bool setBluetoothEnabled(bool enabled) {
    if (enabled) {
        // The Pi firmware may expose the controller soft-blocked at boot.
        commandOk("sudo -n /usr/sbin/rfkill unblock bluetooth >/dev/null 2>&1");
        // BlueZ occasionally returns org.bluez.Error.Busy while the requested
        // transition still completes. Judge success from the resulting state.
        return commandOk("bluetoothctl power on >/dev/null 2>&1 || true; "
                         "sleep 0.2; bluetoothctl show 2>/dev/null | "
                         "grep -q 'Powered: yes'");
    }
    return commandOk("bluetoothctl power off >/dev/null 2>&1 || true; "
                     "sleep 0.2; bluetoothctl show 2>/dev/null | "
                     "grep -q 'Powered: no'");
}

} // namespace LinuxConnectivity
