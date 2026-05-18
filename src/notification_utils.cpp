#include "notification_utils.h"
#include "configuration.h"
#include <cstdio>

extern Configuration Config;

namespace NOTIFICATION_Utils {

void playTone(int freq, uint8_t duration_ms) {
    if (!Config.notification.buzzerActive) return;
    printf("BEEP:TONE:%d:%d\n", freq, (int)duration_ms); fflush(stdout);
}

void start() {
    if (!Config.notification.buzzerActive || !Config.notification.bootUpBeep) return;
    printf("BEEP:BOOT\n"); fflush(stdout);
}

void beaconTxBeep() {
    if (!Config.notification.buzzerActive || !Config.notification.txBeep) return;
    printf("BEEP:TX\n"); fflush(stdout);
}

void messageBeep() {
    if (!Config.notification.buzzerActive || !Config.notification.messageRxBeep) return;
    printf("BEEP:MSG\n"); fflush(stdout);
}

void stationHeardBeep() {
    if (!Config.notification.buzzerActive || !Config.notification.stationBeep) return;
    printf("BEEP:STATION\n"); fflush(stdout);
}

void shutDownBeep() {
    if (!Config.notification.buzzerActive || !Config.notification.shutDownBeep) return;
    printf("BEEP:SHUTDOWN\n"); fflush(stdout);
}

void lowBatteryBeep() {
    if (!Config.notification.buzzerActive || !Config.notification.lowBatteryBeep) return;
    printf("BEEP:LOWBAT\n"); fflush(stdout);
}

} // namespace NOTIFICATION_Utils
