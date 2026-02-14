# PROMPT: Ajouter monitoring Metal + SwiftUI au bridge Mac existant

> **Projet :** ndi-bridge-mac (existant, Swift CLI)
> **Objectif :** Ajouter une fenêtre de monitoring vidéo Metal avec overlays SwiftUI
> **Plateforme :** macOS 14+ (Apple Silicon)
> **Contrainte :** NE PAS casser le fonctionnement CLI existant

---

## Contexte

Le projet `ndi-bridge-mac` est un bridge NDI fonctionnel en Swift CLI :
- **Host mode** : capture NDI → encode H.264 (VideoToolbox) → envoi UDP
- **Join mode** : réception UDP → décode H.264 (VideoToolbox) → output NDI

Il fonctionne avec ~75ms de latence. On veut maintenant ajouter une **fenêtre de preview/monitoring Metal** pour visualiser le flux en transit, avec des overlays broadcast SwiftUI.

Le document `swiftui-metal.md` à la racine contient la recherche technique complète. **Lis-le intégralement** — il contient les shaders Metal, les patterns d'architecture, et tous les détails de pipeline. Les sections pertinentes pour macOS sont signalées (NSViewRepresentable, storage modes macOS, etc.).

---

## Points d'injection dans le code existant

### Host mode — `HostMode.swift`
Le delegate `ndiReceiver(_:didReceiveVideoFrame:pixelBuffer:timestamp:frameNumber:)` reçoit un `CVPixelBuffer` NV12 IOSurface-backed directement du NDI SDK. C'est notre source vidéo côté Host.

```swift
// DANS HostMode.swift — delegate existant
func ndiReceiver(_ receiver: NDIReceiver, didReceiveVideoFrame pixelBuffer: CVPixelBuffer, timestamp: UInt64, frameNumber: UInt64) {
    // Existant : encode et envoi réseau
    try encoder.encode(pixelBuffer: pixelBuffer, timestamp: timestamp)
    
    // AJOUTER : feed le monitoring Metal (zero-copy, même CVPixelBuffer)
    monitorRenderer?.updateFrame(pixelBuffer)
}
```

### Join mode — `JoinMode.swift`
Le delegate `videoDecoder(_:didDecodeFrame:pixelBuffer:timestamp:)` reçoit un `CVPixelBuffer` NV12 IOSurface-backed du décodeur VideoToolbox. C'est notre source vidéo côté Join.

```swift
// DANS JoinMode.swift — delegate existant
func videoDecoder(_ decoder: VideoDecoder, didDecodeFrame pixelBuffer: CVPixelBuffer, timestamp: UInt64) {
    // Existant : envoi NDI ou buffer
    try ndiSender.send(pixelBuffer: pixelBuffer, timestamp: timestamp)
    
    // AJOUTER : feed le monitoring Metal (zero-copy, même CVPixelBuffer)
    monitorRenderer?.updateFrame(pixelBuffer)
}
```

**Les deux CVPixelBuffers sont IOSurface-backed** → zero-copy vers Metal via CVMetalTextureCache, pas de copie nécessaire.

---

## Modifications à apporter

### Étape 1 — Conversion CLI → SwiftUI App avec fenêtre

Actuellement le projet utilise `main.swift` comme entry point CLI. Il faut :

1. **Garder le fonctionnement CLI** : les arguments `host`, `join`, `discover` doivent continuer à fonctionner
2. **Ajouter une fenêtre SwiftUI** : quand on lance en mode `host` ou `join`, ouvrir une fenêtre de monitoring
3. **Entry point** : remplacer `main.swift` par un `@main struct` SwiftUI App qui parse les arguments puis lance le mode approprié avec la fenêtre

```swift
// BridgeApp.swift — nouveau entry point
import SwiftUI

@main
struct BridgeApp: App {
    @StateObject private var bridgeController = BridgeController()
    
    var body: some Scene {
        WindowGroup {
            MonitorView(controller: bridgeController)
                .frame(minWidth: 640, minHeight: 360)
        }
        .windowStyle(.titleBar)
        .defaultSize(width: 960, height: 540)
    }
}
```

`BridgeController` encapsule la logique existante de `HostMode` / `JoinMode` et expose le `VideoRenderer` pour l'affichage.

### Étape 2 — Metal pipeline (fichiers à créer)

Créer un dossier `Sources/NDIBridge/Monitor/` :

```
Sources/NDIBridge/Monitor/
├── Shaders.metal           # Compute shaders NV12→BGRA
├── MetalVideoView.swift    # NSViewRepresentable wrapping MTKView
├── VideoRenderer.swift     # MTKViewDelegate + CVMetalTextureCache
└── TexturePool.swift       # Pool de MTLTextures pré-allouées
```

#### Shaders.metal
Copier le shader `nv12ToBGRA` du doc `swiftui-metal.md` §4.1. Le shader NV12→UYVY n'est PAS nécessaire ici (on affiche seulement, on ne réencode pas pour NDI).

#### MetalVideoView.swift — **NSViewRepresentable** (macOS, PAS UIViewRepresentable)
```swift
import SwiftUI
import MetalKit

struct MetalVideoView: NSViewRepresentable {
    @ObservedObject var renderer: VideoRenderer
    
    func makeNSView(context: Context) -> MTKView {
        let mtkView = MTKView(frame: .zero, device: renderer.device)
        mtkView.delegate = renderer
        mtkView.colorPixelFormat = .bgra8Unorm
        mtkView.framebufferOnly = false
        // Mode on-demand : dessine uniquement quand une nouvelle frame arrive
        mtkView.isPaused = true
        mtkView.enableSetNeedsDisplay = true
        mtkView.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        renderer.mtkView = mtkView
        return mtkView
    }
    
    func updateNSView(_ nsView: MTKView, context: Context) {}
}
```

#### VideoRenderer.swift
Implémente `MTKViewDelegate` avec :
- `CVMetalTextureCache` créé une seule fois (réutilisé, jamais recréé)
- `makeTextures(from: CVPixelBuffer) -> (MTLTexture, MTLTexture)?` pour import NV12 bi-planar zero-copy (§3.2 du doc)
- `draw(in: MTKView)` : compute dispatch NV12→BGRA → blit vers drawable (§5.1)
- `DispatchSemaphore(value: 3)` pour limiter les frames in-flight (§11.2)
- `CVMetalTextureCacheFlush` après chaque commit (§11.3)
- `updateFrame(_ pixelBuffer: CVPixelBuffer)` : appelé depuis le thread NDI/décodeur, protégé par `NSLock`, trigger `setNeedsDisplay()` sur main thread

**Storage mode macOS Apple Silicon** : `.private` pour les textures GPU-only intermédiaires. Le CVPixelBuffer IOSurface-backed est importé via `CVMetalTextureCacheCreateTextureFromImage` qui gère le mapping automatiquement.

#### TexturePool.swift
Pool de 3 textures BGRA `.private` pré-allouées pour le compute output (§5.3 du doc, mais avec `.private` au lieu de `.shared` car macOS).

### Étape 3 — Overlays SwiftUI

Créer `Sources/NDIBridge/Monitor/Overlays/` :

```
Sources/NDIBridge/Monitor/Overlays/
├── TallyBorderView.swift      # Bordure rouge/verte
├── TimecodeView.swift         # Timecode SMPTE monospace
├── AudioMeterView.swift       # VU-mètres dBFS
├── SafeAreaOverlay.swift      # Title safe + Action safe + crosshair
└── StatusBarView.swift        # Barre d'état : mode, source, bitrate, latence, FPS
```

Le code exact des overlays est dans `swiftui-metal.md` §6.2. Les adapter pour macOS (pas de changement nécessaire, SwiftUI est identique).

Ajouter une `StatusBarView` spécifique au bridge qui affiche :
- Mode actif (HOST / JOIN)
- Nom de la source NDI connectée
- Résolution et framerate
- Bitrate d'encodage (host) ou débit réseau reçu (join)
- Latence estimée
- Compteur FPS

### Étape 4 — Vue principale MonitorView

```swift
struct MonitorView: View {
    @ObservedObject var controller: BridgeController
    
    var body: some View {
        ZStack {
            // Vidéo Metal (fond)
            MetalVideoView(renderer: controller.renderer)
            
            // Overlays broadcast
            if controller.showSafeAreas {
                SafeAreaOverlay()
            }
            
            TallyBorderView(tallyState: controller.tallyState)
            
            VStack {
                // Barre d'état en haut
                StatusBarView(controller: controller)
                Spacer()
                // Timecode en bas à gauche
                HStack {
                    TimecodeView(timecode: controller.currentTimecode)
                    Spacer()
                }
                .padding(8)
            }
            
            // Audio meters à droite
            HStack {
                Spacer()
                AudioMeterView(levels: controller.audioLevels)
                    .frame(width: 40)
                    .padding(.trailing, 8)
            }
        }
        .background(Color.black)
    }
}
```

### Étape 5 — BridgeController (glue)

`BridgeController` est un `ObservableObject` qui :
1. Parse les arguments CLI au lancement (`CommandLine.arguments`)
2. Instancie `HostMode` ou `JoinMode` selon le mode
3. Expose un `VideoRenderer` partagé
4. Se branche en delegate sur les modes existants pour recevoir les CVPixelBuffers
5. Extrait les métadonnées (timecode, audio levels) pour les overlays
6. Gère les signal handlers (SIGINT) pour shutdown propre

```swift
class BridgeController: ObservableObject {
    let renderer = VideoRenderer()
    
    @Published var mode: BridgeMode = .idle  // .host, .join, .idle
    @Published var sourceName: String = ""
    @Published var resolution: String = ""
    @Published var fps: Double = 0
    @Published var currentTimecode: String = "00:00:00:00"
    @Published var audioLevels: [Float] = [-60, -60]
    @Published var tallyState: TallyState = .none
    @Published var showSafeAreas: Bool = false
    
    private var hostMode: HostMode?
    private var joinMode: JoinMode?
    
    init() {
        parseArgumentsAndStart()
    }
    
    func parseArgumentsAndStart() {
        // Reprendre la logique de main.swift existant
        // Lancer le mode approprié sur un background thread
        // Connecter le renderer pour recevoir les frames
    }
}
```

---

## Fichiers à ne PAS modifier (sauf injection du renderer)

| Fichier | Modification |
|---------|-------------|
| `Host/HostMode.swift` | Ajouter 1 ligne dans `ndiReceiver(_:didReceiveVideoFrame:...)` pour feed le renderer |
| `Join/JoinMode.swift` | Ajouter 1 ligne dans `videoDecoder(_:didDecodeFrame:...)` pour feed le renderer |
| `Host/VideoEncoder.swift` | Aucune modification |
| `Host/NetworkSender.swift` | Aucune modification |
| `Join/VideoDecoder.swift` | Aucune modification |
| `Join/NetworkReceiver.swift` | Aucune modification |
| `Join/NDISender.swift` | Aucune modification |
| `Common/BridgeLogger.swift` | Aucune modification |
| `CNDIWrapper/*` | Aucune modification |

Le fichier `main.swift` est **remplacé** par `BridgeApp.swift` (entry point SwiftUI `@main`).

---

## Différences macOS vs iOS (IMPORTANT)

| Aspect | iOS (dans le doc) | macOS (ce qu'on fait) |
|--------|-------------------|----------------------|
| View wrapper | `UIViewRepresentable` | **`NSViewRepresentable`** |
| make/update | `makeUIView` / `updateUIView` | **`makeNSView`** / `updateNSView` |
| Storage mode textures intermédiaires | `.shared` (UMA) | **`.private`** (GPU-only, optimal Apple Silicon) |
| IOSurface inter-process | ❌ Interdit (sandbox) | ✅ Supporté |
| CVPixelBuffer des sources | Pas IOSurface (NDI raw) | ✅ IOSurface-backed (NDI SDK + VideoToolbox) |
| ProMotion 120Hz | Gérer le judder 25fps | **Non applicable** (écrans 60Hz) |
| Thermal throttling | Adapter framerate | **Non applicable** (Mac branché secteur) |

---

## Package.swift

Le projet utilise SwiftPM. Il faut ajouter les dépendances framework macOS :

```swift
// Dans le target NDIBridge, ajouter :
.linkedFramework("MetalKit"),
.linkedFramework("Metal"),
.linkedFramework("CoreVideo"),
.linkedFramework("QuartzCore"),
```

Et s'assurer que le target peut importer SwiftUI (macOS 14+ deployment target).

---

## Ordre d'implémentation

1. Lis `swiftui-metal.md` intégralement
2. Crée `Sources/NDIBridge/Monitor/Shaders.metal` (shader nv12ToBGRA du §4.1)
3. Crée `Sources/NDIBridge/Monitor/TexturePool.swift`
4. Crée `Sources/NDIBridge/Monitor/VideoRenderer.swift`
5. Crée `Sources/NDIBridge/Monitor/MetalVideoView.swift` (NSViewRepresentable)
6. Crée `Sources/NDIBridge/Monitor/BridgeController.swift`
7. Remplace `main.swift` par `Sources/NDIBridge/App/BridgeApp.swift` + `MonitorView.swift`
8. Ajoute 1 ligne dans `HostMode.swift` et 1 ligne dans `JoinMode.swift` pour feed le renderer
9. Crée les overlays (TallyBorder, Timecode, AudioMeter, SafeArea, StatusBar)
10. Compile et corrige

**Compile après chaque fichier créé.**
