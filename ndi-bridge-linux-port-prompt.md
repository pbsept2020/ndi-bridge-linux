# Portage NDI Bridge Mac → Linux

## Contexte
Le bridge NDI Mac fonctionne et est validé avec ~75ms de latence end-to-end (encode H.264 + decode + NDI). Le code source de référence est dans `~/Projets/ndi-bridge-mac/Sources/`.

Lis d'abord le fichier `~/Projets/ndi-bridge-mac/NDI_BRIDGE_LINUX_PORTABILITY_PROMPT.md` pour le contexte complet (architecture, protocole NDIB 38 bytes, structure cible, dépendances).

## Ce qui a été validé côté Mac (à reproduire sur Linux)

3 optimisations critiques de latence, à intégrer dès le départ :

1. **Encoder : zéro buffer de frames** — FFmpeg libx264 avec `tune=zerolatency` et `preset=ultrafast`, pas de B-frames (`max_b_frames=0`)
2. **Réseau : envoi UDP non-bloquant** — `sendto()` POSIX direct, pas de callback ou d'attente entre fragments
3. **Décodeur : flush synchrone** — Après chaque `avcodec_send_packet()` + `avcodec_receive_frame()`, pas de buffering asynchrone

## Consignes

1. Langage : **C++17**
2. Build : **CMake** avec `FindNDI.cmake`
3. Encoder/Décodeur : **FFmpeg** (libavcodec, libavutil, libswscale) avec **libx264 software only** — PAS de VAAPI/NVENC pour l'instant, on veut la portabilité max
4. Réseau : **Sockets POSIX UDP** direct (socket, sendto, recvfrom)
5. NDI : **NDI SDK Linux** — le wrapper C existant (`CNDIWrapper/ndi_wrapper.c` et `.h`) est réutilisable quasi tel quel
6. Protocole : **Identique au Mac** — header NDIB 38 bytes big-endian, fragmentation MTU 1400, video H.264 Annex-B, audio PCM 32-bit float planar
7. Pixel format : l'entrée NDI est en BGRA, il faut convertir en YUV420P/NV12 pour libx264 via swscale, puis le décodeur sort en YUV420P qu'il faut reconvertir en BGRA pour NDI output

## Ordre d'implémentation

1. `Protocol.h` — header NDIB (déjà fourni dans le prompt de portabilité)
2. `NetworkSender.cpp` + `NetworkReceiver.cpp` — UDP POSIX avec fragmentation/reassemblage
3. `VideoEncoder.cpp` — FFmpeg libx264 avec les optimisations latence
4. `VideoDecoder.cpp` — FFmpeg decode synchrone
5. `NDIReceiver.cpp` + `NDISender.cpp` — basés sur le wrapper C existant
6. `HostMode.cpp` + `JoinMode.cpp` — orchestrateurs
7. `main.cpp` — CLI avec `--host`/`--join`/`--discover`
8. `run.sh` — script wrapper comme sur Mac

## Test de validation

Le bridge Linux doit être interopérable avec le bridge Mac :
- Mac host → Linux join (et inversement)
- Latence end-to-end < 100ms en local
- Protocole NDIB v2 compatible dans les deux sens

## Dépendances à installer

```bash
sudo apt install -y build-essential cmake pkg-config \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libx264-dev
# + NDI SDK Linux installé dans /usr/lib et /usr/include
```

Commence par lire les sources Swift de référence, puis implémente dans l'ordre indiqué. Compile et corrige les erreurs au fur et à mesure.
