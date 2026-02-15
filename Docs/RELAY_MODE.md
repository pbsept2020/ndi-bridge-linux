# Mode Relay — Forwarding UDP sans transcode

## Problème

Quand EC2 sert de relais entre deux machines, l'architecture actuelle impose un double transcode :

```
Mac Host → encode H.264 → UDP → EC2 Join (DECODE H.264 → NDI raw)
                                  EC2 Host (NDI raw → ENCODE H.264) → UDP → Mac Join
```

Le Join décode pour publier en NDI (pixels non compressés). Le Host recapture ce NDI pour réencoder.
Sur t3.medium (2 vCPU), x264 à 1080p tient ~14fps → qdrop ~47%, saccades massives.

Ce double transcode est inutile si EC2 n'a pas besoin de voir les pixels.

## Solution : mode relay

Un troisième mode CLI qui forwarde les paquets UDP bruts sans y toucher :

```
Mac Host → encode H.264 → UDP:5990 → EC2 Relay → UDP:5991 → Mac Join
```

Zéro FFmpeg, zéro NDI SDK côté relay. Le bitstream H.264 passe intact. Un t3.micro suffit.

## Vision produit : du relay au rendez-vous

Le relay évolue en trois phases. Chaque phase est autonome et testable.

### Phase 1 — Packet-level relay, target fixe (implémenter maintenant)

Chaque paquet UDP reçu est immédiatement renvoyé vers une cible fixe passée en CLI.
Pas de reassembly des fragments, pas de notion de "frame".

```bash
./build/ndi-bridge relay --port 5990 --target 192.168.1.9:5991
```

Un flux = un process relay = un port. Pour un échange bidirectionnel (Mac↔PC), lancer deux relays sur deux ports.

**Avantages :** latence minimale, code trivial (~150 lignes), aucune dépendance FFmpeg/NDI.

**Limites :** target fixe (IP:port en CLI), pas de stats par frame, pas de traversée NAT.

**Topologie validable :** Mac→EC2→Mac round-trip (remplace join+host EC2, élimine les qdrop).

### Phase 2 — Mode rendez-vous (planifié, pas maintenant)

Équivalent du "rendez-vous" de NDI Bridge officiel (NewTek). Le relay n'a plus besoin de `--target` en dur : les deux parties se connectent vers le serveur, et le relay apprend dynamiquement les adresses.

```
Mac host  ──UDP──→  EC2 relay  ←──UDP──  PC join
                    (apprend les deux adresses source)
                    (forwarde host→join et join→host)
```

**Principe :**
1. Le host et le join envoient tous les deux des paquets **vers** EC2 (connexion sortante → passe le NAT des deux côtés sans port forwarding)
2. Le relay note les `recvfrom()` source addresses
3. Il forwarde les paquets du host vers le join et vice-versa
4. Le join envoie un keepalive périodique pour maintenir le trou NAT ouvert

**C'est le même `recvfrom()` + `sendto()` que la phase 1**, sauf que la destination est apprise dynamiquement au lieu d'être un argument CLI.

```bash
# Phase 2 — CLI envisagé
./build/ndi-bridge relay --port 5990 --rendez-vous
# Le host et le join se connectent tous les deux vers EC2:5990
# Le relay identifie qui est host (envoie des gros paquets H.264) et qui est join (envoie des keepalives)
```

**Cas d'usage :** topologie étoile Mac↔VPS↔PC où le PC est derrière un NAT domestique (box internet). Aucune configuration réseau côté PC (pas de port forwarding, pas de VPN).

### Phase 3 — Frame-level relay (futur)

Le relay reassemble les frames H.264 puis re-fragmente et renvoie. Permet :

- Stats par frame (latence, drops, keyframe ratio, bitrate instantané)
- Re-fragmentation avec MTU différent (1400→1200 pour lien dégradé)
- Injection de keyframe request (IDR forcé si perte downstream)
- Fan-out multi-target (un relais → N destinations)
- Routeur/switcher de bitstreams H.264 (sans pixels)

## Design Phase 1

### Nouveau : `src/relay/RelayMode.h` + `RelayMode.cpp`

```
RelayMode {
    NetworkReceiver  (écoute UDP, callback raw par paquet)
    NetworkSender    (forward vers target)
    
    onRawPacket(buf, len) {
        if len < 4 || magic != 0x4E444942 → invalid++, drop
        sendRaw(buf, len)
        packets++, bytes += len
    }

    // Stats toutes les 5s :
    // [RELAY] pkts=12847  bytes=18.2MB  elapsed=30s  rate=4.9Mbps  invalid=0
}
```

Config : `listenPort`, `targetHost`, `targetPort`. 
Le relay ne touche PAS au contenu des paquets. Le `sendTimestamp` est préservé — le join final mesure la latence end-to-end (host→relay→join), plus utile que relay→join.

### Modification : `NetworkReceiver` — callback raw

Callback additionnel **avant** le reassembly, dans `receiveLoop()` après `recvfrom()` :

```cpp
using OnRawPacket = std::function<void(const uint8_t* data, size_t size)>;
void setOnRawPacket(OnRawPacket callback);

// Dans receiveLoop(), après recvfrom() réussi, AVANT processPacket() :
if (onRawPacket_) {
    onRawPacket_(buffer.data(), static_cast<size_t>(received));
}
```

Le Join existant n'est pas affecté (il ne set pas ce callback). Le raw callback est optionnel.

### Modification : `main.cpp` — sous-commande `relay`

Même pattern que host/join. `--target` obligatoire, `--port` défaut 5990.

### `NetworkSender::sendRaw()` — existe déjà

Déclaré dans `NetworkSender.h`, envoie un buffer brut en un seul `sendto()`. Pas besoin de l'ajouter.

## Commandes de test

### Round-trip Mac→EC2→Mac (valide que qdrop = 0)

```bash
# EC2 (relay aller) :
./build/ndi-bridge relay --port 5990 --target 192.168.1.9:5991

# Mac (host vers EC2) :
./build-mac/ndi-bridge host --source "Test Pattern LTC" --target 54.93.225.67:5990 -v

# Mac (join retour) :
sudo ./build-mac/ndi-bridge join --name "EC2 Relay" --port 5991 -v
```

### Étoile Mac↔EC2↔PC (phase 2 — nécessite rendez-vous ou port ouvert côté PC)

```bash
# EC2 (relay aller Mac→PC) :
./build/ndi-bridge relay --port 5990 --target <PC_IP>:5990

# EC2 (relay retour PC→Mac) :
./build/ndi-bridge relay --port 5991 --target 192.168.1.9:5991

# Mac host → EC2:5990, PC join écoute :5990
# PC host → EC2:5991, Mac join écoute :5991
```

Note : la topologie étoile avec target fixe nécessite que le PC ait un port ouvert ou un VPN. Le mode rendez-vous (phase 2) élimine cette contrainte.

## Résultats attendus (phase 1)

- **qdrop EC2 = 0** (pas d'encodeur)
- **CPU EC2 ≈ 0%** (juste du forwarding UDP)
- **Latence ajoutée par le relay : < 1ms** (un recvfrom + un sendto)
- Round-trip Mac→EC2→Mac : ~400ms attendus (2× one-way ~190ms) au lieu de ~800ms avec double transcode
