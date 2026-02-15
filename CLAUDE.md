# CLAUDE.md — NDI Bridge

## Projet

NDI Bridge cross-platform — permet de faire transiter du NDI entre subnets distincts via un bridge H.264/UDP.
Ce produit n'existe pas chez NewTek pour Mac (seulement Windows).

### Vision produit (décidée le 29 jan 2026)

**Un seul codebase C++, trois plateformes : Mac, Linux, Windows.**

La version Swift (ndi-bridge-mac) est vouée à être remplacée par ce codebase C++ unifié.
Le seul avantage de Swift était VideoToolbox (encodage hardware), mais FFmpeg peut
utiliser VideoToolbox sur Mac via le codec `h264_videotoolbox`. Donc le C++ couvre tout.

**Différenciateurs vs NDI Bridge officiel (NewTek/Vizrt) :**
1. **Mac support** — n'existe pas chez NewTek
2. **Cloud/headless** — CLI scriptable, déployable sur EC2 sans écran
3. **Contrôle** — bitrate, codec, port, buffer exposés (vs boîte noire NewTek)
4. **Métrologie broadcast** — métriques interprétables directement en frames (latence, drops, décodage), grâce à une chaîne d'horloges synchronisées (chrony). Voir § "Métrologie broadcast" ci-dessous

**Décision : PAS de WSL2 pour Windows.** Le C++ compile nativement sur Windows
(WinSock au lieu de POSIX sockets, quelques `#ifdef _WIN32`). WSL2 ajoute de la
complexité (NAT interne, mDNS cassé, déploiement lourd) sans bénéfice.

**Décision : PAS d'Electron.** Si GUI il faut, soit SwiftUI (Mac only), soit un
serveur web intégré au binaire (`ndi-bridge --web-ui` → localhost:8080).

### Architecture

- **Host** (Mac, Linux ou Windows) : capture les sources NDI locales, encode en H.264, envoie par UDP
- **Join** (Mac, Linux ou Windows) : reçoit le flux bridgé, décode, et republie en NDI local
- Le Join peut tourner sur : VM Parallels, serveur cloud, EC2, PC Windows
- **Les deux modes sont cross-platform**, le code est le même
- Seule différence par plateforme : le choix de l'encodeur FFmpeg
  - Mac : `h264_videotoolbox` (hardware, **FAIT** v2.0)
  - Windows : `h264_nvenc` (NVIDIA GPU) ou `h264_qsv` (Intel) avec fallback libx264 (**FAIT** v2.1)
  - Linux/fallback : `libx264` (software, actuel)

### Port Windows — **FAIT** (v2.1)

Le port Windows est dans `src/common/Platform.h` qui abstrait les différences :
```
POSIX socket()      → WinSock WSAStartup() + socket()    (Platform.h)
poll()              → WSAPoll()                            (platform_poll())
close(fd)           → closesocket(fd)                      (platform_close_socket())
__builtin_bswap     → _byteswap (MSVC)                    (platform::bswap*)
clock_gettime       → GetSystemTimePreciseAsFileTime       (platform::wallClockNs())
fcntl O_NONBLOCK    → ioctlsocket FIONBIO                  (platform_set_nonblocking())
errno/strerror      → WSAGetLastError/FormatMessage         (platform_socket_errno/strerror())
```
Tout le reste (FFmpeg, NDI SDK, Protocol, Video*, JoinMode, HostMode) compile tel quel.
CMakeLists.txt ajoute `ws2_32` sur Windows. FindNDI.cmake cherche le SDK Windows.

### Zones d'ombre non adressées

1. **Traversée NAT** : en WAN sans Tailscale, le join doit avoir un port ouvert.
   Facile sur EC2 (security group), compliqué chez un particulier (port forwarding).
2. **Sécurité** : flux UDP en clair, pas de chiffrement ni d'authentification.
   OK sur réseau privé ou Tailscale, problématique en WAN public.
3. **Reconnexion** : si le réseau coupe, le join doit attendre un nouveau keyframe.
   Pas de mécanisme de détection/reconnexion automatique.
4. **Multi-sources** : 1 bridge = 1 source = 1 port. Pour 4 caméras il faut 4 instances.

### Roadmap

1. ~~Ajouter `h264_videotoolbox` dans VideoEncoder.cpp~~ — **FAIT** (v2.0, auto-detect + fallback)
2. ~~Encodage async (thread dédié)~~ — **FAIT** (v2.0, queue bornée 3 frames)
3. ~~Fix exclude pattern~~ — **FAIT** (v2.0, --source bypasse le filtre)
4. Retester performance EC2→Mac avec hwaccel + async
5. ~~Port Windows (ajouter `#ifdef _WIN32` dans Network*)~~ — **FAIT** (v2.1, Platform.h + NVENC + CMake)
6. Deprecate ndi-bridge-mac (Swift) une fois le C++ complet

### Viewers

Les deux viewers (Linux SDL2 et macOS natif) sont **notre code**, pas des outils officiels NewTek.

- **ndi-viewer (Linux)** : `src/tools/ndi_viewer.cpp` — SDL2 + SDL2_ttf, PTZ, fullscreen
- **ndi-viewer-mac** : projet séparé dans `~/Projets/ndi-viewer-mac/` — Swift/AppKit natif

## NDI SDK

### macOS

Le SDK est installé dans `/Library/NDI SDK for Apple/`.
- Headers : `/Library/NDI SDK for Apple/include/`
- Lib : `/Library/NDI SDK for Apple/lib/macOS/libndi.dylib`
- **Ne pas utiliser** `/usr/local/lib/libndi.dylib` (ancienne version)

Le `cmake/FindNDI.cmake` utilise `NO_DEFAULT_PATH` sur macOS pour forcer le SDK récent.

### Linux

SDK installé en system (`/usr/include/`, `/usr/lib/`) ou dans `~/NDI SDK for Linux/`.

## Configuration NDI

Fichier : `~/.ndi/ndi-config.v1.json`

Points critiques :
- `tcp.recv.enable` doit etre `true` pour les cameras BirdDog (qui envoient en TCP)
- `adapters.allowed` : laisser VIDE (`""`) — ne pas filtrer les adaptateurs (voir bug NWI ci-dessous)

## BUG CRITIQUE macOS : interface réseau dédiée NDI et routage unicast

### Le problème

Sur macOS, quand un adaptateur Ethernet (USB ou Thunderbolt) est configuré en IP manuelle
sur un sous-réseau isolé (ex: 10.0.0.21/16 pour un réseau de caméras NDI), **sans passerelle/routeur**,
macOS exclut cette interface de sa liste d'interfaces réseau actives (NWI = Network Workflow Information).

**Conséquence** : le kernel refuse d'envoyer du trafic unicast (TCP/UDP) via cette interface,
même si la route existe dans la table de routage. `sendto()` retourne `EHOSTUNREACH` (errno 65, "No route to host").

### Pourquoi la découverte NDI fonctionne quand même

La découverte NDI utilise mDNS (multicast DNS, port 5353). Le multicast opère en couche 2 (liaison)
et ne passe pas par le mécanisme NWI. Donc les sources NDI sont **découvertes** normalement.
Mais quand le SDK tente d'ouvrir une connexion TCP/UDP pour recevoir les données vidéo,
le kernel bloque car l'interface n'est pas dans le NWI. Résultat : `connections=0` en permanence.

### Symptômes

- `NDIlib_find_get_current_sources()` trouve les caméras (discovery OK)
- `NDIlib_recv_connect()` ne retourne pas d'erreur
- `NDIlib_recv_get_no_connections()` reste à 0 indéfiniment
- `NDIlib_recv_capture_v2()` ne retourne que `NDIlib_frame_type_none` (type=0)
- `ping` vers les caméras retourne "No route to host"
- `ifconfig` montre l'interface UP avec la bonne IP et le bon masque — tout semble correct
- `route get <ip_camera>` confirme que la route existe via la bonne interface
- `arp -a` montre les adresses MAC des caméras — la couche 2 fonctionne

### Diagnostic

```bash
# Vérifier si l'interface est dans le NWI
scutil --nwi
# Si l'interface NDI (ex: en7) n'apparaît PAS dans "Network interfaces:", c'est le bug.
```

### Cause racine

`scutil --nwi` ne liste que les interfaces ayant un routeur configuré.
Une interface en IP manuelle sans routeur est exclue du NWI.
Le kernel macOS utilise le NWI pour autoriser le trafic unicast sortant.
Pas dans le NWI = pas de trafic unicast = pas de connexion NDI data.

### Solution

Configurer un routeur sur l'interface, même si le réseau NDI est isolé (pas d'accès internet).
Utiliser l'IP du switch ou d'un autre équipement sur le réseau comme passerelle :

```bash
# Configurer l'adaptateur avec un routeur (ici 10.0.0.1 = switch Netgear)
networksetup -setmanual "<NOM_SERVICE>" <IP> <MASQUE> <ROUTEUR>
# Exemple :
networksetup -setmanual "AX88179A" 10.0.0.21 255.255.0.0 10.0.0.1
```

**ATTENTION** : ajouter un routeur sur l'interface NDI peut la rendre prioritaire pour TOUT le trafic
(y compris internet). Il faut impérativement régler l'ordre des services réseau
pour que l'interface internet (Wi-Fi, iPhone USB, Thunderbolt...) reste prioritaire :

```bash
# Mettre l'interface internet EN PREMIER, l'adaptateur NDI EN DERNIER
networksetup -ordernetworkservices "Wi-Fi" "iPhone USB" "AX88179A"
```

### Vérification

```bash
# 1. Vérifier que l'interface NDI est dans le NWI
scutil --nwi
# Doit afficher : "Network interfaces: en0 en7 ..." (en7 présent)

# 2. Vérifier que internet fonctionne toujours
ping -c 1 google.com

# 3. Vérifier que les caméras sont joignables
ping -c 1 <IP_CAMERA>
```

### Impact produit

Ce bug affecte TOUS les utilisateurs macOS qui ont un réseau NDI sur un adaptateur dédié
sans passerelle. C'est un cas d'usage très courant (réseau caméras isolé du réseau internet).
Le setup guide du produit DOIT inclure la configuration du routeur et l'ordre des services réseau.

Ce n'est documenté nulle part par Apple. Le comportement est silencieux : tout semble configuré
correctement (ifconfig, route, arp) mais le trafic ne passe pas.

## BUG CRITIQUE macOS : NECP bloque le trafic non-root sur interface secondaire

### Statut : NON RÉSOLU — contournement par sudo

### Le problème

Sur macOS (testé Sequoia 15.x), les processus non-root ne peuvent pas envoyer de trafic
unicast (TCP/UDP/ICMP) vers des hôtes sur une interface réseau **secondaire** (ex: en7/AX88179A)
quand une autre interface (Wi-Fi/en0) est l'interface primaire.

`sendto()` retourne immédiatement `EHOSTUNREACH` (errno 65, "No route to host").
Le paquet n'est jamais envoyé sur le câble.

### Ce qui fonctionne et ce qui ne fonctionne PAS

| Test | Non-root | Root (sudo/setuid) |
|------|----------|-------------------|
| `ping 10.0.0.64` | EHOSTUNREACH | OK (0.5ms) |
| `traceroute 10.0.0.64` | OK (setuid root) | OK |
| `curl http://10.0.0.64/` | EHOSTUNREACH | OK |
| Python `socket.sendto()` | EHOSTUNREACH | — |
| C `sendto()` compilé | EHOSTUNREACH | — |
| Network.framework (Swift) | errno 50 "Network is down" | — |
| `ndi_test_recv` (NDI SDK) | connections=0 | OK (1920x1080) |
| ARP (couche 2) | OK | OK |
| Broadcast UDP | OK | OK |
| Loopback (10.0.0.21→self) | OK | OK |

### Preuves clés

1. **traceroute** (`/usr/sbin/traceroute`) est **setuid root** (`-r-sr-xr-x`) → fonctionne
2. **ping** (`/sbin/ping`) n'est PAS setuid (`-r-xr-xr-x`) → bloqué
3. Les deux ont les mêmes entitlements Apple (`com.apple.private.virtualswitch.underlay-scoped`, etc.)
4. La seule différence est l'EUID=0 (root) vs EUID=501 (user)
5. Codesigner un binaire avec `com.apple.security.network.client` ne change rien
6. Le problème existe même avec `IP_BOUND_IF` (socket forcé sur en7)

### Cause racine probable : NECP (Network Extension Control Policy)

macOS maintient **1239 socket policies NECP** dans le kernel (`sysctl net.necp.socket_policy_count`).
Ces policies contrôlent quel processus peut utiliser quelle interface réseau.
Le système NECP est géré par `nehelper` (PID ~204) et `nesessionmanager` (PID ~161).

Hypothèse : les policies NECP autorisent uniquement les processus root ou signés Apple
à utiliser les routes scopées (IFSCOPE) d'une interface secondaire. Les processus
non-root sans entitlements privés Apple sont restreints à l'interface primaire.

### Ce qui a été éliminé comme cause

- ~~Tailscale Network Extension~~ — désinstallé, extension retirée du kernel
- ~~Parallels Desktop~~ — quitté, bridges supprimés
- ~~macOS Application Firewall~~ — désactivé
- ~~pf (packet filter)~~ — config par défaut (Apple uniquement)
- ~~Profils de configuration MDM~~ — aucun installé
- ~~Extensions réseau tierces~~ — aucune (seules caméras virtuelles)
- ~~Permission "Réseau local" Privacy~~ — Terminal est autorisé
- ~~Sandbox~~ — Terminal n'est pas sandboxé
- ~~Location réseau~~ — testé avec location fraîche, même résultat
- ~~Configuration DHCP vs manuelle~~ — testé les deux, même résultat
- ~~Ordre des services réseau~~ — testé en7 primaire, même résultat
- ~~Interface Wi-Fi active/inactive~~ — testé sans Wi-Fi, même résultat

### Diagnostic rapide

```bash
# 1. Vérifier que en7 est UP et configuré
ifconfig en7  # doit montrer UP,RUNNING, inet 10.0.0.21

# 2. Vérifier le NWI
scutil --nwi  # en7 doit apparaître dans "Network interfaces:"

# 3. Vérifier la route
route get 10.0.0.64  # doit montrer interface: en7

# 4. Tester root vs non-root
ping -c 1 10.0.0.64          # → EHOSTUNREACH (non-root)
sudo ping -c 1 10.0.0.64     # → OK (root)
traceroute -n -m 1 10.0.0.64 # → OK (setuid root)

# 5. Compter les policies NECP
sysctl net.necp.socket_policy_count  # ~1239 policies
```

### Contournement actuel

Lancer les outils NDI avec `sudo` :
```bash
sudo ./build-mac/ndi_test_recv
sudo ./build-mac/ndi-bridge host --auto
```

### Pistes de recherche (non résolues)

1. **NECP policies** — Comment inspecter/modifier les policies NECP du kernel ?
   Outil possible : `neutil` ou `networksetup` avec options non documentées
2. **Entitlements privés Apple** — `com.apple.private.virtualswitch.underlay-scoped`
   permet peut-être de bypasser, mais nécessite signature Apple
3. **SIP (System Integrity Protection)** — désactiver SIP pourrait relâcher les restrictions
4. **sysctl tunables** — `net.necp.drop_unentitled_level` est à 0, vérifier si d'autres valeurs aident
5. **Network.framework** — l'API Swift retourne errno 50 "Network is down" au lieu de 65,
   peut-être une route d'investigation différente
6. **Kernel log** — `process_is_plugin_host: running binary "bash" in keys-off mode due to identity: com.apple.bash`
   → le kernel met bash en "keys-off mode", potentiellement lié aux restrictions réseau
7. **Comparaison avec un Mac propre** — tester sur un Mac sans Tailscale/Parallels pour
   vérifier si le comportement est natif macOS ou dû à un résidu logiciel
8. **NDI Studio Monitor** — tester si l'app officielle NewTek fonctionne sans sudo
   (si oui, analyser ses entitlements et socket options)

## RÈGLE CRITIQUE : pas de flux vidéo UDP via Tailscale

**JAMAIS** faire transiter le flux vidéo/audio UDP du bridge via Tailscale (IPs `100.x.x.x`).
Le tunnel WireGuard de Tailscale cause une perte massive de paquets UDP (~90%).

- **VM Parallels locale** : utiliser le réseau Parallels direct (`172.20.10.x`)
- **Machine distante** : utiliser l'IP réseau direct, pas l'IP Tailscale
- Tailscale est OK pour SSH, gestion, mais PAS pour le flux vidéo

Pour trouver l'IP Parallels de la VM :
```bash
prlctl exec "Ubuntu 22.04.2 (x86_64 emulation) (1)" 'ip addr show enp0s5 | grep "inet "'
```

## BUG CORRIGÉ : NDIlib_send_create() retourne nullptr sur EC2 (v1.9)

**Symptôme :** `ndi-test-pattern` échoue avec "Failed to create NDI sender" sur EC2 headless.
`NDIlib_send_create()` retourne nullptr.

**Cause :** name collision. Si un processus `ndi-test-pattern` est tué sans signal propre
(kill -9, crash, SSH perdu), le NDI SDK ne libère pas le nom de source enregistré.
Le prochain `NDIlib_send_create()` avec le même nom échoue silencieusement.

**Fix :** tuer TOUS les processus NDI (`pkill -9 ndi`) et attendre 1-2s avant de relancer.
Le SDK nettoie les noms orphelins au bout de quelques secondes.

**Diagnostic :**
```bash
# Vérifier s'il y a des processus NDI orphelins
pgrep -a ndi
# Tuer tout
pkill -9 ndi
# Attendre et relancer
sleep 2 && ./build/ndi-test-pattern --name "EC2 Ball" --fps 25
```

**Ce n'est PAS :** un problème de `clock_video=true` sur headless, ni un bug du SDK Linux.

## BUG CORRIGÉ : décodage slice-par-slice (v1.8)

L'encodeur x264 avec `slices=10` produit 10 NAL units par frame. Le décodeur
envoyait chaque slice individuellement à FFmpeg, causant ~7 frames partielles
avec concealment errors par frame réel (ratio decoded/recv de 7:1).

**Fix** : envoyer le blob H.264 Annex-B entier en un seul `avcodec_send_packet()`
au lieu de parser et envoyer chaque NAL unit séparément.

**Diagnostic** : si `decoded >> recv` dans les stats, le décodeur traite les NAL
units individuellement au lieu du frame complet.

## Version
Actuelle : v2.1 (15 fév 2026)


La version est définie dans `src/common/Version.h` (macro `NDI_BRIDGE_VERSION`).
Ce fichier est inclus par `main.cpp`, `ndi_viewer.cpp` et `ndi_test_pattern.cpp`.
**Ne jamais** définir la version dans un autre fichier.

## Déploiement (VM locale ou EC2)

Utiliser `deploy-vm.sh` qui fait tout automatiquement :
1. Tue les anciens processus (ndi-bridge, ndi-viewer, ndi-test-pattern)
2. Copie les sources
3. Reconfigure CMake + rebuild (inclut ndi-test-pattern)
4. Remet les permissions
5. Relance le join
6. Relance le viewer

```bash
bash deploy-vm.sh
```

Après le deploy, lancer le host côté Mac :
```bash
./build-mac/ndi-bridge host --source "Test Pattern LTC" --target 54.93.225.67:5990 -v
```

## Build

```bash
# macOS
cmake -B build-mac && cmake --build build-mac

# Linux (VM Parallels ARM64)
cmake -B build && cmake --build build

# Windows (Visual Studio 2022, depuis cmd ou PowerShell)
cmake -B build -G "Visual Studio 17 2022" && cmake --build build --config Release
```

### Pré-requis Windows
- Visual Studio Build Tools 2022 (ou VS Community)
- CMake 3.16+
- FFmpeg avec support NVENC (build pré-compilé gyan.dev ou build custom `--enable-nvenc`)
- NDI SDK 6 pour Windows (`C:\Program Files\NDI\NDI 6 SDK`)
- Variable d'environnement `NDI_SDK_DIR` (optionnelle, FindNDI.cmake cherche les paths standard)

## Test Pattern (ndi-test-pattern)

**Signal de calibration broadcast UTC-déterministe.** Les 4 canaux sont synchronisés sur UTC :
1. **Audio LTC** (SMPTE 12M via libltc) — UTC
2. **Timecode visuel orange** (7-segment overlay) — UTC
3. **Position de la bille** (triangle wave depuis `utcTotalFrames()`) — UTC
4. **Métadonnées NDI** (`videoFrame.timecode`) — SDK-synthétisé (process-relatif)

Même UTC = même position de bille sur n'importe quelle machine. Le décalage spatial entre
deux billes vues en multiviewer = latence du chemin réseau, lisible à l'œil.

```bash
./build/ndi-test-pattern --name "EC2 Ball" --resolution 1920x1080 --fps 25 --ball-color red
```

Options `--ball-color` : green (défaut), red, blue, yellow, cyan, magenta, white.

- Source : `src/tools/ndi_test_pattern.cpp`
- Pas de dépendance FFmpeg, juste le NDI SDK + libltc
- Inclus dans `Version.h` pour le numéro de version
- Build : cible CMake `ndi-test-pattern`
- **Validé cross-machine** (15 fév 2026) : bille verte Mac + bille rouge EC2 via bridge,
  delta TC = 3 frames = ~120ms one-way (cohérent avec SpeedFusion RTT 225ms / 2)

## État actuel (v2.1 — 15 fév 2026)

### Ce qui marche
- **Build cross-platform** : Mac (build-mac/), Linux (build/) et Windows depuis le même codebase C++
- **Port Windows** (v2.1) : `Platform.h` abstrait sockets (WinSock), byte-swap (MSVC), wall clock (GetSystemTimePreciseAsFileTime), non-blocking mode (ioctlsocket). CMake ajoute ws2_32 et cherche le NDI SDK Windows
- **NVENC hardware encoding** (v2.1) : sur Windows avec GPU NVIDIA, `h264_nvenc` (preset p4, tune ll, zerolatency) avec fallback `h264_qsv` (Intel) puis `libx264` (software)
- **Encodage async** (v2.0) : thread dédié pour l'encodage H.264, découplé du thread NDI recv. Queue bornée (3 frames, drop-oldest). Le host peut maintenant recevoir à 25fps sans blocage par l'encodeur
- **VideoToolbox hardware encoding** (v2.0) : sur macOS, `h264_videotoolbox` (~1-5ms par frame) avec fallback automatique vers `libx264` (~60-100ms). Le gain combiné async+hwaccel = host Mac à 25fps stable
- **VideoToolbox hardware decoding** (v2.0) : sur macOS, décodage GPU via `av_hwframe_transfer_data()` avec fallback software
- **Exclude pattern fixé** (v2.0) : `--source` explicite bypasse les excludePatterns
- **NDI async send** (v2.0) : double-buffer pour `send_video_async_v2()` dans NDISender
- **Host Mac → EC2 Frankfurt** : zero-drop, 25fps, audio OK
- **Join EC2 décode et publie en NDI** : dropped=0, qdrop=0, decode_ms ~8.4ms
- **Host EC2 → Mac via SpeedFusion** : fonctionne via IP LAN (192.168.1.9)
- **Round-trip EC2↔Mac** : validé dans les deux sens via SpeedFusion
- **NDI Viewer v1.9** : 10 pro features (OSD, safe area, grid, tally, source switching)
- **Test pattern UTC-déterministe** : bille + TC + LTC tous synchronisés UTC, `--ball-color` pour distinguer les sources, validé cross-machine
- **SpeedFusion VPN** : tunnel Balance 20 ↔ FusionHub AWS opérationnel, EC2 ping Mac LAN en ~200ms
- **ChronyControl Mac** : remplace `timed`, offset NTP < 1ms (15 fév 2026)
- **Mesure latence sendTimestamp VALIDÉE** (15 fév 2026) :
  - Header protocole étendu à 46 bytes, `latency_ms` affiché dans stats join
  - **Résultats Mac → EC2 Frankfurt** : latence one-way **173-208ms** (moyenne ~190ms)
  - dropped=2/908 (0.2%), qdrop=0, decode_ms=8.4avg/45max
  - Test longue durée (93 min) : 142 671 frames, dropped=10 (0.007%), qdrop=637 (0.45%)
  - Pré-requis : chrony des deux côtés (biais NTP < 2ms)

- **Round-trip NVENC validé** (15 fév 2026) : Mac → Vmix1-frankfurt (g6.xlarge, NVIDIA L4) → Mac
  - Encodeur retour : **h264_nvenc** (hardware GPU), confirmé dans les logs
  - **qdrop=0** (vs 47% sur t3.medium x264) — amélioration majeure
  - dropped=6/3051 (0.2%), decode_ms=10.1avg, latency_ms=424ms stable
  - FPS retour : ~17fps (source webcam à 21fps, 4fps perdus dans la queue d'encodage)
  - Delta TC round-trip : 22 frames = **880ms** (cohérent avec RTT SpeedFusion 221ms × 2 + encode/decode)
  - Bitrate : 4 Mbps, MTU 1200 (réduit pour éviter les artefacts UDP sur internet public)

### Ce qui bloque
- **sudo obligatoire pour join Mac via en7/SpeedFusion** : bug NECP macOS, les processus non-root ne reçoivent pas l'UDP via l'interface secondaire en7 (AX88179A). Contournement : `sudo ./build-mac/ndi-bridge-x join`. Voir section dédiée ci-dessous.
- **FPS retour Vmix1 = 17fps au lieu de 25fps** : la source NDI sur Vmix1 (webcam bridgée) est à 21fps, et ~4fps sont droppés par la queue d'encodage host. À investiguer côté Vmix1 (stats host)

### Infra EC2 Frankfurt (eu-central-1)

#### Relay (ltc-ndi-test) — host Mac→EC2, join EC2→NDI local
- Instance : ltc-ndi-test (t3.medium spot)
- IP publique : 54.93.225.67
- IP privée : 172.31.44.6
- Clé SSH : `~/.secrets/aws/ltc-ndi-test-frankfurt.pem`
- User : ubuntu
- Commande : `ssh -i ~/.secrets/aws/ltc-ndi-test-frankfurt.pem ubuntu@54.93.225.67`
- NDI Discovery Server : 3.74.169.239
- SG : sg-0761c1bb83aecc17a (NDI_Security_aws_group) — UDP 5990-5999 ouvert depuis 0.0.0.0/0
- Déployé : ndi-bridge-linux compilé, NDI SDK Linux 6, libltc-dev
- **Instance ltc-ndi-source (i-01a2f7179d76c22c2) TERMINÉE** le 15 fév — ancienne instance spot qui floodait le port 5990 avec des paquets 38 bytes (ancien protocole)

#### Return (Vmix1-frankfurt) — host EC2→Mac retour avec GPU
- Instance : Vmix1-frankfurt (i-0a9313dae0af02d4b)
- Type : **g6.xlarge** — NVIDIA L4 GPU (h264_nvenc, **av1_nvenc** disponible)
- OS : **Windows Server**
- IP publique : 63.181.214.196 (quand démarrée — l'instance est normalement stoppée)
- IP privée VPC : 172.31.41.28
- Accès : DCV viewer via Tailscale (pas RDP)
- Même VPC et SG que ltc-ndi-test (sg-0761c1bb83aecc17a)
- Accès : **DCV viewer via Tailscale** (pas RDP, pas SSH)
- Démarrer : `aws ec2 start-instances --instance-ids i-0a9313dae0af02d4b`
- Arrêter : `aws ec2 stop-instances --instance-ids i-0a9313dae0af02d4b`
- **Coût : ~$0.70-1.00/h on-demand** — toujours arrêter après usage

**⚠️ ERREUR CORRIGÉE (v2.1)** : l'ancienne doc mentionnait "vmix-frankfurt" à 18.156.120.21 avec clé SSH `vmix-frankfurt-key.pem`. Cette instance est en réalité `ndi-bridge-ec2-bis` (Linux t3.medium, PAS de GPU). La vraie machine vMix/GPU est Vmix1-frankfurt ci-dessus.

#### ndi-bridge-ec2-bis (anciennement "vmix-frankfurt" dans la doc)
- Instance : ndi-bridge-ec2-bis (Linux t3.medium, PAS de GPU)
- IP publique : 18.156.120.21
- Clé SSH : `~/.secrets/aws/vmix-frankfurt-key.pem`
- User : ubuntu
- Commande : `ssh -i ~/.secrets/aws/vmix-frankfurt-key.pem ubuntu@18.156.120.21`
- **Ne pas utiliser pour le retour Frankfurt** — pas de GPU, x264 trop lent pour 1080p (47% qdrop)

**⚠️ Les instances ltc-ndi-test et ndi-bridge-ec2-bis utilisent des clés SSH DIFFÉRENTES. Ne pas intervertir.**

### SpeedFusion VPN (Balance 20 ↔ FusionHub AWS)
- Le tunnel SpeedFusion relie le LAN Peplink (192.168.1.x) au VPC AWS (172.31.x.x)
- EC2 peut atteindre le Mac directement via son IP LAN (192.168.1.9), pas besoin d'IP publique
- Latence tunnel : ~200ms (Frankfurt ↔ France)
- **IMPORTANT** : pour le bridge EC2→Mac, toujours utiliser l'IP LAN (192.168.1.x), PAS l'IP publique

### IMPORTANT : sudo obligatoire pour le join Mac via SpeedFusion (en7)

Le Mac est connecté au Peplink Balance 20 via l'adaptateur USB Ethernet AX88179A (**en7**).
À cause du **bug NECP macOS** (documenté plus haut), les processus non-root ne peuvent pas
envoyer NI recevoir du trafic unicast UDP via en7 (interface secondaire).

**Conséquence directe :** le `ndi-bridge join` sur Mac doit tourner en `sudo` pour recevoir
les paquets UDP qui arrivent via SpeedFusion → Balance 20 → en7.

```bash
# SANS sudo : pkts=0, rien n'arrive (NECP bloque)
./build-mac/ndi-bridge join --name "EC2 Ball Bridge" --port 5991 -v

# AVEC sudo : les paquets arrivent normalement
sudo ./build-mac/ndi-bridge join --name "EC2 Ball Bridge" --port 5991 -v
```

**Diagnostic rapide :**
```bash
ping 192.168.1.1          # → "No route to host" (NECP bloque)
sudo ping 192.168.1.1     # → OK, 0.6ms (root bypass NECP)
```

**Cela NE concerne PAS :**
- Le host Mac → EC2 via IP publique (passe par la gateway internet, pas par en7)
- Le test pattern local (tout est localhost/loopback)
- Linux/EC2 (pas de NECP)

### IP Mac (15 fév 2026)
- IP LAN Peplink (en7) : 192.168.1.9 ← **utiliser celle-ci pour le bridge EC2→Mac**
- IP en0 : peut varier (192.168.1.2 ou autre) — **ne pas utiliser**, en7 est l'interface Peplink
- Gateway par défaut : doit être 192.168.1.1 (Peplink), pas 172.20.10.1 (iPhone USB)
- Si gateway = iPhone, le trafic SpeedFusion ne passe pas. Vérifier : `route -n get default`
- Tailscale : 100.126.165.122

### Déploiement EC2
Utiliser deploy-vm.sh (mis à jour pour EC2) :
- Installe libltc-dev
- Tue les anciens processus
- Copie, build, relance test-pattern + join

### Commandes de test validées (15 fév 2026)

```bash
# === Via Vmix1-frankfurt (Windows g6.xlarge, NVENC) — RECOMMANDÉ ===

# Sur Mac — host vers Vmix1 (IP publique)
./build-mac/ndi-bridge-x host --source "Test Pattern LTC" --target 63.181.214.196:5990 -v

# Sur Vmix1 (PowerShell) — join (reçoit du Mac)
.\build\Release\ndi-bridge-x.exe join --name "Pierre Frankfurt" --port 5990 -v

# Sur Vmix1 (PowerShell) — host retour NVENC vers Mac (IP LAN via SpeedFusion)
.\build\Release\ndi-bridge-x.exe host --source "Pierre Frankfurt" --target 192.168.1.9:5991 -v --bitrate 4 --mtu 1200

# Sur Mac — join retour (sudo obligatoire pour NECP/en7)
sudo ./build-mac/ndi-bridge-x join --name "Pierre Frankfurt Round-Trip" --port 5991 -v

# === Via ltc-ndi-test (Linux t3.medium, x264 software) ===

# Sur EC2 — lancer le test pattern
~/ndi-bridge-linux/build/ndi-test-pattern --name "EC2 Ball" --resolution 1920x1080 --fps 25

# Sur EC2 — lancer le join (reçoit du Mac)
~/ndi-bridge-linux/build/ndi-bridge-x join --name "Test Pattern EC2" --port 5990 -v

# Sur Mac — lancer le host vers EC2 (IP publique EC2)
./build-mac/ndi-bridge-x host --source "Test Pattern LTC" --target 54.93.225.67:5990 -v

# Sur EC2 — lancer le host retour vers Mac (IP LAN via SpeedFusion)
~/ndi-bridge-linux/build/ndi-bridge-x host --source "EC2 Ball" --target 192.168.1.9:5991 -v

# Sur Mac — lancer le join retour (port 5991)
sudo ./build-mac/ndi-bridge-x join --name "EC2 Ball" --port 5991 -v
```

**IMPORTANT pour les targets :**
- Mac → EC2/Vmix1 : utiliser IP publique — le security group autorise le port
- EC2/Vmix1 → Mac : utiliser IP LAN Peplink (192.168.1.9) via SpeedFusion — PAS l'IP publique (NAT bloque)
- Vmix1 retour : **--bitrate 4 --mtu 1200** pour éviter les artefacts UDP sur internet public

### Prochaines étapes
1. ~~Compiler et tester sur Vmix1-frankfurt~~ — **FAIT** (v2.1, NVENC validé, qdrop=0)
2. ~~Tester round-trip via Vmix1~~ — **FAIT** (880ms round-trip, 0% qdrop, 424ms latence one-way)
3. **Investiguer 17fps retour** — la queue d'encodage host Vmix1 droppe ~4fps, vérifier si c'est la queue bornée à 3 frames ou un problème NDI recv
4. **Calibration NTP optionnelle** — échange ping/pong au démarrage pour soustraire l'offset NTP résiduel du delta sendTimestamp
5. **Support AV1** (futur) — le L4 a `av1_nvenc` hardware. Nécessite : champ codec dans le header protocole (actuellement implicitement H.264), décodeur AV1 côté join (libdav1d ou VideoToolbox AV1 sur macOS 14+), format OBU au lieu d'Annex-B. Gain attendu : meilleure qualité à débit égal ou débit réduit sur le lien WAN

## Protocole

UDP, header **46 bytes** Big-Endian, compatible cross-platform Mac/Linux/Windows.
Video : H.264 Annex-B. Audio : PCM float32 planar.
MTU : 1400 bytes. MAX_UDP_PAYLOAD = 1354 bytes (1400 - 46).

### Header (46 bytes)

```
Offset | Field          | Type   | Description
-------|----------------|--------|---------------------------
0-3    | magic          | U32    | 0x4E444942 ("NDIB")
4      | version        | U8     | Protocol version (2)
5      | mediaType      | U8     | 0=video, 1=audio
6      | sourceId       | U8     | Source ID (multi-source future)
7      | flags          | U8     | Bit 0 = keyframe (video)
8-11   | sequenceNumber | U32    | Frame sequence number
12-19  | timestamp      | U64    | PTS (10,000,000 ticks/sec)
20-23  | totalSize      | U32    | Total frame size in bytes
24-25  | fragmentIndex  | U16    | Current fragment (0-based)
26-27  | fragmentCount  | U16    | Total fragments
28-29  | payloadSize    | U16    | Payload size in this packet
30-33  | sampleRate     | U32    | Audio: sample rate (48000)
34     | channels       | U8     | Audio: channel count (2)
35-37  | reserved       | U8[3]  | Reserved
38-45  | sendTimestamp   | U64    | Wall clock at send time (ns since epoch)
```

### Mesure de latence (sendTimestamp)

Le `sendTimestamp` est écrit par le host (`Protocol::wallClockNs()` = `clock_gettime(CLOCK_REALTIME)`)
juste avant chaque `sendto()`. Le join mesure le delta à la réception sur le premier fragment de
chaque frame et affiche `latency_ms=XXX` dans les stats toutes les 5s.

**Pré-requis :** chrony sur les deux machines. macOS `timed` a ~200-600ms de dérive → inutilisable.
- **EC2** : chrony natif (offset < 0.01ms)
- **Mac** : ChronyControl installé (15 fév 2026), remplace `timed`, offset < 1ms

Le delta mesuré = latence réseau + offset NTP résiduel. Avec chrony des deux côtés,
le biais NTP est < 2ms → le delta est une bonne approximation de la latence one-way.

**Ce que le sendTimestamp NE mesure PAS :**
- Temps de génération du test pattern
- Temps d'encodage H.264 (côté host)
- Temps de décodage H.264 (côté join, visible dans `decode_ms`)
- Temps de rendu dans le moniteur NDI

### Backward compat

Le deserialize accepte les paquets >= 38 bytes (ancien header) et lit sendTimestamp
uniquement si >= 46 bytes (sinon 0). `LEGACY_HEADER_SIZE = 38` est défini dans Protocol.h.

## Métrologie broadcast — pourquoi nos métriques ont de la valeur

**Ne pas banaliser.** Les métriques du bridge (latence, drops, decode_ms) ne sont pas de simples chiffres de debug IT. Ce sont des **mesures broadcast interprétables directement en frames** parce que toute la chaîne de métrologie est correcte. Retirer un seul maillon rend les chiffres inutilisables.

### La chaîne complète (chaque maillon est nécessaire)

```
ChronyControl Mac (< 1ms vs UTC)  ─┐
                                    ├─→ sendTimestamp delta = latence réseau réelle
chrony EC2 (< 0.01ms vs UTC)       ┘      173-208ms (mesurable, stable, interprétable)
```

1. **ChronyControl Mac** (< 1ms) — horloge de départ fiable. Contribution de Pierre : a identifié que `timed` Apple avait +612ms de dérive et installé ChronyControl
2. **chrony EC2** (< 0.01ms) — horloge d'arrivée fiable (Amazon Time Sync)
3. **`CLOCK_REALTIME` dans `wallClockNs()`** — mesure au bon endroit (juste avant `sendto()`)
4. **Header 46 bytes** — le timestamp voyage avec le paquet (pas de corrélation externe)
5. **Mesure sur fragment 0** — on mesure le début de la frame, pas la fin
6. **Test pattern UTC-déterministe** — la bille ET le TC sont des références visuelles indépendantes qui confirment les chiffres numériques

### Traduction directe en frames (25fps = 40ms/frame)

| Métrique | Valeur | Signification broadcast |
|----------|--------|------------------------|
| Latence one-way | 190ms | **4.75 frames** — visible à l'œil sur le multiviewer (décalage bille verte/rouge) |
| Précision NTP | < 2ms | **< 0.05 frame** — invisible, n'affecte pas la lecture |
| decode_ms | 8.4ms | **0.21 frame** — le décodeur est instantané en termes broadcast |
| dropped 0.007% | 10/142671 | 10 frames perdues en 93 min — **indétectable** à l'œil |

Ces chiffres permettent de rédiger un cahier des charges broadcast : "le bridge ajoute ~5 frames de latence" (compréhensible par un réalisateur), "le taux de perte est < 1 frame/10 min" (acceptable pour du monitoring, pas pour de la master).

### Contre-exemple : sans chrony (ancien `timed` Apple)

```
sendTimestamp delta = latence réseau (190ms) + dérive NTP (~200-600ms, variable)
                    = 390-790ms ← INUTILISABLE
```

La dérive de `timed` était **3x plus grande** que la latence mesurée. C'est comme mesurer une distance de 19cm avec une règle qui bouge de 60cm aléatoirement. On aurait eu "ça marche à peu près" au lieu de "190ms ± 17ms one-way".

### Biais IT à éviter

Ne pas traiter chrony comme "un prérequis d'installation banal". En broadcast, le timing est le **produit** — la précision temporelle est ce qui fait qu'un signal est utilisable ou non. ChronyControl sur Mac n'est pas un fix d'infra, c'est ce qui fait passer le bridge de "jouet IT qui envoie de la vidéo" à "outil avec des métriques broadcast interprétables".

## Horloges et timecodes — LIRE AVANT DE TOUCHER

**Skill de référence :** `~/Projets/skills/clockmaster/SKILL.md`

### Règles absolues

1. **Ne JAMAIS décider seul qu'un problème de timecode "n'est pas grave"** — remonter à Pierre
2. **Framerate LTC = 25fps** (PAL/SECAM, La Réunion) sauf instruction contraire explicite
3. **CLOCK_REALTIME** = pour timestamps inter-machines (requiert chrony). Pour les durées intra-machine → CLOCK_MONOTONIC
4. **Le LTC n'est pas "juste un repère visuel"** — c'est le premier maillon de SmoothBond (estimateur Kalman FEC)

### État NTP des machines (15 fév 2026)

| Machine | Daemon | Offset vs UTC | Qualité |
|---------|--------|---------------|---------|
| Mac M1  | **chrony** (via ChronyControl) | < 1ms | Excellent |
| EC2 Frankfurt | chrony natif (Amazon Time Sync) | < 0.01ms | Référence |

**Historique :** avant ChronyControl, le Mac utilisait `timed` (Apple) avec +612ms de dérive.

### Test pattern LTC — source d'horloge

`ndi_test_pattern.cpp` : le timecode visuel ET audio LTC sont synchronisés sur
`clock_gettime(CLOCK_REALTIME)` relu **à chaque frame** (`syncFromUTC()` ligne 648).
Le champ NDI `videoFrame.timecode` est `NDIlib_send_timecode_synthesize` (synthétisé par le SDK).
Le champ `videoFrame.timestamp` (PTS) est propagé de bout en bout via le header du protocole.
