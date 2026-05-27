# Port Linux — état complet

## ✅ Modules portés (identiques au firmware)

| Module | Fichier | Notes |
|--------|---------|-------|
| APRS-IS | `aprs_is_utils.cpp` | socket TCP, auth, upload |
| Configuration | `configuration.cpp` | JSON nlohmann, writeFile/readFile |
| GPS | `gps_utils.cpp` | gpsd au lieu de NeoGPS |
| LoRa | `lora_utils.cpp` | RadioLib SX1262, SPI via LinuxHal |
| Messages | `msg_utils.cpp` | APRS+Winlink, conversations |
| Notifications | `notification_utils.cpp` | beeps via stdout BEEP: |
| SmartBeacon | `smartbeacon_utils.cpp` | identique |
| Stations | `station_utils.cpp` | mapStations, traces, stats |
| Stockage | `storage_utils.cpp` | /data/LoRa_Tracker |
| Dashboard | `ui_dashboard.cpp` | LVGL, DRM 1024×600 |
| Messages UI | `ui_messaging.cpp` | conversations, compose, stats |
| Popups | `ui_popups.cpp` | TX/RX popups |
| Settings | `ui_settings.cpp` | freq, speed, display, wifi, bt |
| Symboles | `shared_symbols.cpp` | 24 icônes APRS bitmap |

## ⚠️ Différences dans les modules portés

### GPS (`gps_utils.cpp`)
- **Heure**: Linux utilise `gettimeofday()` (système), ESP32 utilise `gpsFix.dateTime` (GPS satellite)
- **setDateFromData**: neutralisé sur Linux (NTP gère l'heure), actif sur ESP32
- **Seuil de données**: ESP32 lit heure + satellites même sans fix 2D, Linux bloque tout

### Speed Screen (`ui_settings.cpp`)
- Linux: 6 vitesses fixes (1200/610/300/244/209/183) au lieu des profils loraTypes

## ❌ Non portés (hardware ESP32 — stubs uniquement)

| Module | Raison |
|--------|--------|
| `battery_utils.cpp` | ADC ESP32 → remplacer par sysfs ou stub |
| `ble_utils.cpp` | Bluetooth LE ESP32 → stub |
| `bluetooth_utils.cpp` | Bluetooth ESP32 → stub |
| `button_utils.cpp` | GPIO hardware → stub |
| `display.cpp` | init display ESP32 → DRM/fbdev sur Linux |
| `gpx_writer.cpp` | écriture GPX → réalisable sur Linux |
| `joystick_utils.cpp` | GPIO ESP32 → stub |
| `keyboard_utils.cpp` | clavier T-Deck → evdev sur Linux |
| `kiss_utils.cpp` | protocole KISS → port réalisable |
| `menu_utils.cpp` | menu ESP32 → déjà dans ui_settings |
| `power_utils.cpp` | gestion alim ESP32 → stub |
| `sd_logger.cpp` | SD SPI → storage_utils le fait déjà |
| `sleep_utils.cpp` | deep sleep ESP32 → stub |
| `telemetry_utils.cpp` | encodage telemetry → port réalisable |
| `touch_utils.cpp` | touch ESP32 → evdev sur Linux |
| `utils.cpp` | utilitaires → partiellement dans ui_common.h |
| `web_utils.cpp` | serveur web ESP32 → webconf_httpd.cpp |
| `wifi_utils.cpp` | WiFi ESP32 → stub (géré par OS) |
| `winlink_utils.cpp` | email Winlink → partiellement dans msg_utils |
| `wx_utils.cpp` | station météo → stub |

## Modules Linux (pas dans le firmware)

| Module | Rôle |
|--------|------|
| `arduino_compat.cpp` | stub Arduino (String, millis, delay) |
| `linux_hal.cpp` | GPIO/SPI via sysfs |
| `main.cpp` | entry point headless + WITH_DISPLAY |
| `thorvg_stubs.cpp` | stubs ThorVG |
| `webconf_httpd.cpp` | serveur HTTP libmicrohttpd port 8080 |

## Map (src/map/)

| Fichier firmware | Porté ? | Fichier Linux |
|-----------------|---------|---------------|
| `map_coordinate_math.cpp` | ✅ | `map_coordinate_math.cpp` |
| `map_engine.cpp` | ❌ | — (MVT decoder + vector render) |
| `map_input.cpp` | ❌ | — (touch map) |
| `map_nav_render.cpp` | ❌ | — (vector rendering engine) |
| `map_render.cpp` | ❌ | — (stations sur carte) |
| `map_state.cpp` | ❌ | — (état map) |
| `map_tiles.cpp` | ❌ | — (chargement tuiles) |
| `trace_sd.cpp` | ❌ | — (traces GPS) |
| `ui_map_manager.cpp` | ❌ | — (gestion écran map) |
| — | ✅ ajouté | `map_raster.cpp` (tuiles raster) |
| — | ✅ ajouté | `map_vector.cpp` (tuiles vectorielles PMTiles) |

---

## Résumé

| Catégorie | Nombre |
|-----------|--------|
| ✅ Portés | 14 modules (APRS, GPS, LoRa, UI complet...) |
| ❌ Non portés — dépendance hardware | 10 (ESP32 ADC, deep sleep, GPIO, BLE...) |
| ❌ Non portés — mais réalisables | 12 + 8 (map engine, GPX, KISS, telemetry, Winlink, WX...) |

**À porter** (par ordre de dépendance firmware) : télémesure, Winlink, KISS, GPX writer, moteur map complet.
