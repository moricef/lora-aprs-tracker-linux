# Diff GPS firmware ESP32 vs Linux

## Fonctions — toutes portées

| Fonction | ESP32 | Linux | Statut |
|----------|-------|-------|--------|
| setup() | NeoGPS + Serial | gpsd thread | ✅ |
| getData() | read from NeoGPS | read from gpsd | ✅ |
| hasNewFix() | newFixAvailable | !_fixConsumed | ✅ |
| setDateFromData() | settimeofday(GPS) | vide (neutralisé) | ⚠️ |
| calculateDistanceCourse() | calcDist + calcCourse | identique | ✅ |
| calculateDistanceTraveled() | anti-jitter filter | identique | ✅ |
| calculateHeadingDelta() | heading delta | identique | ✅ |
| checkStartUpFrames() | GPS init check | stub (gpsd) | ✅ |
| getCardinalDirection() | N/E/S/W | identique | ✅ |

## Différences clés

### 1. Source de l'heure GPS
- **ESP32**: `gpsFix.dateTime.hours/minutes/seconds` depuis NeoGPS (parsé des trames NMEA)
- **Linux**: `gettimeofday()` → heure système, PAS l'heure GPS
- **Impact**: l'heure GPS satellite n'est jamais affichée sur l'Odroid

### 2. setDateFromData
- **ESP32**: `settimeofday()` avec l'heure GPS (mktime)
- **Linux**: corps vide (neutralisé car mktime + tm local cassait l'horloge système)
- **Impact**: plus de sync auto de l'horloge, mais NTP le fait déjà

### 3. Seuil de lecture des données
- **ESP32**: NeoGPS met à jour `gpsFix.dateTime` et `gpsFix.satellites` même sans fix 2D
- **Linux**: tout est derrière `if (mode >= MODE_2D)` → sans fix 2D, rien n'est lu
- **Impact**: pas d'heure GPS ni de compteur satellites sans position fix
