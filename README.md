# Oric sur Raspberry Pi Zero (bare-metal)

Émulateur Oric-1 / Atmos pour Raspberry Pi Zero, sans Linux, construit avec
[Circle](https://github.com/rsta2/circle) et
[circle-stdlib](https://github.com/smuehlst/circle-stdlib).

Ce n'est **pas un nouvel émulateur** : le cœur est celui de
[Phosphoric](https://github.com/benedictemarty/Phosphoric), recopié **sans
modification** ; ce dépôt n'ajoute que la colle nécessaire pour Circle
(écran, clavier USB, son HDMI, carte SD). Provenance de chaque fichier :
`SOURCES.md`.

État : BASIC 1.1, HIRES, couleurs, son, clavier USB (AZERTY), cassette `.tap`,
disquettes Microdisc, cartouche LOCI en option (émulation de haut niveau de
Phosphoric : menu, API fichiers). Pas d'ACIA/modem ni de réseau.
Sur un Pi Zero d'origine : environ 13 ms par trame de 20 ms sans LOCI, mais
le LOCI dépasse le budget (≈ 23 ms) ; il vise un Pi 3 ou plus.

## Construire

```sh
# 1. circle-stdlib (une fois), pour Pi Zero / Zero W
#    paquets : gcc-arm-none-eabi, libnewlib-arm-none-eabi, texinfo
git clone --recursive https://github.com/smuehlst/circle-stdlib.git
cd circle-stdlib && ./configure -r 1 && make
cd libs/circle/boot && make            # télécharge le firmware du Pi

# 2. cloner Phosphoric (version validée : 675cb06)
git clone https://github.com/benedictemarty/Phosphoric.git phosphoric
cd phosphoric && git checkout 675cb06 && cd ..

# 3. importer ses sources dans ce dépôt (crée le dossier phosphoric/)
cd <ce dépôt>
sh importer_phosphoric.sh /chemin/vers/phosphoric

# 4. le kernel
make CIRCLESTDLIB=/chemin/vers/circle-stdlib     # produit kernel.img
```

## Carte SD (FAT32)

```
bootcode.bin  start.elf  fixup.dat      ← circle-stdlib/libs/circle/boot
kernel.img                              ← ce dépôt
cmdline.txt   config.txt                ← sdcard/
oric/basic11b.rom                       ← ROM Atmos (non fournie)
oric/microdis.rom, oric/xxx.dsk         ← optionnel (Microdisc)
oric/xxx.tap                            ← optionnel (cassette)
oric/loci/locirom                       ← avec LOCI : phosphoric/roms/loci/
oric/loci/basic11b.rom, microdis.rom    ← avec LOCI (non fournies)
oric/usb/                               ← avec LOCI : vos .dsk, .tap…
```

Avec LOCI, `oric/loci/` est sa mémoire flash interne (« 0: ») et
`oric/usb/` sa clé USB (« 1: »).

Options de `cmdline.txt` (une seule ligne) : `width=240 height=224`,
`keymap=FR` (AZERTY), `rom=`, `diskrom=` + `disk=` (les deux pour activer le
Microdisc), `tape=`, `fdctiming=fast` (Microdisc sans délais mécaniques ; par
défaut `real`, vitesse du vrai lecteur).

LOCI : `loci=1`, et **pas** de `rom=` : la ROM système est alors le menu
(`oric/loci/locirom`). `lociflash=` et `lociusb=` changent les deux
dossiers. Menu : flèches, Espace pour ouvrir/choisir, Échap pour démarrer
l'Oric.

Touches : F5 = reset, Maj+F5 = NMI, F8 = bouton Action du LOCI (retour au
menu), F12 = mesure de performance dans
`oric/perf.txt`.

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
