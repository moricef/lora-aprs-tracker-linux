# LoRa APRS Tracker - Port Linux

Port du firmware ESP32 vers Linux (Odroid C2 / aarch64) pour tablette véhicule 7".

## État au 2026-05-13

### Fonctionnel
- **SPI LoRa** : HT-RA62 (SX1262, 433 MHz) sur Odroid C2 via `spi-gpio` kernel driver
- **RadioLib** : port Linux avec `LinuxHal` (GPIO sysfs, SPI spidev, IRQ thread)
- **RX/TX APRS** : réception et émission de trames LoRa APRS via RadioLib
- **Dashboard LVGL** : compilation native du vrai `ui_dashboard.cpp` ESP32 dans le simulateur v9

### En cours
- Écrans Messages, Settings, Map non portés
- GPS : pas encore branché (thread NMEA à faire)

## Architecture

```
linux_tracker/          ← projet RadioLib Linux (Odroid)
├── src/
│   ├── linux_hal.cpp   ← implémentation RadioLibHal (GPIO sysfs, SPI spidev)
│   ├── linux_hal.h
│   └── main.cpp        ← test RX/TX LoRa APRS
├── lib/RadioLib/       ← RadioLib 7.1.0 (depuis .pio/libdeps)
├── Makefile
└── README.md

lv_port_pc_eclipse/     ← simulateur LVGL v9 SDL2 (PC)
├── main.cpp            ← point d'entrée : SDL2 + UIDashboard::createDashboard()
├── ui_dashboard.cpp    ← vrai code ESP32 (copié), modifié pour v9
├── stubs/              ← stubs pour dépendances ESP32
│   ├── Arduino.h       ← millis(), delay(), String
│   ├── esp_log.h       ← ESP_LOGI/D/W/E
│   ├── configuration.h ← struct Configuration
│   ├── thorvg_stubs.cpp← stubs ThorVG (LVGL v9 vector graphics)
│   └── ...             ← 40+ autres stubs
├── lvgl → .pio/libdeps/ttgo_t_deck_plus_433/lvgl  ← LVGL v9
├── build.sh            ← script de build
└── lv_conf.h           ← config LVGL v9 (ThorVG/Lottie désactivés)
```

## Pinout Odroid C2 J2 → HT-RA62

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

## Activation SPI sur Odroid

```bash
# Compiler l'overlay
dtc -@ -I dts -O dtb -o /tmp/spi-gpio.dtbo odroid_spi_gpio_overlay.dts
sudo cp /tmp/spi-gpio.dtbo /boot/overlay-user/
echo "user_overlays=spi-gpio" | sudo tee -a /boot/armbianEnv.txt
sudo reboot
```

Overlay minimal :
```dts
/dts-v1/;
/plugin/;
/ { compatible = "hardkernel,odroid-c2", "amlogic,meson-gxbb"; };
&{/spi-gpio} { status = "okay"; };
```

## Points techniques critiques

### SetDio3AsTcxoCtrl obligatoire
La commande SX1262 `SetDio3AsTcxoCtrl` (0x97) est requise même si DIO3 non câblé. Elle déclenche la calibration interne XOSC.

### Pipeline SPI SX1262
- Commandes sans adresse (GetIrqStatus) : données à rx[2] (pipeline 1 byte)
- Commandes avec adresse (ReadRegister) : données à rx[offset_cmd] (pas de pipeline)

## Build

### lora_rx (Odroid C2)
```bash
cd linux_tracker
# Copier RadioLib depuis .pio
cp -r ../CA2RXU/LoRa_APRS_Tracker-devel/.pio/libdeps/ttgo_t_deck_plus_433/RadioLib/src lib/RadioLib/
make
sudo ./lora_rx [callsign]
```

### Dashboard simulateur (PC)
```bash
cd lv_port_pc_eclipse
# Compiler LVGL + real ESP32 code
bash build.sh
# Lancer avec données live Odroid
ssh odroid 'sudo ./lora_rx 2>/dev/null' | ./demo
```

## Modifications v8→v9 sur ui_dashboard.cpp

| Original (v8) | Modifié (v9) |
|---|---|
| `lv_canvas_set_px_color(c, x, y, color)` | `lv_canvas_set_px(c, x, y, color, LV_OPA_COVER)` |
| `LV_IMG_CF_TRUE_COLOR` | `LV_COLOR_FORMAT_NATIVE` |
| `&lv_font_mono_16` | `&lv_font_montserrat_16` |
| `SCREEN_WIDTH/HEIGHT` 320x240 | Ajout `#elif defined(LINUX_SIM)` → 1024x600 |

## À faire

1. **GPS** : lire `/dev/ttyACM0` sur Odroid, thread NMEA → `UIDashboard::updateGPS()`
2. **Écrans** : porter ui_messaging.cpp, ui_settings.cpp (même méthode : copier + fixes v9 + stubs)
3. **Framebuffer** : remplacer SDL2 par `/dev/fb0` pour déploiement Odroid
4. **TX beacon** : bouton BCN → `sendUpdate = true` → pipe ou socket vers lora_rx
5. **Carte** : brancher le placeholder map à des tuiles réelles (MBTiles ou online)
