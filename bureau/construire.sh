#!/bin/sh
# construire.sh — Phosphoric pour Windows et pour le web, avec les
# améliorations du port oric-rpi (patchs de bureau/patches/).
#
#   sh bureau/construire.sh [windows] [web]       (les deux par défaut)
#
# Clone Phosphoric au commit validé (acfeacc, 2.31.15) dans bureau/build/,
# y applique les patchs (git am) et construit :
#   bureau/build/phosphoric-windows/   oric1-emu.exe + DLL + roms/ + .bat
#   bureau/build/phosphoric-web/       page web (servir en HTTP)
#
# Prérequis (Debian / Raspberry Pi OS, RPI 400 comprise) :
#   Windows : sudo apt install mingw-w64 zip
#             + SDL2 pour MinGW dans /opt/sdl2-mingw (téléchargé ici si absent)
#   Web     : emsdk actif (source .../emsdk/emsdk_env.sh), voir README.md
# ROM (non fournies) : basic11b.rom et microdis.rom dans bureau/roms/, ou
# ROMS=/chemin/vers/les/rom.
set -e
ICI=$(cd "$(dirname "$0")" && pwd)
BUILD="$ICI/build"
ROMS=${ROMS:-$ICI/roms}
COMMIT=acfeacc
SDL2_VER=2.30.9
CIBLES=${*:-windows web}

mkdir -p "$BUILD"
SRC="$BUILD/phosphoric"
if [ ! -d "$SRC/.git" ]; then
  git clone -q https://github.com/benedictemarty/Phosphoric.git "$SRC"
fi
cd "$SRC"
git am --abort 2>/dev/null || true
git checkout -q -B oric-rpi "$COMMIT"
git -c user.name=oric-rpi -c user.email=oric-rpi@localhost am -q "$ICI"/patches/*.patch
echo "Phosphoric $COMMIT + $(ls "$ICI"/patches/*.patch | wc -l) patchs"

for r in basic11b.rom microdis.rom; do
  if [ -f "$ROMS/$r" ]; then cp "$ROMS/$r" roms/; else echo "Attention : $ROMS/$r absente"; fi
done

for cible in $CIBLES; do
case $cible in
windows)
  SDL2_WIN_PREFIX=${SDL2_WIN_PREFIX:-/opt/sdl2-mingw/x86_64-w64-mingw32}
  if [ ! -d "$SDL2_WIN_PREFIX" ]; then
    SDL2_WIN_PREFIX="$BUILD/sdl2-mingw/x86_64-w64-mingw32"
    if [ ! -d "$SDL2_WIN_PREFIX" ]; then
      mkdir -p "$BUILD/sdl2-mingw"
      curl -sL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VER/SDL2-devel-$SDL2_VER-mingw.tar.gz" \
        | tar xz -C "$BUILD/sdl2-mingw" --strip-components=1
    fi
  fi
  make WIN=1 SDL2=1 SDL2_WIN_PREFIX="$SDL2_WIN_PREFIX" -j4
  OUT="$BUILD/phosphoric-windows"
  rm -rf "$OUT" && mkdir -p "$OUT/loci" "$OUT/usb"
  cp oric1-emu.exe "$SDL2_WIN_PREFIX/bin/SDL2.dll" "$OUT/"
  cp /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll "$OUT/" 2>/dev/null || \
    cp /usr/lib/gcc/x86_64-w64-mingw32/*-posix/libwinpthread-1.dll "$OUT/"
  cp roms/loci/locirom "$OUT/loci/"
  for r in basic11b.rom microdis.rom; do [ -f roms/$r ] && cp roms/$r "$OUT/loci/"; done
  cp LICENSE "$OUT/LICENCE-Phosphoric-EUPL-1.2.txt"
  cp "$ICI"/windows/* "$OUT/"
  (cd "$BUILD" && rm -f phosphoric-windows.zip && zip -qr phosphoric-windows.zip phosphoric-windows)
  echo "Windows : $OUT (et $BUILD/phosphoric-windows.zip)"
  ;;
web)
  # Le port SDL2 d'Emscripten se télécharge depuis github.com ; si le réseau
  # le refuse : git clone -b release-2.32.10 https://github.com/libsdl-org/SDL
  # puis EMCC_LOCAL_PORTS="sdl2=/chemin/SDL" sh bureau/construire.sh web
  make wasm
  OUT="$BUILD/phosphoric-web"
  rm -rf "$OUT" && mkdir -p "$OUT"
  cp web/phosphoric.html web/phosphoric.js web/phosphoric.wasm web/phosphoric.data \
     web/shell.js web/picowifi_js.js "$OUT/"
  cp "$ICI"/web/* "$OUT/"
  echo "Web : $OUT (servir en HTTP : sh $OUT/lancer.sh)"
  ;;
*) echo "Cible inconnue : $cible (windows, web)"; exit 1 ;;
esac
done
