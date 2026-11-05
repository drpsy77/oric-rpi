# Oric sur Raspberry Pi (bare-metal)

Émulateur Oric-1 / Atmos pour Raspberry Pi 3B+ et Zero 2 W, sans Linux, construit avec
[Circle](https://github.com/rsta2/circle) et
[circle-stdlib](https://github.com/smuehlst/circle-stdlib).

Ce n'est **pas un nouvel émulateur** : le cœur est celui de
[Phosphoric](https://github.com/benedictemarty/Phosphoric), recopié **sans
modification** ; ce dépôt n'ajoute que la colle nécessaire pour Circle
(écran, clavier USB, son HDMI, carte SD, Wi-Fi). Provenance de chaque fichier :
`SOURCES.md`.

État : BASIC 1.1, HIRES, couleurs, son, clavier USB (AZERTY), cassette `.tap`,
disquettes Microdisc, cartouche LOCI en option (émulation de haut niveau de
Phosphoric : menu, API fichiers), modem Wi-Fi de LOCI en option.
L'émulation tourne sur le cœur 1, le reste (clavier, son, réseau) sur le
cœur 0. Sur une Pi 3B+ à 1,4 GHz : environ 7 ms par trame de 20 ms, LOCI
compris. Le Pi Zero d'origine (un seul cœur, trop lent avec LOCI) n'est plus
visé.

## Construire

```sh
# 1. circle-stdlib (une fois), pour Pi 3B+ / Zero 2 W, multicœur
#    paquets : gcc-arm-none-eabi, libnewlib-arm-none-eabi, texinfo
git clone --recursive https://github.com/smuehlst/circle-stdlib.git
cd circle-stdlib && ./configure -r 3 -o ARM_ALLOW_MULTI_CORE && make
(cd libs/circle/boot && make)                     # firmware du Pi
(cd libs/circle/addon/wlan/firmware && make firmware)   # firmware Wi-Fi
# pilote Wi-Fi recompilé avec les en-têtes de circle-newlib (sinon jmp_buf
# de la mauvaise taille et plantage au démarrage avec modem=1)
STDLIB=$PWD
(cd libs/circle/addon/wlan && ./makeall --nosample clean && \
 EXTRAINCLUDE="-isystem $STDLIB/install/arm-none-circle/include" ./makeall --nosample)

# 2. cloner Phosphoric (version validée : 675cb06)
git clone https://github.com/benedictemarty/Phosphoric.git phosphoric
cd phosphoric && git checkout 675cb06 && cd ..

# 3. importer ses sources dans ce dépôt (crée le dossier phosphoric/)
cd <ce dépôt>
sh importer_phosphoric.sh /chemin/vers/phosphoric

# 4. le kernel
make        # ou make CIRCLESTDLIB=/chemin/vers/circle-stdlib
            # produit kernel8-32.img
```

## Carte SD

Table MBR, première partition en FAT32 (son nom est sans importance).

```
bootcode.bin  start.elf  fixup.dat      ← circle-stdlib/libs/circle/boot
kernel8-32.img                          ← ce dépôt
cmdline.txt   config.txt                ← sdcard/
firmware/  wpa_supplicant.conf          ← avec le modem (modem=1) :
                                          addon/wlan/firmware, sdcard/
oric/basic11b.rom                       ← ROM Atmos (non fournie)
oric/microdis.rom, oric/xxx.dsk         ← optionnel (Microdisc)
oric/xxx.tap                            ← optionnel (cassette)
oric/loci/locirom                       ← avec LOCI : phosphoric/roms/loci/
oric/loci/basic11b.rom, microdis.rom    ← avec LOCI (non fournies)
oric/usb/                               ← avec LOCI : vos .dsk, .tap…
```

Avec LOCI, `oric/loci/` est sa mémoire flash interne (« 0: ») et
`oric/usb/` sa clé USB (« 1: »).

Options de `cmdline.txt` (une seule ligne) : `width=590 height=448`
(taille de l'image envoyée à l'écran), `pixels=tele` (défaut : points 1,23
fois plus larges que hauts, comme sur un téléviseur PAL) ou `pixels=carres`
(avec `width=240 height=224`), `couleurs=tele` (défaut : couleurs d'un
téléviseur) ou `couleurs=pures`,
`keymap=FR` (AZERTY), `rom=`, `diskrom=` + `disk=` (les deux pour activer le
Microdisc), `tape=`, `fdctiming=fast` (Microdisc sans délais mécaniques ; par
défaut `real`, vitesse du vrai lecteur).

LOCI : `loci=1`, et **pas** de `rom=` : la ROM système est alors le menu
(`oric/loci/locirom`). `lociflash=` et `lociusb=` changent les deux
dossiers. Menu : flèches, Espace pour ouvrir/choisir, Échap pour démarrer
l'Oric. Entrée (reprendre la session interrompue par F8) ne marche pas
encore : limite de Phosphoric, voir `docs/phosphoric-f8.md`.

Modem Wi-Fi de LOCI : `modem=1`. L'Oric voit le modem picowifi en `$0380`
(commandes AT, `ATDT-hôte:port`), comme avec un Pico W branché sur le vrai
LOCI ; c'est la Pi qui se connecte au Wi-Fi. Il faut à la racine de la carte
le dossier `firmware/` (fichiers `brcmfmac*`, obtenus par `make firmware` dans
`libs/circle/addon/wlan/firmware` de circle-stdlib) et `wpa_supplicant.conf`
(modèle dans `sdcard/`, à compléter avec le nom du réseau et le mot de passe).
L'adresse obtenue est notée dans `oric/journal.txt`. L'ACIA du modem n'est
pas cadencée (comme sur le vrai LOCI) ; `modemcadence=1` rend la cadence de
Phosphoric (9600 bauds).

Démarrage direct sur une disquette, sans le menu : `locidisk=0:/cpa.dsk`
(avec `loci=1`), l'image étant dans `oric/loci/`. Elle est en A: de LOCI ; F8
montre le menu avec A: rempli, `USBMOUNT` monte les autres depuis la clé.

Réglages modifiables depuis l'Oric : `oric/loci/oric.cfg` (`0:ORIC.CFG` pour
LOCI), mêmes noms que dans `cmdline.txt`, qu'ils remplacent (sauf `width`,
`height`, `lociflash`). Modèle dans `sdcard/`.

Touches : F5 = reset, Maj+F5 = NMI, F8 = bouton Action du LOCI (retour au
menu), F12 = ajoute une mesure de performance à
`oric/perf.txt`, Ctrl+Alt+Suppr = disquettes réécrites, puis redémarrage de
la Pi.

## Merci

- **bmarty**, auteur de Phosphoric : tout le travail d'émulation de ce port
  est le sien.
- **Sodiumlightbaby**, créateur du LOCI et de sa ROM de menu, que Phosphoric
  émule et livre.
- Et à R. Stange (Circle), S. Mühlstrasser (circle-stdlib) et aux auteurs
  d'Oricutron, dont Phosphoric reprend une partie.

## Licences

Ce dépôt : EUPL-1.2 (`LICENSE`). Phosphoric : EUPL-1.2. Circle et circle-stdlib : GPL-3.0. ROM du menu LOCI :
BSD-3-Clause. Détails dans `SOURCES.md`.
