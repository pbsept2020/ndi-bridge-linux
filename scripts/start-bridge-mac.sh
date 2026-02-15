#!/bin/bash
# start-bridge-mac.sh — Lance la Web UI en root (contourne NECP pour les joins via en7)
#
# Le bug NECP de macOS empêche les processus non-root de recevoir de l'UDP
# via une interface secondaire (en7/AX88179A/SpeedFusion). En lançant
# ndi-bridge-x en sudo, TOUS les pipelines (hosts ET joins) héritent
# des privilèges root → le join fonctionne sans terminal séparé.

set -e
cd "$(dirname "$0")/.."

BINARY="./build-mac/ndi-bridge-x"

if [ ! -f "$BINARY" ]; then
    echo "Erreur: $BINARY introuvable. Lancez d'abord: cmake -B build-mac && cmake --build build-mac"
    exit 1
fi

echo "NDI Bridge X — lancement en mode privilegie (sudo)"
echo "Web UI accessible sur http://$(hostname):8080"
echo ""
sudo "$BINARY" --web-ui --clean "$@"
