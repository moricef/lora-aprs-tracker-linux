# Parité carte : ancien moteur logiciel ↔ MapLibre GPU

Référence de parité fonctionnelle pour la migration du rendu carte vers MapLibre.
La **source de vérité des features** est l'ancien moteur logiciel (rendu maison
tuiles + overlays LVGL). Toute fonction listée ici doit exister à l'identique
dans le chemin MapLibre avant de retirer l'ancien.

Convention statut : ✅ porté · ⚠️ partiel / à vérifier · ❌ manquant.
Le statut est indicatif au moment de l'écriture (l'intégration MapLibre est en
cours) — la colonne de référence (ancien moteur + comportement attendu), elle,
est stable.

## Architecture des deux chemins

| | Ancien moteur | MapLibre GPU |
|---|---|---|
| Rendu tuiles | maison, tuiles vecto → canvas LVGL software | MapLibre Native, GLES/V3D → framebuffer 0 |
| Sélection | défaut | env absente de `TRACKER_NO_MAPLIBRE` (opt-out) — `main.cpp` |
| Écran carte | `MapView::create` → `MapEngine`/`MapLabels`/`MapInput` | `MapView::create` branche GPU → overlay LVGL transparent + `gpu*` |
| Overlays (stations/traces) | dessinés dans le canvas software | widgets LVGL positionnés par `MaplibreDisplay::project` |
| Style | tuiles maison | `osm-bright.json` (nos réglages : shields, tracks, places) |

## Matrice de parité

### Navigation / gestes — réf. `src/map/map_input.cpp` (`touchCb`)

| Action | Ancien : comportement attendu | Statut MapLibre |
|---|---|---|
| **Pan** (glisser) | `PRESSING` → déplacement continu de la vue | ✅ `gpuTouchCb` → `moveBy` |
| **Inertie / fling** | vélocité lissée (moyenne pondérée poids 0.7) puis décélération après relâcher | ⚠️ `gpuTimerTick` (décél. 0.85) — vérifier ressenti |
| **Double-tap** | `DOUBLE_CLICKED` → `toggleFullscreen` | ❌ non branché sur gpuTouch |
| **Pan coupe le suivi GPS** | 1er déplacement → `mapFollowGps=false` + `markFollowGpsDisabled` | ⚠️ à vérifier dans gpuTouchCb |
| **Tap court sur station** | `hitTest` → `showStationPopup` | ✅ `gpuMarkerClicked` |
| **Appui long (>400 ms) sur station** | → `UIMessaging::openComposeWithCallsign` (répondre) | ❌ manquant (clic simple seulement) |
| **Seuil pan vs tap** | déplacement > 10 px = pan (sinon hit-test) | ⚠️ à vérifier |

### Zoom — réf. `src/map/map_view.cpp`, `map_engine.cpp`

| Action | Ancien : comportement attendu | Statut MapLibre |
|---|---|---|
| Bouton Z+ / Z− | `zoomIn/zoomOut` | ✅ `setGpuZoom` |
| Bornes zoom | `zoomMin`/`zoomMax` (découverts par `MapIO::discoverZooms`) | ⚠️ vérifier respect des bornes |
| **Zoom mémorisé** | variable globale `zoom` conservée entre ouvertures | ✅ `gpuScreenDeleted` sauve `zoom`+centre |
| Recentrage au zoom | `recenterForZoom` garde le centre écran | ⚠️ MapLibre zoome sur le centre — à vérifier |

### Suivi GPS / recentrage — réf. `MapView::create` (bouton recenter)

| Action | Ancien | Statut |
|---|---|---|
| Bouton recentrer GNSS | recentre sur `gpsLat/gpsLon` + réactive follow | ✅ setCenter + `gpuFollowGps=true` |
| Suivi continu | `mapFollowGps` → recadre sur chaque fix GPS | ✅ `setCenter` sur fix |
| Couleur bouton (bleu suivi / orange libre) | oui | ⚠️ à vérifier en GPU |

### Plein écran — réf. `MapView::toggleFullscreen`

| Action | Ancien | Statut |
|---|---|---|
| Double-tap → masquer bandeaux (carte pleine hauteur) | cache tbar/ibar, repositionne | ❌ manquant en GPU |

### Stations — réf. `src/map/map_markers.cpp`

| Élément | Ancien | Statut |
|---|---|---|
| Marqueurs stations | symbole APRS (table/overlay) + fallback pastille | ✅ `ensureGpuMarker`/`positionGpuMarker` |
| Couleur par ancienneté | récent orange / vieux gris | ✅ (seuil 10 min) |
| Marqueur position propre | symbole du beacon | ✅ slot 0 |
| Popup info station | `showStationPopup` | ✅ réutilisé |
| Compteur stations (info bar) | `Stn:N` | ⚠️ `refreshGpuInfoBar` — vérifier |

### Traces — réf. `src/map/map_traces.cpp`

| Élément | Ancien | Statut |
|---|---|---|
| Trace GPS propre | polyligne | ⚠️ `refreshGpuTraces` — vérifier |
| Traces stations | polylignes par station | ⚠️ à vérifier |

### GPX — réf. `src/gpx_writer.*`

| Action | Ancien | Statut |
|---|---|---|
| Bouton GPX record on/off | `GPXWriter::start/stopRecording` | ✅ bouton présent |
| Enregistrement points | `addPoint` sur fix | ✅ indépendant du rendu |

### Bandeaux — réf. `MapView::create`, `refreshInfoBar`

| Élément | Ancien | Statut |
|---|---|---|
| Barre titre `MAP (Zxx)` | zoom courant | ✅ |
| Barre info `Lat / Lon / Stn` | `refreshInfoBar` après pan | ⚠️ `refreshGpuInfoBar` |
| **Z-order bandeaux** | bandeaux au-dessus des overlays | ❌ **BUG** : markers/traces créés après → passent par-dessus les bandeaux (visible en zone dense de stations et en bas d'écran / sud) |

### Labels carte

| Élément | Ancien | Statut |
|---|---|---|
| Noms villes/routes | `MapLabels` software (FreeType) | ✅ MapLibre natif |
| Accents (é è à ç î…) | FreeType | ✅ corrigé (reset `GL_UNPACK_ROW_LENGTH` avant render) |
| Cartouches N° route | rendu maison | ✅ `highway-shield` (style) |

## Comportements subtils à ne pas perdre

- **Inertie** : vélocité = moyenne pondérée (poids 0.7) des vitesses instantanées
  pendant le drag, puis décélération multiplicative après relâcher.
- **Long-press 400 ms** sur une station = répondre (compose message), pas juste
  afficher le popup. Distinction tap court / appui long.
- **Pan > 10 px** = considéré comme déplacement (sinon c'est un tap → hit-test).
- **1er pixel de pan** coupe le suivi GPS (la vue reste où l'utilisateur l'amène).
- **Double-tap** = plein écran (masque les bandeaux), pas un zoom.
- **Sortie de carte** : mémoriser zoom **et** centre pour la réouverture.

## Points ouverts (à corriger en bloc)

1. Double-tap → plein écran (masquer bandeaux).
2. Appui long station → compose message.
3. Z-order : bandeaux toujours au-dessus des markers/traces.
4. Pan coupe le suivi GPS (vérifier/porter).
5. Bornes de zoom respectées.
6. Recentrage au zoom (centre écran conservé).
7. Rafraîchissement info bar (lat/lon/compteur) après pan et sur fix.
