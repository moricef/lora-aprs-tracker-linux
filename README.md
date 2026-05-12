# LoRa APRS Tracker - Linux Port

Portage du firmware ESP32 LoRa APRS Tracker sur Linux (Odroid C2 / aarch64).

## Hardware

- **Odroid C2** (Amlogic S905, Armbian 6.12)
- **HT-RA62** (SX1262, 433 MHz) sur header J2 40-pin
- SPI via `spi-gpio` (bit-bang kernel driver)

## Pinout J2

| HT-RA62 | Signal | J2 Pin | GPIO |
|---------|--------|--------|------|
| 12 | SCK | 23 | 621 |
| 13 | MISO | 21 | 623 |
| 14 | MOSI | 19 | 626 |
| 15 | NSS | 22 | 622 |
| 6 | DIO1 | 35 | 605 |
| 4 | RST | 36 | 609 |
| 10 | BUSY | 31 | 610 |
| 3 | 3V3 | 1 | - |
| 2,9,16 | GND | 6 | - |

## Build

Prérequis : `RadioLib/src` dans `lib/RadioLib/`.

```bash
make
```

## Run

```bash
sudo ./lora_rx
```

## Architecture

- `src/linux_hal.cpp` — implémentation `RadioLibHal` pour Linux (GPIO sysfs, SPI spidev avec buffering, IRQ thread)
- `src/main.cpp` — test RX LoRa APRS 433.775 MHz
- Même API RadioLib que le firmware ESP32 (`radio.begin()`, `setSpreadingFactor()`, etc.)
