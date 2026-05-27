# Debug — Segfault fbdev Odroid-C4

**Date** : 2026-05-27
**Contexte** : Portage UI LVGL (fbdev) vers Odroid-C4 aarch64 Armbian.
L'UI fonctionne via SDL2 sur PC x86_64 mais segfault sur Odroid.

## Fichiers clés

- `src/main.cpp` : init LVGL fbdev/evdev + createDashboard
- `src/ui_dashboard.cpp` : `createDashboard()` ~700 lignes, crée l'écran principal
- `lib/lvgl/lv_conf.h` : config LVGL
- `include/ui_common.h` : définitions SCREEN_WIDTH/HEIGHT
- `Makefile` : build avec `WITH_DISPLAY=1`

## Symptôme

```
[UI] lv_init...
[Warn] lv_display_refr_timer: No draw buffer lv_refr.c:388
[UI] fbdev...
[UI] evdev...
[UI] dashboard...
[DASH] A     ← screen_main = lv_obj_create(NULL) OK
[DASH] B     ← lv_obj_set_style_bg_color OK
[DASH] C1    ← create=0xaaab237ea540 (screen_main valide)
[DASH] C2    ← bar=0xaaab237ea5e0 (status_bar créé)
segfault     ← crash dans lv_obj_set_size(status_bar, 1024, 50)
```

## Ce qui marche

Test minimal dans `main.cpp` (4 widgets) : tout fonctionne.
```cpp
lv_obj_t *scr = lv_screen_active();
lv_obj_t *c1 = lv_obj_create(scr);
lv_obj_t *myscr = lv_obj_create(NULL);
lv_obj_t *c2 = lv_obj_create(myscr);
// → OK, tracker + UI opérationnels, LVGL refresh normal
```

## Config LVGL en vigueur

```c
LV_USE_OS                  LV_OS_PTHREAD
LV_USE_STDLIB_MALLOC       LV_STDLIB_CLIB
LV_USE_LINUX_FBDEV         1
LV_LINUX_FBDEV_BUFFER_COUNT 1
LV_LINUX_FBDEV_BUFFER_SIZE  120
LV_LINUX_FBDEV_MMAP        0
LV_LINUX_FBDEV_RENDER_MODE LV_DISPLAY_RENDER_MODE_PARTIAL
LV_USE_EVDEV               1
LV_USE_LINUX_DRM           0
LV_DRAW_BUF_ALIGN          4
LV_CONF_INCLUDE_SIMPLE     défini
LV_USE_ASSERT_MEM_INTEGRITY 1
LV_USE_ASSERT_OBJ          1
```

## Tentatives de fix échouées

| # | Modification | Résultat |
|---|-------------|----------|
| 1 | `LV_STDLIB_MALLOC LV_STDLIB_BUILTIN` | `malloc(): invalid size (unsorted)` |
| 2 | `LV_STDLIB_MALLOC LV_STDLIB_CLIB` | segfault (inchangé) |
| 3 | `LV_USE_OS LV_OS_NONE` | segfault |
| 4 | `LV_USE_OS LV_OS_PTHREAD` | segfault (inchangé) |
| 5 | `BUFFER_COUNT 0→1` | Warning "No draw buffer" persiste |
| 6 | `BUFFER_SIZE 60→120` | inchangé |
| 7 | `LV_LINUX_FBDEV_MMAP 1→0` | inchangé |
| 8 | `SCREEN_WIDTH 320→1024` (`!defined(ARDUINO)`) | inchangé |

## Pistes non explorées

- Driver DRM (`LV_USE_LINUX_DRM=1`) au lieu de fbdev. Le kernel expose `simpledrm`.
- Allocation buffer fbdev avec `lv_draw_buf_create()` au lieu de `lv_malloc()` direct.
- Framebuffer `/dev/fb0` peut être utilisé par la console — conflit possible.
- Compiler LVGL avec assertions désactivées (`LV_USE_ASSERT_OBJ=0`) pour voir si l'assert handler `while(1)` interfère.

## Environnement

- Board : Odroid-C4 (Amlogic S905X3, 4× A55, Mali-G31, 4 GB)
- Kernel : `simpledrm` framebuffer, lima GPU
- Display : Waveshare WS170120 HDMI 1024×600, touch USB HID `/dev/input/event0`
- fb0 : 1024×600, 32 bpp
- OS : Armbian (aarch64)
