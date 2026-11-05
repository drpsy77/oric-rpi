# Signalement pour Phosphoric : bouton Action de LOCI (F8), reprise de session, ACIA de LOCI

À envoyer à bmarty (Phosphoric 2.30.0, commit 675cb06). Observé sur le port
Circle (Pi 3B+), analysé dans le code ; **pas encore reproduit** avec
Phosphoric sur PC.

## Symptôme

LOCI en émulation de haut niveau (`--loci -r roms/loci/locirom`), CP/A démarré
depuis le menu LOCI (disquette Microdisc servie par LOCI). Un appui sur F8 ne
montre pas le menu : CP/A redémarre à chaud. Les appuis suivants sont sans
effet (le journal dit « LOCI: Action button inside the menu — ignored »).

## Cause

CP/A s'installe dans la RAM overlay (`$C000-$FFFF`, ROMDIS du Microdisc :
`memory.basic_rom_disabled = true`, `overlay_active = false`).

1. Appui (`loci_action_install_irq_trap`, `src/io/loci_glue.c`) : le vecteur
   IRQ est détourné dans `memory.rom[0x3FFE]`, invisible quand la ROM est
   désactivée ; le piège `$03BA` n'est jamais pris.
2. Relâchement (`loci_action_release_irq_trap`) : `loci_rom_swap_cb` charge
   le menu dans `memory.rom`, encore caché, puis `cpu_reset` lit le vecteur de
   reset dans `upper_ram` : c'est celui de CP/A.
3. `loci_menu_active` passe pourtant à `true` : tout F8 suivant est ignoré
   jusqu'au prochain `MIA_BOOT`.

Sur la vraie cartouche, LOCI reprend le bus et sert sa ROM quel que soit
l'état du Microdisc.

## Correction proposée

Dans `loci_action_release_irq_trap`, avant `loci_rom_swap_cb` vers le menu :

```c
emu->memory.basic_rom_disabled = false;
emu->memory.overlay_active = false;
```

La reprise de session (RETURN dans le menu) restaure ces deux indicateurs par
`savestate_load`, donc CP/A retrouve sa RAM. Le piège IRQ de l'appui a le même
défaut (vecteur écrit dans la ROM cachée), sans conséquence ici puisque le
relâchement fait le reste.

## Contournement dans le port

En attendant, `oric_machine_loci_button` (`oric_machine.c`) remet ces deux
indicateurs à `false` juste avant `loci_action_button_release`, quand le
relâchement va ouvrir le menu.

## Second point : RETURN (reprendre la session) n'est jamais possible

Dans le menu, RETURN appelle `boot(true)` (`src/main.c` de loci-rom, identique
en v0.3.0 et sur la branche principale), qui ne fait rien si
`return_possible` est faux. Ce drapeau vient de `mia_restore_buffer_ok`
(`src/libsrc/restore.s`) : il lit l'octet `RSTR_LAST` ($3F0E) de la XRAM de
LOCI et attend `'L'`, écrit par `mia_save_state`, la routine que la ROM du
menu exécute après le piège du bouton Action. La reprise se fait ensuite par
`mia_restore_state`, depuis cette XRAM.

Phosphoric saute cette routine (le relâchement va droit au menu par le
vecteur de reset) et prévoit sa propre reprise, `MIA_BOOT` avec
`LOCI_BOOT_RESUME` (0x40) et `loci_resume_session_cb` ; mais la ROM 0.3.0 ne
pose jamais ce bit (`mia_set_ax(0x80 | ...)`). D'après le code, l'octet `'L'`
n'est donc jamais présent : l'entrée RETURN reste inactive et l'instantané
`loci_resume.ost` ne sert pas. Au premier démarrage, c'est normal aussi sur la
vraie cartouche (aucune session à reprendre).

## Troisième point : l'ACIA de LOCI perd des octets (console à distance de CP/A)

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
