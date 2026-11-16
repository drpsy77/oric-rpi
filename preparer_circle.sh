#!/bin/sh
# preparer_circle.sh — Réglages du port dans circle-stdlib (FatFs de Circle)
#
# Usage : sh preparer_circle.sh [dossier_circle_stdlib]
#   défaut : /home/pi/Projets/circle-stdlib
#
# libs/circle/addon/fatfs/ffconf.h :
#   FF_FS_EXFAT  0 -> 1   clés USB en exFAT (format des clés récentes,
#                         celles du vrai LOCI comprises)
#   FF_USE_LABEL 0 -> 1   nom et numéro de série du volume (f_getlabel) :
#                         une clé retirée puis remise est reconnue
#
# Les structures de FatFs changent de taille : circle-newlib (libgloss)
# les contient. Tout circle-stdlib doit donc être reconstruit ensuite
# (make clean && make, puis le pilote Wi-Fi : voir CLAUDE.md). Le script
# ne fait rien si les réglages sont déjà en place.
set -eu

STDLIB=${1:-/home/pi/Projets/circle-stdlib}
FFCONF=$STDLIB/libs/circle/addon/fatfs/ffconf.h
[ -f "$FFCONF" ] || { echo "$FFCONF introuvable"; exit 1; }

regle () {
	if grep -q "^#define $1[[:space:]]*$3\$" "$FFCONF"; then
		echo "$1 : deja $3"
	elif grep -q "^#define $1[[:space:]]*$2\$" "$FFCONF"; then
		sed -i "s/^#define $1\([[:space:]]*\)$2\$/#define $1\1$3/" "$FFCONF"
		echo "$1 : $2 -> $3"
		CHANGE=1
	else
		echo "$1 : valeur inattendue dans $FFCONF"; exit 1
	fi
}

CHANGE=0
regle FF_FS_EXFAT 0 1
regle FF_USE_LABEL 0 1
if [ $CHANGE = 1 ]; then
	echo "A faire : cd $STDLIB && make clean && make, puis le pilote Wi-Fi (CLAUDE.md)"
fi
