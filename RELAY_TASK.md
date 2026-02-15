Implémente le mode `relay` (phase 1) pour ndi-bridge. C'est un forwarding UDP packet-level sans transcode.

## Contexte

Lis d'abord `Docs/RELAY_MODE.md` — il explique le problème (double transcode inutile sur EC2), le design phase 1, et la vision produit vers le mode rendez-vous (phases 2-3 non implémentées ici).

## Ce qui existe déjà — vérifie avant de coder

- `NetworkSender::sendRaw(const uint8_t* data, size_t size)` dans `src/network/NetworkSender.h` — vérifie que l'implémentation dans le .cpp fait bien un `sendto()` brut
- `NetworkReceiver` dans `src/network/` — fait du reassembly fragment→frame via `processPacket()`. Le raw callback sera ajouté **avant** `processPacket()`
- `PROTOCOL_MAGIC = 0x4E444942` dans `src/common/Protocol.h` — pour la validation magic
- `Protocol::wallClockNs()` dans `src/common/Protocol.h` — horloge murale (pas utilisée par le relay phase 1, mais disponible)
- `main.cpp` — dispatch `discover` / `host` / `join`, pattern à reproduire pour `relay`
- Le Logger utilise `LOG_INFO`, `LOG_SUCCESS`, `LOG_ERROR` et `Logger::instance().infof()` pour le formaté

## Étapes dans l'ordre

### 1. `src/network/NetworkReceiver.h` — ajouter callback raw

Ajouter dans la section des callback types :
```cpp
using OnRawPacket = std::function<void(const uint8_t* data, size_t size)>;
```

Ajouter le membre privé `OnRawPacket onRawPacket_;` et le setter public :
```cpp
void setOnRawPacket(OnRawPacket callback) { onRawPacket_ = std::move(callback); }
```

### 2. `src/network/NetworkReceiver.cpp` — fire le raw callback

Dans `receiveLoop()`, après le `recvfrom()` réussi (`received > 0`), **avant** l'appel à `processPacket()` :
```cpp
if (onRawPacket_) {
    onRawPacket_(buffer.data(), static_cast<size_t>(received));
}
```

C'est tout. Le reste de `receiveLoop()` (processPacket, stats, reassembly) continue normalement.
Le Join existant n'est pas affecté — il ne set pas ce callback.

### 3. Créer `src/relay/RelayMode.h`

```cpp
struct RelayModeConfig {
    uint16_t listenPort = 5990;
    std::string targetHost;
    uint16_t targetPort = 5991;
};
```

Classe `RelayMode` avec :
- `int start(std::atomic<bool>& running)` — même signature que HostMode/JoinMode
- `void stop()`
- Membres : `NetworkReceiver`, `NetworkSender`, compteurs atomiques (packetsRelayed, bytesRelayed, invalidPackets)

### 4. Créer `src/relay/RelayMode.cpp`

Implémentation :
- `start()` : crée le receiver (listenPort) et le sender (targetHost:targetPort), branche `setOnRawPacket`, démarre l'écoute, puis boucle d'affichage stats toutes les 5s jusqu'à `!running`
- Le raw callback :
  - Vérifie `size >= 4`
  - Vérifie le magic en network byte order : `uint32_t magic; memcpy(&magic, data, 4); if (ntohl(magic) != 0x4E444942)` → incrémente invalidPackets, return
  - Forward via `sender->sendRaw(data, size)`
  - Incrémente packetsRelayed et bytesRelayed
- Stats (toutes les 5s, dans la boucle du main thread) :
  ```
  [RELAY] pkts=12847  bytes=18.2MB  elapsed=30.0s  rate=4.9Mbps  invalid=0
  ```
  Formater bytes en KB/MB/GB selon la taille. Calculer rate = bytesRelayed * 8 / elapsed.

- `stop()` : arrête receiver et sender, affiche stats finales

### 5. `src/main.cpp` — ajouter mode relay

- Ajouter `#include "relay/RelayMode.h"`
- Ajouter `Relay` dans l'enum `Config::Mode`
- Dans `parseArgs()` : parser `"relay"` comme mode, réutiliser `--target` (obligatoire) et `--port` existants
- Ajouter `runRelay(const Config& config)` — instancie `RelayModeConfig`, appelle `start(g_running)`
- Dans le switch `main()` : ajouter `case Config::Mode::Relay`
- Dans `printUsage()` : ajouter section relay :
  ```
  Modes:
    ...
    relay               Forward UDP packets without transcoding

  Relay mode options:
    --target <ip:port>    Forward destination (required)
    --port <port>         UDP listen port (default: 5990)
  ```

Note : `NDIlib_initialize()` est appelé pour tous les modes. Le relay n'en a pas besoin mais ça ne cause aucun problème — ne pas conditionner pour l'instant.

### 6. `CMakeLists.txt` — ajouter le source

Ajouter `src/relay/RelayMode.cpp` dans la liste `COMMON_SOURCES` (après `src/join/JoinMode.cpp`).

### 7. `CLAUDE.md` — documenter

Ajouter après la section "État actuel" une section :

```markdown
## Mode relay (v2.0)

Forwarding UDP packet-level sans transcode. Élimine le double decode/encode
quand EC2 sert uniquement de relais (pas besoin de voir les pixels).

### Usage
\`\`\`bash
./build/ndi-bridge relay --target <ip:port> [--port <listen_port>]
\`\`\`

### Test round-trip Mac→EC2→Mac
\`\`\`bash
# EC2 :
./build/ndi-bridge relay --port 5990 --target 192.168.1.9:5991

# Mac host :
./build-mac/ndi-bridge host --source "Test Pattern LTC" --target 54.93.225.67:5990 -v

# Mac join :
sudo ./build-mac/ndi-bridge join --name "EC2 Relay" --port 5991 -v
\`\`\`

### Caractéristiques
- Forwarde chaque paquet UDP brut (pas de reassembly, pas de notion de frame)
- Zéro dépendance FFmpeg/NDI côté relay
- sendTimestamp préservé (le join mesure la latence end-to-end host→relay→join)
- Design doc complet : `Docs/RELAY_MODE.md`
```

## Fichiers touchés (exhaustif)

| Fichier | Action |
|---------|--------|
| `src/network/NetworkReceiver.h` | Ajouter OnRawPacket callback type + setter + membre |
| `src/network/NetworkReceiver.cpp` | Ajouter 3 lignes dans receiveLoop() |
| `src/relay/RelayMode.h` | **CRÉER** |
| `src/relay/RelayMode.cpp` | **CRÉER** |
| `src/main.cpp` | Ajouter mode relay (include, enum, parse, run, usage) |
| `CMakeLists.txt` | Ajouter 1 ligne dans COMMON_SOURCES |
| `CLAUDE.md` | Ajouter section relay |

**Ne toucher AUCUN autre fichier.**

## Validation

Après implémentation :
1. `cmake -B build && cmake --build build` — compile sans erreur
2. `./build/ndi-bridge relay` — affiche l'aide (target manquant)
3. `./build/ndi-bridge relay --target 127.0.0.1:5991 --port 5990 -v` — démarre, affiche stats toutes les 5s, s'arrête proprement sur Ctrl+C
4. `./build/ndi-bridge join --name Test --port 5990 -v` — vérifie que le join existant fonctionne toujours (pas de régression NetworkReceiver)
