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
  Un contournement côté port est permis s'il est marqué `[port]` et décrit
  (exemple : `docs/phosphoric-f8.md`).
- Licence du dépôt : EUPL-1.2 (`LICENSE`).
- Ne jamais coder sans l'accord de Pierre quand il pose une question.
- Les essais sur la Pi sont faits par Pierre (carte SD déplacée à la main) :
  ne pas répéter « pas encore essayé sur la cible ».

## Dépôt

GitHub : https://github.com/drpsy77/oric-rpi (branche unique `main`).
Pas de Codeberg (refuse le code écrit avec une IA).

## Cible

**Pi 3B+** (Zero 2 W ensuite, même noyau). Pour Circle c'est la même cible
`-r 3` : un seul `kernel8-32.img`, 32 bits. Le Pi Zero d'origine est
abandonné (un cœur, trop lent avec LOCI, gel).

## Environnement (RPI 400, machine de compilation)

```
/home/pi/Projets/circle-stdlib   clone neuf, ./configure -r 3 -o ARM_ALLOW_MULTI_CORE
/home/pi/Projets/phosphoric      clone, commit validé 675cb06
/home/pi/Projets/oric-rpi        ce dépôt
/home/pi/Projets/cpa-oric        CP/A, console à distance (remote.py, vitrine)
```

Construire : `sh importer_phosphoric.sh /home/pi/Projets/phosphoric` puis
`make clean && make` → `kernel8-32.img` (le Makefile vise circle-stdlib par
défaut). Firmware Wi-Fi : `make firmware` dans
`circle-stdlib/libs/circle/addon/wlan/firmware`. `carte-sd/` (ignoré par Git)
est la copie de travail de la carte. La carte SD de la 3B+, branchée sur la
RPI 400 par un lecteur USB, apparaît en `/dev/sda1` (étiquette ORIC) ; la
monter par `udisksctl mount -b /dev/sda1` (→ `/media/pi/ORIC`), la démonter
par `udisksctl unmount -b /dev/sda1` avant que Pierre la retire.

Changer une option de `./configure` : `make mrproper` d'abord (sinon newlib
refuse : « CFLAGS has changed »), puis configure et make (une vingtaine de
minutes).

## Carte SD

Table MBR, première partition FAT32 (nom sans importance). À la racine :
`bootcode.bin`, `start.elf`, `fixup.dat` (`libs/circle/boot`),
`kernel8-32.img`, `config.txt` (`arm_64bit=0`, `kernel=kernel8-32.img`, sans
section `[...]` ; modèle `sdcard/`), `cmdline.txt` (une ligne), `oric/`.
Avec le modem : `firmware/` et `wpa_supplicant.conf` (modèle `sdcard/`).

`cmdline.txt` :
- commun : `width=590 height=448 keymap=FR` (`pixels=tele` par défaut,
  `pixels=carres` avec `width=240 height=224` ; `couleurs=tele|pures`) ;
- sans LOCI : `rom=oric/basic11b.rom`, plus `diskrom=oric/microdis.rom
  disk=oric/xxx.dsk` pour le Microdisc ;
- avec LOCI : `loci=1`, **sans** `rom=` (la ROM système est le menu) ;
  `oric/loci/` = flash « 0: » (`locirom`, `basic11b.rom`, `microdis.rom`),
  `oric/usb/` = clé « 1: » (`.dsk`, `.tap`) ; le menu ne voit rien d'autre ;
- `modem=1` : modem Wi-Fi de LOCI.

Fichiers écrits par le noyau : `oric/journal.txt` (journal de Phosphoric,
session précédente dans `journal-precedent.txt`), `oric/perf.txt` (F12, une
mesure ajoutée par appui), `oric/loci/loci_resume.ost` (F8),
`oric/loci/picowifi.cfg` (AT&W du modem).

## Fichiers du port

| Fichier | Rôle |
|---|---|
| `kernel.cpp/.h` | cœur 0 : clavier USB, son HDMI, Wi-Fi, température, cadence 50 Hz, F12 ; cœur 1 : émulation et affichage (`pixels=`, `couleurs=`) |
| `oric_machine.c/.h` | colle extraite de `main.c` de Phosphoric, API C opaque |
| `journal.cpp/.h` | journal en mémoire (verrou entre cœurs), écrit par le cœur 0 |
| `netrelay.cpp/.h` | sockets et `_close` du cœur 1 exécutés par le cœur 0 (`--wrap`) |
| `phosphoric_stubs.c`, `newlib_compat.c`, `compat/ifaddrs.h` | cartes absentes ; `access`, `popen`, `getifaddrs` |
| `main.cpp` | copie circle-stdlib |
| `Makefile`, `sdcard/`, `README.md`, `SOURCES.md`, `docs/` | |

## Points techniques

- circle-stdlib + GCC : forcer le `stdatomic.h` C de GCC pour les .c
  (fait dans le Makefile, `C_STANDARD`).
- Le C++ ne doit pas inclure `emulator.h` (`_Atomic`) → API opaque.
- Les options sont passées à Phosphoric via un `cli_opts_t` rempli dans
  `oric_machine_init` (comme `main.c`). Options des cartes (`--serial`) :
  par `card_modules_long_options` + `card_modules_set_option`.
- Vitesse du Microdisc : `fdctiming=fast|real` → `cfg.fdc_timing_arg`. Ne pas
  toucher aux constantes `FDC_TIMING_*` de `disk.h`.
- **Fréquence** : `CCPUThrottle` à `CPUSpeedMaximum` (sinon le firmware de la
  3B+ redescend à 600 MHz après ~1 min), `Update()` dans la boucle du cœur 0 ;
  ralentit au-delà de `socmaxtemp` (60 °C par défaut).
- **Multicœur** : `CEmuCores` (`CMultiCoreSupport`) ; le cœur 0 demande une
  trame quand la file du son en a besoin, le cœur 1 la calcule. Le cœur 0
  n'utilise la carte SD (journal, F12) qu'entre deux trames.
- **Ordonnanceur** : `CStdlibAppNetwork` crée un `CScheduler`, cœur 0
  seulement. circle-newlib y rend la main dans les sockets **et dans
  `_close`** : d'où `netrelay.cpp`, qui exécute ces appels du cœur 1 sur le
  cœur 0. Le cœur 0 appelle `mScheduler.Yield()` et `NetRelayService()` à
  chaque tour de boucle. Tout nouvel appel de circle-newlib depuis le cœur 1
  qui rendrait la main (`Yield`) doit être relayé.
- Sockets de circle-newlib : pas de `O_NONBLOCK` (fcntl → ENOSYS) ; le relais
  le mémorise et lit par `recv(MSG_DONTWAIT)`. DNS : pas de noms `.local`.
- Affichage : le firmware agrandit le framebuffer à l'écran en gardant les
  proportions, avec un léger flou (`scaling_kernel=8` dans `config.txt` pour
  des points nets, à essayer). `pixels=tele` : 590x448 (lignes doublées,
  largeur x 2,46).

## Leçons

- Mire multicolore figée + LED normale : noyau chargé mais bloqué avant
  l'écran. Cause rencontrée : circle-stdlib copié par `rsync` (newlib cassé) ;
  un clone neuf règle le problème. Tester un arbre avec
  `circle-stdlib/samples/02-stdio-hello` (les exemples de Circle seul ne
  compilent pas dans circle-stdlib).
- « Aucune règle pour fabriquer kernel.h » : fichier versionné effacé,
  `git restore .`.

## État (05/11/2026)

- Pi 3B+, LOCI, CP/A : image, son, clavier, F8 répétés (puis Échap), stable.
  À 1400 MHz : ~6,7 ms/trame en BASIC, ~6,9 ms avec LOCI (budget 20 ms).
- Défauts de Phosphoric signalés dans `docs/phosphoric-f8.md` (à envoyer à
  bmarty) : F8 sous CP/A (contourné dans `oric_machine_loci_button`) ;
  Entrée du menu (reprise de session) jamais possible.
- Disquettes MFM servies par LOCI : écritures perdues à la fin de la session
  (« no MFM re-encoder », Phosphoric).
- Modem Wi-Fi (`modem=1`) écrit, à essayer : `REMOTE <IP de la Pi 400>`
  dans CP/A vers `remote.py serve` (port 2324). Étude : `docs/wifi.md`.

## Prochaines étapes envisagées

1. Essayer le modem avec la chaîne de cpa-oric (démon, vitrine, VPS).
2. Pour CP/A : « LOCI allégé » (API fichiers seule, servie depuis la SD).
3. Ouverts : pas de menu de choix des supports sans LOCI (on édite
   `cmdline.txt`) ; `scaling_kernel=8` à essayer.
