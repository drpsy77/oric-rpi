#!/bin/sh
# installer_carte.sh — Copie sur la carte SD (branchée sur la RPI 400 par le
# lecteur USB) les disquettes de CP/A construites par build.sh, et le noyau
# avec --noyau.
#
# Usage : sh installer_carte.sh [--noyau] [dossier_cpa_oric]
#   défaut : /home/pi/Projets/cpa-oric
#
# Disquettes (build/*.dsk) copiées dans :
#   oric/usb/    clé de LOCI (« 1: », ou « 2: » avec le modem)
#   oric/loci/   cpa.dsk et cpa-en.dsk seulement : flash « 0: », dont
#                locidisk=0:/cpa-en.dsk démarre le système. À ne pas oublier :
#                c'est le système qui tourne (CPA.BNK refusé sinon).
# Les images remplacées dans oric/loci/ (qui peuvent porter des fichiers
# écrits sur A:) sont gardées dans oric/loci/ancien/.
# Sans LOCI : disk=oric/usb/cpa.dsk (rien à la racine oric/).
# La carte est montée si besoin, puis démontée : prête à être retirée.
set -eu

NOYAU=0
if [ "${1:-}" = "--noyau" ]; then NOYAU=1; shift; fi
CPA=${1:-/home/pi/Projets/cpa-oric}
PORT=$(cd "$(dirname "$0")" && pwd)
DEV=/dev/sda1
CARTE=/media/pi/ORIC

ls "$CPA"/build/*.dsk >/dev/null 2>&1 || { echo "Pas de disquettes dans $CPA/build (build.sh ?)"; exit 1; }
[ -b "$DEV" ] || { echo "Carte absente ($DEV)"; exit 1; }
mountpoint -q "$CARTE" || udisksctl mount -b "$DEV"
[ -d "$CARTE/oric" ] || { echo "$CARTE/oric absent : est-ce la bonne carte ?"; exit 1; }

mkdir -p "$CARTE/oric/usb" "$CARTE/oric/loci/ancien"
cp "$CPA"/build/*.dsk "$CARTE/oric/usb/"
for d in cpa.dsk cpa-en.dsk; do
	if [ -f "$CARTE/oric/loci/$d" ] && ! cmp -s "$CPA/build/$d" "$CARTE/oric/loci/$d"; then
		cp "$CARTE/oric/loci/$d" "$CARTE/oric/loci/ancien/$d"
	fi
	cp "$CPA/build/$d" "$CARTE/oric/loci/"
done
echo "Disquettes copiées (oric/usb/, oric/loci/)."

if [ $NOYAU = 1 ]; then
	[ -f "$PORT/kernel8-32.img" ] || { echo "kernel8-32.img absent (make ?)"; exit 1; }
	cp "$PORT/kernel8-32.img" "$CARTE/"
	echo "Noyau copié."
fi

sync
udisksctl unmount -b "$DEV"
echo "Carte démontée : tu peux la retirer."
