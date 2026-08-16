# Compilation Pi 4B avec MapLibre — guide autonome

## Prérequis

### 1. Dépendances système (Pi 4B)

```bash
sudo apt install build-essential pkg-config git gpsd libgps-dev \
  nlohmann-json3-dev libdrm-dev libfreetype-dev libmicrohttpd-dev \
  libpng-dev zlib1g-dev libegl-dev libgles-dev libgbm-dev \
  libcurl4-openssl-dev libjpeg-dev libwebp-dev libuv1-dev libicu-dev \
  libsqlite3-dev libglib2.0-dev libbluetooth-dev bluez
```

### 2. MapLibre Native — archives cross-compilées ARM64

MapLibre Native est cross-compilé sur une machine x86_64, pas sur le Pi. Les archives
statiques (`.a`) produites sont copiées sur le Pi.

Sur le Pi, deux chemins à connaître :

| Variable | Défaut | Contenu |
|----------|--------|---------|
| `ML` | `/home/adrasec09/maplibre-native` | Sources + headers MapLibre Native |
| `MLBUILD` | `$(ML)/build-cross` | Archives `.a` ARM64 |

Produits attendus dans `$(MLBUILD)` :

```
libmbgl-core.a
libmbgl-vendor-parsedate.a
vendor/maplibre-tile-spec/cpp/libmlt-cpp.a
libmbgl-vendor-csscolorparser.a
libmbgl-harfbuzz.a
libmbgl-freetype.a
libmbgl-vendor-nunicode.a
libmbgl-vendor-sqlite.a
```

### 3. Styles et polices MapLibre

- Style JSON : `/data/LoRa_Tracker/MapLibre/osm-bright.json`
- Polices PBF : `/data/LoRa_Tracker/MapLibre/fonts/{fontstack}/{range}.pbf`
- PMTiles : `/data/LoRa_Tracker/VectMaps/FranceSud/FranceSud.pmtiles`

Le sprite dans `osm-bright.json` pointe vers `https://openmaptiles.github.io/...`.
Sans internet, les icônes ne se chargent pas. À corriger si besoin (sprite local).

---

## Compilation

### Commande standard

```bash
cd /home/adrasec09/linux_tracker
make WITH_MAPLIBRE=1 -j2
```

Le `-j2` est adapté au Pi 4B (4 cœurs, mais la RAM limite).

Les objets sont isolés par profil de build :

| Profil | Commande | Objets |
|--------|----------|--------|
| headless | `make` | `build/rpi4-headless/` |
| display LVGL DRM | `make WITH_DISPLAY=1` | `build/rpi4-display/` |
| MapLibre EGL/KMS | `make WITH_MAPLIBRE=1` | `build/rpi4-maplibre/` |

`WITH_MAPLIBRE=1` active automatiquement `WITH_DISPLAY=1`, mais utilise son
propre répertoire d'objets. Un build LVGL DRM ne peut donc plus réutiliser les
`.o` d'un build MapLibre, ni l'inverse.

### Surcharger les chemins MapLibre

```bash
make WITH_MAPLIBRE=1 ML=/autre/path/maplibre-native MLBUILD=/autre/path/build-cross -j2
```

### Cibles

| Commande | Effet |
|----------|-------|
| `make WITH_MAPLIBRE=1` | Compile tout et linke le binaire |
| `make clean` | Supprime `build/`, les anciens `.o/.d` historiques dans `src/` et `lib/`, et les binaires |
| `make WITH_MAPLIBRE=1 -j2` | Recompile après un clean |

### Ce que fait `WITH_MAPLIBRE=1`

- Définit `-DWITH_MAPLIBRE` dans CXXFLAGS et CFLAGS
- Active automatiquement `WITH_DISPLAY` (l'UI LVGL)
- Ajoute `src/maplibre_display.cpp` aux sources
- Compile `maplibre_display.cpp` en **C++20 avec `-fno-rtti`** (contrairement au reste en C++17)
- Linke les 8 archives `.a` listées ci-dessus
- Linke les libs système : `EGL GLESv2 gbm curl jpeg png webp uv icuuc icui18n icudata sqlite3`

---

## Pièges connus

### Objets de build par profil

Le Makefile ne produit plus d'objets directement dans `src/` ou `lib/`.
Chaque profil a son espace :

```text
build/rpi4-headless/
build/rpi4-display/
build/rpi4-maplibre/
```

Cela évite le cas où un objet compilé avec `WITH_DISPLAY=1` est réutilisé par
erreur dans un build `WITH_MAPLIBRE=1`. C'est le point critique pour ne pas
relancer un binaire `LVGL DRM software` alors qu'on croit avoir recompilé
MapLibre.

### Vérifier les objets utilisés

```bash
make -n WITH_MAPLIBRE=1 | grep 'build/rpi4-maplibre' | head
make -n WITH_MAPLIBRE=1 build/rpi4-maplibre/src/maplibre_display.o
```

La règle spéciale de `maplibre_display.cpp` doit sortir :

```text
g++ -std=gnu++20 ... -DWITH_MAPLIBRE ... -o build/rpi4-maplibre/src/maplibre_display.o
```

### Ancien cas `.o` périmé non recompilé

**Symptôme** : le binaire se compile et se linke sans erreur, mais au runtime
c'est l'ancien moteur logiciel qui tourne (`Display: LVGL DRM software` au lieu
de `Display: MapLibre EGL/KMS`).

**Cause historique** : les anciens builds écrivaient les objets dans `src/*.o`
et `lib/*.o`. Un changement de flag (`WITH_DISPLAY=1` vs `WITH_MAPLIBRE=1`)
n'invalidait pas toujours les objets.

**Diagnostic** :
```bash
find src lib \( -name '*.o' -o -name '*.d' \) -print
```

**Solution** :
```bash
make clean
make WITH_MAPLIBRE=1 -j2
```

Après `make clean`, `find src lib \( -name '*.o' -o -name '*.d' \) -print` ne
doit rien retourner.

---

## Vérification post-compilation

### 1. Le binaire contient les symboles MapLibre

```bash
nm lora_aprs_tracker | grep -c 'mbgl\|MaplibreDisplay'
# Doit retourner > 10000
```

### 2. Le binaire est ARM64

```bash
file lora_aprs_tracker
# ELF 64-bit LSB pie executable, ARM aarch64
```

### 3. Le binaire a été compilé avec -DWITH_MAPLIBRE

Vérifier dans les logs de compilation que les objets MapLibre ont
`-DWITH_MAPLIBRE` et sortent dans `build/rpi4-maplibre/` :
```bash
make -n WITH_MAPLIBRE=1 | grep -E 'build/rpi4-maplibre/(src/main|src/maplibre_display|src/map/map_view).o'
```

---

## Lancement et test

### Service systemd

Le service est `/etc/systemd/system/lora-aprs-tracker.service` :

```ini
[Service]
Type=simple
User=adrasec09
Group=adrasec09
WorkingDirectory=/home/adrasec09/linux_tracker
ExecStart=/home/adrasec09/linux_tracker/lora_aprs_tracker
Restart=on-failure
RestartSec=5
```

Commandes utiles :
```bash
sudo systemctl restart lora-aprs-tracker   # redémarrer après compilation
journalctl -u lora-aprs-tracker -f         # logs en direct
journalctl -u lora-aprs-tracker -n 50      # 50 dernières lignes
```

### Vérifier que MapLibre est actif au runtime

Dans les logs de démarrage, chercher :
```
[maplibre] GL_RENDERER: V3D 4.2.14.0
[I][Main] Display: MapLibre EGL/KMS 1024x600, touch=OK
```

Si on voit `Display: LVGL DRM software` au lieu de `MapLibre EGL/KMS`, MapLibre
n'a pas réussi à s'initialiser (problème DRM/EGL).

### Vérifier le binaire réellement lancé

```bash
systemctl show -p ExecStart lora-aprs-tracker.service
readlink -f /proc/$(systemctl show -p MainPID --value lora-aprs-tracker.service)/exe
```

Le second chemin est le fichier réellement exécuté par le process courant.

### Vérifier que la carte utilise bien MapLibre

Dans les logs, quand on appuie sur MAP, on **ne doit pas** voir :
```
[map] recenter -> GPS ...
```

Ce log est émis uniquement par l'ancien moteur logiciel.

### Fallback logiciel

Pour forcer le moteur logiciel (dépanage) :
```bash
TRACKER_NO_MAPLIBRE=1 /home/adrasec09/linux_tracker/lora_aprs_tracker
```

### Screenshot MapLibre via SIGUSR2

`SIGUSR2` demande au backend MapLibre d'écrire une capture PNG dans `/tmp`.

```bash
sudo kill -USR2 "$(systemctl show -p MainPID --value lora-aprs-tracker.service)"
sleep 1
ls -l /tmp/screenshot_*.png
journalctl -u lora-aprs-tracker.service -n 50 --no-pager | grep screenshot
```

Le log attendu :

```text
[maplibre] screenshot -> /tmp/screenshot_YYYYMMDD_HHMMSS.png
```

Si aucun fichier n'est créé, vérifier d'abord que le service tourne bien en
`Display: MapLibre EGL/KMS`.

---

## Architecture résumée

```
main.cpp
  ├─ MaplibreDisplay::init()         → prend DRM master sur card0
  │   ├─ KMS/DRM (by-path-platform-gpu-card)
  │   ├─ EGL/GBM (contexte GLES2)
  │   ├─ MapLibre Native (carte rendue dans FBO 0)
  │   └─ LVGL overlay (texture GL par-dessus FBO 0)
  ├─ UIDashboard::createDashboard()  → UI principale
  └─ MapView::create()               → écran carte
      ├─ Si MaplibreDisplay::isActive() → écran LVGL transparent
      │   + gpuMapLayer (markers/traces) sur la carte MapLibre
      └─ Sinon → MapEngine logiciel (fond 0x2F4F4F, tiles PMTiles/MVT)
```

Deux moteurs coexistent dans le binaire, un seul est actif à la fois.
Le choix se fait via `MaplibreDisplay::isActive()` dans `map_view.cpp`.

---

## Fichiers clés

| Fichier | Rôle |
|---------|------|
| `Makefile` | Build, variables `WITH_MAPLIBRE`, `ML`, `MLBUILD` |
| `src/main.cpp` | Init display, boucle principale |
| `src/maplibre_display.{h,cpp}` | Backend KMS/EGL/GBM + MapLibre Native |
| `src/map/map_view.cpp` | Écran carte, branchement GPU vs logiciel |
| `src/ui_dashboard.cpp` | Dashboard, appel à `MapView::create()` |
| `maplibre/osm-bright.json` | Style MapLibre (sources PMTiles, couches, polices) |

## Récapitulatif rapide

```bash
# Installer les dépendances (une fois)
sudo apt install build-essential pkg-config git gpsd libgps-dev \
  nlohmann-json3-dev libdrm-dev libfreetype-dev libmicrohttpd-dev \
  libpng-dev zlib1g-dev libegl-dev libgles-dev libgbm-dev \
  libcurl4-openssl-dev libjpeg-dev libwebp-dev libuv1-dev libicu-dev \
  libsqlite3-dev libglib2.0-dev libbluetooth-dev bluez

# Compiler
cd /home/adrasec09/linux_tracker
make clean && make WITH_MAPLIBRE=1 -j2

# Contrôler le profil utilisé sans compiler
make -n WITH_MAPLIBRE=1 | grep 'build/rpi4-maplibre' | head

# Déployer
sudo systemctl restart lora-aprs-tracker

# Vérifier
journalctl -u lora-aprs-tracker -n 20 | grep -E 'MapLibre|Display:|maplibre'
```
