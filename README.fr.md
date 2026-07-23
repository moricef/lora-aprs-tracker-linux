# LoRa APRS Tracker — Port Linux

*[Read in English](README.md)*

Tracker LoRa APRS autonome pour ordinateurs monocartes ARM (Raspberry Pi
4B, Odroid C2). Reprend la pile APRS / SmartBeacon / messagerie éprouvée
d'un firmware existant et l'adapte à un environnement Linux + LVGL sur
écran HDMI (testé en 1024×600).

## Points clés

- **LoRa SX1262** via RadioLib sous Linux (SPI via `spidev`, GPIO/IRQ via
  sysfs, voir `LinuxHal`). RX/TX, digipeating, encodage de trames MicE
  et Base91.
- **Rendu carte vectorielle offline** : MapLibre Native sur le GPU V3D du
  Raspberry Pi 4 via EGL/GBM/KMS, avec l'interface LVGL compositée en overlay.
  Le renderer logiciel PMTiles/MVT reste disponible pour l'Odroid et comme
  solution de repli.
- **Overzoom vectoriel** : le pmtiles s'arrête à z14 ; le renderer
  redessine n'importe quelle sous-région jusqu'au z17 — net car la
  géométrie est redessinée, pas une image étirée.
- **gpsd** pour position / vitesse / cap, et SmartBeacon complet.
- **APRS-IS** uplink TCP.
- **Bluetooth Classic SPP et BLE GATT**, configurables en TNC2 ou KISS.
- **WebConf HTTP** sur le port 8080 pour modifier la configuration du tracker.
- **Conversations et messagerie** (ACK, dedup, persistance dans
  `/data/LoRa_Tracker/`).
- **UI LVGL 9** : dashboard, écran messagerie, écran réglages, écran
  carte avec pan/zoom/inertie, hit-test marqueurs, recentrage GPS,
  enregistrement GPX.

## Cibles matérielles

| Carte | Notes |
|---|---|
| Raspberry Pi 4B (1 Go) | Cible principale. MapLibre accéléré par V3D/GLES via EGL/GBM/KMS. |
| Odroid C2 (2 Go) | Renderer PMTiles/MVT logiciel historique. |

Radio LoRa : tout module SX1262 câblé en SPI + GPIO DIO1/RESET/BUSY. Testé
avec un HT-RA62 (433 MHz).

## Compilation

```bash
sudo apt install build-essential pkg-config git gpsd libgps-dev \
  nlohmann-json3-dev libdrm-dev libfreetype-dev libmicrohttpd-dev \
  libpng-dev zlib1g-dev libegl-dev libgles-dev libgbm-dev \
  libcurl4-openssl-dev libjpeg-dev libwebp-dev libuv1-dev libicu-dev \
  libsqlite3-dev libglib2.0-dev libbluetooth-dev bluez

make WITH_MAPLIBRE=1 -j$(nproc)
```

`WITH_MAPLIBRE=1` attend les archives ARM64 de MapLibre Native sous
`$MLBUILD` (`ML` et `MLBUILD` sont surchargeables dans la commande `make`).
`WITH_DISPLAY=1` conserve le renderer logiciel sans MapLibre.

## Exécution

```bash
./lora_aprs_tracker [callsign]
```

Au premier lancement, `/data/LoRa_Tracker/tracker_conf.json` est créé.
Éditer callsign / symbole beacon / chemins / profil radio dedans. Le
Sur un build `WITH_MAPLIBRE=1`, MapLibre est le mode par défaut. La variable
`TRACKER_NO_MAPLIBRE=1` force le renderer logiciel de secours. Le tactile
passe par evdev.

Le chemin du fichier pmtiles est lu depuis le fichier de config ; générer
ce pmtiles avec `tilemaker` à partir des scripts fournis (voir plus bas).

## Tuiles vectorielles

Lieux, noms de routes et de cours d'eau suivent une ladder population
(les cities/towns/villages apparaissent à des zooms liés à leur tag
`population` plutôt qu'au simple `place=*`). POIs, sentiers piétons,
numéros de rue et autres pollutions web-map sont retirés à la génération.

Les deux fichiers source du pipeline de génération sont gardés dans
`tilemaker/` du dépôt :

- `tilemaker/process-aprs.lua` — sélection features / brackets population
- `tilemaker/config-aprs.json` — layout des tuiles (z6-14)

Lancement typique :

```bash
tilemaker --input region.osm.pbf \
          --output region.pmtiles \
          --config tilemaker/config-aprs.json \
          --process tilemaker/process-aprs.lua
```

Une région de la taille du sud de la France tient en ~1,3 Go en z6-14
avec ces fichiers. Les zooms écran 15-17 sont gratuits via l'overzoom
vectoriel.

## Architecture (sous-système carte)

```
src/map/
├── map_state.{h,cpp}      état viewport / GPS / flags partagés
├── map_io.{h,cpp}         métadonnées PMTiles + chemins symboles
├── map_engine.{h,cpp}     grille 5×5, reloadTiles, zoom, tick inertie
├── map_input.{h,cpp}      handler touch (pan, hit-test, double-tap)
├── map_traces.{h,cpp}     overlay traces stations + traces propres
├── map_labels.{h,cpp}     overlay labels : collision, hystérésis, glyphes
├── map_markers.{h,cpp}    marqueurs stations + popup info
├── map_view.{h,cpp}       écran LVGL (titlebar, boutons, lifecycle)
├── map_vector.{h,cpp}     décodage PMTiles + MVT, rendu vectoriel
└── map_coordinate_math.{h,cpp}  lat/lon ↔ tuile/pixel

src/maplibre_display.{h,cpp}  backend MapLibre EGL/GBM/KMS du Raspberry Pi
```

Le reste du projet (APRS, LoRa, gpsd, messagerie, configuration, stockage,
écrans UI) est au sommet de `src/`.

## État

Fonctionnel de bout en bout sur Raspberry Pi 4B :

- LoRa RX/TX, digipeating, SmartBeacon, MicE
- Position gpsd avec accès thread-safe
- Uplink APRS-IS + reconnexion
- Persistance stations, contacts, GPX
- UI LVGL : dashboard, messagerie, réglages, carte MapLibre + overzoom
- Carte : pan/zoom/inertie, traces APRS/GNSS, marqueurs et spiderfy, plein écran
- Bluetooth Classic/BLE avec transport APRS TNC2 ou KISS
- WebConf HTTP sur le port 8080

Non porté (sans pertinence pour la cible Linux) :
monitoring batterie, deep sleep et claviers d'appareils portatifs.

## Licence

Ce projet est publié sous licence MIT. Voir `LICENSE`.

Voir les sources individuelles pour les attributions des licences amont
(RadioLib, LVGL, vtzero, PMTiles, nlohmann/json, schémas tilemaker).
