#include "esp_log.h"
#include "linux_hal.h"
#include "Module.h"
#include "modules/SX126x/SX1262.h"
#include "lora_utils.h"
#include "configuration.h"
#include "storage_utils.h"
#include "notification_utils.h"

static const char* TAG = "LoRa";

// Odroid C2 — HT-RA62 pins on gpiochip
#define PIN_CS   622
#define PIN_DIO1 605
#define PIN_RST  609
#define PIN_BUSY 610

extern Configuration Config;
extern LoraType*     currentLoRaType;
extern uint8_t       loraIndex;
extern int           loraIndexSize;

static LinuxHal* _hal    = nullptr;
static Module*   _mod    = nullptr;
static SX1262*   _radio  = nullptr;

static volatile bool operationDone = true;
static volatile bool transmitFlag  = false;
static bool          loraInitOk    = false;

bool pendingFrequencyChange = false;
int  pendingLoraIndex       = -1;
bool pendingDataRateChange  = false;
int  pendingDataRate        = -1;

namespace LoRa_Utils {

    void setFlag() { operationDone = true; }

    void requestFrequencyChange(int idx) { pendingLoraIndex = idx; pendingFrequencyChange = true; }
    void requestDataRateChange(int rate) { pendingDataRate  = rate; pendingDataRateChange  = true; }

    int calculateDataRate(int sf, int cr, int bw) {
        if (bw == 125000) {
            struct { int sf; int cr; int rate; } p[] = {
                {12,5,300},{12,6,244},{12,7,209},{12,8,183},{10,8,610},{9,7,1200}
            };
            for (auto& x : p) if (x.sf==sf && x.cr==cr) return x.rate;
        }
        if (sf<5||sf>12||cr<5||cr>8||bw<=0) return 0;
        double r = (double)sf*((double)bw/(1<<sf))*(4.0/cr);
        return (int)(r+0.5);
    }

    DataRateConfig getDataRateConfig(int dataRate) {
        const DataRateConfig c[] = {
            {300,12,5,125000},{244,12,6,125000},{209,12,7,125000},
            {183,12,8,125000},{610,10,8,125000},{1200,9,7,125000}
        };
        for (int i = 0; i < 6; i++) if (c[i].dataRate == dataRate) return c[i];
        return c[0];
    }

    int getNextDataRate(int cur) {
        const int rates[] = {300,244,209,183,610,1200};
        for (int i=0;i<6;i++) if (rates[i]==cur) return rates[(i+1)%6];
        return 300;
    }

    void setDataRate(int dataRate) {
        if (!loraInitOk) return;
        DataRateConfig cfg = getDataRateConfig(dataRate);
        Config.loraTypes[loraIndex].dataRate        = cfg.dataRate;
        Config.loraTypes[loraIndex].spreadingFactor = cfg.spreadingFactor;
        Config.loraTypes[loraIndex].codingRate4     = cfg.codingRate4;
        Config.loraTypes[loraIndex].signalBandwidth = cfg.signalBandwidth;
        currentLoRaType = &Config.loraTypes[loraIndex];
        _radio->setSpreadingFactor(cfg.spreadingFactor);
        _radio->setCodingRate(cfg.codingRate4);
        _radio->setBandwidth((float)(cfg.signalBandwidth / 1000));
        ESP_LOGI(TAG, "DataRate → %d bps (SF%d CR4/%d)", dataRate, cfg.spreadingFactor, cfg.codingRate4);
        Config.writeFile();
    }

    void changeDataRate() { setDataRate(getNextDataRate(Config.loraTypes[loraIndex].dataRate)); }

    void applyLoraConfig() {
        if (!loraInitOk) return;
        if (loraIndex < 0 || loraIndex >= loraIndexSize) loraIndex = 0;
        currentLoRaType = &Config.loraTypes[loraIndex];
        float freq = (float)currentLoRaType->frequency / 1000000.0f;
        _radio->setFrequency(freq);
        _radio->setSpreadingFactor(currentLoRaType->spreadingFactor);
        _radio->setBandwidth((float)(currentLoRaType->signalBandwidth / 1000));
        _radio->setCodingRate(currentLoRaType->codingRate4);
        _radio->setOutputPower(currentLoRaType->power + 2);
        ESP_LOGI(TAG, "LoRa → %.3f MHz SF%d BW%ldkHz CR4/%d pwr%d",
                 freq, currentLoRaType->spreadingFactor,
                 currentLoRaType->signalBandwidth/1000,
                 currentLoRaType->codingRate4, currentLoRaType->power);
    }

    void changeFreq() {
        loraIndex = (loraIndex >= (uint8_t)(loraIndexSize - 1)) ? 0 : loraIndex + 1;
        applyLoraConfig();
    }

    void processPendingChanges() {
        if (!loraInitOk) return;
        if (pendingFrequencyChange) {
            pendingFrequencyChange = false;
            if (pendingLoraIndex >= 0 && pendingLoraIndex < loraIndexSize && pendingLoraIndex != (int)loraIndex) {
                loraIndex = (uint8_t)pendingLoraIndex;
                applyLoraConfig();
            }
        }
        if (pendingDataRateChange) {
            pendingDataRateChange = false;
            if (pendingDataRate > 0) setDataRate(pendingDataRate);
        }
    }

    void setup() {
        if (Config.loraTypes.empty()) { ESP_LOGE(TAG, "No LoRa types in config"); return; }
        currentLoRaType = &Config.loraTypes[loraIndex];

        _hal   = new LinuxHal("/dev/spidev0.0", PIN_CS, PIN_BUSY, PIN_DIO1, PIN_RST);
        _mod   = new Module(_hal, PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY);
        _radio = new SX1262(_mod);

        float freq = (float)currentLoRaType->frequency / 1000000.0f;
        int state = _radio->begin(freq, 125.0f, 9, 7,
                                  RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10, 8, 3.3f);
        if (state != RADIOLIB_ERR_NONE) {
            ESP_LOGE(TAG, "radio.begin() failed: %d", state);
            return;
        }

        _radio->setDio1Action(setFlag);
        _radio->setSpreadingFactor(currentLoRaType->spreadingFactor);
        _radio->setBandwidth((float)(currentLoRaType->signalBandwidth / 1000));
        _radio->setCodingRate(currentLoRaType->codingRate4);
        _radio->setCRC(true);
        _radio->setOutputPower(currentLoRaType->power + 2);
        _radio->setRxBoostedGainMode(true);
        _radio->setDio2AsRfSwitch(true);

        state = _radio->startReceive(RADIOLIB_SX126X_RX_TIMEOUT_NONE);
        if (state != RADIOLIB_ERR_NONE) {
            ESP_LOGE(TAG, "startReceive() failed: %d", state);
            return;
        }

        loraInitOk    = true;
        operationDone = false;
        transmitFlag  = false;
        ESP_LOGI(TAG, "LoRa init OK — %.3f MHz SF%d BW%ldkHz",
                 freq, currentLoRaType->spreadingFactor, currentLoRaType->signalBandwidth/1000);
    }

    void sendNewPacket(const String& newPacket) {
        if (!loraInitOk) return;
        ESP_LOGI(TAG, "TX → %s", newPacket.c_str());
        // Prepend LoRa APRS preamble 0x3c 0xff 0x01
        std::string pkt = "\x3c\xff\x01";
        pkt += newPacket.c_str();
        int state = _radio->transmit((uint8_t*)pkt.data(), pkt.size());
        transmitFlag = true;
        if (state == RADIOLIB_ERR_NONE) {
            STORAGE_Utils::updateTxStats();
            NOTIFICATION_Utils::beaconTxBeep();
        } else {
            ESP_LOGE(TAG, "TX failed: %d", state);
        }
        // Re-arm RX
        _radio->startReceive(RADIOLIB_SX126X_RX_TIMEOUT_NONE);
        transmitFlag = false;
    }

    ReceivedLoRaPacket receivePacket() {
        ReceivedLoRaPacket pkt;
        if (!loraInitOk) return pkt;
        if (!operationDone) return pkt;
        operationDone = false;

        if (transmitFlag) {
            _radio->startReceive(RADIOLIB_SX126X_RX_TIMEOUT_NONE);
            transmitFlag = false;
            return pkt;
        }

        // Vérifie un vrai RxDone : sans ça, une interruption redéclenchée
        // sans nouveau paquet fait relire le buffer périmé (trame dupliquée).
        uint32_t irq = _radio->getIrqFlags();
        if (!(irq & RADIOLIB_SX126X_IRQ_RX_DONE)) {
            _radio->startReceive(RADIOLIB_SX126X_RX_TIMEOUT_NONE);
            return pkt;
        }

        // Read received data
        size_t len = _radio->getPacketLength();
        if (len > 0 && len <= 255) {
            uint8_t buf[256];
            int state = _radio->readData(buf, len);
            _radio->startReceive(RADIOLIB_SX126X_RX_TIMEOUT_NONE);
            if (state == RADIOLIB_ERR_NONE && len > 3) {
                // Prepend sync word \x3c\xff\x01 so substring(3) in main loop strips it
                // (same as ESP32 C3 proto bridge, see lora_utils.cpp onLoraRx)
                pkt.text = String("\x3c\xff\x01") + String(std::string((char*)buf, len).c_str());
                pkt.rssi      = (int)_radio->getRSSI();
                pkt.snr       = _radio->getSNR();
                pkt.freqError = (int)_radio->getFrequencyError();
            }
        } else {
            _radio->startReceive(RADIOLIB_SX126X_RX_TIMEOUT_NONE);
        }
        return pkt;
    }

    ReceivedLoRaPacket receiveFromSleep() { return receivePacket(); }

    void wakeRadio() {
        if (!loraInitOk) return;
        _radio->startReceive(RADIOLIB_SX126X_RX_TIMEOUT_NONE);
    }

    void sleepRadio() {
        if (!loraInitOk) return;
        _radio->sleep();
    }
}
