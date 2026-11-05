# Wi-Fi et multicœur — étude (05/11/2026)

But : montrer CP/A à distance comme avec le vrai Oric (projet `cpa-oric`,
`docs/console-distance.md` et `docs/loci-modem-wifi.md`), avec la Pi 3B+ à la
place de l'Oric, de LOCI et du Pico W.

## La chaîne de cpa-oric ne change pas

    Oric (CP/A, REMOTE) -> ACIA LOCI $0380 -> modem Pico W (AT, TCP) -> Wi-Fi
        -> démon remote.py (Pi 400, port 2324) -> vitrine, page web
        -> WireGuard -> VPS (Apache, HTTPS, lecture seule)

C'est l'Oric qui appelle (`ATDT-hôte:2324`, nom pris dans `HOSTS`) ; le démon,
la vitrine, WireGuard et le VPS restent sur la Pi 400 et le VPS. La 3B+ n'est
qu'un client du réseau de la maison.

## Ce que Phosphoric fournit déjà

- `src/io/serial_picowifi.c` : le modem PicoWiFiModemUSB (commandes AT,
  vraies connexions TCP), d'après le firmware de sodiumlb.
- `src/cards/card_acia.c`, `src/io/acia6551.c`, `src/io/serial_backend.c` :
  l'ACIA 6551 ; avec LOCI, elle est en `$0380` (`--serial picowifi
  --acia-addr 0380`).
- Essai de compilation pour Circle (5 fichiers, `utils/netutil.c` compris) :
  tout passe, sauf `<ifaddrs.h>` absent de newlib (en-tête de remplacement
  côté port) et `CLOCK_MONOTONIC` (option `-D_POSIX_MONOTONIC_CLOCK`). Édition
  de liens pas encore essayée.

## Ce que circle-stdlib fournit

- Sockets POSIX (`socket`, `connect`, `send`, `recv`, `poll`, `getaddrinfo`,
  `MSG_DONTWAIT`) : `libs/circle-newlib/libgloss/circle/`.
- Wi-Fi de la 3B+ : `CStdlibAppNetwork` en `NetDeviceTypeWLAN`, firmware dans
  `/firmware/` de la carte SD (`libs/circle/addon/wlan/firmware`), réseau dans
  `/wpa_supplicant.conf`. `InitializeNoWaitForActivate` démarre sans attendre
  le réseau.
- Contrainte : la pile réseau et les sockets passent par l'ordonnanceur
  coopératif de Circle, **cœur 0 seulement**.

## Multicœur

`./configure -r 3 -o ARM_ALLOW_MULTI_CORE` (doc/multicore.txt de Circle).

| Cœur | Rôle |
|---|---|
| 0 | clavier USB, son HDMI, réseau, température, carte SD (journal, F12), cadence 50 Hz |
| 1 | émulation (6502, LOCI, Microdisc, fichiers de l'Oric) et copie de l'image |
| 2, 3 | arrêtés |

- Depuis le cœur 1 : FatFs et la carte SD (verrou par attente active, pas
  d'ordonnanceur), `malloc` (allocateur de Circle), timer, écran : permis.
- Échange : le cœur 0 demande une trame quand la file du son en a besoin ; le
  cœur 1 la calcule puis la déclare faite. Entre les deux le cœur 1 attend :
  le cœur 0 écrit alors le son et, s'il le faut, sur la carte SD. Les deux
  cœurs ne sont donc jamais en même temps dans newlib.
- Ne règle pas : une lecture de la carte SD demandée par l'Oric bloque toujours
  l'émulation pendant sa durée.

## Le modem sur le cœur 1

Le modem de Phosphoric appelle les sockets depuis l'émulation (cœur 1), mais
elles ne marchent que sur le cœur 0. Sans modifier Phosphoric : relais dans le
port, par l'option `--wrap` de l'éditeur de liens (`socket`, `connect`,
`send`, `recv`, `poll`, `getaddrinfo`, `close`...). Chaque appel est passé au
cœur 0, qui l'exécute et rend le résultat. Presque tout est non bloquant ;
seule la composition (`ATDT`) arrête l'émulation le temps de la connexion TCP.

Une fois la connexion établie, les données ne sont plus relayées : l'ACIA,
sans cadence (comme celle du vrai LOCI, `modemcadence=1` pour la cadence de
Phosphoric), interroge le modem toutes les 32 cycles. Le cœur 0 lit lui-même
la socket (`recv` non bloquant) dans une file de 8 Ko et envoie ce que le
cœur 1 dépose dans une file de 4 Ko ; `poll`, `read` et `write` du modem ne
consultent que ces files. File de réception pleine, le cœur 0 ne lit plus et
la fenêtre TCP retient l'émetteur.

## Étapes

1. Journal `oric/journal.txt` et émulation sur le cœur 1 (sans réseau) :
   fait, validé sur la 3B+.
2. Modem de Phosphoric et relais (`netrelay.cpp`) : fait. Le relais couvre
   aussi `_close`, qui rend la main à l'ordonnanceur dans circle-newlib.
3. Wi-Fi (`modem=1`), firmware et `wpa_supplicant.conf` sur la carte SD :
   fait.
4. Essai : `REMOTE <IP de la Pi 400>` dans CP/A vers `remote.py serve`
   (port 2324) : à faire.
