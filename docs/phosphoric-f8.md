# Signalement pour Phosphoric : ACIA de LOCI (pertes, cadence), disquettes jamais réécrites, montage au démarrage

À envoyer à bmarty. Constaté sur le port Circle (Pi 3B+) et sur Phosphoric
natif (Linux) ; les quatre points sont revus sur **Phosphoric 2.31.15**
(commit `acfeacc`) : `acia6551.c`, `card_acia.c`, `loci_bus.c` et
`sedoric.c` n'ont pas changé depuis 2.30.0, où ils ont été observés.

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
