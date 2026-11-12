# Phosphoric pour Windows et le web, avec les améliorations du port

Le port Pi a corrigé ou complété Phosphoric sur plusieurs points (clavier,
affichage, ACIA de LOCI, démarrage sur une disquette…). Ce dossier les
reporte **dans Phosphoric lui-même**, sous forme de patchs, pour en faire une
version Windows et une version web utilisables tout de suite, sans attendre
bmarty, et prêtes à lui être proposées.

Règle du dépôt respectée : le clone de Phosphoric n'est pas touché ; les
modifications sont des patchs documentés, appliqués à une copie dans
`bureau/build/` (non versionné).

## Construire

```sh
cp ~/Projets/oric-rpi/carte-sd/oric/basic11b.rom bureau/roms/   # vos ROM
cp ~/Projets/oric-rpi/carte-sd/oric/microdis.rom bureau/roms/
sh bureau/construire.sh            # windows et web ; ou : windows, web
```

Résultat : `bureau/build/phosphoric-windows/` (et son `.zip`) et
`bureau/build/phosphoric-web/`. Prérequis : `mingw-w64` et `zip` pour
Windows (SDL2 pour MinGW est téléchargé), emsdk actif pour le web
(`git clone https://github.com/emscripten-core/emsdk && cd emsdk &&
./emsdk install latest && ./emsdk activate latest && . ./emsdk_env.sh`).

## Les patchs (Phosphoric acfeacc, 2.31.15)

| # | Patch | Origine dans le port |
|---|---|---|
| 1 | Clavier `-k fr` : AZERTY par position (scancodes = codes HID), menu F1 et `phosphoric.cfg` (`clavier=fr`) | `kernel.cpp`, `keymap=FR` |
| 2 | Web : `?clavier=fr` | |
| 3 | Windows : ROM par défaut sans `readlink` (le build MinGW ne se liait plus) | correctif |
| 4 | Affichage : `--pixels tv`, `--colors tv`, `--saturation`, `--brightness`, `--color NOM=RRGGBB` ; web `?pixels=tele&couleurs=tele` | `kernel.cpp`, `pixels=`, `couleurs=` |
| 5 | ACIA : `--serial-rx-backpressure`, `--serial-no-cadence` ; web avec LOCI + modem : file 256, contre-pression, sans cadence | points 1 et 2 de `docs/phosphoric-f8.md` |
| 6 | Windows : modem picowifi (Winsock) | indispensable à la console à distance |
| 7 | `--loci-disk 0:/x.dsk` : démarrage direct sur une disquette de LOCI | `locidisk.c`, point 4 |
| 8 | picowifi : `+++` après une seconde de silence (garde en temps) | point 5 |
| 9 | Clavier FR : signes de la v0.1 (ù ; * ' ; . : / ² ! \\) et `--kma FICHIER` | `e758bca`, `f33956b` |
| 10 | Cassette : interceptions seulement si la ROM BASIC est visible | point 6 |
| 11 | Windows : pile de 16 Mo (débordement au démarrage) | correctif |
| 12 | `--disk-autosave` : pistes modifiées réécrites 2 s après la dernière écriture (Microdisc et LOCI, MFM compris), à la sortie et avant un changement de disquette ; web : disquettes gardées par le navigateur, bouton DSK (téléchargement), IDBFS des Emscripten récents | `disquette.c`, point 3 |
| 13 | Horloge : débordement de la dernière instruction de la trame reporté sur la suivante (`emu_clock_frame_begin`) : le faisceau garde exactement 19 968 cycles par trame, un T1 à 50 Hz reste calé (VSYNC.COM de CP/A) | constat du port, correction amont |

Les patchs 3 et 11 corrigent le build Windows de la 2.31.15 lui-même : sans
eux, il ne se lie pas, puis plante au démarrage.

Non repris (propre à la Pi) : son HDMI, cadence multicœur, Wi-Fi de Circle,
NTP, Ctrl+Alt+Suppr/Fin, F12. `mfm_inject_track` de `sedoric.c` est rendue
publique au lieu d'être copiée comme dans `disquette.c`.

## Vérifié

- Tests de Phosphoric touchés : clavier (41, dont 5 nouveaux pour `-k fr` et
  `--kma`), menu F1, picowifi (77, dont `+++` sur ligne calme, qui échoue
  sans le patch 8), ACIA, cassette, LOCI (167), CLI.
- Windows (sous Wine) : LOCI + `--loci-disk 0:/cpa-en.dsk` démarre CP/A ;
  `REMOTE 127.0.0.1:port` donne `CONNECT 9600` par Winsock, `+++` reconnu.
- Web (Chrome) : touches physiques Q, 1, M, W, `;`, `:`, ù, ! donnent
  `A1MZ./;\` ; affichage téléviseur.
- Disquettes (patch 12) : `REN X.SPR=DUN.SPR` sous CP/A, en LOCI (MFM) et en
  Microdisc : piste réécrite pendant l'exécution, la disquette relue montre
  X.SPR. Web : réécrite, gardée après rechargement, téléchargée par DSK.
- Horloge (patch 13) : nouveaux tests de `test-clock`, T1 à 19 968 cycles
  calé à une instruction près sur 300 trames et somme des cycles exacte ;
  tous deux échouent sans le patch. `test-cycle`, `test-raster-split`,
  `test-savestate-determinism`, vidéo, système, Atmos : inchangés.
- Natif : sans le patch 10, `HEX DUN.SPR` sous CP/A (Microdisc) déclenche une
  fausse interception CSAVE ; avec, aucune.

## Proposer à bmarty

Les patchs s'appliquent par `git am` sur `acfeacc` ; un par sujet, avec
tests. Pour une demande de fusion : fork de Phosphoric sur GitHub, branche
depuis `main`, `git am bureau/patches/*.patch`, puis une PR par groupe
(Windows 3+6+11, clavier 1+2+9, affichage 4, ACIA 5+8, LOCI 7, cassette 10,
disquettes 12, horloge 13),
avec `docs/phosphoric-f8.md` comme description des défauts.
