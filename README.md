# NDI Bridge Linux

Port C++ de NDI Bridge pour Linux - Stream NDI sur WAN avec encodage H.264.

## Objectif

Version Linux compatible avec la version macOS Swift (`ndi-bridge-mac/`).
Le protocole UDP (header 38 bytes Big-Endian) est **identique** pour permettre l'interopérabilité cross-platform.

## Structure

```
ndi-bridge-linux/
├── CMakeLists.txt           # Build system
├── cmake/
│   └── FindNDI.cmake        # NDI SDK detection
├── src/
│   ├── main.cpp             # CLI entry point
│   ├── common/
│   │   ├── Protocol.h/.cpp  # UDP protocol (38-byte header)
│   │   └── Logger.h/.cpp    # Logging system
│   ├── host/                # TODO: Sender mode
│   └── join/                # TODO: Receiver mode
├── scripts/
│   └── build.sh             # Build helper
└── README.md
```

## Dépendances

### NDI SDK for Linux

Télécharger depuis https://ndi.video/sdk/ et installer :

```bash
# Option 1: Installation système
sudo cp -r "NDI SDK for Linux/include/"* /usr/include/
sudo cp "NDI SDK for Linux/lib/x86_64-linux-gnu/"* /usr/lib/

# Option 2: Installation utilisateur
cp -r "NDI SDK for Linux" ~/
```

### FFmpeg

```bash
# Ubuntu/Debian
sudo apt install libavcodec-dev libavutil-dev libswscale-dev libavformat-dev

# Fedora
sudo dnf install ffmpeg-devel
```

### Build Tools

```bash
sudo apt install cmake build-essential pkg-config
```

## Build

```bash
# Méthode recommandée
./scripts/build.sh

# Ou manuellement
cmake -B build
cmake --build build

# Mode debug
./scripts/build.sh debug
```

## Usage

```bash
# Aide
./build/ndi-bridge --help

# Découvrir les sources NDI
./build/ndi-bridge discover

# Mode host (sender) - TODO
./build/ndi-bridge host --auto --target 192.168.1.100:5990

# Mode join (receiver) - TODO
./build/ndi-bridge join --name "Remote Camera" --port 5990
```

## Protocole UDP

Header 38 bytes, Big-Endian, identique à la version macOS :

| Offset | Champ          | Type  | Description              |
|--------|----------------|-------|--------------------------|
| 0-3    | magic          | U32   | 0x4E444942 ("NDIB")      |
| 4      | version        | U8    | 2                        |
| 5      | mediaType      | U8    | 0=video, 1=audio         |
| 6-7    | reserved       | U16   | Padding                  |
| 8-11   | sequenceNumber | U32   | Frame number             |
| 12-19  | timestamp      | U64   | PTS (10M ticks/sec)      |
| 20-23  | totalSize      | U32   | Total frame size         |
| 24-25  | fragmentIndex  | U16   | Fragment index (0-based) |
| 26-27  | fragmentCount  | U16   | Total fragments          |
| 28-29  | payloadSize    | U16   | This packet payload      |
| 30-33  | sampleRate     | U32   | Audio: 48000             |
| 34     | channels       | U8    | Audio: 2                 |
| 35-37  | reserved2      | U8[3] | Padding to 38 bytes      |

**Formats:**
- Video: H.264 Annex-B
- Audio: PCM 32-bit float planar, 48kHz

## Progression

| Phase | Status |
|-------|--------|
| Structure projet | DONE |
| Protocol.h (parsing) | DONE |
| Logger | DONE |
| CLI skeleton | DONE |
| NDI discover | DONE |
| Host mode (FFmpeg encode) | TODO |
| Join mode (FFmpeg decode) | TODO |
| Audio support | TODO |

## Compatibilité

- **Linux x86_64**: Ubuntu 20.04+, Debian 11+, Fedora 35+
- **Compilateur**: GCC 9+ ou Clang 10+ (C++17)
- **Cross-platform**: Compatible avec ndi-bridge-mac (Swift)

## Auteur

Pierre Bessette - Broadcast Technology & AI Professor
