#!/bin/sh
# carte.sh — La carte SD de l'Oric (et ses clés USB) par le réseau, depuis
# la RPI 400, sans retirer la carte.
#
# Usage :
#   sh carte.sh ls [dossier]               liste (racine de la carte par défaut)
#   sh carte.sh envoyer dossier fichier... envoi, un transfert par fichier
#   sh carte.sh lire chemin [local]        lecture d'un fichier
#   sh carte.sh journal                    oric/journal.txt (refus, erreurs)
#
# Dossiers et chemins : ceux de la carte (« oric/usb », « . » ou « / » pour
# la racine, où va kernel8-32.img), ou d'une clé USB de la Pi (« USB: »,
# « USB:/dSK », « USB2: »). Exemples :
#   sh carte.sh ls oric/usb
#   sh carte.sh envoyer oric/usb ../cpa-oric/build/*.dsk
#   sh carte.sh envoyer . kernel8-32.img      (puis Ctrl+Alt+Suppr)
#   sh carte.sh lire USB:/dSK/rogue.dsk
#
# Machine : ORIC=oric.local par défaut (variable d'environnement). Le noyau
# doit avoir tftp=<cette machine> et web= dans cmdline.txt (seul ce client
# est servi) : listes et journal par la page web (/l/, /f/), transferts
# par TFTP (tftp-hpa, 32 Mo par fichier).
#
# Vérification d'un envoi : le serveur TFTP acquitte le dernier bloc avant
# d'écrire le fichier, et tftp -c rend toujours 0 (tftp-hpa 5.2, même sur
# « Error code ») : un refus se voit dans ce qu'il affiche, un échec
# d'écriture seulement dans le journal. Après chaque envoi, la liste du
# dossier doit donc montrer le fichier à la bonne taille, daté de
# l'envoi (heure de la Pi, réglée par NTP) : sinon « non écrit ».
# -l (mode littéral) : sans lui, tftp-hpa prend « USB: » pour un nom de
# machine.
set -u

ORIC=${ORIC:-oric.local}

usage () {
	sed -n '5,9p' "$0" | sed 's/^# \{0,1\}//' >&2
	exit 2
}

# Chemin dans une adresse (« / » et « : » gardés)
encoder () {
	python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe="/:"))' "$1"
}

# Dossier sans « / » final ; racine de la carte : vide
dossier_net () {
	case $1 in
	.|./|/|"") echo "" ;;
	*) printf '%s\n' "$1" | sed 's|^/*||; s|\(.\)/*$|\1|' ;;
	esac
}

# Liste brute de la page web (/l/) ; 1 si la page ne répond pas ou refuse
liste () {
	sortie=$(curl -sS --max-time 20 "http://$ORIC/l/$(encoder "$1")" 2>&1) || {
		echo "$ORIC : pas de réponse de la page web ($sortie) ; web= dans cmdline.txt ?" >&2
		return 1
	}
	case $sortie in
	*"<h1>Not Found</h1>"*)
		echo "$ORIC : adresse /l/ inconnue (noyau plus ancien que carte.sh)" >&2
		return 1 ;;
	"<!DOCTYPE"*|"<html"*)
		echo "$ORIC : accès refusé ou page inattendue (cette machine doit être le client de tftp=)" >&2
		return 1 ;;
	"erreur : "*)
		printf '%s : %s\n' "${1:-/}" "${sortie#erreur : }" >&2
		return 1 ;;
	esac
	printf '%s\n' "$sortie"
}

cmd_ls () {
	liste "$(dossier_net "${1:-}")"
}

# « taille date heure » d'un fichier de la liste (nom sans tenir compte
# de la casse, comme FAT), rien s'il n'y est pas
taille_date () {
	printf '%s\n' "$1" | awk -v nom="$2" '
		/^f / {
			ligne = $0
			sub(/^f +[0-9]+  /, "", ligne)	# reste : date sur 16, 2 espaces, nom
			n = substr(ligne, 19)
			sub(/  montee$/, "", n)
			if (tolower(n) == tolower(nom)) { print $2, substr(ligne, 1, 16); exit }
		}'
}

cmd_envoyer () {
	[ $# -ge 2 ] || usage
	dossier=$(dossier_net "$1")
	shift
	rc=0
	for f in "$@"; do
		if [ ! -f "$f" ]; then
			echo "$f : introuvable ici"
			rc=1
			continue
		fi
		nom=$(basename "$f")
		distant=${dossier:+$dossier/}$nom
		taille=$(stat -c %s "$f")
		debut=$(date -d '-1 min' '+%Y-%m-%d %H:%M')
		sortie=$(tftp -l -m binary "$ORIC" -c put "$f" "$distant" 2>&1)
		if [ -n "$sortie" ]; then
			# refus annoncé par le serveur (disquette montée, clé absente,
			# nom invalide, client) ou pas de réponse
			echo "$distant : non envoyé ($sortie ; voir sh carte.sh journal)"
			rc=1
			continue
		fi
		# écrit par la Pi juste après le dernier acquittement : quelques
		# essais
		etat=""
		for essai in 1 2 3 4 5; do
			l=$(liste "$dossier") || { etat="?"; break; }
			td=$(taille_date "$l" "$nom")
			t=${td%% *}
			d=${td#* }
			if [ -n "$td" ] && [ "$t" = "$taille" ]; then
				case $d in
				-*|19*|20[01]*) etat="heure" ;;	# horloge de la Pi pas réglée
				*) if [ "$d" \> "$debut" ] || [ "$d" = "$debut" ]; then etat="ok"; break; fi ;;
				esac
			fi
			sleep 1
		done
		case $etat in
		ok)	echo "$distant : écrit ($taille octets)" ;;
		heure)	echo "$distant : taille juste, mais heure de la Pi pas réglée (NTP) : non vérifié" ;;
		"?")	echo "$distant : envoyé, non vérifié (liste impossible)" ; rc=1 ;;
		*)	echo "$distant : non écrit (voir sh carte.sh journal)" ; rc=1 ;;
		esac
	done
	return $rc
}

cmd_lire () {
	[ $# -ge 1 ] || usage
	chemin=$(printf '%s\n' "$1" | sed 's|^/*||')
	local_=${2:-$(basename "$chemin")}
	[ -d "$local_" ] && local_=$local_/$(basename "$chemin")
	# dans un fichier à part : tftp crée (ou vide) le fichier local même en
	# cas d'erreur
	tmp=$(mktemp "${local_}.XXXXXX") || exit 1
	sortie=$(tftp -l -m binary "$ORIC" -c get "$chemin" "$tmp" 2>&1)
	if [ -n "$sortie" ]; then
		rm -f "$tmp"
		echo "$chemin : non lu ($sortie)" >&2
		return 1
	fi
	mv "$tmp" "$local_"
	echo "$chemin -> $local_ ($(stat -c %s "$local_") octets)"
}

cmd_journal () {
	curl -sS --fail --max-time 30 "http://$ORIC/f/oric/journal.txt"
}

[ $# -ge 1 ] || usage
commande=$1
shift
case $commande in
ls)		cmd_ls "$@" ;;
envoyer)	cmd_envoyer "$@" ;;
lire)		cmd_lire "$@" ;;
journal)	cmd_journal ;;
*)		usage ;;
esac
