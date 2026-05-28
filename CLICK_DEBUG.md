# Clic/Long-press marqueurs station — debug

## Problème

Sur Odroid C4 (ARM64, Debian), les marqueurs de stations APRS sur la carte ne reçoivent **jamais**
les événements CLICKED/LONG_PRESSED de LVGL. Le même code fonctionne parfaitement dans le
simulateur SDL2 sur PC.

## Architecture

- **Display** : DRM (`/dev/dri/card0`, `lv_linux_drm_create`)
- **Touch** : evdev (`/dev/input/event5`, `lv_evdev_create(LV_INDEV_TYPE_POINTER, ...)`)
- **LVGL** : 9.x, buffer ARGB8888
- **Simu** : SDL2 (`lv_sdl_window_create` + `lv_sdl_mouse_create`), **même code map_raster.cpp**, CLICKED OK

## Code concerné

### Création des marqueurs (`src/map/map_raster.cpp:createMarkerObj()`)
```cpp
lv_obj_t *m = lv_obj_create(parent);  // parent = mapCont
lv_obj_set_size(m, MARKER_W, MARKER_H);  // 80x40
lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);
// Enfants : lv_image (icône 24x24), lv_label (callsign, fond semi-transparent)
// Les enfants NE sont PAS cliquables
lv_obj_add_event_cb(m, station_click_cb,   LV_EVENT_CLICKED,      user_data);
lv_obj_add_event_cb(m, station_longpress_cb, LV_EVENT_LONG_PRESSED, user_data);
lv_obj_move_foreground(m);
```

### Touch handler (`src/map/map_raster.cpp:mapTouchCB()`)
- `LV_EVENT_PRESSED` : vérifie si `lv_event_get_target(e) != mapCont`. Si oui (marqueur),
  return early. Si non (fond de carte), active `panActive = true`.
- `LV_EVENT_PRESSING` : pan (déplacement tuiles + inertie)
- `LV_EVENT_RELEASED` : si drag > 10px → fin de pan. Sinon → hit-test manuel sur les
  marqueurs (itération `markers[]`, `lv_obj_get_coords`, comparaison avec pressPt)

### Init input (`src/main.cpp`)
```cpp
lv_group_set_default(lv_group_create());
lv_display_t *disp = lv_linux_drm_create();
lv_linux_drm_set_file(disp, "/dev/dri/card0", -1);
lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event5");
lv_indev_set_display(touch, disp);
lv_evdev_set_calibration(touch, 0, 0, 1023, 599);
// PAS de lv_indev_set_group — retiré car dans la simu SDL2 il y est et ça marche,
// sur Odroid avec ou sans group, les marqueurs ne reçoivent pas d'événements
```

## Logs observés sur Odroid

```
[MAP] PRESSED onMarker=0 markers=1
[MAP] PRESSED onMarker=0 markers=1
[MAP] PRESSED onMarker=0 markers=1
...
```

**`onMarker=0` systématiquement** même avec `markers=1` ou `markers=2`.
`lv_event_get_target(e)` retourne toujours l'adresse de `mapCont`, jamais un marqueur.
Le pan (drag) fonctionne normalement.

## Ce qui a été essayé (sans succès)

1. `lv_obj_move_foreground(m)` après création du marqueur — aucun effet
2. Rendre les enfants du marqueur cliquables (`LV_OBJ_FLAG_CLICKABLE` sur label/image) — aucun effet
3. Retirer `lv_indev_set_group(touch, ...)` — aucun effet
4. `ESP_LOGD` → no-op sur Linux (macro `do {} while(0)`) → remplacé par `fprintf(stderr)`
5. Hit-test manuel dans RELEASED — bloqué par `return` après pan-end (corrigé, à tester)

## Ce qui reste à tester (commit ea8050b)

Le RELEASED fait maintenant le hit-test même quand `panActive=true`, si le drag < 10px.
Logs attendus :
```
[MAP] TAP held=Xms pos(X,Y) markers=N
[MAP]   marker[0] area=(X1,Y1)-(X2,Y2) idx=N
[MAP] HIT marker idx=N held=Xms
```

Ces logs montreront si les marqueurs ont des coordonnées écran correctes
(`lv_obj_get_coords`) et si le toucher tombe dedans.

## Question

Pourquoi `lv_event_get_target(e)` dans le handler PRESSED de `mapCont` retourne-t-il
toujours `mapCont` lui-même, même quand l'utilisateur touche un marqueur (enfant de
mapCont, créé avec `LV_OBJ_FLAG_CLICKABLE`) ? Le même code fonctionne avec SDL2.
Différence fondamentale entre SDL2 mouse et evdev touch dans la hit-test LVGL9 ?
