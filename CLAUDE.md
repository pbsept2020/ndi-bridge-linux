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
  - Mac : `h264_videotoolbox` (hardware, à implémenter)
  - Windows : `h264_qsv` ou `h264_nvenc` (hardware, futur)
  - Linux/fallback : `libx264` (software, actuel)

### Port Windows — ce qu'il faut changer

Le port Windows est minimal. Modifications nécessaires :
```
POSIX socket()      → WinSock WSAStartup() + socket()
poll()              → WSAPoll()
close(fd)           → closesocket(fd)
#include <unistd.h> → #ifdef _WIN32 conditionnel
```
Tout le reste (FFmpeg, NDI SDK, Protocol, Video*, JoinMode, HostMode) compile tel quel.

### Zones d'ombre non adressées

1. **Traversée NAT** : en WAN sans Tailscale, le join doit avoir un port ouvert.
   Facile sur EC2 (security group), compliqué chez un particulier (port forwarding).
2. **Sécurité** : flux UDP en clair, pas de chiffrement ni d'authentification.
   OK sur réseau privé ou Tailscale, problématique en WAN public.
3. **Reconnexion** : si le réseau coupe, le join doit attendre un nouveau keyframe.
   Pas de mécanisme de détection/reconnexion automatique.
4. **Multi-sources** : 1 bridge = 1 source = 1 port. Pour 4 caméras il faut 4 instances.

### Roadmap

1. Débugger l'affichage "Linux Ball" dans Studio Monitor
2. Ajouter `h264_videotoolbox` dans VideoEncoder.cpp (encodage hardware Mac)
3. Test EC2 mac2.metal (valider fps réels et WAN)
4. Port Windows (ajouter `#ifdef _WIN32` dans Network*)
5. Deprecate ndi-bridge-mac (Swift) une fois le C++ complet

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

## BUG CORRIGÉ : décodage slice-par-slice (v1.8)

L'encodeur x264 avec `slices=10` produit 10 NAL units par frame. Le décodeur
envoyait chaque slice individuellement à FFmpeg, causant ~7 frames partielles
avec concealment errors par frame réel (ratio decoded/recv de 7:1).

**Fix** : envoyer le blob H.264 Annex-B entier en un seul `avcodec_send_packet()`
au lieu de parser et envoyer chaque NAL unit séparément.

**Diagnostic** : si `decoded >> recv` dans les stats, le décodeur traite les NAL
units individuellement au lieu du frame complet.

## Version

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
```

## Test Pattern (ndi-test-pattern)

Outil de test : bille verte qui rebondit sur fond sombre + tonalité 440 Hz.
Tout saut/saccade de la bille = dropped frames. Carré blanc clignotant en haut à gauche = frames arrivent.

```bash
./build/ndi-test-pattern --name "VM Ball" --resolution 1920x1080 --fps 30
```

- Source : `src/tools/ndi_test_pattern.cpp`
- Pas de dépendance FFmpeg, juste le NDI SDK
- Inclus dans `Version.h` pour le numéro de version
- Build : cible CMake `ndi-test-pattern`

## État actuel (v1.9 — 14 fév 2026)

### Ce qui marche
- **Build cross-platform** : Mac (build-mac/) et Linux (build/) depuis le même codebase C++
- **Host Mac → EC2 Frankfurt** : zero-drop, ~29fps, audio OK
- **Join EC2 décode et publie en NDI** : dropped=0, qdrop=0, decode_ms ~11ms
- **Host EC2 → Mac** : encode et envoie OK (eagain_drops=0)
- **NDI Viewer v1.9** : 10 pro features (OSD, safe area, grid, tally, source switching)
- **Test pattern avec LTC** : timecode embarqué, full-range color, H.264
- **deploy-vm.sh** : mis à jour pour EC2 (libltc-dev, kill test-pattern, clean viewer)

### Ce qui bloque
- **Retour EC2→Mac bloqué par NAT** : le host EC2 envoie vers 80.12.254.168:5991 mais la box ne forward pas UDP entrant. Solutions : port forwarding sur box/Peplink, ou tunnel SpeedFusion.
- **Exclude pattern trop agressif** : le host filtre toute source contenant "Bridge" AVANT de vérifier --source explicite. Quand --source est explicite, le filtre ne devrait pas s'appliquer. Voir findNDISource() lignes 325-330.

### Infra EC2 Frankfurt (eu-central-1)
- Instance : ltc-ndi-test (t3.medium spot)
- IP publique : 54.93.225.67
- IP privée : 172.31.44.6
- Clé SSH : ~/.secrets/aws/ltc-ndi-test-frankfurt.pem
- User : ubuntu
- Commande : `ssh -i ~/.secrets/aws/ltc-ndi-test-frankfurt.pem ubuntu@54.93.225.67`
- NDI Discovery Server : 3.74.169.239
- SG : sg-0761c1bb83aecc17a (NDI_Security_aws_group)
- Déployé : ndi-bridge-linux compilé, NDI SDK Linux 6, libltc-dev

### IP Mac (14 fév 2026)
- IP publique : 80.12.254.168
- IP locale : 192.168.8.101
- Tailscale : 100.126.165.122

### Déploiement EC2
Utiliser `deploy-vm.sh` (mis à jour pour EC2) :
- Installe libltc-dev
- Tue les anciens processus
- Copie, build, relance test-pattern + join

### Commandes de test validées (14 fév 2026)
```bash
# Sur EC2 — lancer le join
~/ndi-bridge-linux/build/ndi-bridge join --name "Test Pattern EC2" --port 5990 -v

# Sur Mac — lancer le host vers EC2
./build-mac/ndi-bridge host --source "Test Pattern LTC" --target 54.93.225.67:5990 -v

# Sur Mac — lancer le join retour (port 5991)
./build-mac/ndi-bridge join --name "EC2 Bridge" --port 5991 -v

# Sur EC2 — lancer le host retour vers Mac
~/ndi-bridge-linux/build/ndi-bridge host --source "Test Pattern EC2" --target <IP_MAC>:5991 -v
```

### IMPORTANT pour Claude Code
- La source NDI s'appelle **"Test Pattern LTC"** (PAS "P20064", PAS "WebCam")
- Le nom du join sur EC2 ne doit PAS contenir "Bridge" (sinon filtré par excludePatterns)
- Ne PAS lancer le flux vidéo via Tailscale (100.x.x.x) — perte massive de paquets

### Prochaines étapes
1. **Résoudre le NAT retour** — port forwarding UDP 5991 sur la box ou utiliser SpeedFusion
2. **Fix exclude pattern** — quand --source est explicite, bypasser excludePatterns dans findNDISource()
3. **Tester round-trip complet** une fois le NAT résolu
4. **Mesurer latence end-to-end** (LTC timecode permet la mesure précise)

## Protocole

UDP, header 38 bytes Big-Endian, compatible cross-platform Mac/Linux.
Video : H.264 Annex-B. Audio : PCM float32 planar.
MTU : 1200 bytes (compatible WireGuard/Tailscale si nécessaire).
