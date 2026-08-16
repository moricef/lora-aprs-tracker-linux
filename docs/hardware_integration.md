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

Le point le plus délicat de l'intégration, et le retour d'expérience du
CrowPanel 3.5" en donne la mesure.

### 4.1 Le précédent CrowPanel

Empilement, de la face utilisateur vers le fond :

```
carte CrowPanel (écran + ESP32-S3)
plaque acrylique ajourée
feuille d'aluminium 103 × 63 mm, ajourée pour les connecteurs et le module LoRa
modules LoRa, MAX17048, u-blox M8N
fond nylon
compartiment batteries, antenne GNSS, haut-parleur
plaque acrylique de fermeture
```

Boîtier nylon imprimé (`OpenSCAD/crowpanel35.scad`) : 101 × 63 × 34,5 mm, patch
GNSS 25 × 25 sur berceau incliné à −63°, liaison IPEX, batterie 88 × 41 × 22.

**Résultat mesuré sur le terrain :**

| Configuration | Comportement |
|---|---|
| sans la feuille | aucun fix, même après 4 h, même en roulant |
| avec la feuille | fix obtenu, mais très long à venir et fragile |

C'est l'observation la plus importante du dossier. Un M8N acquiert à froid vers
33 dB-Hz et poursuit jusque vers 25. N'accrocher jamais signifie que tous les
satellites étaient sous le seuil d'acquisition : il ne s'agit pas d'une
dégradation de quelques décibels mais d'un aveuglement. La feuille a fait
franchir ce seuil — de l'ordre de 6 à 10 dB — et il en manque autant pour un fix
rapide et stable.

L'antenne est pourtant déjà reportée à l'arrière du boîtier, et cela ne suffit
pas — ce qui n'a rien de surprenant : la limite entre champ proche et champ
lointain se situe à λ/2π, soit **3,0 cm** à 1 575 MHz. Dans un objet de 10 cm,
l'antenne reste partout dans la zone de transition des sources. Déplacer de deux
ou trois centimètres ne produit aucune atténuation exploitable face à un
brouillage de 20 ou 30 dB au-dessus du seuil.

Le CrowPanel interdit donc les trois leviers à la fois : pas d'éloignement
réel, pas de plan de masse étendu, pas de déport hors du volume. La feuille était
le seul levier disponible dans cette géométrie, et elle a rendu l'objet
fonctionnel.

Conséquence directe pour le Pi : dans un volume compact, le brouillage d'un
ensemble calculateur + écran est du niveau qui empêche purement et simplement le
fonctionnement, pas du niveau qui le dégrade. Le Pi 4B est un émetteur plus
sévère qu'un ESP32-S3 (CPU 1,5 GHz, LPDDR4, USB, HDMI, plusieurs découpages).
L'antenne déportée n'y est donc pas une amélioration mais une condition de
fonctionnement — et « déportée » doit s'entendre **hors du boîtier**, au bout
d'un coax, à plusieurs dizaines de centimètres. Un compartiment arrière ne
compte pas comme un déport.

### 4.2 Ce qui limite encore ce blindage

Le placement est correct — plan complet aux dimensions du boîtier, interposé
entre la couche bruyante et toute la couche RF. Les limites sont ailleurs.

À 1 575,42 MHz, λ = 19,0 cm.

- **Les conducteurs traversent l'écran.** C'est le point dominant. SPI du LoRa,
  I2C du MAX17048, UART du M8N viennent tous de l'ESP32, côté bruyant, et passent
  de l'autre côté par les ouvertures. Le bruit ne franchit plus la feuille par
  l'air, il la contourne par les fils, puis rayonne à nouveau en dessous, à
  quelques centimètres de l'antenne. Un écran percé de conducteurs non filtrés
  cesse d'être un écran. Traitement : perle de ferrite sur chaque conducteur **au
  point de traversée**, et à défaut nappes plaquées contre la feuille sur tout
  leur trajet.
- **Les ouvertures elles-mêmes ne sont pas en cause.** Une ouverture ne rayonne
  significativement qu'en approchant λ/2 = 9,5 cm. Les perçages de connecteurs et
  du module LoRa restent électriquement petits.
- **La dimension de la plaque est défavorable.** 103 mm correspond à λ/2 pour
  1 455 MHz : la résonance fondamentale d'une plaque flottante de ce format tombe
  à moins de 10 % de la bande L1, et une plaque résonnante réémet efficacement ce
  qu'elle capte. Casser cette résonance demande un contact surfacique en
  plusieurs points répartis.
- **Un fil de masse n'y suffit pas.** Ordre de grandeur usuel, 1 nH par
  millimètre : 10 mm de fil valent ~100 Ω à 1 575 MHz, 50 mm ~500 Ω. Élargir en
  tresse ne gagne qu'un facteur 2 à 3, l'inductance variant logarithmiquement
  avec la largeur. À ces fréquences on ne raccorde pas une masse par un
  conducteur, on met des surfaces en contact. Mal raccorder est en outre pire que
  ne pas raccorder : un fil qui rejoint un point de masse arbitraire peut injecter
  le bruit de ce plan dans la feuille et la transformer en radiateur.
- **Le couplage conduit par l'alimentation.** Un 100 nF est en place entre VCC et
  GND du module. Il traite l'ondulation basse fréquence, pas la bande GNSS : avec
  une inductance de montage de l'ordre du nanohenry, sa résonance série tombe vers
  16 MHz, au-delà de laquelle il devient inductif et présente une dizaine d'ohms à
  1 575 MHz. À cette fréquence tous les MLCC se valent — l'impédance est fixée par
  l'inductance de montage, pas par la capacité — donc changer la valeur
  n'apporterait rien. Le levier est la **perle de ferrite en série**, qui présente
  quelques centaines d'ohms entre 100 MHz et 1 GHz. Ferrite en série puis
  condensateur : les deux forment un filtre, seuls ils ne font qu'une moitié du
  travail.

### 4.3 Identifier la source dominante avant de raffiner

Les contributions s'additionnent en puissance : si trois sources sont espacées de
10 dB, la plus forte fixe le niveau à elle seule. Avant tout travail de
blindage, il faut savoir si une source domine.

Suspects habituels sur ce type de carte, par ordre de nuisance constatée
ailleurs : convertisseur boost du rétroéclairage (commutation à quelques
centaines de kHz, harmoniques jusqu'au gigahertz, boucle de courant rayonnante),
interface et nappe de l'écran, puis Wi-Fi/BT et LoRa.

Protocole : couper une source à la fois — rétroéclairage seul, puis écran entier,
puis Wi-Fi et BT, puis LoRa en veille — et relever le C/N0. Le M8N le fournit
dans les trames `$GPGSV`, aucun instrument supplémentaire n'est nécessaire.

Les deux issues sont exploitables : un gain net à une coupure désigne un coupable
et un traitement ciblé ; 1 à 2 dB partout signifie qu'aucun filtrage ciblé ne
suffira et que seuls le blindage et l'éloignement comptent.

### 4.4 Les chemins d'entrée du récepteur

Un récepteur GNSS ne se protège pas en blindant le récepteur : le module M8N est
déjà sous capot métallique. Tout ce qui entre par le connecteur d'antenne est
traité comme du signal — le corrélateur ne distingue pas une harmonique d'horloge
d'un satellite, il voit du bruit dans sa bande et le C/N0 s'effondre. Seuls
comptent donc :

1. **Le patch.** Il capte ce qui l'entoure. Un patch 25 × 25 a un plan de masse
   intégré à sa propre dimension, soit 0,13 λ, alors que les fiches techniques en
   spécifient 50 à 70 mm de côté. En dessous, le gain baisse et le rapport
   avant/arrière se dégrade : l'antenne devient sensible par l'arrière, donc vers
   l'électronique. Sur le CrowPanel ce n'est pas corrigeable — le berceau fait
   26,0 × 26,4 mm, l'empreinte exacte du patch, sans marge pour étendre le plan de
   masse. Sur le Pi, un plan de 60 à 70 mm se décide au dessin du boîtier.
2. **Le câble d'antenne.** Son blindage capte en mode commun sur tout son trajet
   dans le volume bruyant et conduit directement à l'entrée RF. Self de mode
   commun au ras de la paroi, contact **360°** du corps SMA sur la paroi — une
   embase montée isolée sur du plastique n'apporte rien.
3. **Le bias-tee**, si l'antenne est active : le LNA est alimenté depuis le rail
   du calculateur, et ce bruit conduit entre dans l'étage le plus sensible de la
   chaîne. Filtrage LC sur le bias, ou alimentation séparée.
4. **La position du filtre SAW.** Beaucoup d'antennes actives placent le SAW
   *après* le LNA. Un brouilleur fort hors bande sature alors l'ampli avant tout
   filtrage : compression, désensibilisation, effondrement du C/N0 sans qu'aucune
   raie ne soit dans la bande utile. Choisir une antenne dont le SAW est **avant**
   le LNA ; les bonnes en ont un de chaque côté.

### 4.5 Mode vidéo du Pi — calcul des harmoniques

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

### 4.6 Mesurer avant de traiter — côté Pi

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

### 4.7 Ordre des interventions

1. Mesurer le C/N0 par configuration, et identifier une éventuelle source
   dominante.
2. Sortir l'antenne du boîtier, au bout d'un coax, sur une distance qui compte —
   plusieurs dizaines de centimètres, pas un compartiment voisin. En champ
   lointain l'atténuation suit 1/d.
3. Lui donner un plan de masse de 60 à 70 mm, décidé au dessin du support.
4. Traiter les entrées conduites : mode commun du coax, bias-tee, alimentation et
   ligne série du récepteur.
5. Blinder en dernier, et seulement avec des traversées filtrées.

Le plancher de bruit large bande résiduel — PMIC à découpage du Pi, SDRAM,
contrôleur USB — ne se supprime pas, il se distance.

L'expérience du CrowPanel fixe l'enjeu : dans un volume où les points 2 et 3 sont
impossibles, on passe de « aucun fix en 4 heures » à « fix lent et fragile ». Sur
le Pi, ces deux points sont accessibles, et c'est là que se joue la différence
entre un objet qui se positionne et un objet qui n'y arrive pas.

---

## 5. À trancher

La forme du coffret conditionne fixation de l'écran, sorties d'antennes,
ventilation et autonomie visée : sacoche portable pour les courses en montagne,
poste embarqué véhicule, ou valise posée sur table.
