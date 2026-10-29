# CLAUDE.md — Oric sur Raspberry Pi (Circle + Phosphoric)

Contexte pour Claude Code. Échanges en français.

## Principe (règles du projet)

- Émulateur Oric-1/Atmos **bare-metal** avec Circle (via circle-stdlib).
- On ne code **pas** un nouvel émulateur : le cœur est Phosphoric, recopié
  **sans modification** par `importer_phosphoric.sh` dans `phosphoric/`
  (dossier ignoré par Git, régénéré). On n'ajoute que le strict nécessaire.
- Tout fichier repris est référencé dans `SOURCES.md`.
- Si une modification de Phosphoric devient nécessaire : la proposer et la
  documenter (idéalement en amont, auprès de bmarty), jamais en silence.
- Licence du dépôt : EUPL-1.2 (`LICENSE`).
- Ne jamais coder sans l'accord de Pierre quand il pose une question.

## Dépôt

GitHub : https://github.com/drpsy77/oric-rpi (branche unique `main`).
Pas de Codeberg (refuse le code écrit avec une IA).

## Environnement (RPI 400, machine de compilation)

```
/home/pi/Projets/circle-stdlib   configuré -r 1 (Pi Zero), compilé
/home/pi/Projets/phosphoric      clone, commit validé 675cb06
/home/pi/Projets/oric-rpi        ce dépôt
```

Construire : `sh importer_phosphoric.sh /home/pi/Projets/phosphoric` puis
`make` (le Makefile vise /home/pi/Projets/circle-stdlib). Le `kernel.img`
est ensuite copié à la main sur la carte SD du Pi cible.

Carte SD : firmware (`bootcode.bin`, `start.elf`, `fixup.dat`),
`kernel.img`, `cmdline.txt` (options, une ligne), `config.txt`, `oric/`
(ROM non versionnées : `*.rom` ignorés par Git).

## Fichiers du port

`kernel.cpp/.h` (écran, clavier USB, son HDMI, F5/F8/F12, lecture de
`cmdline.txt`), `oric_machine.c/.h` (colle extraite de `main.c` de
Phosphoric, API C opaque), `phosphoric_stubs.c`, `newlib_compat.c`
(`access()`), `main.cpp` (copie circle-stdlib), `Makefile`, `sdcard/`
(modèle de carte), `README.md`, `SOURCES.md`.

## Points techniques

- circle-stdlib + GCC 13 : forcer le `stdatomic.h` C de GCC pour les .c
  (fait dans le Makefile, `C_STANDARD`).
- Le C++ ne doit pas inclure `emulator.h` (`_Atomic`) → API opaque.
- Les options sont passées à Phosphoric via un `cli_opts_t` rempli dans
  `oric_machine_init` (comme `main.c`). Options LOCI fines à −1 par défaut.
- Vitesse du Microdisc : `fdctiming=fast|real` dans `cmdline.txt` →
  `cfg.fdc_timing_arg` (option `--fdc-timing` de Phosphoric, lue par
  `src/cards/card_microdisc.c`). Défaut `real` = délais mécaniques du vrai
  lecteur. Ne pas toucher aux constantes `FDC_TIMING_*` de `disk.h`
  (elles ne sont comparées que par leur nom).

## État (29/10/2026)

- Pi Zero d'origine, sans LOCI : BASIC 1.1, HIRES, couleurs, son HDMI,
  clavier AZERTY (`keymap=FR`), cassette, Microdisc. Stable.
  13,2 ms/trame en BASIC (budget 20 ms) ; 16 à 21 ms sur un programme
  graphique (LOGO DEMO) : le Zero est à la limite.
- Le Zero tourne déjà à 1 GHz (`force_turbo=1` ne change rien).
- LOCI en **option** dans le noyau (`loci=1`, `lociflash=`, `lociusb=`,
  F8 = bouton Action), réintégré le 29/10 depuis l'ancienne archive.
  Avec LOCI sur le Zero : 22,7 ms/trame (113 %),
  gel après 1 à 3 min. Surcoût : `fdc_ticktock` et `loci_adj_tick` à chaque
  cycle via `io_bus_tick`, tests LOCI dans `memory_read`.
- Menu LOCI : la ROM système doit être `oric/loci/locirom` (donc **pas** de
  `rom=` dans `cmdline.txt`). Espace ouvre/choisit, flèches, Échap démarre.

## Prochaines étapes envisagées

1. Compiler et valider le noyau unique (LOCI en option + `fdctiming=`).
2. Cible suivante : **Pi 3B+** disponible (Zero 2 W en rupture) — second
   circle-stdlib configuré `-r 3`, noyau `kernel8-32.img`, code inchangé.
3. Pour CP/A : « LOCI allégé » (API fichiers seule, servie depuis la SD).
4. Ouverts : image probablement étirée en 16:9 ; pas de menu de choix des
   supports sans LOCI (on édite `cmdline.txt`).
