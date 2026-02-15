Implémente le mode `relay` pour ndi-bridge. C'est un forwarding UDP packet-level sans transcode.

## Contexte

Lis d'abord `Docs/RELAY_MODE.md` pour comprendre le pourquoi et le design complet.

## Ce qui existe déjà

- `NetworkSender::sendRaw()` — existe dans `src/network/NetworkSender.h`, vérifie l'implémentation dans le .cpp
- `NetworkReceiver` — fait du reassembly fragment→frame. Il faut ajouter un callback raw **avant** le reassembly
- `Protocol.h` — constante `PROTOCOL_MAGIC = 0x4E444942` pour validation
- `main.cpp` — dispatch `host` / `join` / `discover`, pattern à suivre pour ajouter `relay`

## Étapes dans l'ordre

### 1. NetworkReceiver — ajouter callback raw

Dans `NetworkReceiver.h` :
- Ajouter `using OnRawPacket = std::function<void(const uint8_t* data, size_t size)>;`
- Ajouter membre `OnRawPacket onRawPacket_;`
- Ajouter setter `void setOnRawPacket(OnRawPacket callback)`

Dans `NetworkReceiver.cpp`, dans `receiveLoop()`, après le `recvfrom()` réussi et **avant** `processPacket()` :
```cpp
if (onRawPacket_) {
    onRawPacket_(buffer.data(), static_cast<size_t>(received));
}
```

Le mode Join existant n'est pas affecté (il ne set pas ce callback).

### 2. Créer `src/relay/RelayMode.h` et `src/relay/RelayMode.cpp`

Config :
```cpp
struct RelayModeConfig {
    uint16_t listenPort = 5990;
    std::string targetHost;
    uint16_t targetPort = 5991;
};
```

L'orchestrateur :
- Crée un `NetworkReceiver` et un `NetworkSender`
- Branche `setOnRawPacket` sur un callback qui :
  - Valide que le paquet fait >= 4 bytes et commence par magic `0x4E444942` (network byte order)
  - Forward le buffer brut via `sendRaw()`
  - Incrémente compteurs (packets, bytes, invalid)
- Affiche des stats toutes les 5 secondes : `[RELAY] pkts=... bytes=... elapsed=... rate=...Mbps invalid=...`
- Respecte `std::atomic<bool>& running` pour le shutdown propre (même pattern que HostMode/JoinMode)

Le relay ne touche PAS au contenu des paquets. Le sendTimestamp est préservé tel quel.

### 3. main.cpp — ajouter mode relay

- Ajouter `Mode::Relay` dans l'enum Config
- Parser la sous-commande `relay` avec `--target <ip:port>` (obligatoire) et `--port <port>` (défaut 5990)
- Ajouter `runRelay()` qui instancie RelayMode et appelle `start(g_running)`
- Mettre à jour `printUsage()` avec la doc relay
- `#include "relay/RelayMode.h"`

### 4. CMakeLists.txt

Ajouter `src/relay/RelayMode.cpp` dans la liste `COMMON_SOURCES`.

### 5. CLAUDE.md — documenter

Ajouter une section "Mode relay" dans CLAUDE.md avec :
- La commande d'usage
- Les commandes de test (copier depuis Docs/RELAY_MODE.md)
- Mentionner que c'est du packet-level forwarding, pas de FFmpeg/NDI utilisé

## Contraintes

- **Ne PAS toucher** aux fichiers existants sauf : NetworkReceiver.h, NetworkReceiver.cpp, main.cpp, CMakeLists.txt, CLAUDE.md
- Le callback raw dans NetworkReceiver doit être **additif** — le comportement existant (reassembly + callbacks frame) ne doit pas changer
- Le relay ne doit PAS appeler de fonctions FFmpeg ni NDI (même si elles sont linkées dans le binaire)
- Suivre le style C++17 du projet (namespace `ndi_bridge`, `LOG_INFO`/`LOG_SUCCESS`/`LOG_ERROR` via Logger)
- Tester que ça compile : `cmake -B build && cmake --build build`

## Validation

Après implémentation, vérifier :
1. `./build/ndi-bridge relay --help` affiche l'usage
2. `./build/ndi-bridge relay --target 127.0.0.1:5991 --port 5990` démarre et affiche les stats
3. Le mode `join` existant fonctionne toujours (pas de régression sur NetworkReceiver)
