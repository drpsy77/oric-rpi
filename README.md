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
disquettes Microdisc. Environ 13 ms d'émulation par trame de 20 ms sur un
Pi Zero d'origine. Pas d'ACIA/modem ni de réseau ; l'émulation LOCI de
Phosphoric est trop lourde pour le Zero.

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
```

Options de `cmdline.txt` (une seule ligne) : `width=240 height=224`,
`keymap=FR` (AZERTY), `rom=`, `diskrom=` + `disk=` (les deux pour activer le
Microdisc), `tape=`, `fdctiming=fast` (Microdisc sans délais mécaniques ; par
défaut `real`, vitesse du vrai lecteur).

Touches : F5 = reset, Maj+F5 = NMI, F12 = mesure de performance dans
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
