# Signalement pour Phosphoric : ACIA de LOCI (pertes, cadence), disquettes jamais réécrites, montage au démarrage, +++ du modem, cassette sous ROMDIS

À envoyer à bmarty. Constaté sur le port Circle (Pi 3B+) et sur Phosphoric
natif (Linux) ; les cinq points sont revus sur **Phosphoric 2.31.15**
(commit `acfeacc`) : `acia6551.c`, `card_acia.c`, `loci_bus.c` et
`sedoric.c` n'ont pas changé depuis 2.30.0, où ils ont été observés ; le
point 5 est observé sur 2.31.15.

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

Pour démarrer sur une disquette avec LOCI, il faut passer par le menu à
chaque fois (choisir l'image, puis Échap). Une option du genre
`--loci-mount A:0:/cpa.dsk` (montage, configuration du menu en XRAM, puis
`MIA_BOOT` avec `LOCI_BOOT_FDC`) éviterait le menu. Le port le fait dans
`locidisk.c` avec `resolve_path`, `loci_dsk_open` et `loci_rom_swap_cb`.

## 5. Le picowifi ne reconnaît pas « +++ » sur une ligne calme

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
