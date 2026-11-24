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
  (exemple : `docs/phosphoric-f8.md`). À chaque nouvelle version de
  Phosphoric : comparer les fonctions de `main.c` reprises dans
  `oric_machine.c` (`git diff` des deux commits) et revoir les contournements.
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
/home/pi/Projets/phosphoric      clone, commit validé acfeacc (2.31.15)
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
Noyau et disquettes de CP/A : `sh installer_carte.sh` (`--sans-noyau`)
(copie dans `oric/usb/` et `oric/loci/`, dont le `cpa-en.dsk` est
le A: de `locidisk` ; anciennes images de `oric/loci/` dans
`oric/loci/ancien/` ; `oric/azerty.kma` s'il manque ; démonte la carte).
Avant la copie, `sauvegarde_disquettes.py` met les fichiers ajoutés ou
modifiés sur ces disquettes depuis l'installation précédente (référence :
`sauvegarde/installe/`) dans `sauvegarde/SAUVE.DSK` (hors Git, avec
`sauvegarde/ancien/`, les images remplacées), remise ensuite en
`oric/usb/sauve.dsk`.

**Réglages du port dans circle-stdlib** : `sh preparer_circle.sh` (FatFs de
Circle : `FF_FS_EXFAT 1` pour les clés USB en exFAT, `FF_USE_LABEL 1` ;
seule modification de Circle, notée dans le script). À refaire sur un clone
neuf, avant `make` ; si elle est faite après, `make clean && make` (les
structures de FatFs sont dans circle-newlib), pas besoin de `mrproper`.

**Après chaque `make` de circle-stdlib**, recompiler le pilote Wi-Fi avec
les en-têtes de circle-newlib (voir Leçons) :

```
cd /home/pi/Projets/circle-stdlib/libs/circle/addon/wlan
./makeall --nosample clean
EXTRAINCLUDE="-isystem /home/pi/Projets/circle-stdlib/install/arm-none-circle/include" ./makeall --nosample
```

puis `rm -f kernel8-32.img && make` ici (le Makefile ne voit pas le
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
- commun : `width=590 height=448 keymap=FR socmaxtemp=70` (`clavier=oric/azerty.kma` :
  correspondance d'Oricutron, modèle `sdcard/`, remplace la table intégrée ;
  `pixels=tele` par défaut,
  `pixels=carres` ; `couleurs=tele|pures`, `saturation=`, `luminosite=` en %,
  `rouge=RRGGBB`... pour les 8 couleurs, palette notée dans le journal) ;
- sans LOCI : `rom=oric/basic11b.rom`, plus `diskrom=oric/microdis.rom
  disk=oric/usb/xxx.dsk` pour le Microdisc ;
- avec LOCI : `loci=1`, **sans** `rom=` (la ROM système est le menu) ;
  `oric/loci/` = flash « 0: » (`locirom`, `basic11b.rom`, `microdis.rom`),
  `oric/usb/` = clé « 1: » (`.dsk`, `.tap`) ; le menu ne voit rien d'autre ;
- `modem=1` : modem Wi-Fi de LOCI ; ACIA sans cadence (`modemcadence=1`
  pour la cadence de Phosphoric, 9600 bauds). Avec le modem, il est « 1: » et
  la clé devient « 2: » (sans lui, la clé est « 1: ») : « 0: » est le seul
  repère fixe.
- `reseau=ethernet` (défaut `wifi`) : liaison de la Pi pour `modem=1` et
  `tftp=` par la prise de la 3B+ (LAN7515, pilote `lan7800` de Circle), sans
  `firmware/` ni `wpa_supplicant.conf`. **`cmdline.txt` seulement** : le type
  est passé au constructeur de `CStdlibAppNetwork` (`mNet` le fixe), lu par
  `NetworkDeviceType` dans la ligne de commande (`CBcmPropertyTags`), avant
  `mOptions` et la carte ; dans `oric.cfg`, ignoré (noté au journal). Câble
  branché/retiré noté au journal (`IsLinkUp`, tâche PHY de Circle) ; sans
  câble au démarrage, le DHCP réessaie.
- `tftp=cyberlab.local` (nom ou adresse) : serveur TFTP de toute la carte
  (`tftp.cpp`), pour ce client seulement (nom résolu à nouveau quand une
  autre adresse se présente) ; démarre le réseau même sans modem. Fichiers lus
  ou reçus en entier en mémoire (32 Mo au plus), lus et écrits par le
  cœur 2 (`carte.cpp`, l'Oric continue) ; contrôles et renommages quand le
  cœur 1 est entre deux trames (`WaitEmulationIdle`, sans rendre la main) ;
  fichier reçu écrit en `x.nouveau`, l'ancien renommé `x.ancien`
  puis effacé. Disquette montée (Microdisc ou LOCI,
  `oric_machine_disk_mounted`) : lecture permise (pistes en attente
  réécrites avant), remplacement refusé. Nouveau noyau : Ctrl+Alt+Suppr.
  Clés USB de la Pi : `USB:/dSK/x.dsk`, `USB2:/...` (`CarteChemin`, clé
  absente refusée). Client sur la RPI 400 : `sh carte.sh` (voir `web=`), ou
  `tftp -l -m binary oric.local -c get chemin` (`-l` : sinon `USB:` est pris
  pour une machine ; un `put` par fichier ; **tftp -c rend toujours 0**, même
  sur « Error code », et crée le fichier local vide), ou
  `curl --tftp-no-options -T f tftp://oric.local/chemin` (`-o f` pour lire).
  Chaque transfert dans le journal ; le dernier bloc est acquitté avant
  l'écriture (échec vu seulement au journal). Pas de liste par TFTP.
- `web=80` (avec `tftp=`, même client ; `web=1` = 80) : page web de la
  carte (`web.cpp`, `CHTTPDaemon`, une tâche du cœur 0 par connexion) :
  `http://oric.local/?d=oric/usb` liste (disquettes montées signalées),
  `/f/chemin` lecture, `POST /envoi?d=` envoi multipart (20 Mo en tout,
  `MAX_FICHIERS` : CHTTPDaemon garde la requête entière en mémoire, tampon
  à sa taille, et la réponse dans un tampon de 4 Mo réservé par connexion ;
  fichiers écrits un par un par le cœur 2 ; pas de limite de durée,
  le délai de 30 s ne compte que le silence), état de la machine, `POST /redemarrer`
  (Ctrl+Alt+Suppr 1,5 s plus tard). Clés USB présentes en tête de la
  racine (`/?d=USB:/dSK`). `/l/chemin` : liste en texte (`d`/`f`, taille
  exacte, date, `montee`), refus en une ligne `erreur : ...` avec le code
  200 (CHTTPDaemon remplace le contenu de toute autre réponse par sa page
  générique : les pages d'erreur du port sont aussi en 200). Code de la
  carte commun avec TFTP : `carte.cpp` (client autorisé, `CarteChemin`,
  `CarteEcrire` par renommage). `carte.sh` (RPI 400, `ORIC=oric.local`) :
  `ls`, `envoyer dossier fichier...` (un `put` par fichier, vérifié par
  `/l/` : taille et date de l'envoi), `lire`, `journal`.
- `dav=81` (avec `tftp=`, même client ; `dav=1` = 81) : serveur WebDAV
  de la carte (`dav.cpp`, classe 1 sans verrous, HTTP/1.1 persistant sur
  `CSocket`, une tâche du cœur 0 par connexion, 4 au plus ; tâches
  comptées avant chaque création, `MAX_TASKS` 20 : panique au-delà) :
  `dav://oric.local:81/` dans PCManFM, `gio mount`. Copier, effacer,
  renommer, déplacer, créer des dossiers ; fichiers de 64 Mo au plus, en
  mémoire (PUT puis `CarteEcrire`). Dossier non vide jamais effacé (409 :
  le client confirme puis efface fichier par fichier ; gvfs refuse
  lui-même). Protégés : disquette montée (423), racine d'un volume et
  fichiers de démarrage de la carte (403 en DELETE et MOVE ; PUT du noyau
  permis). Noms créés en ASCII seulement (400) ; noms existants en page
  850 rendus en UTF-8 (`ff_oem2uni`). Entre carte et clé : fichier copié
  puis effacé, dossier refusé. LOCK → 405 (`davfs2` : `use_locks 0`).
  Opérations dans `carte.cpp` (`CarteListe`, `CarteInfo`, `CarteEffacer`,
  `CarteCreerDossier`, `CarteDeplacer`, `CarteCopier`). Banc natif :
  `banc-dav/` (`make`, `./banc-dav 8181 RACINE` : `RACINE/sd`,
  `RACINE/usb`, `oric/usb/monte.dsk` vue montée, client 127.0.0.1 ;
  `gio mount dav://127.0.0.1:8181/`). Plan, décisions et essais :
  `docs/plan-webdav.md`.
- `nom=oric` : mDNS du port (`mdns.cpp`, tâche du cœur 0, avec le réseau) :
  la Pi répond à `oric.local` ; les noms en `.local` (`tftp=`, `getaddrinfo`
  du modem dans `netrelay.cpp`, donc ATDT et `HOSTS` de CP/A) sont cherchés
  en multicast (2 s au plus, réponses gardées 60 s). Le démon mDNS de Circle
  n'est pas utilisé : nom figé à « raspberrypi » par circle-stdlib
  (`DEFAULT_HOSTNAME`). Pas de recherche de conflit de nom.
- Heure (avec le réseau : modem ou TFTP) : NTP (`ntp=pool.ntp.org`) dès que le
  réseau a son adresse ; `fuseau=` règle POSIX de `TZ` (défaut
  `CET-1CEST,M3.5.0,M10.5.0/3`). `UpdateClock` (cœur 0, chaque seconde)
  applique le décalage à `CTimer` (il garde l'heure locale : `SetTimeZone`
  puis `SetTime` UTC), dont la FatFs date les fichiers ; « Heure : ... »
  dans le journal à chaque changement.
- `clesusb=apres|remplace|non` (avec `loci=1`) : vraies clés USB de la Pi
  (`cles.cpp`, deux, FAT ou exFAT) servies par LOCI après `oric/usb`, ou
  la 1ʳᵉ à sa place ; branchement et retrait à chaud (voir point 8 des
  prochaines étapes).
- `locidisk=0:/cpa-en.dsk` (avec `loci=1`) : image en A: de LOCI, démarrage
  direct sans le menu ; F8 montre le menu avec A: rempli. Essayé sur le banc
  natif (CP/A démarre, `USBMOUNT` voit A:, `USBMOUNT C: x.DSK` depuis la clé
  marche). `USBMOUNT` refuse les images de 0: (le vrai LOCI s'y est figé) :
  les disquettes en plus vont sur la clé.
- `oric/loci/oric.cfg` (`0:ORIC.CFG` pour l'Oric) : mêmes réglages que
  `cmdline.txt` (sauf `width`, `height`, `lociflash`, `reseau`), qui y sont remplacés ;
  `nom=valeur`, `#` commentaire ; noté dans le journal. Modèle `sdcard/`
  complet (toutes les options en commentaire), en ASCII (lu sous CP/A).
- Souris USB (`mouse1`, avec `loci=1`) : vue par l'API de LOCI (5 octets
  en XRAM, `XREG` 0,0,1), comme `sdl_mouse_event` de Phosphoric ; cumuls
  tenus par le cœur 0, différences passées par le cœur 1 en début de trame
  (`ApplyMouse`). Protocole, piège du `POKE` et programme d'essai :
  `docs/souris-loci.md`.
- Ctrl+Alt+Suppr : disquettes réécrites, fichiers de LOCI fermés
  (`loci_cleanup`), journal, puis redémarrage. Ctrl+Alt+Fin : pareil, carte
  démontée (`Cleanup`), message à l'écran, LED fixe, arrêt : seul moment
  sûr pour débrancher. Pas de touche de fonction (utilisées par les
  émulateurs).

Fichiers écrits par le noyau : `oric/journal.txt` (journal de Phosphoric,
session précédente dans `journal-precedent.txt`), `oric/perf.txt` (F12, une
mesure ajoutée par appui ; cœur 1 : durée des trames ; cœur 0 : retard
des demandes de trame sur l'échéance de 20 ms et plus longue durée de
chaque activité ; en plus, « Coeur 0 : trame en retard de N ms » dans le
journal au-delà de 10 ms, avec l'activité la plus longue),
`oric/loci/picowifi.cfg` (AT&W du modem).

## Fichiers du port

| Fichier | Rôle |
|---|---|
| `kernel.cpp/.h` | cœur 0 : clavier et souris USB, son HDMI, réseau (Wi-Fi ou Ethernet), température, cadence 50 Hz, F12 ; cœur 1 : émulation et affichage (`pixels=`, `couleurs=`) ; cœur 2 : `CarteCoeur2` |
| `oric_machine.c/.h` | colle extraite de `main.c` de Phosphoric, API C opaque |
| `disquette.c/.h` | réécriture des `.dsk` modifiés (pistes changées, 2 s après la dernière écriture) : relevé par le cœur 1 entre deux trames, lecture et écriture du fichier par le cœur 2 |
| `locidisk.c/.h` | `locidisk=0:/x.dsk` : A: de LOCI monté, XRAM du menu, démarrage direct (comme le menu puis Échap) |
| `journal.cpp/.h` | journal en mémoire (verrou entre cœurs), écrit par le cœur 0 |
| `netrelay.cpp/.h` | sockets et `_close` du cœur 1 exécutés par le cœur 0 (`--wrap`) |
| `tftp.cpp/.h` | serveur TFTP de la carte SD (`tftp=`), tâche du cœur 0 |
| `web.cpp/.h` | page web de la carte (`web=`) : liste, lecture, envoi, état, redémarrage |
| `dav.cpp/.h` | serveur WebDAV de la carte (`dav=`) : liste, lecture, envoi, effacement, dossiers, déplacement, copie |
| `banc-dav/` | banc natif de `dav.cpp` et `carte.cpp` (Linux, FatFs sur fichiers POSIX), essais par `gio` et `curl` |
| `cles.cpp/.h` | clés USB de la Pi (umsd1/2 → `USB:`, `USB2:`) servies par LOCI : branchement et retrait à chaud, `statvfs` des volumes |
| `carte.cpp/.h` | accès réseau à la carte, commun à TFTP et web (client, renommage, disquettes montées) ; lectures et écritures par le cœur 2 |
| `mdns.cpp/.h` | mDNS : `nom.local` de la Pi, noms en `.local` des autres machines |
| `phosphoric_stubs.c`, `newlib_compat.c`, `compat/ifaddrs.h` | cartes absentes ; `access`, `popen`, `getifaddrs` |
| `main.cpp` | copie circle-stdlib |
| `preparer_circle.sh` | réglages de FatFs dans circle-stdlib (exFAT, noms de volume) |
| `installer_carte.sh`, `sauvegarde_disquettes.py` | installation sur la carte, sauvegarde des fichiers écrits sur l'Oric |
| `carte.sh` | carte SD et clés USB de la Pi par le réseau, depuis la RPI 400 (liste, envoi vérifié, lecture, journal) |
| `Makefile`, `sdcard/`, `README.md`, `SOURCES.md`, `docs/` | |
| `bureau/` | patchs du port reportés dans Phosphoric (versions Windows et web, `construire.sh`), prêts pour bmarty |

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
  au-delà de `socmaxtemp` (60 °C par défaut), Circle passe **directement à
  600 MHz** (contrôle toutes les 4 s, retour 3 °C plus bas) : trame 25 ms
  (émulation 16, image 9) pour 20 ms de budget, Oric à ~77 % par à-coups
  de 100 ms, son haché. Vu le 16/11/2026 sur la 3B+ (dissipateur sans
  ventilateur, boîtier ouvert, 57-60 °C : les deux cœurs tournent en
  boucle active). Réglé par `socmaxtemp=70` (`cmdline.txt`, Circle
  accepte 40-78) et `temp_soft_limit=70` (`config.txt` : limite douce du
  firmware, 1200 MHz au-delà ; 70 = maximum).
  **Conclusion thermique (16/11/2026)** : les 56-60 °C relevés avant
  n'étaient que le plafond de Circle (la Pi, jamais éteinte, a dû
  osciller entre 600 et 1400 MHz depuis le début). Sans plafond,
  équilibre à **63-64 °C à 1400 MHz** (page web relevée chaque minute
  pendant 25 min, Ethernet, boîtier posé sur une boîte en métal) : 6 °C
  de marge. La carte chauffe par le dessous alors que le dissipateur
  reste presque à l'ambiante : contact puce-dissipateur défaillant
  (pâte ou tampon sec), chaleur évacuée par la carte (le LAN7515 voisin
  chauffe aussi, plus actif en Ethernet). À faire par Pierre : refaire
  le contact (pâte ou tampon thermique) et changer de boîtier (aluminium
  massif servant de radiateur, ou aéré avec ventilateur). Côté logiciel,
  faire dormir les cœurs entre les trames réduirait la chaleur à la
  source (point 11 des prochaines étapes) : n'est plus urgent.
- **Multicœur** : `CEmuCores` (`CMultiCoreSupport`) ; le cœur 0 demande une
  trame toutes les 20 ms du timer, le cœur 1 la calcule. La file du son ne
  fait que corriger la dérive (moyenne sur 1 s, ±200 µs par trame au plus) :
  le son HDMI la vide par blocs de ~43 ms, et cadencer dessus donnait des
  trames par paires puis un trou (saccades). Écran en 50 Hz : `hdmi_group=1`,
  `hdmi_mode=31` dans `config.txt`. Le cœur 0
  n'utilise la carte SD (journal, F12) qu'entre deux trames.
- **Cœur 2** (`carte.cpp`) : lit et écrit les fichiers de TFTP et de la
  page web (case de travail, un travail à la fois ; `WFE` entre deux,
  réveil par `SEV` du cœur 0, dont la tâche rend la main en attendant).
  FatFs seulement (`f_*`, pas de stdio ni de journal), par morceaux de
  64 Ko (`CARTE_MORCEAU`) : verrou de volume de FatFs (`FF_FS_REENTRANT 1`,
  verrou tournant de Circle, sans couper les interruptions) rendu entre
  deux. `malloc` de Circle (noms longs de FatFs) : verrou du tas. Piège :
  `diskio.cpp` de Circle copie un tampon non aligné sur 4 octets dans
  **un tampon statique commun à tous les volumes**, sans verrou ; le cœur 2
  passe donc par son propre tampon aligné (`s_Morceau`) ; ceux de FatFs
  (`FATFS.win`, `FIL.buf`) sont alignés. Disquette montée lue par le
  cœur 2 : compteur de réécritures de `disquette.c` relu après, relue si
  le cœur 1 l'a réécrite pendant ce temps (au 3ᵉ essai, entre deux
  trames) ; avant, pistes en attente seulement réécrites
  (`disquette_flush_attente` : `disquette_flush` chargerait 1 Mo pour
  comparer). Listes et `f_stat` de la page web : `CarteReserver` (fin du
  travail du cœur 2 attendue en rendant la main) puis entre deux trames ;
  sinon chaque appel de FatFs y attend un morceau du cœur 2. `x.ancien`
  effacé par le cœur 2. Branchements USB reportés pendant un travail sur
  une clé (`CarteCoeur2SurCle`) ; arrêt et redémarrage attendent la fin du
  travail (`CarteCoeur2Attendre`).
- **Disquettes par le cœur 2** (`disquette.c`, 24/11/2026) : le cœur 1
  compare l'image au contenu du fichier et recopie les pistes changées
  (court), le cœur 2 les écrit (`f_lseek`/`f_write`) et lit le fichier
  la première fois (`decoder`, d'après `sedoric_load`) ; case à part de
  celle de `carte.cpp`, passée en premier, résultat et journal relevés par
  le cœur 1 à une trame suivante. Avant, sur le cœur 1 : trames de 40 à
  75 ms à chaque réécriture (journal du 22/11, 193 trames longues sur
  245). Pendant le travail, le cœur 1 ne touche à aucune copie.
  `disquette_flush` (arrêt, lecture par le réseau) attend sa fin puis
  travaille sur place. Sections entre deux trames de `carte.cpp`
  (`AttenteTrame`) : sans réécriture en cours (fichier lu, effacé,
  renommé). Banc natif (fil pour le cœur 2, FatFs sur POSIX) : MFM et
  brut, LOCI et Microdisc, écriture de l'Oric pendant le travail,
  `disquette_flush` : fichier identique à `sedoric_save`.
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
- `+++` du modem : `[port]` `esc_send` dans `oric_machine.c` (`send`, `poll`,
  `recv` du backend remplacés). Le picowifi compte son silence en appels de
  `recv`, que l'ACIA ne fait que si un octet est arrivé : `+++` de `REMOTE
  OFF` jamais reconnu. Un « + » après 1 s sans émission (cycles), puis deux
  autres à moins de 1 s, reçoivent 50 appels de `recv` (octets gardés et
  rendus avant les autres). Vérifié sur le banc natif (copie de Phosphoric,
  même code sous `PORT_ESC`) : REMOTE, REMOTE OFF (hangup), REMOTE.
- Sockets de circle-newlib : pas de `O_NONBLOCK` (fcntl → ENOSYS) ; le relais
  le mémorise et lit par `recv(MSG_DONTWAIT)`. DNS de Circle (`CDNSClient`) :
  serveur de la box seulement, qui ne connaît ni les noms `.local` ni ceux
  des machines ; les `.local` passent par `mdns.cpp`.
- Écran : `config.txt` propose des blocs (A mode préféré actif, 60 Hz natif de l'écran de Pierre ; B 720p50,
  C 1080p50, D 576p50, E 720p60). 1080p50 (`hdmi_mode=31`) donnait sur
  l'écran de Pierre une ligne sur deux et un scintillement, et des couleurs
  pâles : en CEA le firmware sort du 16-235, d'où `hdmi_pixel_encoding=2`.
- Journal de Circle (`mLogger`) : à l'écran jusqu'à l'image de l'Oric, puis
  `CPanicScreen` (seules les paniques, texte en gras) ; le reste est lu dans
  la mémoire du logger et recopié dans `oric/journal.txt` (`LogToJournal`,
  toutes les 2 s). Il défilait sur l'image (flashs, Wi-Fi qui cherche un
  réseau absent).
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
- Panique `ipaddress.cpp(62): assertion failed` : comparaison (`==`, `!=`)
  d'un `CIPAddress` jamais rempli (aussi `IsNull`, `CopyTo`). Tester
  `IsSet ()` d'abord. Rencontré dans `tftp.cpp` (client pas encore résolu).
- Mise à jour du noyau par TFTP : un noyau qui plante au premier transfert
  ne peut pas être remplacé ainsi ; carte dans le lecteur de la RPI 400.
- Mire figée après l'ajout d'un tableau statique de 64 Ko (22/11/2026) :
  `_end` (fin de la BSS) dépassait `MEM_KERNEL_END` (0x8000 +
  `KERNEL_MAX_SIZE`, 4 Mo dans le `Config.mk` de Circle) ; la BSS
  recouvrait la pile du cœur 0. Circle ne le vérifie pas : `limite.ld`
  (Makefile) refuse maintenant l'édition de liens. Gros tampons par
  `malloc`, pas en statique (`arm-none-eabi-nm kernel8-32.elf | grep
  " _end$"`). Depuis le 22/11/2026, `s_emu` (2,6 Mo) est dans le tas
  (`oric_emu ()` de `oric_machine.c`) : `_end` à ~1,4 Mo.

## État (16/11/2026)

- Pi 3B+, LOCI, CP/A : image, son, clavier, F8 répétés (puis Échap), stable.
  À 1400 MHz : ~6,7 ms/trame en BASIC, ~6,9 ms avec LOCI (budget 20 ms).
- **Console à distance de cpa-oric : marche** depuis la 3B+ en Wi-Fi
  (`modem=1`) : `REMOTE <nom de HOSTS ou IP>` vers `remote.py serve` de la
  RPI 400 (port 2324), vitrine et VPS (cycleamen.fr/cpa/) compris. Session de
  11 min : 5272 trames, aucune refusée, collages de la vitrine (1024 touches)
  sans perte. Le DNS de Circle ne connaît pas les noms de la box : le nom doit
  être dans `HOSTS` de la disquette (Pierre le tient hors de Git), ou être un
  nom en `.local` (mDNS du port, 16/11/2026).
- Rappel automatique de REMOTE (cpa-oric 359f59c, `sr_rap` corrigé le
  06/11/2026) : la vitrine tourne plusieurs jours de suite sur la 3B+ ; une
  coupure TCP éventuelle est rattrapée par le rappel, sans effet visible.
  Cause des coupures (chaque côté voyait la fermeture venir de l'autre) non
  recherchée plus loin : sans objet. Le message `setsockopt(6, 1) is
  unimplemented` (TCP_NODELAY) est sans effet.
- Corrections qui l'ont permis : pilote Wi-Fi recompilé (`jmp_buf`, voir
  Leçons) ; relais : tampon de 1600 octets par socket (Circle rend un segment
  entier par lecture) ; `[port]` dans `oric_machine_init` : `--serial-buffer
  256` et contre-pression de l'ACIA (`acia_set_rx_backpressure`).
- Phosphoric passé de 2.30.0 (675cb06) à 2.31.15 (acfeacc), 05/11/2026 :
  F8 sous CP/A et Entrée du menu (reprise de session) corrigés en amont
  (2.31.6), vérifiés sur le banc natif (CP/A, `--control`, `loci-button`) ;
  contournement F8 du port retiré ; `loci_resume.ost` n'est plus écrit ;
  imprimante : STROBE sur PB4 (2.31.4) reporté dans `oric_machine.c`.
- Défauts de Phosphoric restant dans `docs/phosphoric-f8.md` (à envoyer à
  bmarty) : ACIA de LOCI qui écrase les octets non lus (contourné), ACIA
  cadencée (contourné), disquettes non réécrites (contourné), pas de
  montage au démarrage (`locidisk`), `+++` non reconnu sur une ligne calme
  (contourné), cassette interceptée sous ROMDIS (contourné), débordement
  de la dernière instruction de chaque trame perdu (point 7, contourné ;
  VSYNC.COM de CP/A), MIA_BOOT qui laisse la RAM overlay en place (CP/A
  redémarre au lieu du BASIC après `USBMOUNT x.TAP /B`) et ignore le
  chargement automatique de la cassette (point 9, contourné par
  `__wrap_loci_rom_swap_cb`, octets du firmware de LOCI, 19/11/2026 ;
  vérifié sur la 3B+ ; chargement une seule fois, `ald_once`, 20/11/2026 :
  un `JMP ($FFFC)` en fin de programme mène au BASIC, pas à un nouveau
  `CLOAD""`).
  Les mêmes corrections, en patchs de Phosphoric, sont dans `bureau/`.
- **Réseau sans retirer la carte, vérifié sur la 3B+ (16/11/2026)** : la Pi
  répond à `oric.local` (10.0.0.23) ; `tftp=cyberlab.local` résolu par mDNS
  (10.0.0.38) ; journal lu par TFTP (`journal.txt` ouvert compris, entier) ;
  noyau envoyé par TFTP (1,25 Mo en ~56 s, vitrine en marche) et relu
  identique. Mise à jour : `tftp -m binary oric.local -c put
  kernel8-32.img`, puis Ctrl+Alt+Suppr.
- Disquettes : Phosphoric ne réécrit un `.dsk` qu'à la sortie (jamais une
  MFM_DISK servie par LOCI) ; sur la Pi, jamais. `[port]` `disquette.c` :
  pistes changées réécrites sur place 2 s après la dernière écriture (banc
  natif : identique à `sedoric_save`) ; 3ᵉ point de `docs/phosphoric-f8.md`.

## Banc d'essai natif (sans la Pi)

Phosphoric compilé pour Linux (`make -j4` dans une **copie** de
`/home/pi/Projets/phosphoric`, binaire `oric1-emu`), même `cpa-en.dsk` (copiée en `cpa.dsk`),
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
- Bouton Action sans fenêtre : `--control` (commandes `continue`, `pause`,
  `keys DIR\n`, `loci-button`, `read BB80 460` pour l'écran texte) ;
  `--loci-menu-at` ne sert qu'avec le vrai firmware (co-simulation).
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
   **Fait (05/11/2026), en service (vitrine)** : `[port]` par `--wrap` de
   `acia_write` (`oric_machine.c`), option `modemcadence=1` ; 2ᵉ point de
   `docs/phosphoric-f8.md`.
2. **Lecture anticipée par le cœur 0**, indispensable avec le point 1 : sans
   cadence, l'ACIA interroge le modem toutes les 32 cycles (`poll` relayé :
   des dizaines de milliers d'allers-retours par seconde). Le cœur 0 lirait
   lui-même les sockets (`recv` non bloquant) dans le tampon de
   `netrelay.cpp` ; le cœur 1 ne consulterait que ce tampon (une file à un
   écrivain et un lecteur), sans relais pour `poll` ni `recv`. **Fait
   (05/11/2026), en service** : files de réception et d'émission dans
   `netrelay.cpp` (voir Points techniques).
3. Envoyer `docs/phosphoric-f8.md` à bmarty (Pierre).
4. Pour CP/A : « LOCI allégé » (API fichiers seule, servie depuis la SD).
5. Ouverts : pas de menu de choix des supports sans LOCI ; `scaling_kernel=8`
   à essayer.
6. **Serveur TFTP** (`tftp=`) et **mDNS** (`oric.local`, noms en `.local`) :
   faits et vérifiés les 15 et 16/11/2026. Reste, côté cpa-oric : un
   script qui récupère par TFTP les `.dsk` de la carte (fichiers ajoutés sur
   l'Oric) avant d'y envoyer une nouvelle version de CP/A (comme
   `sauvegarde_disquettes.py` aujourd'hui avec la carte en main). La
   disquette de démarrage étant montée, la remplacer demande de démarrer sur
   une autre ou sans disquette.
7. Liste de la carte à distance : **page web** (`web=`, 16/11/2026), à
   essayer sur la 3B+ : liste, lecture, envoi, état, redémarrage, client
   de `tftp=` seul. Pas encore : effacement, renommage, dossiers. Un serveur
   FTP (FileZilla) est écarté : trop lourd pour le gain.
8. **Clés USB réelles pour LOCI** (décidé le 16/11/2026) : deux clés au
   plus, branchées et retirées à chaud sans prévenir, comme sur le vrai
   LOCI. Option `clesusb=apres` (défaut : après `oric/usb`) ou `remplace`
   (la 1ʳᵉ clé prend le numéro d'`oric/usb`). Places réservées dans LOCI au
   démarrage (Phosphoric ne retire pas un périphérique), « absente » sans
   clé (erreur sous CP/A). Disquette montée depuis une clé : gardée en
   mémoire, CP/A continue clé retirée ; écritures reportées au retour de la
   même clé (numéro de série du volume), jamais sur une autre. FAT16/32
   et exFAT (`preparer_circle.sh` : la clé du vrai LOCI de Pierre est en
   exFAT, format conseillé aujourd'hui). Étape 1 (journal) vérifiée sur la
   3B+. **Étape 2 faite et vérifiée sur la 3B+ (16/11/2026)** : clé du
   vrai LOCI (exFAT) en « 3: », DUNGEON.DSK montée en B:, clé retirée,
   `ROGUE.TXT` sauvé sous CP/A (« write-back postponed »), clé remise
   (même série) : « 2 track(s) written back » ; seconde clé en « 4: »
   (`USBDIR 4:`). Noms de fichiers : ASCII seulement (FatFs en page 850 :
   un accent arrive à l'Oric en octet > 127, illisible et impossible à
   taper ; en exFAT pas de nom court de secours) ; décidé : rien côté
   émulateur, les fichiers sont nommés sans accent sur la clé. Places
   réservées par
   `oric_machine_init` (racine « USB: », `loci_add_usb_storage`), texte
   réécrit par `oric_machine_loci_key_status` (`[port]`, point 8 de
   `docs/phosphoric-f8.md`) ; `disquette.c` : `associer` relève la clé
   (`cles_usb_etat`), `cle_prete` attend la même clé (nouvel essai chaque
   seconde), fichier relu à son retour ; `UpdateKeyboard`
   (`UpdatePlugAndPlay`) seulement entre deux trames (le retrait libère le
   périphérique que le cœur 1 lirait) ; `__wrap_statvfs` (taille des
   volumes FatFs pour le menu, celui de circle-newlib rend -1). Clé arrachée
   pendant une copie (cœur 1 en plein transfert), vérifié : « umsd: CBW
   transfer failed », erreur rendue, pas de blocage ; disquette de la clé
   réécrite à son retour (2 puis 7 pistes).
9. **Ethernet de la 3B+** (décidé le 16/11/2026) : le Wi-Fi se coupe et se
   reconnecte en ~7 s plusieurs fois par heure (journal du 16/11 : 11:05,
   11:20, 11:25, sans lien avec l'USB), rattrapé par le rappel de REMOTE.
   **Fait et vérifié sur la 3B+ (16/11/2026)** : `reseau=ethernet` dans
   `cmdline.txt` (voir Carte SD) ; « liaison etablie, 1000BASE-T
   Full-duplex », adresse 10.0.0.27 en ~9 s, `oric.local` (mDNS), TFTP,
   NTP, REMOTE vers `cyberlab.local:2324`. Reste à voir : câble retiré puis
   remis pendant REMOTE, démarrage sans câble, tenue sur plusieurs heures
   (plus de coupures comme en Wi-Fi ?).
10. **Souris USB pour LOCI** (16/11/2026), pour l'éditeur d'écran HIRES
   prévu dans CP/A : faite, vérifiée sur le banc natif pour le protocole
   (programme BASIC de `docs/souris-loci.md`, souris simulée), à essayer
   sur la 3B+ avec une vraie souris.
11. **Cadence et chaleur** (avis de Pierre, 16/11/2026) : plutôt que de
   faire dormir le cœur 1 entre les trames (`WFE`), répartir la charge sur
   un cœur de plus (réseau, USB, carte SD hors du cœur de l'émulation) et
   garantir à l'émulateur **seul** une cadence parfaite, avec une pause
   maîtrisée comprise dans les 20 ms. Contrainte de Circle : interruptions
   et ordonnanceur (réseau) sur le cœur 0 seulement. Mesures en place pour
   en juger : F12 (`perf.txt`, cœur 0 et cœur 1) et « Coeur 0 : trame en
   retard » dans le journal (durées émulation/son/image, MHz, °C, état du
   firmware, rapports du clavier).
12. **Clés USB et listes par le réseau** (accord de Pierre, 21/11/2026) :
   **fait (21/11/2026)**, à essayer sur la 3B+ : `USB:`/`USB2:` dans la
   page web et pour TFTP, liste en texte `/l/<chemin>` (pas de liste par
   TFTP), script `carte.sh`. Plan et essais à faire :
   `docs/plan-carte-reseau.md`. **Vérifié sur la 3B+ (21/11/2026)** :
   noyau envoyé par TFTP et relu identique, redémarrage par la page web
   (~10 s) ; `carte.sh ls` (racine avec `USB:/`, `oric/usb`, `USB:/Dsk`,
   `montee` sur `cpa-en.dsk`), `ls USB2:` → « cle USB absente »,
   `carte.sh envoyer . kernel8-32.img` → « écrit ». Reste : refus d'une
   disquette montée par `carte.sh envoyer`, envoi sur une clé, page web
   des clés dans Dillo. Le `printf` de circle-newlib ne connaît pas `%llu`
   (`CPage::Printf`, vsnprintf) ; `JournalPrintf` si.
13. Idées, par intérêt : manette USB → joystick IJK (déjà émulé par Phosphoric,
   `CUSBGamePad` de Circle) ; son sur la prise jack (`CPWMSoundDevice`,
   cadence à revoir) ; imprimante vers un fichier (à vérifier : le port ne
   l'écrit peut-être pas) ; instantanés par Ctrl+Alt+touche (`savestate`
   déjà lié) ; LED d'activité pendant la réécriture des disquettes.
14. **Accès réseau à la carte par le cœur 2** (accord de Pierre,
   22/11/2026) : TFTP et la page web arrêtaient l'Oric pendant
   chaque accès à la carte (`WaitEmulationIdle` sans rendre la main :
   ~0,25 à 0,5 s par Mo écrit). **Fait (22/11/2026), à essayer sur la
   3B+** : le cœur 2 fait les lectures et l'écriture de `x.nouveau` par
   morceaux de 64 Ko (voir Points techniques) ; le protocole reste sur le
   cœur 0 (pile TCP/IP), la fin d'écriture (contrôles, renommages) entre
   deux trames ; `PAUSE_MS` de `web.cpp` retiré. **Essais par le réseau
   faits (22/11/2026)** : lectures sans arrêt, envoi par TFTP ~30 ms par
   fichier (renommages), page web ~38 ms de plus par Mo
   (`GetMultipartFormPart` de Circle, cœur 0). Restent les essais de
   Pierre (son, CP/A, clé retirée, Ctrl+Alt+Suppr pendant un envoi) et
   `oric/loci/montee.dsk` à effacer : `docs/plan-coeur2-carte.md`.
   Ensuite : serveur **WebDAV** (`dav://oric.local:81/` dans PCManFM par
   gvfs, `davfs2` pour un point de montage ; effacer, renommer, dossiers),
   préféré à 9P, NFS et SMB. **Fait (22/11/2026), vérifié au banc natif
   (`banc-dav/` : gio, curl), à essayer sur la 3B+** : `dav=81` (voir
   Carte SD) ; décisions de Pierre et essais restants :
   `docs/plan-webdav.md`. Étape 0 faite : `s_emu` par `calloc`.
15. **Réécriture des disquettes par le cœur 2** (accord de Pierre,
   24/11/2026) : la vitrine arrêtait l'Oric 40 à 75 ms à chaque
   réécriture de `cpa-en.dsk`. **Fait (24/11/2026), banc natif**, à
   essayer sur la 3B+ : journal sans « trame en retard » près des
   « track(s) written back » ; Ctrl+Alt+Suppr juste après une
   écriture sous CP/A (rien de perdu) ; clé retirée puis remise avec une
   disquette montée ; lecture d'une disquette montée par `carte.sh lire`.
   Le journal ne note que les 1000 premiers retards
   (`m_nLateLogged`, `kernel.cpp`) : 13 h de vitrine le 22/11.
