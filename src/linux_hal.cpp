#include "linux_hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

// ---- GPIO helpers ----

void LinuxHal::gpioExport(uint32_t pin) {
    char buf[16];
    int fd = ::open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) return;
    int n = snprintf(buf, sizeof(buf), "%u", pin);
    (void)!write(fd, buf, n);
    ::close(fd);
    usleep(100000);
}

static void gpioSetDir(uint32_t pin, const char *dir) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/direction", pin);
    int fd = ::open(path, O_WRONLY);
    if (fd < 0) return;
    (void)!write(fd, dir, strlen(dir));
    ::close(fd);
}

static void gpioWrite(uint32_t pin, int val) {
    char path[64]; char c = val ? '1' : '0';
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/value", pin);
    int fd = ::open(path, O_WRONLY);
    if (fd < 0) return;
    (void)!write(fd, &c, 1);
    ::close(fd);
}

static int gpioRead(uint32_t pin) {
    char path[64], buf[2] = {0};
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/value", pin);
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return -1;
    (void)!read(fd, buf, 1);
    ::close(fd);
    return (buf[0] == '1') ? 1 : 0;
}

int LinuxHal::gpioGetFd(uint32_t pin) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/value", pin);
    return ::open(path, O_RDONLY);
}

// ---- Constructor ----

LinuxHal::LinuxHal(const char* spiDev,
                   uint32_t csPin, uint32_t busyPin,
                   uint32_t dio1Pin, uint32_t rstPin)
    : RadioLibHal(0, 1, 0, 1, 2, 3), // GPIO: in=0, out=1, lo=0, hi=1, rising=2, falling=3
      _spiFd(-1), _csPin(csPin), _busyPin(busyPin),
      _dio1Pin(dio1Pin), _rstPin(rstPin),
      _rxDst(nullptr), _rxLen(0), _rxOffset(0),
      _interruptPin(RADIOLIB_NC), _interruptCb(nullptr), _irqRunning(false)
{
    // SPI
    _spiFd = ::open(spiDev, O_RDWR);
    if (_spiFd < 0) {
        fprintf(stderr, "LinuxHal: cannot open %s\n", spiDev);
        return;
    }
    uint8_t mode = 0, bits = 8;
    uint32_t speed = 2000000;
    ioctl(_spiFd, SPI_IOC_WR_MODE, &mode);
    ioctl(_spiFd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
    ioctl(_spiFd, SPI_IOC_WR_BITS_PER_WORD, &bits);

    // GPIOs
    gpioExport(rstPin);
    gpioExport(busyPin);
    gpioExport(dio1Pin);
    gpioSetDir(rstPin, "out");
    gpioSetDir(busyPin, "in");
    gpioSetDir(dio1Pin, "in");

    // RST high
    gpioWrite(rstPin, 1);
    usleep(10000);

    // Init time
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    _startMs = (RadioLibTime_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

LinuxHal::~LinuxHal() {
    detachInterrupt(_interruptPin);
    if (_spiFd >= 0) ::close(_spiFd);
}

// ---- GPIO ----

void LinuxHal::pinMode(uint32_t pin, uint32_t mode) {
    if (pin == RADIOLIB_NC) return;
    gpioSetDir(pin, mode == GpioModeInput ? "in" : "out");
}

void LinuxHal::digitalWrite(uint32_t pin, uint32_t value) {
    if (pin == RADIOLIB_NC) return;
    gpioWrite(pin, value);
}

uint32_t LinuxHal::digitalRead(uint32_t pin) {
    if (pin == RADIOLIB_NC) return 0;
    return (uint32_t)gpioRead(pin);
}

void LinuxHal::attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode) {
    (void)mode; // we only support RISING for now
    if (interruptNum == RADIOLIB_NC) return;
    _interruptPin = interruptNum;
    _interruptCb = interruptCb;

    // Configure edge detection
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/edge", interruptNum);
    int fd = ::open(path, O_WRONLY);
    if (fd >= 0) {
        (void)!write(fd, "rising", 6);
        ::close(fd);
    }

    // Start polling thread
    _irqRunning = true;
    _irqThread = std::thread([this]() {
        int fd = gpioGetFd(_interruptPin);
        if (fd < 0) return;
        while (_irqRunning) {
            char buf[2];
            lseek(fd, 0, SEEK_SET);
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            struct timeval tv = {0, 100000}; // 100ms timeout
            int ret = select(fd + 1, NULL, NULL, &fds, &tv);
            if (ret > 0) {
                (void)!read(fd, buf, 1);
                if (_interruptCb) _interruptCb();
            }
        }
        ::close(fd);
    });
}

void LinuxHal::detachInterrupt(uint32_t interruptNum) {
    if (interruptNum != _interruptPin) return;
    _irqRunning = false;
    if (_irqThread.joinable()) _irqThread.join();
    _interruptPin = RADIOLIB_NC;
    _interruptCb = nullptr;
}

// ---- Timing ----

void LinuxHal::delay(RadioLibTime_t ms) {
    usleep((useconds_t)ms * 1000);
}

void LinuxHal::delayMicroseconds(RadioLibTime_t us) {
    usleep((useconds_t)us);
}

RadioLibTime_t LinuxHal::millis() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    RadioLibTime_t now = (RadioLibTime_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
    return now - _startMs;
}

RadioLibTime_t LinuxHal::micros() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    RadioLibTime_t now = (RadioLibTime_t)(ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL);
    return now - (_startMs * 1000);
}

long LinuxHal::pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) {
    // Timeout in us
    RadioLibTime_t start = micros();
    while (digitalRead(pin) != state) {
        if (micros() - start > timeout) return 0;
    }
    RadioLibTime_t pulseStart = micros();
    while (digitalRead(pin) == state) {
        if (micros() - start > timeout) return 0;
    }
    return (long)(micros() - pulseStart);
}

// ---- SPI ----

void LinuxHal::spiBegin() {}

void LinuxHal::spiEnd() {}

void LinuxHal::spiBeginTransaction() {
    _txBuf.clear();
    _rxDst = nullptr;
    _rxLen = 0;
    _rxOffset = 0;
}

void LinuxHal::spiTransfer(uint8_t* out, size_t len, uint8_t* in) {
    if (out) {
        _txBuf.insert(_txBuf.end(), out, out + len);
    } else {
        _txBuf.insert(_txBuf.end(), len, 0x00); // NOP
    }
    if (in) {
        _rxDst = in;
        _rxLen = len;
        _rxOffset = _txBuf.size() - len;
    }
}

void LinuxHal::spiEndTransaction() {
    if (_txBuf.empty() || _spiFd < 0) return;

    std::vector<uint8_t> rx(_txBuf.size());
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)_txBuf.data(),
        .rx_buf = (unsigned long)rx.data(),
        .len = (uint32_t)_txBuf.size(),
        .speed_hz = 2000000,
        .bits_per_word = 8,
    };
    ioctl(_spiFd, SPI_IOC_MESSAGE(1), &tr);

    // Distribute received data
    if (_rxDst && _rxLen > 0) {
        // SX1262 SPI pipeline: chip needs 1 byte after opcode to process
        // If cmd has params (addr/offset bytes), they serve as pipeline
        // If cmd is just opcode (1 byte), add 1 extra pipeline byte
        size_t srcOff = _rxOffset + (_rxOffset == 1 ? 1 : 0);
        for (size_t i = 0; i < _rxLen; i++) {
            if (srcOff + i < rx.size())
                _rxDst[i] = rx[srcOff + i];
            else
                _rxDst[i] = 0;
        }
    }

    _txBuf.clear();
    _rxDst = nullptr;
}
