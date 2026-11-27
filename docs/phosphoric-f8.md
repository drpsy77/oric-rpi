# Signalement pour Phosphoric : ACIA de LOCI (pertes, cadence), disquettes jamais réécrites, montage au démarrage, +++ du modem, cassette sous ROMDIS, démarrage de LOCI (MIA_BOOT)

Points 1 à 6 envoyés à bmarty (issue #98), tous corrigés ; points 7 à 10
pas encore envoyés. Points 1 à 9 constatés sur le port Circle (Pi 3B+) et
sur Phosphoric natif (Linux), revus sur **Phosphoric 2.31.15** (commit
`acfeacc`). Point 10 : nouveau, vu sur **2.31.25** (`0434aa7`).

## Bilan après 2.31.25 (27/11/2026)

| Point | Phosphoric | Port |
|---|---|---|
| 1. ACIA de LOCI : pertes | **corrigé en 2.31.22** : contre-pression d'office sous `--loci`, picowifi, ACIA en $0380 | file de 256 octets et contre-pression du port retirées |
| 2. ACIA de LOCI : cadence | **corrigé en 2.31.24** : sans cadence d'office, `--loci-acia-pacing` la rétablit | `__wrap_acia_write` retiré ; `modemcadence=1` passe `--loci-acia-pacing` |
| 3. Disquettes jamais réécrites | **corrigé en 2.31.17**, MFM_DISK compris, à la fin de chaque commande d'écriture | réécriture du port gardée ; celle de Phosphoric neutralisée sur la Pi : voir point 10 |
| 4. Montage au démarrage | **fait en 2.31.23** : `--loci-mount D:CHEMIN` | `locidisk.c` retiré ; `locidisk=` appelle `loci_mount_drive` et `loci_startup_boot` (copie de `main_loci_mounts`) |
| 5. `+++` sur ligne calme | **corrigé en 2.31.20** (picowifi), 2.31.21 (modem Hayes) : garde en cycles | `esc_send` retiré |
| 6. Cassette sous ROMDIS | **corrigé** (8b61dfb) : garde dans `tape_patches` et le chaînage du BASIC ; proposition 2 (pas d'interception sans cassette) non reprise | condition du port retirée |
| 7. Débordement de la dernière instruction | pas encore envoyé | contournement gardé |
| 8. Retrait d'une clé USB | pas encore envoyé | contournement gardé |
| 9. MIA_BOOT | pas encore envoyé | contournement gardé |
| 10. Réécriture des disquettes sur le fil de l'émulation | nouveau, pas encore envoyé | contournement ajouté |

Vérifié au banc natif sur 2.31.25 (sans aucun réglage du port) : CP/A
démarré par `--loci-mount A:0:/cpa.dsk` ; `REMOTE`, `REMOTE OFF` (« +++
escape », « hangup »), `REMOTE` de nouveau ; 1040 caractères collés dans
`EDIT` sans perte, aucun OVERRUN.

## Déjà corrigé en 2.31.6 (merci)

Deux défauts relevés sur 2.30.0 sont corrigés par « LOCI intégré : RETURN
reprend la session après le bouton Action » :

- F8 sous CP/A (RAM overlay à la place de la ROM, ROMDIS du Microdisc) :
  le menu restait caché et CP/A redémarrait à chaud. Le relâchement mémorise
  maintenant `overlay_active` et `basic_rom_disabled`, les remet à faux et
  la reprise les rétablit.
- RETURN du menu jamais possible (marque `'L'` absente de la XRAM, routine
  `mia_save_state` sautée).

Vérifié en natif avec CP/A (`--control`, `DIR`, `loci-button`, RETURN) :
le menu s'affiche, RETURN rend CP/A avec son écran, qui répond ensuite. Le
contournement du port pour F8 est retiré.

## 1. L'ACIA de LOCI perd des octets (console à distance de CP/A)

**Corrigé en 2.31.22.**


Reproduit avec Phosphoric natif (Linux), sans le port : `--loci --serial
picowifi:x`, CP/A (`cpa.dsk`) puis `REMOTE 127.0.0.1:2325` vers `remote.py
serve` de cpa-oric. La liaison s'ouvre (CONNECT 9600), mais `--serial-trace`
montre « OVERRUN (RDR not read) » sur presque chaque demande du démon (`$FD`
puis une lettre) : le modem pousse un octet toutes les ~1040 cycles, que le
pilote de CP/A (scrutation, sans interruption) ait lu le précédent ou non. Le
démon ne reçoit presque rien et déclare la liaison perdue.

Sur le vrai LOCI, rien ne se perd : il ne prend au modem que ce que l'Oric a
lu (contrôle de flux par l'USB). Avec `--serial-buffer 256`, plus aucun
OVERRUN et la console marche (écran et police lus). Proposition : sous
`--loci` avec le picowifi, une file par défaut, ou ne délivrer l'octet
suivant qu'après la lecture du registre de données.

La file seule ne suffit pas : pleine, elle jette à son tour les octets
(« OVERRUN (FIFO full) »), et un texte collé dans la console (1024 touches
d'un coup) perd des lettres. Il faut aussi la contre-pression
(`acia_set_rx_backpressure`), que `card_acia.c` n'active que pour `--serial
tcp:` : file pleine, l'octet reste dans le modem, qui cesse de lire la
liaison, comme le vrai LOCI. Vérifié en natif : 980 caractères collés, aucune
perte.

Contournement dans le port : `oric_machine_init` passe `--serial-buffer 256`
et active la contre-pression avec le modem.

## 2. L'ACIA de LOCI est cadencée comme une vraie 6551

**Corrigé en 2.31.24.**


Phosphoric cadence l'ACIA sur le débit de son registre de contrôle : à 9600
bauds, ~1040 cycles par octet reçu et ~1180 par octet envoyé. Le vrai LOCI
n'a pas cette limite : son ACIA est dans le firmware et le modem (Pico W) est
sur l'USB ; le firmware modifié de cpa-oric (`tools/pico-modem`) dépasse
largement 9600 bauds. Il n'y a pas d'option : `acia_update_timing` recalcule
`rx_reload` et `tx_reload` à chaque écriture du registre de contrôle ou de
commande, et `--serial-baud` ne vaut que pour l'horloge externe.

Mesuré en natif (CP/A, console à distance, collage de 980 caractères,
contre-pression, `baud = 0` forcé dans `acia_update_timing` pour l'essai) :
écran complet 5,6 s → 1,9 s, police 5,4 s → 2,3 s, écran après collage
3,4 s → 0,6 s, aucune perte. Proposition : sous `--loci` avec le picowifi,
ACIA sans cadence (transfert instantané, comme à débit 0), ou une option.

Contournement dans le port : `acia_write` détourné par l'éditeur de liens
(`--wrap`) ; après chaque écriture, `rx_reload` et `tx_reload` sont remis à 1
(`oric_machine.c`). `modemcadence=1` rend la cadence de Phosphoric.

## 3. Les disquettes de LOCI ne sont jamais réécrites

**Corrigé en 2.31.17** (voir le point 10 pour le port).


`loci_dsk_open` (`src/io/loci_bus.c`) charge le `.dsk` en mémoire ; les
écritures de l'Oric ne vont que dans cette copie. `loci_dsk_flush` ne la
réécrit qu'au démontage ou à l'arrêt de LOCI, et jamais pour une `MFM_DISK`
(« writes are session-only (no MFM re-encoder) »), alors que `sedoric.c` sait
réinjecter les secteurs (`sedoric_save_mfm`, utilisé pour le Microdisc) :
LOCI libère le conteneur MFM gardé par `sedoric_load`. Le Microdisc, lui,
note `disk_dirty` mais ne réécrit qu'à la sortie. Le vrai LOCI écrit sur la
clé au fil de l'eau : on peut retirer la clé sans rien éteindre. Proposition :
garder le conteneur MFM dans LOCI et réécrire les pistes modifiées peu après
l'écriture (ou à chaque secteur), pour LOCI comme pour le Microdisc.

Contournement dans le port (`disquette.c`) : 2 s après la dernière écriture
(`disk_modified` du contrôleur de LOCI, `disk_dirty` du Microdisc), les
pistes qui diffèrent du fichier sont réinjectées (copie de
`mfm_inject_track`) et réécrites sur place.

## 4. Pas de montage d'une disquette de LOCI au démarrage

**Fait en 2.31.23** (`--loci-mount`).


Pour démarrer sur une disquette avec LOCI, il faut passer par le menu à
chaque fois (choisir l'image, puis Échap). Une option du genre
`--loci-mount A:0:/cpa.dsk` (montage, configuration du menu en XRAM, puis
`MIA_BOOT` avec `LOCI_BOOT_FDC`) éviterait le menu. Le port le fait dans
`locidisk.c` avec `resolve_path`, `loci_dsk_open` et `loci_rom_swap_cb`.

## 5. Le picowifi ne reconnaît pas « +++ » sur une ligne calme

**Corrigé en 2.31.20.**


`picowifi_send` (`src/io/serial_picowifi.c`) ne prend un « + » pour
l'échappement que si `pw->silence >= 50`, remis à 0 à chaque octet envoyé.
Or `pw->silence` n'avance que dans `picowifi_recv`, et l'ACIA
(`acia6551.c`, chemin RX) n'appelle `recv` que si `poll` a vu un octet
arrivé : le compteur compte des octets reçus, pas du temps. Sur une ligne
calme, et avec trois « + » envoyés d'affilée (comme le fait tout logiciel
de communication), « +++ » n'est jamais reconnu : le modem reste en ligne,
`ATH0` part vers l'hôte, et la commande suivante (`AT`) ne reçoit pas de
réponse. Le vrai Pico W (comme un modem Hayes) demande une seconde de
silence avant le premier « + », pas entre les « + ».

Reproduit en natif avec CP/A : `REMOTE 127.0.0.1:2325`, puis `REMOTE OFF`
(une seconde de silence, `+++`, une seconde, `ATH0`), puis `REMOTE` de
nouveau : « No answer from the modem », et ni « +++ escape » ni « hangup »
dans le journal. Proposition : mesurer la garde en temps (cycles émulés),
avant le premier « + » et entre les « + » (S12), plutôt qu'en appels de
`recv`.

Contournement dans le port (`oric_machine.c`, `esc_send`) : `send`, `poll`
et `recv` du modem détournés ; un « + » envoyé après une seconde sans
émission (en cycles), puis les deux suivants à moins d'une seconde, sont
précédés de 50 appels à `recv`, dont les octets éventuels sont gardés et
rendus à l'ACIA avant les autres.

## 6. Les interceptions de la cassette visent aussi la RAM overlay

**Corrigé** (8b61dfb, proposition 1).


`tape_patches` (`src/io/tape_patches.c`), appelé avant chaque instruction,
compare seulement le PC aux adresses de la ROM BASIC (`rom_patches.c`). La
garde qui vérifie que la ROM est vraiment servie n'existe qu'en co-simulation
LOCI (`loci_emu_active`). Avec le Microdisc (ou LOCI intégré), sous ROMDIS,
le code de la RAM overlay qui passe à l'une de ces adresses est détourné.

Reproduit en natif avec CP/A (`-m atmos --disk-rom microdis.rom -d cpa.dsk`,
`HEX DUN.SPR`) : la lecture directe de CP/A (BDOS 33, `rnd_rec`) passe en
$E75A (`writeleader_entry` de BASIC 1.1) ; le détournement « CSAVE » (journal :
« CSAVE: built TAP … prog $0000-$0000 ») finit par un `RTI` sur une pile qui
n'est pas la sienne : retour en $07E7 avec le drapeau D mis. HEX affiche des
zéros et des adresses décimales, LOGO s'arrête sur `*BRK*`.

Oricutron, dont vient l'idée, enferme tout son `tape_patches` (`tape.c`)
dans `if (oric->romon)` : il n'intercepte que si la ROM est visible.

Propositions :

1. Ne rien intercepter quand `memory.basic_rom_disabled` est vrai ou
   `memory.rom_enabled` faux (la garde d'Oricutron, celle de la
   co-simulation étendue à tous les cas). Vérifié en natif sur une copie
   corrigée de `main.c` : HEX et LOGO normaux.
2. Mieux, pour un émulateur qui vise l'exactitude au cycle : n'activer les
   interceptions de CSAVE que si une cassette est demandée (`--tape`, ou une
   option explicite), et non par défaut sans cassette. Un programme qui ne
   touche pas à la cassette ne devrait jamais voir son exécution changée
   par un raccourci d'émulateur.

Contournement dans le port (`oric_machine.c`, `oric_machine_run_frame`) :
`tape_patches` appelé seulement sous cette condition.

## 7. Le faisceau perd le débordement de la dernière instruction de chaque trame

`run_frame_instructions` (`src/main.c`) exécute des instructions entières tant
que `raster_cycle < CYCLES_PER_FRAME` : la dernière déborde de 1 à 6 cycles.
`emu_clock_frame_begin` (`src/emu_clock.c`) remet ensuite `raster_cycle` à 0 :
le CPU et le VIA ont compté ces cycles, le faisceau non. Chaque trame dure
donc pour lui 19 968 cycles plus le débordement. Un T1 à 50 Hz (latch 19 966,
période 19 968) dérive de 1 à 3 lignes par seconde par rapport au balayage :
`VSYNC.COM` de CP/A, qui le cale sur l'écran, ne tient pas ; sur Oricutron,
qui reporte le débordement, il tient.

Proposition : dans `emu_clock_frame_begin`, `raster_cycle -= CYCLES_PER_FRAME`
(si le résultat est entre 0 et 63) au lieu de `= 0`, `frame_cycles` de même,
`raster_next_line` à 64, et en ULA au cycle, rejouer les cellules de la
ligne 0 déjà balayées (comme `emu_clock_resume`). Fait et testé dans le patch
13 de `bureau/patches/` (tests de `test-clock`, qui échouent sans lui).

Contournement dans le port : `oric_machine_run_frame` reporte le débordement
après `emu_clock_frame_begin`.

## 8. Pas de retrait d'une clé USB de LOCI

`loci_add_usb_storage` (`src/io/loci_core.c`) ajoute une clé à la liste des
périphériques, mais rien ne l'en retire ni ne change son texte : une clé
retirée de l'hôte reste affichée, et une clé branchée plus tard ne peut
qu'être ajoutée à la suite (4 places en tout, `LOCI_USB_DEV_MAX`). Sur le
vrai LOCI, la liste suit les branchements.

Proposition : une fonction qui change le texte d'une place
(`loci_set_usb_status(loci, n, status)`), et une qui la vide (racine
effacée, chemins « N: » en erreur), sans renuméroter les suivantes.

Contournement dans le port : une place réservée par clé au démarrage
(`oric_machine_init`, racine « USB: »), dont `oric_machine_loci_key_status`
réécrit directement le texte (`usb_dev[n]`) au branchement et au retrait.
Les disquettes montées depuis une clé restent en mémoire (comme toutes les
disquettes de Phosphoric) ; `disquette.c` ne réécrit leurs pistes que sur la
même clé (numéro de série du volume).


## 9. MIA_BOOT : CP/A redémarre au lieu du BASIC, chargement automatique ignoré

Comparé au firmware de LOCI (sodiumlb/loci-firmware `54f6147`,
`src/mia/sys/mia.c`, `mia_task`, et `src/mia/api/api.h`). Quand l'Oric
envoie l'ordre `$A0` (MIA_BOOT), le vrai LOCI :

1. charge la ROM BASIC (1.0 ou 1.1) ;
2. avec une cassette (`LOCI_BOOT_TAP`), modifie la ROM : synchronisation,
   lecture des bits et des octets, écriture ;
3. avec le chargement automatique (`LOCI_BOOT_TAP_ALD`), ajoute deux
   modifications : la préparation de `CLOAD""` dans la place libérée par la
   synchronisation (`$E738` en 1.1, `$E699` en 1.0) et un saut vers elle à
   la place du `JMP $C4A8` qui finit le démarrage à froid (`$ED83` en 1.1,
   `$EB3E` en 1.0) ;
4. rend la ROM BASIC visible (`mia_set_rom_ram_enable(false, true)` ; avec
   le Microdisc, `(true, false)`) et rend la main par `JMP ($FFFC)`
   (`api_return_boot`).

Phosphoric 2.31.15 (`op_mia_boot`, `src/io/loci_boot.c`, et
`loci_rom_swap_cb`, `src/io/loci_glue.c`) charge la ROM et remet le 6502 à
zéro (`cpu_reset`), mais :

- **ne rend pas la ROM visible** : `memory.basic_rom_disabled` et
  `overlay_active` (ROMDIS, `$0314` de LOCI ou du Microdisc) restent tels
  quels. Depuis le menu, rien n'est en overlay et le démarrage marche ; depuis
  un programme en RAM overlay (CP/A, `USBMOUNT JEU.TAP /B`, drapeaux `$96`),
  `cpu_reset` lit le vecteur `$FFFC` dans cette RAM : CP/A redémarre à chaud
  au lieu du BASIC ;
- **ignore `LOCI_BOOT_TAP_ALD`** (défini dans `loci.h`, jamais lu) ; le
  commentaire de `loci_tape_mount_cb` prévoit que l'utilisateur tape
  `CLOAD""`, même quand le chargement automatique est coché dans le menu.

L'étape 2 n'est pas nécessaire dans Phosphoric : `tape_patches` intercepte
ces routines (et prend `GetSync` à son entrée, avant le saut de la
synchronisation).

Reproduit en natif (CP/A sur le Microdisc avec `--loci`, ou servi par LOCI
avec le patch 7 : `--loci-disk 0:/cpa.dsk`), `USBMOUNT 1:/SNAKE.TAP /B` :
sans correction, l'invite `A>` de CP/A revient ; avec, le BASIC 1.1 démarre,
charge la cassette tout seul et SNAKE (code machine, lancement
automatique) se lance.

Proposition (faite et testée dans le patch 14 de `bureau/patches/`) :

- `loci_rom_swap_cb` : ROM BASIC visible avant le `cpu_reset` du `$C000`
  (drapeaux de la mémoire, `dsk_ctrl` de LOCI bits 7 et 1 à 1, `romdis` et
  `diskrom` du Microdisc s'il est présent), ROMDIS et ROM du Microdisc pour
  le `$A000` ;
- `op_mia_boot` : après le `$C000`, avec `LOCI_BOOT_TAP`,
  `LOCI_BOOT_TAP_ALD`, une cassette montée et la ROM BASIC de LOCI (pas une
  ROM montée), les trois modifications du firmware (synchronisation,
  préparation, saut), octets recopiés de `mia.c`, par `rom_poke_cb`. Trois
  tests dans `test_loci.c` (1.1, 1.0, cas sans effet).

Contournement dans le port : `__wrap_loci_rom_swap_cb` (`oric_machine.c`,
`--wrap` dans le `Makefile`) fait les deux à la fois, après la fonction
d'origine : ROM rendue visible, modifications si une cassette est chargée,
nouveau `cpu_reset`.

Écart volontaire du port, à ne pas reprendre tel quel : les modifications
restent dans la ROM, sur le vrai LOCI comme ici. Un démarrage à froid
ultérieur (`JMP ($FFFC)`, fin d'un programme GRXASM `/O` de CP/A, comme
`DEMO.TAP` de la vitrine) relance `CLOAD""` sur une cassette arrivée au
bout : le firmware rend alors des zéros sans revenir au début (`tap.c`,
`tap_read_byte`), et l'Oric attend sans fin (« Searching .. »). Le port ne
charge qu'une fois : dès que le PC atteint la préparation de `CLOAD""`
(`$E738`, `$E699` en 1.0), ROM BASIC visible et saut encore modifié, le
`JMP $C4A8` d'origine est remis ; le démarrage à froid suivant mène au
BASIC (« Ready »), comme sur un Atmos avec un magnétophone. Vérifié en natif
(même logique ajoutée pour l'essai) avec une cassette dont le programme
finit par `JMP ($FFFC)`. Pour revenir à CP/A : le menu de LOCI.


## 10. Réécriture des disquettes de LOCI sur le fil de l'émulation

2.31.17 réécrit le `.dsk` à la fin de chaque commande d'écriture
(`loci_dsk_sync`, appelé par `loci_dsk_tick` et `loci_dsk_write`) :
`fopen`, secteurs changés, `fclose`, sur le fil de l'émulation. Sur un PC,
le cache du système le rend invisible. Sur le port (Pi sans système), chaque
ouverture et fermeture passe par FatFs et la carte SD pendant la trame :
quelques millisecondes à plusieurs dizaines par commande, et une copie sous
CP/A en fait des centaines. L'Oric et le son s'arrêtent : c'est ce que le
port avait déjà supprimé en confiant ses réécritures à un autre cœur. Il
garde aussi les écritures d'une disquette dont la clé USB est retirée,
jusqu'au retour de la même clé (point 8).

Proposition, sans changer le comportement par défaut : laisser l'hôte faire
la réécriture lui-même. Par exemple, un rappel
`loci_set_dsk_writeback_callback(loci, cb, ctx)` appelé à la place de
`loci_dsk_flush` à la fin d'une commande d'écriture, ou une option qui
désactive la réécriture (`disk_modified` restant alors vrai pour l'hôte,
comme avant 2.31.17).

Contournement dans le port (`disquette.c`, « Réécriture de LOCI ») : trois
`--wrap`. `loci_dsk_open` libère `dsk_shadow` après le montage :
`loci_dsk_flush` ne fait plus rien, comme quand `malloc` échoue.
`loci_dsk_tick` ne fait avancer que le contrôleur, sans `loci_dsk_sync`.
`loci_dsk_write` note une commande d'écriture vue avant ou après l'appel,
car le `loci_dsk_sync` final remet `disk_modified` à faux. Vérifié en
natif (`COPY` sous CP/A) : Phosphoric seul réécrit le `.dsk`, avec les trois
détournements il reste intact et les écritures sont notées.
