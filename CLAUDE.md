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

**Après chaque `make` de circle-stdlib**, recompiler le pilote Wi-Fi avec
les en-têtes de circle-newlib (voir Leçons) :

```
cd /home/pi/Projets/circle-stdlib/libs/circle/addon/wlan
./makeall --nosample clean
EXTRAINCLUDE="-isystem /home/pi/Projets/circle-stdlib/install/arm-none-circle/include" ./makeall --nosample
```

puis `rm -f kernel8-32.elf && make` ici (le Makefile ne voit pas le
changement de bibliothèque).

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
- `modem=1` : modem Wi-Fi de LOCI ; ACIA sans cadence (`modemcadence=1`
  pour la cadence de Phosphoric, 9600 bauds).

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
  trame toutes les 20 ms du timer, le cœur 1 la calcule. La file du son ne
  fait que corriger la dérive (moyenne sur 1 s, ±200 µs par trame au plus) :
  le son HDMI la vide par blocs de ~43 ms, et cadencer dessus donnait des
  trames par paires puis un trou (saccades). Écran en 50 Hz : `hdmi_group=1`,
  `hdmi_mode=31` dans `config.txt`. Le cœur 0
  n'utilise la carte SD (journal, F12) qu'entre deux trames.
- **Ordonnanceur** : `CStdlibAppNetwork` crée un `CScheduler`, cœur 0
  seulement. circle-newlib y rend la main dans les sockets **et dans
  `_close`** : d'où `netrelay.cpp`, qui exécute ces appels du cœur 1 sur le
  cœur 0. Le cœur 0 appelle `mScheduler.Yield()` et `NetRelayService()` à
  chaque tour de boucle. Tout nouvel appel de circle-newlib depuis le cœur 1
  qui rendrait la main (`Yield`) doit être relayé.
- Connexion TCP établie (`connect`, `accept`) : plus de relais pour les
  données. Le cœur 0 (`NetRelayService`) lit la socket dans une file de 8 Ko
  (seulement s'il y reste `FRAME_BUFFER_SIZE` : Circle rend un segment entier)
  et envoie la file de 4 Ko remplie par le cœur 1 ; `poll`/`read`/`write` du
  cœur 1 ne voient que les files (un écrivain, un lecteur, `DataMemBarrier`).
  `close` : le cœur 0 envoie le reste puis ferme.
- ACIA sans cadence : `[port]` dans `oric_machine.c`, `acia_write` détourné
  (`--wrap`) ; `rx_reload`/`tx_reload` remis à 1 après chaque écriture.
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
- `core0: Data abort` au démarrage avec `modem=1`, PC dans `setjmp`, LR dans
  `etherbcmattach` (`arm-none-eabi-addr2line -f -e kernel8-32.elf <PC> <LR>`) :
  avec GCC, circle-stdlib compile les bibliothèques de Circle avec les
  en-têtes newlib du système (`/usr/include/newlib`, paquet Debian), où
  `jmp_buf` fait 23 × 4 octets ; le `setjmp` lié est celui de circle-newlib,
  qui en écrit 104 (`jmp_buf` de 20 × 8). La pile d'erreurs du pilote Wi-Fi
  (`p9error.h`) déborde sur son compteur. Correction : recompiler
  `addon/wlan` avec `EXTRAINCLUDE` (ci-dessus). Les autres bibliothèques de
  Circle liées n'utilisent pas `setjmp`.

## État (05/11/2026, fin d'après-midi)

- Pi 3B+, LOCI, CP/A : image, son, clavier, F8 répétés (puis Échap), stable.
  À 1400 MHz : ~6,7 ms/trame en BASIC, ~6,9 ms avec LOCI (budget 20 ms).
- **Console à distance de cpa-oric : marche** depuis la 3B+ en Wi-Fi
  (`modem=1`) : `REMOTE <nom de HOSTS ou IP>` vers `remote.py serve` de la
  RPI 400 (port 2324), vitrine et VPS (cycleamen.fr/cpa/) compris. Session de
  11 min : 5272 trames, aucune refusée, collages de la vitrine (1024 touches)
  sans perte. Le DNS de Circle ne connaît pas les noms de la box : le nom doit
  être dans `HOSTS` de la disquette (Pierre le tient hors de Git).
- Corrections qui l'ont permis : pilote Wi-Fi recompilé (`jmp_buf`, voir
  Leçons) ; relais : tampon de 1600 octets par socket (Circle rend un segment
  entier par lecture) ; `[port]` dans `oric_machine_init` : `--serial-buffer
  256` et contre-pression de l'ACIA (`acia_set_rx_backpressure`).
- Défauts de Phosphoric dans `docs/phosphoric-f8.md` (à envoyer à bmarty) :
  F8 sous CP/A (contourné) ; Entrée du menu (reprise de session) jamais
  possible ; ACIA de LOCI qui écrase les octets non lus (contourné).
- Disquettes MFM servies par LOCI : écritures perdues à la fin de la session
  (« no MFM re-encoder », Phosphoric).

## Banc d'essai natif (sans la Pi)

Phosphoric compilé pour Linux (`make -j4` dans une **copie** de
`/home/pi/Projets/phosphoric`, binaire `oric1-emu`), même `cpa.dsk`,
`basic11b.rom`, `microdis.rom` (copiés depuis la carte, `oric/loci/`) :

```
python3 tools/remote.py serve --port 2325 --http 8041 --log essai.log &   # dans cpa-oric
HOME=. oric1-emu -n --realtime --no-config -m atmos -r basic11b.rom \
  --disk-rom microdis.rom --disk cpa.dsk --loci --loci-flash flash --loci-usb usb \
  --serial picowifi:x --serial-buffer 256 --serial-trace trace.txt \
  --type-keys "15000000:R\p1E\p1M...\p1\n" -c 110000000 \
  --screenshot-text-at 100000000:ecran.txt
python3 tools/remote.py --demon http://localhost:8041 coller texte.txt    # pendant l'appel
```

- Taper `REMOTE 127.0.0.1:2325` lettre par lettre (`\p1` entre chaque) :
  sinon CP/A perd des touches au démarrage.
- **Avec LOCI** (`--loci`) : sans lui, CP/A prend l'ACIA d'Oricutron ($031C)
  et compose sans « - » : mode telnet, non représentatif.
- `trace.txt` : chaque octet, et « OVERRUN » si un octet est écrasé.
- Le démon de test sur 2325/8041 ne gêne pas celui de Pierre (2324/8040).
  Ne pas lancer `remote.py serve --help` : il démarre un démon.
- La contre-pression et le transfert sans cadence n'ont pas d'option :
  essais faits en ajoutant dans la copie de Phosphoric un test de variable
  d'environnement (`card_acia.c` après `acia_set_backend` ; `acia6551.c`
  `acia_update_timing`, `baud = 0`). Ne jamais modifier le clone.

## Prochaines étapes

**But : les mêmes performances qu'avec le vrai Oric** (vrai LOCI + Pico W au
firmware modifié, `cpa-oric/tools/pico-modem`, qui dépasse 9600 bauds) ; ce
que l'émulateur fait moins bien est à remonter à bmarty.

1. **ACIA de LOCI sans cadence.** Phosphoric cadence l'ACIA sur les 9600 bauds
   du registre de contrôle (~1040 cycles par octet reçu, ~1180 envoyé) ; le
   vrai LOCI (ACIA du firmware, USB) n'en a pas. Mesuré sur le banc natif
   (collage de 980 caractères, contre-pression) : écran complet 5,6 s → 1,9 s,
   police 5,4 s → 2,3 s, écran après collage 3,4 s → 0,6 s, aucune perte.
   **Fait (05/11/2026), à essayer sur la 3B+** : `[port]` par `--wrap` de
   `acia_write` (`oric_machine.c`), option `modemcadence=1` ; 4ᵉ point de
   `docs/phosphoric-f8.md`.
2. **Lecture anticipée par le cœur 0**, indispensable avec le point 1 : sans
   cadence, l'ACIA interroge le modem toutes les 32 cycles (`poll` relayé :
   des dizaines de milliers d'allers-retours par seconde). Le cœur 0 lirait
   lui-même les sockets (`recv` non bloquant) dans le tampon de
   `netrelay.cpp` ; le cœur 1 ne consulterait que ce tampon (une file à un
   écrivain et un lecteur), sans relais pour `poll` ni `recv`. **Fait
   (05/11/2026), à essayer** : files de réception et d'émission dans
   `netrelay.cpp` (voir Points techniques).
3. Envoyer `docs/phosphoric-f8.md` à bmarty (Pierre).
4. Pour CP/A : « LOCI allégé » (API fichiers seule, servie depuis la SD).
5. Ouverts : pas de menu de choix des supports sans LOCI ; `scaling_kernel=8`
   à essayer.
