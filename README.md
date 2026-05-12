# LoRa APRS Tracker - Linux Port

Portage du firmware ESP32 LoRa APRS Tracker sur Linux (Odroid C2 / aarch64) pour tablette véhicule 7".

## Hardware

- **Odroid C2** (Amlogic S905, Armbian 6.12.44-current-meson64)
- **HT-RA62** (SX1262, 433 MHz, TCXO 32MHz) sur header J2 40-pin
- **Écran** : Waveshare 7" HDMI (1024x600) tactile capacitif — en attente de livraison
- SPI via `spi-gpio` kernel driver (le `spi-meson-spicc` hardware ne probe pas)

## Pinout J2

| HT-RA62 | Signal | J2 Pin | GPIO Linux |
|---------|--------|--------|------------|
| 12 | SCK | 23 | 621 |
| 13 | MISO | 21 | 623 |
| 14 | MOSI | 19 | 626 |
| 15 | NSS | 22 | 622 |
| 6 | DIO1 | 35 | 605 |
| 4 | RST | 36 | 609 |
| 10 | BUSY | 31 | 610 |
| 3 | 3V3 | 1 | - |
| 2,9,16 | GND | 6,9,14... | - |

## Activation SPI sur Odroid C2

L'overlay DT active le `spi-gpio` existant (bit-bang). Fichier source dans `/tmp/odroid_spi_gpio_overlay.dts`.

```bash
sudo apt install device-tree-compiler
scp odroid_spi_gpio_overlay.dts fab2@odroid:/tmp/spi-gpio.dts
ssh fab2@odroid
dtc -@ -I dts -O dtb -o /tmp/spi-gpio.dtbo /tmp/spi-gpio.dts
sudo cp /tmp/spi-gpio.dtbo /boot/overlay-user/
echo "user_overlays=spi-gpio" | sudo tee -a /boot/armbianEnv.txt
sudo reboot
```

## Points découverts

### SetDio3AsTcxoCtrl obligatoire

La commande SX1262 `SetDio3AsTcxoCtrl` (0x97, 3.3V, 5ms) est **obligatoire** même si DIO3 n'est pas câblé. Elle déclenche la calibration interne de l'oscillateur XOSC. Sans elle, SetRx échoue (CommandStatus=1, chip bloqué en STDBY_RC).

Le TCXO du HT-RA62 est alimenté par VDD, DIO3 n'est pas connecté physiquement. La commande 0x97 agit sur le contrôleur XOSC interne, pas sur le pin DIO3.

### Pipeline SPI SX1262

Le SX1262 a un délai de pipeline d'1 byte en SPI stream :
- Commandes sans paramètres (GetIrqStatus, GetDeviceErrors) : données à rx[2]
- Commandes avec paramètres (ReadRegister, ReadBuffer) : données à rx[n+1] où n = nombre de bytes de commande

### GPIO chips

- gpiochip512 : aobus-banks (GPIOAO), 15 pins
- gpiochip527 : periphs-banks, 119 pins (GPIOX/Y/Z/H/DV/CARD/BOOT)

## Build

### lora_rx (test LoRa TX/RX)

```bash
# Sur l'Odroid C2
cd /home/fab2/Developpement/LoRa_APRS/linux_tracker
make
sudo ./lora_rx [callsign]
# SIGUSR1 pour beacon
```

Nécessite `RadioLib/src` dans `lib/RadioLib/` (copié depuis `.pio/libdeps/`).

### dashboard (UI LVGL SDL2)

```bash
# Sur PC dev
cd /home/fab2/Developpement/lv_port_pc_eclipse
make
./demo
```

## Architecture

```
linux_tracker/
├── src/
│   ├── linux_hal.cpp   — RadioLibHal pour Linux (GPIO sysfs, SPI spidev, IRQ thread)
│   └── main.cpp        — TX/RX LoRa APRS (beacon + réception)
├── include/
│   └── linux_hal.h
├── lib/RadioLib/       — RadioLib 7.1.0 (SX1262)
├── Makefile
└── README.md

lv_port_pc_eclipse/     — LVGL v9.5.0 + SDL2 (PC simulator)
```

## Prochaines étapes

- [ ] Intégrer GPS via `/dev/ttyACM0` (NeoGPS)
- [ ] Dashboard LVGL complet (messages, carte, settings)
- [ ] Déploiement framebuffer sur Odroid C2
- [ ] Intégration boîtier avec écran Waveshare 7"
