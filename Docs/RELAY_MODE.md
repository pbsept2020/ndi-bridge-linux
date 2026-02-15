# Mode Relay — Forwarding UDP sans transcode

## Problème

Quand EC2 sert de relais entre deux Mac (ou Mac↔n'importe quoi), l'architecture actuelle impose :

```
Mac Host → encode H.264 → UDP → EC2 Join (DECODE H.264 → NDI raw)
                                  EC2 Host (NDI raw → ENCODE H.264) → UDP → Mac Join
```

Le Join décode pour publier en NDI. Le Host recapture ce NDI pour réencoder.
Sur un t3.medium (2 vCPU), x264 à 1080p tient ~14fps → **qdrop ~47%**, saccades massives.

Ce double transcode est **inutile** si EC2 n'a pas besoin de voir les pixels.

## Solution : mode relay

Un troisième mode qui forwarde les paquets UDP bruts sans les toucher :

```
Mac Host → encode H.264 → UDP:5990 → EC2 Relay → UDP:5991 → Mac Join
```

Zéro FFmpeg, zéro NDI SDK côté relay. Le bitstream H.264 passe intact.
Un t3.micro suffit.

## Choix d'architecture

### Phase 1 : Packet-level relay (à implémenter maintenant)

Chaque paquet UDP reçu est immédiatement renvoyé vers la cible.
Pas de reassembly des fragments, pas de notion de "frame".

**Avantages :**
- Latence minimale (pas d'attente de frame complète)
- Code trivial (~100 lignes)
- Aucune dépendance FFmpeg/NDI

**Limites :**
- Pas de stats par frame (seulement packets/bytes)
- Pas de re-fragmentation (MTU entrant = MTU sortant)
- Pas de fan-out multi-target

### Phase 2 : Frame-level relay (planifié, pas maintenant)

Le relay reassemble les frames H.264 puis re-fragmente et renvoie.

**Cas d'usage futurs :**
- Stats par frame (latence, drops, keyframe ratio, bitrate)
- Re-fragmentation avec MTU différent (1400→1200 pour lien dégradé)
- Injection de keyframe request (IDR forcé si perte downstream)
- Fan-out multi-target (un relais → N destinations)
- Routeur/switcher de bitstreams H.264 (sans pixels)

## Design Phase 1

### Nouveau fichier : `src/relay/RelayMode.h` + `RelayMode.cpp`

Orchestrateur minimal :

```
RelayMode {
    NetworkReceiver  (écoute UDP, mode raw — callback par paquet brut)
    NetworkSender    (renvoie vers target)
    
    onRawPacket(buf, len) {
        // Valider magic NDIB (4 bytes) — drop si invalide
        // Forward tel quel via sendRaw()
        // Incrémenter compteurs (packets, bytes)
    }
}
```

### Modification : `NetworkReceiver` — ajouter callback raw

Aujourd'hui le receiver ne callback qu'après reassembly des fragments en frames.
Il faut ajouter un second callback **avant** le reassembly, dans `receiveLoop()`,
juste après `recvfrom()` :

```cpp
// Nouveau callback type dans NetworkReceiver.h
using OnRawPacket = std::function<void(const uint8_t* data, size_t size)>;

// Nouveau setter
void setOnRawPacket(OnRawPacket callback) { onRawPacket_ = std::move(callback); }

// Dans receiveLoop(), après recvfrom() réussi, AVANT processPacket() :
if (onRawPacket_) {
    onRawPacket_(buffer.data(), static_cast<size_t>(received));
}
// processPacket() continue normalement (stats, latence) — ou pas si relay-only
```

**Le mode existant (Join) n'est pas affecté** — il continue d'utiliser les callbacks
frame-level. Le raw callback est optionnel : s'il n'est pas set, rien ne change.

### Modification : `main.cpp` — sous-commande `relay`

```
ndi-bridge relay --target <ip:port> [--port <listen>]
```

- `--target` : obligatoire (IP:port de destination)
- `--port` : port d'écoute (défaut 5990)
- Le relay ne touche PAS le contenu des paquets — y compris `sendTimestamp`

**sendTimestamp préservé :** le join final mesure la latence end-to-end
(host original → join final), pas juste relay→join. C'est plus utile.

### CMakeLists.txt

Ajouter `src/relay/RelayMode.cpp` aux sources de `ndi_bridge_common`.

Note : le relay ne link pas FFmpeg/NDI en pratique (il n'appelle aucune
de leurs fonctions), mais il est compilé dans le même binaire. C'est acceptable
pour la phase 1. L'appel `NDIlib_initialize()` dans `main.cpp` est fait pour
tous les modes — le relay n'en a pas besoin mais ça ne cause aucun problème.

### Stats relay

Affichées toutes les 5 secondes (même pattern que host/join) :

```
[RELAY] pkts=12847  bytes=18.2MB  elapsed=30s  rate=4.9Mbps  invalid=0
```

Pas de stats par frame (phase 1 = packet-level uniquement).

## Vérification `sendRaw` existant

`NetworkSender::sendRaw(const uint8_t* data, size_t size)` **existe déjà** dans
`NetworkSender.h`. Pas besoin de l'ajouter.

## Commandes de test

```bash
# EC2 : relay (remplace join+host)
./build/ndi-bridge relay --port 5990 --target 192.168.1.9:5991

# Mac : host vers EC2 (inchangé)
./build-mac/ndi-bridge host --source "Test Pattern LTC" --target 54.93.225.67:5990 -v

# Mac : join retour (inchangé)  
sudo ./build-mac/ndi-bridge join --name "EC2 Relay" --port 5991 -v
```

## Résultat attendu

- **qdrop EC2 = 0** (pas d'encodeur)
- **CPU EC2 ≈ 0%** (juste du forwarding UDP)
- **Latence ajoutée par le relay : < 1ms** (un recvfrom + un sendto)
- Le join Mac voit `latency_ms` = latence end-to-end host→relay→join
