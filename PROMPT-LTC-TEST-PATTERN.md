# Prompt Claude Code : Ajouter LTC Timecode au Test Pattern NDI

## Contexte

Le fichier `src/tools/ndi_test_pattern.cpp` génère un test pattern NDI avec une bille verte qui rebondit + un son sinusoïdal 440Hz optionnel. On veut remplacer/enrichir l'audio avec du **vrai LTC (Linear Timecode SMPTE 12M)** et ajouter une **barre de timecode visuelle** style clap broadcast.

Le projet `ltc-ndi-tool` (voir `~/Projets/ltc-ndi-tool/main.c`) contient déjà une implémentation de référence complète de génération LTC via **libltc**. Utilise-le comme référence pour l'encodage LTC.

## Objectif

Modifier `src/tools/ndi_test_pattern.cpp` pour :

### 1. Ajouter une option `--framerate` avec les framerates broadcast standard

Proposer les valeurs suivantes (menu à la CLI) :
- `23.976` (23.976 fps — cinéma NTSC / Film)
- `24` (24 fps — cinéma)
- `25` (25 fps — PAL/SECAM — **défaut**)
- `29.97` (29.97 fps — NTSC drop-frame)
- `29.97ndf` (29.97 fps — NTSC non-drop-frame)
- `30` (30 fps — progressive)
- `50` (50 fps — PAL haute cadence)
- `59.94` (59.94 fps — NTSC haute cadence)
- `60` (60 fps — progressive haute cadence)

Le `--fps` existant contrôle **le framerate vidéo NDI** (pour l'envoi). Le nouveau `--framerate` (ou `--tc-fps`) contrôle le **framerate du timecode LTC**. Par défaut le timecode suit le fps vidéo si c'est un framerate standard, sinon 25.

**Attention drop-frame** : pour 29.97 et 59.94, il faut gérer le flag drop-frame dans libltc (`LTC_TC_CLOCK` / bit drop-frame dans `LTCFrame`).

### 2. Générer l'audio LTC via libltc

Remplacer le sinus 440Hz par du **vrai signal LTC audio** :

- Utiliser `libltc` (déjà dispo dans le projet `ltc-ndi-tool`)
- Sample rate : 48000 Hz
- Volume : -3 dBFS (SMPTE 12M standard)
- Le timecode LTC démarre à l'heure système UTC (time of day) — cf. `clock_to_smpte()` dans `ltc-ndi-tool/main.c`
- **Canal 1 = LTC, Canal 2 = silence** (ou l'inverse, documenté)
- L'option `--no-audio` désactive l'audio entièrement
- Ajouter une option `--audio-440` pour garder l'ancien sinus 440Hz sur canal 2 (utile debug)

**Synchronisation critique** : chaque frame vidéo doit correspondre exactement à 1 frame LTC audio. Le nombre de samples audio par frame = `48000 / tc_fps`. Pour les framerates non-entiers (23.976, 29.97, 59.94), il faut un compteur de samples cumulé pour éviter la dérive (cadence pattern irrégulière, ex: 1601/1602 samples alternés pour 29.97).

### 3. Afficher un cadrant de timecode visuel — style clap broadcast professionnel

Dessiner dans le rendu vidéo un **cadrant de timecode réaliste** fidèle à l'esthétique des vrais claps timecode broadcast (Tentacle Sync, Ambient ACN, Denecke TS-3). L'image de référence est fournie : digits orange lumineux sur fond noir profond, aspect d'un module LED/LCD encastré.

**L'objectif n'est PAS un simple overlay texte. C'est de reproduire le look d'un vrai cadrant physique de timecode professionnel.**

**Position** : barre horizontale en bas de l'image, centrée horizontalement, légèrement au-dessus du bord inférieur (~85% de la hauteur).

**Look & Feel du cadrant** :

- **Fond du cadrant** : rectangle aux coins légèrement arrondis (rayon ~6px à 1080p), rempli noir profond (#0A0A0A), avec une bordure subtile gris sombre (#2A2A2A, 2px) pour donner l'effet d'un écran LCD/LED encastré. Le cadrant doit "flotter" visuellement sur le fond du test pattern.
- **Effet de profondeur** : ajouter une ombre portée douce sous le cadrant (quelques lignes de pixels semi-transparents en dessous et à droite) pour simuler un relief, comme un vrai module physique incrusté dans un clap.
- **Chiffres au format `HH:MM:SS:FF`** (ou `HH:MM:SS;FF` pour drop-frame, le `;` signale le drop-frame)
- **Police 7-segments fidèle et soignée** : dessiner les 7 segments pixel par pixel avec soin. Chaque segment doit avoir :
  - Des **extrémités biseautées/angulaires** (pas des rectangles plats — les vrais afficheurs 7-segments ont des segments en forme de losange/trapèze aux bouts). C'est ce détail qui fait la différence entre un rendu amateur et un rendu pro.
  - Une **épaisseur confortable** (4-5px à 1080p)
  - Un léger espacement (~1-2px) entre les segments adjacents (comme sur un vrai afficheur)
  - Les **segments éteints visibles** en gris très sombre (#1A1A1A) sur le fond noir — sur un vrai clap, on voit toujours les segments inactifs en filigrane, ça donne la profondeur LCD. Dessiner TOUS les 7 segments en gris sombre, puis par-dessus les segments actifs en orange.
- **Couleur des chiffres actifs** : orange chaud (#FF8C00) avec un **léger effet de glow/halo** (1-2px de #FF8C00 à ~25% opacité autour de chaque segment allumé) pour simuler la luminosité LED qui bave légèrement. Cet effet est subtil mais donne tout le réalisme.
- **Séparateurs `:`** (ou `;` drop-frame) : deux points empilés, même orange, légèrement plus petits que les chiffres. Le `;` en mode drop-frame a le point bas légèrement décalé vers la droite (convention broadcast visuelle).
- **Deux indicateurs ronds à droite** (style Tentacle Sync) : deux cercles orange pleins (~10px diamètre) empilés verticalement à droite du dernier chiffre, qui **clignotent alternativement** à chaque frame (frame paire = haut ON/bas OFF, frame impaire = inverse). C'est le "heartbeat" visuel qui confirme que le timecode tourne — exactement comme sur la photo de référence.
- **Label framerate discret** : sous le cadrant, en petits caractères gris clair (#888888), afficher le framerate actif, ex: `25 fps` ou `29.97 DF` ou `59.94 NDF`. Utiliser une mini-font bitmap 5x7 basique pour ce texte.

**Dimensions suggérées** (à 1080p, tout doit scaler proportionnellement à la résolution) :
- Largeur cadrant : ~55% de la largeur image
- Hauteur cadrant : ~90px (fond noir compris)
- Hauteur chiffres : ~60px
- Largeur chiffre : ~36px
- Épaisseur segments : 4-5px
- Espacement inter-chiffres : ~6px
- Espacement inter-groupes (autour des `:`) : ~14px
- Padding intérieur : 12px horizontal, 15px vertical
- Indicateurs ronds : 10px diamètre, positionnés 8px à droite du dernier chiffre

**Proportionnalité** : toutes ces dimensions sont relatives à la hauteur de l'image. À 720p, tout est réduit proportionnellement (factor = height/1080). À 4K, tout est agrandi. Formule : `dim_px = dim_1080p * (height / 1080.0)`.

**Le timecode affiché doit être IDENTIQUE au timecode LTC audio encodé pour cette frame.** C'est la contrainte #1. Pas d'approximation.

### 4. Architecture de la synchronisation

Le flow doit être :

```
Pour chaque frame N :
  1. Calculer le timecode SMPTE pour frame N (depuis le compteur, pas depuis clock_gettime à chaque frame)
  2. Encoder le timecode dans libltc → buffer audio LTC
  3. Dessiner le même timecode sur la vidéo (barre visuelle)
  4. Envoyer vidéo + audio NDI ensemble
  5. Incrémenter le compteur de frame
```

**NE PAS** utiliser `clock_gettime()` à chaque frame pour le timecode courant — ça crée de la dérive. Utiliser l'horloge système **uniquement au démarrage** pour initialiser le timecode, puis incrémenter frame par frame via `ltc_encoder_inc_timecode()`.

Le timecode NDI (`videoFrame.timecode` et `audioFrame.timecode`) doit rester `NDIlib_send_timecode_synthesize` (le SDK NDI s'en occupe), mais le **contenu LTC** et l'**affichage visuel** sont pilotés par notre propre compteur.

### 5. Dépendance libltc

Ajouter au `CMakeLists.txt` :

```cmake
# libltc (LTC timecode encoding/decoding)
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(LTC ltc)
endif()

# Fallback manual detection
if(NOT LTC_FOUND)
    find_library(LTC_LIBRARIES NAMES ltc)
    find_path(LTC_INCLUDE_DIRS ltc.h)
    if(LTC_LIBRARIES AND LTC_INCLUDE_DIRS)
        set(LTC_FOUND TRUE)
    endif()
endif()

if(LTC_FOUND)
    message(STATUS "Found libltc: ${LTC_LIBRARIES}")
else()
    message(WARNING "libltc not found - LTC timecode will not be available in test pattern")
    message(WARNING "Install with: brew install libltc (Mac) or sudo apt install libltc-dev (Linux)")
endif()
```

Et linker `ndi-test-pattern` avec `${LTC_LIBRARIES}`.

### 6. Interface CLI mise à jour

```
ndi-test-pattern [options]

Options:
  --name <n>            NDI source name (default: "Test Pattern LTC")
  --resolution <WxH>    Resolution (default: 1920x1080)
  --fps <N>             Video frame rate for NDI (default: 25)
  --tc-fps <rate>       Timecode frame rate (default: same as --fps if standard, else 25)
                        Values: 23.976, 24, 25, 29.97, 29.97ndf, 30, 50, 59.94, 60
  --no-audio            Disable all audio
  --audio-440           Add 440Hz sine on channel 2 (debug)
  --tc-start <HH:MM:SS:FF>  Start timecode (default: UTC time of day)
  --no-tc-display       Disable visual timecode bar (keep audio LTC only)
  -h, --help            Show help with framerate list
```

### 7. Contraintes techniques

- **Cross-platform** : doit compiler sur Linux (Ubuntu 22+) et macOS (Apple Silicon). Le code C++ existant est déjà cross-platform.
- **Pas de dépendance font** : la police 7-segments est dessinée en dur (tableau de segments). C'est une contrainte forte — tout le rendu typographique est fait pixel par pixel.
- **Performance** : le rendu des chiffres ne doit pas ralentir le framerate. Le tableau de segments est statique, le dessin est trivial.
- **LTC standard** : respecter SMPTE 12M. Volume -3 dBFS. Le signal LTC est un signal Manchester biphase.

### 8. Tests de validation

Après implémentation, vérifier :
1. `./ndi-test-pattern --tc-fps 25` → la bille rebondit, le cadrant affiche HH:MM:SS:FF en orange avec segments éteints visibles, l'audio contient du LTC décodable
2. `./ndi-test-pattern --tc-fps 29.97` → le séparateur est `;` (drop-frame), le timecode saute les frames 0-1 à chaque minute non-multiple-de-10
3. `ltc-ndi-tool extract "Test Pattern LTC"` → doit décoder le LTC et afficher les mêmes valeurs que l'écran
4. Pas de dérive entre audio LTC et affichage visuel après 10+ minutes de fonctionnement
5. Les indicateurs ronds clignotent alternativement à chaque frame
6. À différentes résolutions (720p, 1080p, 4K), le cadrant scale proportionnellement et reste lisible

## Fichiers à modifier

1. `src/tools/ndi_test_pattern.cpp` — le gros du travail
2. `CMakeLists.txt` — ajouter la dépendance libltc et le link
3. Créer `src/tools/tc_font.h` — tableau de la police 7-segments avec géométrie biseautée des segments

## Référence

- Code LTC existant : `~/Projets/ltc-ndi-tool/main.c` (génération, encodage, décodage LTC complet)
- Image de référence style : clap Tentacle Sync (digits orange sur fond noir, format HH:MM:SS:FF, deux indicateurs ronds à droite)
- Modèles de référence look : Tentacle SYNC E, Ambient ACN Lockit, Denecke TS-3 — tous partagent le même ADN visuel : digits 7-segments orange/rouge sur fond noir, segments éteints visibles, aspect module encastré
- SMPTE 12M standard pour les conventions de timecode
