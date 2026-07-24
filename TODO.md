# TODO

## UI / Display

- [ ] Masquer ou désactiver `Settings > Display > Brightness` sur Linux/Pi HDMI quand aucun contrôle backlight n'est exposé (`/sys/class/backlight` vide). Le slider appelle actuellement `displaySetBrightness()`, stub vide dans `include/display.h`; l'écran se règle par bouton/OSD matériel. Option future : dimming logiciel par overlay LVGL si demandé, sans effet sur le rétroéclairage.
- [ ] Identifier le modèle exact de l'écran HDMI Waveshare et tester le comportement de son bouton power : écran/backlight seulement, coupure alim Pi, ou événement power système visible dans `journalctl`.

## Hardware / Power

- [ ] Ajouter un bouton physique d'arrêt/réveil propre du Pi via GPIO3 : bouton momentané entre pin 5 (`GPIO3`) et pin 6 (`GND`), avec `dtoverlay=gpio-shutdown,gpio_pin=3,active_low=1,gpio_pull=up,debounce=1000` dans `/boot/firmware/config.txt`.
