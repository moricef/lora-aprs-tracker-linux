# État du projet LoRa APRS Linux Tracker — 15 Mai 2026

## Objectif
Port du firmware ESP32 LoRa APRS Tracker vers Linux (Odroid C2 aarch64) + dashboard PC (SDL2/LVGL).

## Architecture

```
linux_tracker/               ← backend LoRa + GNSS (port intégral du firmware ESP32)
├── src/                     ← 12 sources C++ (main, lora, gps, config, smartbeacon…)
├── include/                 ← 13 headers (Arduino.h, FS.h, stubs, configuration…)
├── lib/APRSPacketLib/       ← encodage Base91, MicE, parsing APRS
├── lib/gps_math/            ← Haversine distance/cap
├── lib/RadioLib/            ← RadioLib 7.1.0
├── Makefile                 ← g++ -std=c++11, 23 sources, -lpthread -lgps -lm
└── ETAT_DU_PROJET.md

/home/fab2/Developpement/lv_port_pc_eclipse/   ← dashboard LVGL v9 + SDL2 (projet séparé)
├── main.cpp                 ← pipe stdin → dashboard, heure UTC+locale
├── ui_dashboard.cpp/h       ← écran principal
├── ui_messaging.cpp/h       ← écran Messages
├── ui_settings.cpp/h        ← écran Settings
├── map/                     ← carte raster (tiles JPEG)
├── stubs/                   ← headers remplaçant les dépendances ESP32
└── build.sh                 ← compile LVGL + UI + map → ./demo
```

## Ce qui fonctionne

### Backend linux_tracker (session 2026-05-15 — port intégral)
- **APRSPacketLib** : génération packets Base91, MicE, digipeating, parsing RX
- **GPS gpsd** : position, alt, vitesse, satellites — thread dédié, mutex
- **SmartBeacon** : 3 profils (Slow/Medium/Fast), checkInterval, checkState
- **Stations** : mapStations[15], nearbyStations[4], Douglas-Peucker trace simplification
- **Messages APRS** : ACK, conversations par contact, dédup 30s, buffer 15s
- **APRS-IS** : TCP socket, vérification passcode, reconnect 30s
- **Configuration** : JSON nlohmann, structs identiques firmware ESP32
- **Stockage** : /data/LoRa_Tracker/ (stats.json, contacts.json, frames.log, conversations/)
- **LoRa** : SX1262 via RadioLib + LinuxHal (SPI spidev + GPIO sysfs)
- **Pipeline dashboard** : stdout → lignes `RX RSSI:…` et `GPS:lat,lon,…` pour ./demo
- **Compilation** : `make clean && make` → zéro erreur (warnings APRSPacketLib uniquement)
- **Shim Arduino** : String, millis(), random(), File — identique API Arduino

### Dashboard lv_port_pc_eclipse
- Dashboard, Messages, Settings, Carte raster
- Build : bash build.sh → ./demo (zéro warning)

## Problèmes connus

1. **SPI `/dev/spidev0.0` inexistant sur machine de dev**
   - Sur Odroid C2 : nécessite l'overlay DT spi-gpio (voir `Docs/odroid_c2_spi.md`)
   - Sur PC : pas de SX1262 → radio.begin() échoue (-2), RX/TX LoRa inopérant (normal)
   - GPS (gpsd) et APRS-IS (TCP) fonctionnent indépendamment

2. **Callsign par défaut = NOCALL-7**
   - checkNocall() bloque les TX → configurer dans `/data/LoRa_Tracker/tracker_conf.json`
   - Log d'erreur throttlé à 30 secondes

3. **Pas de compilation croisée pour aarch64**
   - Le Makefile compile pour la machine locale (x86_64 ou aarch64)
   - Pour l'Odroid : compiler directement sur la cible ou ajouter un cross-compiler

## Dépendances
- **Compilation** : g++, nlohmann-json3-dev, libgps-dev
- **Exécution** : gpsd, spidev (overlay DT sur Odroid C2)

## Commandes

```bash
# Backend
cd linux_tracker && make clean && make
./lora_aprs_tracker [callsign]

# Dashboard (projet séparé)
cd /home/fab2/Developpement/lv_port_pc_eclipse && bash build.sh
./lora_aprs_tracker | ./demo
```

## Non porté (hardware ESP32 uniquement)
BLE, WiFi (AP + WebConfig), display LVGL tactile, battery monitoring, sleep/deep sleep, LED/buzzer, GPIO PTT
