#include "smartbeacon_utils.h"
#include "configuration.h"

extern Configuration Config;
extern Beacon*       currentBeacon;
extern bool          smartBeaconActive;
extern uint32_t      txInterval;
extern uint32_t      lastTxTime;
extern bool          sendUpdate;
extern uint8_t       winlinkStatus;
extern bool          wxRequestStatus;
extern uint32_t      wxRequestTime;

SmartBeaconValues currentSmartBeaconValues;
byte              smartBeaconSettingsIndex = 10;
bool              wxRequestStatus         = false;
uint32_t          wxRequestTime           = 0;

SmartBeaconValues smartBeaconSettings[3] = {
    {120,  3, 60, 15,  50, 20, 12, 60},   // SLOW  (Runner)
    {120,  5, 60, 40, 100, 12, 12, 60},   // MEDIUM (Bike)
    {120, 10, 60, 70, 100, 12, 10, 80}    // FAST  (Car)
};

namespace SMARTBEACON_Utils {

    int getProfileDefault(byte profileIndex, byte paramIndex) {
        if (profileIndex > 2) profileIndex = 0;
        const SmartBeaconValues& p = smartBeaconSettings[profileIndex];
        switch (paramIndex) {
            case 0: return p.slowRate;
            case 1: return p.fastRate;
            case 2: return p.slowSpeed;
            case 3: return p.fastSpeed;
            case 4: return p.turnMinDeg;
            case 5: return p.turnSlope;
            case 6: return p.minDeltaBeacon;
            default: return 0;
        }
    }

    void checkSettings(byte index) {
        if (smartBeaconSettingsIndex != index) {
            currentSmartBeaconValues = smartBeaconSettings[index];
            smartBeaconSettingsIndex = index;
        }
    }

    void checkInterval(int speed) {
        if (smartBeaconActive) {
            if (speed < currentSmartBeaconValues.slowSpeed) {
                txInterval = currentSmartBeaconValues.slowRate * 1000;
            } else if (speed > currentSmartBeaconValues.fastSpeed) {
                txInterval = currentSmartBeaconValues.fastRate * 1000;
            } else {
                txInterval = std::min(currentSmartBeaconValues.slowRate,
                                 currentSmartBeaconValues.fastSpeed * currentSmartBeaconValues.fastRate / speed) * 1000;
            }
        }
    }

    void checkFixedBeaconTime() {
        if (!smartBeaconActive) {
            uint32_t elapsed = millis() - lastTxTime;
            if (elapsed >= (uint32_t)Config.nonSmartBeaconRate * 60 * 1000)
                sendUpdate = true;
        }
    }

    void checkState() {
        if (wxRequestStatus && (millis() - wxRequestTime) > 20000) wxRequestStatus = false;
        smartBeaconActive = (winlinkStatus == 0 && !wxRequestStatus) ? currentBeacon->smartBeaconActive : false;
    }
}
