# TODO

## UI / Display

- [ ] Masquer ou désactiver `Settings > Display > Brightness` sur Linux/Pi HDMI quand aucun contrôle backlight n'est exposé (`/sys/class/backlight` vide). Le slider appelle actuellement `displaySetBrightness()`, stub vide dans `include/display.h`; l'écran se règle par bouton/OSD matériel. Option future : dimming logiciel par overlay LVGL si demandé, sans effet sur le rétroéclairage.
