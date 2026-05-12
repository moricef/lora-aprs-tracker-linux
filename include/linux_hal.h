#ifndef LINUX_HAL_H
#define LINUX_HAL_H

#include <stdint.h>
#include <vector>
#include <thread>
#include <atomic>

// RadioLib Hal.h has RadioLibTime_t; we define it before including
#include "TypeDef.h"
#include "Hal.h"

class LinuxHal : public RadioLibHal {
public:
    LinuxHal(const char* spiDev,
             uint32_t csPin, uint32_t busyPin,
             uint32_t dio1Pin, uint32_t rstPin);
    ~LinuxHal();

    // GPIO
    void pinMode(uint32_t pin, uint32_t mode) override;
    void digitalWrite(uint32_t pin, uint32_t value) override;
    uint32_t digitalRead(uint32_t pin) override;
    void attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode) override;
    void detachInterrupt(uint32_t interruptNum) override;

    // Timing
    void delay(RadioLibTime_t ms) override;
    void delayMicroseconds(RadioLibTime_t us) override;
    RadioLibTime_t millis() override;
    RadioLibTime_t micros() override;
    long pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) override;

    // SPI
    void spiBegin() override;
    void spiBeginTransaction() override;
    void spiTransfer(uint8_t* out, size_t len, uint8_t* in) override;
    void spiEndTransaction() override;
    void spiEnd() override;

private:
    int _spiFd;
    uint32_t _csPin, _busyPin, _dio1Pin, _rstPin;

    // SPI transaction buffer
    std::vector<uint8_t> _txBuf;
    uint8_t* _rxDst;
    size_t _rxLen;
    size_t _rxOffset;

    // GPIO cache
    void gpioExport(uint32_t pin);
    int gpioGetFd(uint32_t pin);

    // Interrupt
    int _interruptPin;
    void (*_interruptCb)(void);
    std::thread _irqThread;
    std::atomic<bool> _irqRunning;

    // Time
    RadioLibTime_t _startMs;
};

#endif
