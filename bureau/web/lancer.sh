#!/bin/sh
# Sert la page sur http://localhost:8000/ (le navigateur refuse le .wasm en file://)
cd "$(dirname "$0")"
echo "Ouvrir http://localhost:8000/"
python3 -m http.server 8000
