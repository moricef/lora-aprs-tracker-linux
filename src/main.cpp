#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "linux_hal.h"
#include "Module.h"
#include "modules/SX126x/SX1262.h"

// HT-RA62 pins on Odroid C2
#define PIN_CS   622
#define PIN_DIO1 605
#define PIN_RST  609
#define PIN_BUSY 610

static volatile bool rxFlag = false;
static volatile bool running = true;

static void setFlag(void) {
    rxFlag = true;
}

static void sigHandler(int) {
    running = false;
}

int main() {
    signal(SIGINT, sigHandler);
    printf("=== LoRa APRS RX via RadioLib ===\n");

    // Hal
    LinuxHal hal("/dev/spidev0.0", PIN_CS, PIN_BUSY, PIN_DIO1, PIN_RST);

    // Module
    Module mod(&hal, PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY);

    // SX1262 (Hal is already in Module)
    SX1262 radio(&mod);

    // Same init as ESP32 lora_utils.cpp (TCXO=3.3V for XOSC calib)
    printf("Init SX1262...\n");
    float freq = 433.775f;
    int state = radio.begin(freq, 125.0f, 9, 7,
                            RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10, 8, 3.3f);
    if (state != RADIOLIB_ERR_NONE) {
        fprintf(stderr, "begin() failed: %d\n", state);
        return 1;
    }

    // Configure for APRS (same as ESP32)
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

    printf("RX started on 433.775 MHz (SF12/BW125/CR5)\n");
    printf("Waiting for packets...\n");

    while (running) {
        if (rxFlag) {
            rxFlag = false;

            uint8_t buf[256];
            size_t len = radio.getPacketLength();
            if (len > 0 && len <= 255) {
                state = radio.readData(buf, len);
                if (state == RADIOLIB_ERR_NONE) {
                    printf("RX %zu bytes: ", len);
                    for (size_t i = 0; i < len; i++) printf("%02X ", buf[i]);
                    printf(" | ");
                    for (size_t i = 0; i < len; i++) {
                        char c = buf[i];
                        putchar((c >= 32 && c < 127) ? c : '.');
                    }
                    printf("\n");
                } else {
                    printf("readData error: %d\n", state);
                }
            }

            // Restart RX
            radio.startReceive();
        }
        usleep(10000); // 10ms
    }

    radio.standby();
    printf("\nDone.\n");
    return 0;
}
