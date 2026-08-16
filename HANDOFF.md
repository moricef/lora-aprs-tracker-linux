# Handoff — branche refactor/map-single-sprite-viewport

Port Linux (cible Raspberry Pi 4B, dev sur Odroid) d'un firmware LoRa APRS ESP32.
Rendu carto vectoriel maison : `src/map/map_vector.cpp` (PMTiles/MVT), labels
`src/map/map_labels.cpp`, génération tuiles `tilemaker/process-aprs.lua`.

## Étalons de référence (lire la SOURCE, jamais de mémoire ni de résumé)
- Routes / eau / places, ASPECT et HIÉRARCHIE = OSM-carto, clone local
  `/home/fab2/Developpement/LoRa_APRS/openstreetmap-carto` (`style/*.mss`, `project.mml`).
- Schéma des tuiles = OpenMapTiles, via `process-aprs.lua` (base : process.lua tilemaker).
- Firmware de réf = `/home/fab2/Developpement/LoRa_APRS/CA2RXU/LoRa_APRS_Tracker-async`.
- pmtiles déployées : `/home/fab2/.local/share/LoRa_Tracker/VectMaps/FranceSud/FranceSud.pmtiles`
  (zoom natif **z6-14** ; z15-17 = surzoom dans le renderer).

## Deux endroits, deux rebuilds (cause de "aucune diff")
- Changement dans `map_vector.cpp` / `*.cpp` → **recompiler l'app**.
- Changement dans `process-aprs.lua` → **régénérer les tuiles** (tilemaker), sinon les
  anciennes tuiles restent. L'utilisateur fait build et régénération lui-même.

## Travaux de la session (branche, RIEN n'est commité)

### map_vector.cpp
- **Îles (even-odd)** : `fillPolyMulti()` + `PolyCollector` accumule tous les rings
  (`ringsX/ringsY`) et `flush()` (appelé après `decode_polygon_geometry` dans
  `renderPolyLayer`). Remplace le "remplir tous les rings". VALIDÉ visuellement (photos).
- **Casing ponts** : passe dédiée à la fin du bloc routes, **ordonnée par `layer` OSM
  croissant** (lit `layer` via `readRoad`, niveaux distincts triés). Par niveau :
  casing (noir, `fw + 2*bridgeCasingW(cls,z)`, `endTrim`, **sans** disque de jointure)
  puis fill. Traceur `drawThickPolyButt` (bouts plats + jointures rondes optionnelles).
  `bridgeCasingW(cls,z)` = `roads.mss:271-288`. NON vérifié à l'écran.
- **Liseré routes** (passe outline, majeures wMul≥1.5) : `fw + 2*roadCasingW(key,z)`
  au lieu de `fw+2` fixe. `roadCasingW` = `@major-casing-width`/`@secondary-casing-width`
  de `roads.mss`. NON vérifié.
- **Tailles labels places** : `placeSizePx(cls,z,score)` portée EXACTEMENT de
  `placenames.mss` (city :196-214/249-278, town :287-316, suburb :321-352,
  village :355-392, quarter/hamlet :397-455, country :7-42, state :51-87) +
  `kPlaceSizeBoost = 3` (décalage écran 7" ~169 DPI, HORS-OSM, déclaré). Polices 10-18
  créées dans `initLabelFonts`. NON vérifié.

### process-aprs.lua
- **Canal** (bloc water ~ligne 627) : surfaces `water=canal`/`waterway=canal` →
  `MinZoom(12)` fixe au lieu de `SetMinZoomByArea` (qui enterrait le canal étroit à z14).
  HORS aire-gating, déclaré. NÉCESSITE RÉGÉNÉRATION. Le canal du Midi a bien une surface
  `natural=water,water=canal` en plus de la ligne `waterway=canal`.

## Diagnostics ouverts, NON corrigés
- **Routes trop larges à z14** : après le fix casing, reste un excès sur le **fill**.
  Mesuré : secondary z14 mien=6 / OSM-carto=5. primary z14 mien=7 / OSM **non lu**
  (lecture coupée). Constat utilisateur : oranges (primary) trop larges, jaunes
  (secondary/tertiary) quasi OK → suspecter la ligne primary du fill. La table de fill
  (`roadWidthForZoom`) a été ajustée AVEC l'utilisateur en cours de session :
  **ne pas la modifier sans accord explicite**.
- Casing ponts, labels places, canal, casing routes : à valider après build/régen.

## Règles dures (CLAUDE.md + cette session)
- Lire la source OSM AVANT d'écrire la moindre valeur. Ne JAMAIS fabriquer un chiffre
  "plausible". Toute valeur hors OSM/firmware = déclarée dans le message de commit
  (ligne « Hors-OSM/firmware: »), jamais dans un commentaire de code.
- Commentaires code = anglais, WHY-only, sans provenance/comparaison.
- Messages de commit = français, Conventional Commits. Pas de "Co-Authored-By".
- Si un point prend plus de 2 tentatives → s'arrêter et demander.

## Ratés de l'instance précédente (à ne pas reproduire)
- Casing ponts repris 5+ fois (ovoïde → rails → passe dédiée → largeurs → endTrim →
  disque de jointure) au lieu de diagnostiquer d'abord.
- Faux aveu INVENTÉ sur les tailles `town` ("diverge d'OSM" avec des chiffres OSM
  fabriqués) — alors que town était déjà exact. Invention en cours d'aveu d'invention.
- Affirmation INVENTÉE "MapTiler dessine les routes plus fines qu'OSM-carto" — fausse,
  les deux styles donnent la même largeur de boulevard.
- Diagnostic initial "surzoom" sur le canal, à côté : le vrai sujet était l'aire-gating
  du lua.

## Tics de langage à BANNIR (l'utilisateur ne les supporte pas)
- Annoncer sa vertu : « je regarde X », « je ne devine pas », « je n'invente pas »,
  « sans te raconter d'histoire ».
- « Ma reco », « vrai correctif », « pour être honnête ».
- Finir par « Tu veux que je… ? » / « Je le fais ? » en boucle au lieu d'agir ou de
  proposer une fois.
- Auto-flagellation performative, ton de copain de paillasse, méta-discours sur
  soi-même.
- Donner raison par réflexe ("tu as raison") puis fabriquer des chiffres pour appuyer.
Attendu : concis, concret, technique. Affirmer seulement ce qui est vérifié dans la
source ; marquer explicitement ce qui ne l'est pas.
