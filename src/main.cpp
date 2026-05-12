#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include "linux_hal.h"
#include "Module.h"
#include "modules/SX126x/SX1262.h"

// HT-RA62 pins on Odroid C2
#define PIN_CS   622
#define PIN_DIO1 605
#define PIN_RST  609
#define PIN_BUSY 610

static volatile bool irqFlag  = false;
static volatile bool running  = true;
static volatile bool txDone   = false;

static void setFlag(void) {
    irqFlag = true;
}

static void sigHandler(int sig) {
    if (sig == SIGUSR1) {
        txDone = true; // trigger TX
    } else {
        running = false;
    }
}

// Build APRS position packet
// Format: CALL>APWW11,WIDE2-1:!lat/lon-comment
static size_t buildPositionPacket(char *buf, size_t maxLen,
                                   const char *call, const char *lat,
                                   const char *lon, const char *comment) {
    return snprintf(buf, maxLen, "%s>APWW11,WIDE2-1:!%s/%s-%s",
                    call, lat, lon, comment);
}

static void doTx(SX1262 &radio, const char *call) {
    // Build packet
    char packet[256];
    size_t len = buildPositionPacket(packet, sizeof(packet), call,
        "4259.61N", "00117.22E", "Linux Tracker Odroid C2");
    printf("TX: %s\n", packet);

    int state = radio.startTransmit((uint8_t*)packet, len);
    if (state != RADIOLIB_ERR_NONE) {
        fprintf(stderr, "startTransmit failed: %d\n", state);
        return;
    }
    printf("TX done.\n");
}

int main(int argc, char **argv) {
    const char *call = argc > 1 ? argv[1] : "F6DEV-13";

    signal(SIGINT, sigHandler);
    signal(SIGUSR1, sigHandler);
    printf("=== LoRa APRS Tracker Linux ===\n");
    printf("Call: %s  Freq: 433.775 MHz\n", call);
    printf("SIGUSR1 to beacon, Ctrl+C to quit\n");

    // Hal + Module + Radio
    LinuxHal hal("/dev/spidev0.0", PIN_CS, PIN_BUSY, PIN_DIO1, PIN_RST);
    Module mod(&hal, PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY);
    SX1262 radio(&mod);

    // Init (same as ESP32 lora_utils.cpp)
    float freq = 433.775f;
    int state = radio.begin(freq, 125.0f, 9, 7,
                            RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10, 8, 3.3f);
    if (state != RADIOLIB_ERR_NONE) {
        fprintf(stderr, "begin() failed: %d\n", state);
        return 1;
    }

    // APRS config
    radio.setDio1Action(setFlag);
    radio.setSpreadingFactor(12);
    radio.setBandwidth(125.0f);
    radio.setCodingRate(5);
    radio.setCRC(true);
    radio.setOutputPower(22);

    // Start RX
    state = radio.startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        fprintf(stderr, "startReceive() failed: %d\n", state);
        return 1;
    }
    printf("RX started. Waiting...\n");

    while (running) {
        // TX trigger via SIGUSR1
        if (txDone) {
            txDone = false;
            radio.standby();
            doTx(radio, call);
            // Back to RX
            radio.startReceive();
            printf("Back to RX.\n");
        }

        // RX
        if (irqFlag) {
            irqFlag = false;

            size_t len = radio.getPacketLength();
            if (len > 0 && len <= 255) {
                uint8_t buf[256];
                state = radio.readData(buf, len);
                if (state == RADIOLIB_ERR_NONE) {
                    printf("RX %zu bytes: ", len);
                    for (size_t i = 0; i < len; i++) {
                        char c = buf[i];
                        putchar((c >= 32 && c < 127) ? c : '.');
                    }
                    printf("\n");
                }
            }
            radio.startReceive();
        }
        usleep(10000);
    }

    radio.standby();
    printf("Done.\n");
    return 0;
}
