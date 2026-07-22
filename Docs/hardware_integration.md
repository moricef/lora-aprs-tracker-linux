# Intégration matérielle — Raspberry Pi 4B

Passage du prototype filaire à un ensemble utilisable sur le terrain.
Complète `Docs/pinout-raspberry-pi4-ht-ra62.html` (câblage), qui reste la
référence pour les broches.

Relevés du 22/07/2026 sur `adrasec09@192.168.1.210`.

---

## 1. État constaté du montage

| Élément | État relevé |
|---|---|
| GPS | u-blox 7 (SW 7.03, HW 00040007) sur `/dev/serial0` → `ttyS0`, 9600 bps, piloté par gpsd 3.25 |
| Bluetooth | intact — le mini-UART sert au GPS, le PL011 `ttyAMA0` reste au BT |
| Écran | Waveshare 7", `card0-HDMI-A-1`, mode 1024×600 |
| Tactile | USB `D-WAV WS170120`, derrière un hub VIA Labs 2109:3431 |
| LoRa | HT-RA62 / SX1262 sur SPI0, `dtparam=spi=on` |
| KMS | `dtoverlay=vc4-kms-v3d`, `disable_fw_kms_setup=1`, `max_framebuffers=2` |

Le hub USB entre le tactile et le Pi est un point de panne supplémentaire sans
contrepartie ; à supprimer si le nombre de ports le permet.

## 2. Acquis côté logiciel

Rien à porter : la cible Pi 4B est le défaut du build.

- `Makefile` : `BOARD ?= rpi4` → `-DBOARD_RPI4`.
- `src/lora_utils.cpp:18-30` : broches BCM 8 / 23 / 25 / 24 (CS, DIO1, RST, BUSY).
- `src/linux_hal.cpp:14-43` : la base sysfs est résolue à l'exécution en cherchant
  le gpiochip dont le label est `pinctrl-bcm2711`, donc aucun offset codé en dur.

La dernière ligne du document de câblage HTML, qui annonce que les GPIO Odroid
subsistent dans `lora_utils.cpp`, est périmée.

---

## 3. Chantiers matériels

### 3.1 Alimentation et arrêt propre

Pi 4B + écran 7" : de l'ordre de 6 à 8 W en pointe. La contrainte n'est pas la
puissance mais la **coupure brutale**, qui corrompt la carte SD.

- Convertisseur 5 V / 3 A qui tient les appels de courant sans s'effondrer.
- Bouton d'arrêt via `dtoverlay=gpio-shutdown`.
- Sur batterie : détection de tension basse déclenchant un arrêt automatique
  avant seuil critique.

### 3.2 Stockage

Même sujet vu de l'autre côté. Deux options :

- overlayfs en lecture seule sur la racine, partition de données séparée en
  écriture ;
- SSD USB à la place de la carte SD.

### 3.3 Carte LoRa

Les liaisons Dupont ne tiendront pas en mobile. Une plaque à trous en HAT sur le
connecteur 40 broches, portant le HT-RA62 et l'embase SMA, suffit — à condition
de garder les pistes SPI courtes et un plan de masse continu dessous.

### 3.4 Thermique

Dissipateur obligatoire en boîtier fermé : le V3D monte en charge pendant le
rendu de la carte.

---

## 4. CEM — brouillage du GNSS

Le point le plus délicat, et déjà rencontré sur le CrowPanel : brouillage
atténué — mais pas supprimé — par une feuille d'aluminium prise en sandwich
entre deux bandes de ruban adhésif large, reliée à la masse, séparant le module
GNSS de l'écran.

### 4.1 Pourquoi un tel blindage ne fait qu'une partie du travail

À 1 575,42 MHz, λ = 19,0 cm.

- **Les fentes.** Deux feuilles qui se recouvrent sous du ruban adhésif ne sont
  pas en contact électrique continu. Une fente rayonne dès qu'elle approche
  λ/2 = 9,5 cm. Un recouvrement non soudé de cette dimension est une antenne
  fente, pas un écran.
- **La liaison de masse ponctuelle.** Reliée en un seul point par un fil, la
  feuille devient un élément résonnant : λ/4 = 4,8 cm. Un fil de masse de cette
  longueur se comporte en self, pas en masse. Il faut de la tresse large et
  courte, en plusieurs points répartis.
- **Le couplage conduit.** Le bruit remonte aussi par l'alimentation du module
  GNSS et par la ligne série. Un blindage sans filtrage de ces conducteurs ne
  traite que la moitié du chemin : perles de ferrite sur le Vcc et sur la ligne
  série, découplage 100 nF au ras du module.

### 4.2 Les quatre chemins de couplage

Une antenne active déportée traite le premier, pas les trois autres.

1. **Rayonné vers l'élément rayonnant** — résolu en sortant l'antenne du volume
   bruyant.
2. **Mode commun sur le blindage du coax.** Le câble traverse la zone polluée et
   conduit les courants captés jusqu'à l'entrée du récepteur. Self de mode commun
   (ferrite passante) au ras de la paroi, et contact **360°** du corps SMA sur la
   paroi. Une embase montée isolée sur du plastique n'apporte rien.
3. **Bias-tee.** Le LNA de l'antenne active est alimenté depuis le rail du
   récepteur, donc depuis le Pi, et le bruit conduit entre directement dans
   l'étage le plus sensible de la chaîne. Filtrage LC sur le bias, ou
   alimentation séparée du LNA.
4. **Position du filtre SAW.** Beaucoup d'antennes actives placent le SAW *après*
   le LNA. Un brouilleur fort hors bande sature alors l'ampli avant tout
   filtrage : compression, désensibilisation, effondrement du C/N0 sans qu'aucune
   raie ne soit dans la bande utile. Choisir une antenne dont le SAW est **avant**
   le LNA ; les bonnes en ont un de chaque côté.

### 4.3 Mode vidéo du Pi — calcul des harmoniques

Mode actif relevé (`/sys/kernel/debug/dri/*/state`) :

```
mode: "1024x600": 60 50250 1024 1068 1156 1344 600 603 609 625
```

Horloge pixel 50,25 MHz, htotal 1344, vtotal 625 (rafraîchissement réel
59,8 Hz).

| Harmonique | Fréquence | Bande GPS L1 C/A (1 574,40 – 1 576,44 MHz) |
|---|---|---|
| 31 × 50,25 | 1 557,75 MHz | 16,6 MHz sous la bande |
| 32 × 50,25 | 1 608,00 MHz | 31,6 MHz au-dessus |

La bande L1 passe entre les deux. **Déplacer le timing vidéo pour sortir
l'harmonique de la bande est donc inutile ici** — elle en est déjà sortie. Ce
levier ne redeviendrait pertinent qu'en changeant de résolution ou d'écran ; le
calcul est alors à refaire.

Ce qui subsiste du HDMI est du bruit large bande : étalement spectral dû aux
données TMDS (sérialisation à 10 × l'horloge pixel) et mode commun sur le câble.
Se traite par un câble court et correctement blindé, ferrite aux deux extrémités.

Note : ce calcul est propre au Pi. Le CrowPanel n'a pas de HDMI — son écran est
piloté en RGB parallèle ou SPI depuis l'ESP32 — et la même analyse doit y être
refaite avec ses propres horloges.

### 4.4 Mesurer avant de traiter

gpsd expose le C/N0 par satellite (champ `ss` des messages `SKY`), ce qui rend le
brouillage chiffrable sans instrument supplémentaire.

Méthode : moyenner `ss` sur les satellites au-dessus de 30° d'élévation, puis
faire varier **une seule chose à la fois** — écran débranché / rebranché,
application au repos / en rendu carte, blindage en place / retiré, antenne à
différentes distances. Le delta en dB chiffre le coût de chaque source.

Relevé de référence du 22/07/2026 (antenne en place, système en fonctionnement) :

| PRN | Élévation | `ss` |
|---|---|---|
| 21 | 69° | 39 |
| 30 | 68° | 23 |
| 5 | 63° | 31 |
| 20 | 47° | 24 |
| 14 | 40° | 22 |
| 22 | 28° | 21 |
| 7 | 26° | 13 |

Deux satellites à ~68° d'élévation devraient afficher des valeurs voisines, et
plutôt de l'ordre de 42 à 48 dB-Hz en ciel dégagé avec une antenne correcte. Les
23 dB-Hz du PRN 30 sont à comprendre : antenne, placement intérieur, ou
brouillage — la mesure comparative tranchera.

### 4.5 Ordre des interventions

1. Mesurer le C/N0 par configuration.
2. Distancer l'antenne. En champ lointain l'atténuation suit 1/d : 20 cm
   supplémentaires valent 6 dB, davantage qu'une amélioration de blindage à ce
   stade.
3. Traiter les entrées conduites (mode commun du coax, bias-tee, ligne série).
4. Blinder en dernier.

Le plancher de bruit large bande résiduel — PMIC à découpage du Pi, SDRAM,
contrôleur USB — ne se supprime pas, il se distance.

---

## 5. À trancher

La forme du coffret conditionne fixation de l'écran, sorties d'antennes,
ventilation et autonomie visée : sacoche portable pour les courses en montagne,
poste embarqué véhicule, ou valise posée sur table.
