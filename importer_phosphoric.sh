#!/bin/sh
# importer_phosphoric.sh — Recopie dans le dépôt du port les SEULS fichiers de
# Phosphoric nécessaires (Oric-1/Atmos complet avec LOCI en émulation de haut
# niveau ; sans ACIA/modem, sans réseau, sans SDL), SANS LES MODIFIER.
#
# Usage : sh importer_phosphoric.sh [dossier_phosphoric] [dossier_du_port]
#   défauts : /home/pi/Projets/phosphoric  et  le dossier de ce script
#
# Résultat :
#   <port>/phosphoric/include/...   en-têtes      (arborescence d'origine)
#   <port>/phosphoric/src/...       sources .c    (arborescence d'origine)
#   <port>/phosphoric/roms/loci/    ROM du menu LOCI + notice (BSD-3-Clause)
#   <port>/phosphoric/LICENSE       licence EUPL-1.2 de Phosphoric
#   <port>/phosphoric/COMMIT        commit Git d'origine (traçabilité)
#
# Liste établie par fermeture des dépendances à l'édition de liens sur
# Phosphoric 675cb06 (2026-10-05), LOCI compris. Si ton clone est plus
# récent et qu'un fichier manque, le script s'arrête et le signale.
set -eu

PHOS=${1:-/home/pi/Projets/phosphoric}
PORT=${2:-$(cd "$(dirname "$0")" && pwd)}
DEST="$PORT/phosphoric"

PHOSPHORIC_SOURCES="
  src/audio/ay3891x.c
  src/cards/card_jasmin.c
  src/cards/card_loci.c
  src/cards/card_microdisc.c
  src/cards/card_modules.c
  src/cards/card_ula_ng.c
  src/cpu/addressing.c
  src/cpu/cpu6502.c
  src/cpu/microseq.c
  src/cpu/opcodes.c
  src/emu_clock.c
  src/io/cassette.c
  src/io/io_bus.c
  src/io/jasmin.c
  src/io/joystick.c
  src/io/keyboard.c
  src/io/loci_boot.c
  src/io/loci_bus.c
  src/io/loci_core.c
  src/io/loci_emu_stub.c
  src/io/loci_fs.c
  src/io/loci_gfx.c
  src/io/loci_glue.c
  src/io/loci_sdimg.c
  src/io/mcp40.c
  src/io/microdisc.c
  src/io/printer.c
  src/io/tape_patches.c
  src/io/ula_ng.c
  src/io/via6522.c
  src/memory/banking.c
  src/memory/memory.c
  src/rom_patches.c
  src/savestate.c
  src/storage/disk.c
  src/storage/sedoric.c
  src/storage/tap.c
  src/utils/logging.c
  src/video/hires.c
  src/video/textmode.c
  src/video/video.c
"

PHOSPHORIC_HEADERS="
  include/audio/audio.h
  include/card_module.h
  include/card_ticks.h
  include/cards.h
  include/cards_list.h
  include/cli/cli_opts.h
  include/cpu/cpu6502.h
  include/cpu/cpu_internal.h
  include/cpu/microseq.h
  include/debugger.h
  include/emulator.h
  include/hostfs/hostfs.h
  include/io/acia6551.h
  include/io/bus_timing.h
  include/io/cassette.h
  include/io/io_bus.h
  include/io/io_device.h
  include/io/jasmin.h
  include/io/joystick.h
  include/io/keyboard.h
  include/io/loci.h
  include/io/loci_emu.h
  include/io/loci_glue.h
  include/io/loci_internal.h
  include/io/loci_sdimg.h
  include/io/mcp40.h
  include/io/microdisc.h
  include/io/printer.h
  include/io/serial_backend.h
  include/io/tape_patches.h
  include/io/ula_ng.h
  include/io/via6522.h
  include/memory/memory.h
  include/network/cast_server.h
  include/network/gdbstub.h
  include/rom_patches.h
  include/savestate.h
  include/storage/disk.h
  include/storage/disk_http.h
  include/storage/sedoric.h
  include/storage/tap.h
  include/utils/binio.h
  include/utils/logging.h
  include/utils/movie.h
  include/utils/oscompat.h
  include/utils/profiler.h
  include/utils/symbols.h
  include/utils/trace.h
  include/video/avi_recorder.h
  include/video/iomenu.h
  include/video/osd.h
  include/video/video.h
"

# ROM du menu LOCI (sodiumlb, BSD-3-Clause) et sa notice de provenance
PHOSPHORIC_ROMS="
  roms/loci/locirom
  roms/loci/README.md
"

[ -d "$PHOS/src" ] || { echo "Phosphoric introuvable : $PHOS" >&2; exit 1; }

missing=0
for f in $PHOSPHORIC_SOURCES $PHOSPHORIC_HEADERS $PHOSPHORIC_ROMS LICENSE; do
    [ -f "$PHOS/$f" ] || { echo "MANQUANT dans Phosphoric : $f" >&2; missing=1; }
done
[ $missing -eq 0 ] || exit 1

mkdir -p "$DEST"
for f in $PHOSPHORIC_SOURCES $PHOSPHORIC_HEADERS $PHOSPHORIC_ROMS LICENSE; do
    mkdir -p "$DEST/$(dirname "$f")"
    cp -p "$PHOS/$f" "$DEST/$f"
done

if git -C "$PHOS" rev-parse HEAD >/dev/null 2>&1; then
    git -C "$PHOS" log -1 --format='%H %cs %s' > "$DEST/COMMIT"
else
    echo "inconnu (pas un clone Git)" > "$DEST/COMMIT"
fi

n_src=$(echo $PHOSPHORIC_SOURCES | wc -w)
n_hdr=$(echo $PHOSPHORIC_HEADERS | wc -w)
echo "Copié : $n_src sources + $n_hdr en-têtes depuis $PHOS"
echo "Commit d'origine : $(cat "$DEST/COMMIT")"
