#pragma once
#include <string>
namespace UIPopups {
    void showBeaconPending();
    void showMapLoading();
    void hideMapLoading();
    void closeAll();
    void showTxPacket(const std::string &packet);
    void showRxPacket(const std::string &packet);
    void showCapsLockPopup(bool en);
}
