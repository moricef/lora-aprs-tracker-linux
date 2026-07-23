# LoRa APRS Tracker — Linux port

*[Lire en français](README.fr.md)*

Offline LoRa APRS tracker for ARM single-board computers (Raspberry Pi 4B,
Odroid C2). Adapts a proven APRS / SmartBeacon / messaging stack to a
Linux + LVGL desktop on HDMI displays (tested at 1024×600).

## Highlights

- **LoRa SX1262** via RadioLib on Linux (SPI through `spidev`, GPIO/IRQ via
  sysfs, see `LinuxHal`). RX/TX, digipeating, MicE & Base91 packet encoding.
- **Offline vector map renderer**: MapLibre Native accelerated by the
  Raspberry Pi 4 V3D GPU through EGL/GBM/KMS, with the LVGL UI composited as
  an overlay. The PMTiles/MVT software renderer remains available for the
  Odroid and as a fallback.
- **Overzoom** : the PMTiles file stops at z14 ; the renderer redraws
  any sub-region at screen zooms up to z17 — sharp because geometry is
  redrawn, not bitmap-stretched.
- **gpsd** integration for position / speed / track, plus full SmartBeacon.
- **APRS-IS** TCP uplink.
- **Bluetooth Classic SPP and BLE GATT**, configurable for TNC2 or KISS.
- **HTTP WebConf** on port 8080 for tracker configuration.
- **Conversations & messaging** (ACK, dedup, persistent storage in
  `/data/LoRa_Tracker/`).
- **LVGL 9** UI : dashboard, messaging screen, settings, map screen with
  pan/zoom/inertia, marker hit-test, GPS recenter, GPX recording.

## Hardware targets

| Board | Notes |
|---|---|
| Raspberry Pi 4B (1 GB) | Primary target. V3D/GLES-accelerated MapLibre through EGL/GBM/KMS. |
| Odroid C2 (2 GB) | Historical software PMTiles/MVT renderer. |

LoRa radio : any SX1262 module wired to SPI + DIO1/RESET/BUSY GPIOs. Tested
with HT-RA62 (433 MHz).

## Build

```bash
sudo apt install build-essential pkg-config git gpsd libgps-dev \
  nlohmann-json3-dev libdrm-dev libfreetype-dev libmicrohttpd-dev \
  libpng-dev zlib1g-dev libegl-dev libgles-dev libgbm-dev \
  libcurl4-openssl-dev libjpeg-dev libwebp-dev libuv1-dev libicu-dev \
  libsqlite3-dev libglib2.0-dev libbluetooth-dev bluez

make WITH_MAPLIBRE=1 -j$(nproc)
```

`WITH_MAPLIBRE=1` expects the ARM64 MapLibre Native archives under `$MLBUILD`;
the `ML` and `MLBUILD` paths can be overridden on the `make` command line.
`WITH_DISPLAY=1` keeps the software renderer without MapLibre.

## Run

```bash
./lora_aprs_tracker [callsign]
```

The first launch creates `/data/LoRa_Tracker/tracker_conf.json`. Edit the
callsign / beacon symbol / paths / radio profile there. In a
`WITH_MAPLIBRE=1` build, MapLibre is the default; `TRACKER_NO_MAPLIBRE=1`
forces the software fallback. Touch input uses evdev.

PMTiles file path is read from the config ; generate one with `tilemaker`
using the bundled scripts (see below).

## Vector tiles

Place data, road names and waterway names follow a population-ladder
discipline (cities, towns and villages appear at zooms tied to their
`population` tag rather than just their `place=*` class). POIs, footpaths,
housenumbers and other web-map noise are stripped at generation time.

The two source files for the generator pipeline are kept in `tilemaker/`
in this repo :

- `tilemaker/process-aprs.lua` — feature selector / population brackets
- `tilemaker/config-aprs.json` — tile layout (z6-14)

Typical run :

```bash
tilemaker --input region.osm.pbf \
          --output region.pmtiles \
          --config tilemaker/config-aprs.json \
          --process tilemaker/process-aprs.lua
```

A region the size of southern France fits in ~1.3 GB at z6-14 with these
files. Screen zooms 15-17 come for free via vector overzoom.

## Architecture (map subsystem)

```
src/map/
├── map_state.{h,cpp}      shared viewport / GPS / flags
├── map_io.{h,cpp}         PMTiles view metadata + symbol paths
├── map_engine.{h,cpp}     5×5 tile grid, reloadTiles, zoom, inertia tick
├── map_input.{h,cpp}      touch handler (pan, hit-test, double-tap)
├── map_traces.{h,cpp}     station trails + own GPS trail overlay
├── map_labels.{h,cpp}     label overlay : collision, hysteresis, glyphs
├── map_markers.{h,cpp}    station markers + info popup
├── map_view.{h,cpp}       LVGL screen (titlebar, buttons, lifecycle)
├── map_vector.{h,cpp}     PMTiles + MVT decoding, vector drawing
└── map_coordinate_math.{h,cpp}  lat/lon ↔ tile/pixel

src/maplibre_display.{h,cpp}  Raspberry Pi MapLibre EGL/GBM/KMS backend
```

The rest of the project (APRS, LoRa, gpsd, messaging, configuration,
storage, UI screens) sits at the top of `src/`.

## Status

Functional end-to-end on Raspberry Pi 4B :

- LoRa RX/TX, digipeating, SmartBeacon, MicE
- gpsd position with thread-safe access
- APRS-IS uplink + reconnect
- Persistent storage for stations, contacts, GPX
- LVGL UI: dashboard, messaging, settings, MapLibre map + overzoom
- Map: pan/zoom/inertia, APRS/GNSS trails, markers, spiderfy and fullscreen
- Bluetooth Classic/BLE with APRS TNC2 or KISS transport
- HTTP WebConf on port 8080

Not ported (irrelevant to the Linux target) :
battery monitoring, deep sleep and handheld hardware keyboards.

## License

This project is licensed under the MIT License. See `LICENSE`.

See individual sources for upstream license attributions (RadioLib, LVGL,
vtzero, PMTiles, nlohmann/json, tilemaker schemas).
