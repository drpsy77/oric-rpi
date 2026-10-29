# Provenance des sources

Ce port d'émulateur Oric pour Raspberry Pi Zero (bare-metal, Circle) **n'est pas
un nouvel émulateur** : il réutilise le cœur de Phosphoric sans le modifier et
n'ajoute que le strict nécessaire pour le faire tourner sous Circle.

## Composants repris

| Composant | Origine | Licence | Dossier ici | Modifié ? |
|---|---|---|---|---|
| Phosphoric — cœur de l'émulateur Oric-1/Atmos | <https://github.com/benedictemarty/Phosphoric>, commit dans `phosphoric/COMMIT` (validé sur `675cb06`, 2026-10-05, v2.30.0) — © 2026 bmarty | EUPL-1.2 (`phosphoric/LICENSE`) | `phosphoric/` | **Non** (copie à l'identique par `importer_phosphoric.sh`) |
| Circle — environnement bare-metal C++ pour Raspberry Pi | <https://github.com/rsta2/circle> — © R. Stange | GPL-3.0 | dépendance externe | Non |
| circle-stdlib — newlib (libc) au-dessus de Circle, `fopen` sur la carte SD | <https://github.com/smuehlst/circle-stdlib> — © S. Mühlstrasser | GPL-3.0 | dépendance externe | Non |
| ROM du menu LOCI v0.3.0 (`locirom`) | <https://github.com/sodiumlb/loci-rom>, livrée par Phosphoric dans `roms/loci/` (notice copiée avec) — © 2024 Sodiumlightbaby | BSD-3-Clause | `phosphoric/roms/loci/` | Non |
| Firmware Raspberry Pi (`bootcode.bin`, `start.elf`, `fixup.dat`) | téléchargé par `make` dans `libs/circle/boot` de circle-stdlib | Broadcom (`LICENCE.broadcom`) | carte SD seulement | Non |

Phosphoric lui-même crédite Oricutron pour le décodage PSG/VIA et le scan
clavier (repris dans `oric_machine.c` avec les commentaires d'origine).

## Fichiers de Phosphoric repris (41 sources)

| Domaine | Fichiers `src/` |
|---|---|
| CPU 6502 | `cpu/cpu6502.c`, `cpu/opcodes.c`, `cpu/addressing.c`, `cpu/microseq.c` |
| Mémoire | `memory/memory.c`, `memory/banking.c` |
| Horloge maître / ULA | `emu_clock.c`, `io/ula_ng.c`, `video/video.c`, `video/textmode.c`, `video/hires.c` |
| VIA 6522, clavier | `io/via6522.c`, `io/keyboard.c` |
| Son AY-3-8912 | `audio/ay3891x.c` |
| Cassette | `io/cassette.c`, `io/tape_patches.c`, `storage/tap.c`, `rom_patches.c` |
| Disquettes | `io/microdisc.c`, `io/jasmin.c`, `storage/disk.c`, `storage/sedoric.c` |
| Joystick IJK, imprimante, traceur MCP-40 | `io/joystick.c`, `io/printer.c`, `io/mcp40.c` |
| Bus d'E/S et cartes | `io/io_bus.c`, `cards/card_modules.c`, `cards/card_microdisc.c`, `cards/card_jasmin.c`, `cards/card_ula_ng.c` |
| LOCI, émulation de haut niveau (API MIA, fichiers, menu, Microdisc et cassette de LOCI) | `cards/card_loci.c`, `io/loci_core.c`, `io/loci_fs.c`, `io/loci_bus.c`, `io/loci_boot.c`, `io/loci_glue.c`, `io/loci_gfx.c`, `io/loci_sdimg.c` |
| Co-simulation LOCI absente : stub fourni par Phosphoric | `io/loci_emu_stub.c` |
| Sauvegardes d'état (« reprendre la session » du bouton Action de LOCI) | `savestate.c` |
| Journal | `utils/logging.c` |

Plus les 52 en-têtes qu'ils utilisent (liste exacte dans
`importer_phosphoric.sh`). Certains en-têtes de composants exclus (ACIA,
réseau…) sont présents uniquement parce que `emulator.h` déclare leurs types ;
**aucun code** de ces composants n'est compilé.

## Composants de Phosphoric volontairement exclus

ACIA 6551/6850 et modem série (dont le modem Wi-Fi picowifi de LOCI), DTL 2000,
Mageco MIDI, synthèses vocales SP0256 et MEA8000, co-simulation du firmware
LOCI, LOCI-USB réelle et sélecteur de backends LOCI (`loci_backend.c`,
`loci_emu.c`, `loci_neo.c`, `loci_hw*.c`), disquettes HTTP, interface SDL
(rendu, son, OSD, menu F1), débogueur, gdbstub, API de contrôle, Cast/HTTP,
captures AVI/images, traces/profilage, CLI, hostfs, fast-load et auto-frappe.

## Fichiers ajoutés par le port

| Fichier | Contenu | Origine |
|---|---|---|
| `phosphoric_stubs.c` | Fonctions vides pour les cartes exclues et le disque HTTP, jamais appelées car ces cartes sont absentes | Signatures recopiées des en-têtes de Phosphoric |
| `oric_machine.c` | Callbacks bus/IRQ/PSG/clavier, `emulator_init`, boucle de trame, chargement ROM/TAP/disquettes, étape LOCI, bouton Action, rapport clavier HID | **Extrait de `src/main.c` de Phosphoric** (`main_setup_*`, `sdl_function_key` F8 et `SDL_KEYUP`) et de `src/io/loci_glue.c` (`loci_sync_kbd_from_sdl`), uniquement par suppression de branches (marquées `[port]`) ; plus de petites fonctions d'accès en fin de fichier, écrites pour le port |
| `oric_machine.h` | API C opaque entre la machine et le kernel (le C++ ne voit aucun en-tête de Phosphoric, qui utilise `_Atomic`) | Écrit pour le port |
| `newlib_compat.c` | `access()`, absente de circle-newlib, utilisée par `card_microdisc.c` | Écrit pour le port |
| `main.cpp` | Point d'entrée | **Copie à l'identique** de circle-stdlib `samples/03-stdio-fatfs/main.cpp` |
| `kernel.h` | Classe du kernel | Structure reprise de circle-stdlib `samples/03-stdio-fatfs/kernel.h` |
| `kernel.cpp` | Boucle 50 Hz, affichage, clavier USB, son HDMI | Motifs repris de circle-stdlib `samples/03-stdio-fatfs` (application, SD, stdio), Circle `sample/08-usbkeyboard` (clavier brut, branchement à chaud), Circle `sample/34-sounddevices` (son HDMI en file d'attente) ; table clavier = `qwerty_tab` de Phosphoric `src/io/keyboard.c` (d'après Oricutron), transposée en codes HID USB ; touches F5 / Maj+F5 / F8 comme Phosphoric `main.c` ; rapport HID brut transmis à LOCI comme `loci_sync_kbd_from_sdl` |
| `Makefile` | Construction de `kernel.img` | Repris de circle-stdlib `samples/03-stdio-fatfs/Makefile` |
| `sdcard/cmdline.txt`, `sdcard/config.txt` | Options de démarrage | Écrits pour le port (options Circle documentées dans `doc/cmdline.txt` de Circle) |
| `importer_phosphoric.sh` | Copie des fichiers de Phosphoric depuis un clone | Écrit pour le port |

Versions validées : circle-stdlib `4fbab62` (2026-09-25), Circle `7a81e1b`
(Step51.1.1), avec `arm-none-eabi-gcc` 13.2.1.

## Licences

Phosphoric est sous EUPL-1.2, Circle et circle-stdlib sous GPL-3.0. L'EUPL-1.2
liste la GPL-3.0 parmi ses licences compatibles (annexe), ce qui permet de
distribuer l'ensemble combiné sous GPL-3.0. À confirmer avant toute
publication si tu as un doute — ce n'est pas un avis juridique.
