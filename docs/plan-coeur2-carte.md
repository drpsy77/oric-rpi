# Plan : accès réseau à la carte par le cœur 2 (accord de Pierre, 22/11/2026)

À faire dans une nouvelle conversation. But : plus de gel de l'Oric quand
on lit ou écrit la carte SD (ou une clé USB de la Pi) par le réseau (TFTP,
page web, puis un futur serveur WebDAV). Première étape avant WebDAV
(`dav://oric.local/` dans PCManFM, `davfs2`), qui ferait passer beaucoup
plus de fichiers.

## Constat

- Aujourd'hui, TFTP et la page web touchent la carte depuis une tâche du
  cœur 0, après `WaitEmulationIdle` (cœur 1 entre deux trames) et **sans
  rendre la main** : le cœur 0 ne demande plus de trame, l'Oric s'arrête
  (image, son, LOCI) le temps de tout l'accès.
- Mesuré (journal du 21/11/2026, envois par la page web de 4 `.dsk` de
  1 Mo, sur la carte) : « Coeur 0 : trame en retard de 1366 ms, 1949,
  1081, 958, 1862, 1551 ms » (« plus long : taches ») : **~0,25 à 0,5 s
  par Mo écrit** (fichier `.nouveau`, renommages, FAT).
- Depuis `8187d6c`, la page web écrit les fichiers d'un envoi un par un
  avec 100 ms d'émulation entre deux (`PAUSE_MS`) : le gel est par
  fichier, plus par envoi. TFTP : un fichier par transfert, même gel.
- Lecture (TFTP `get` : fichier entier en mémoire, 32 Mo au plus, avant le
  premier bloc ; `/f/` : 4 Mo au plus ; listes) : même mécanisme, plus
  court (pas d'écriture de FAT), **pas mesuré** : lire un `.dsk`
  (`sh carte.sh lire oric/usb/cpa.dsk`) puis `sh carte.sh journal | grep
  retard` donne le chiffre, à faire au début.

## Ce que Circle permet

- Interruptions, ordonnanceur et donc pile TCP/IP : **cœur 0 seulement**.
  Le protocole (TFTP, HTTP, WebDAV) reste en tâches du cœur 0 ; il est
  léger (le cœur 0 attend le cœur 1 la plupart du temps).
- Cœurs 2 et 3 libres : `CEmuCores::Run` (`kernel.cpp`) ne fait rien pour
  eux. Le cœur 2 peut faire les accès à la carte.
- **FatFs de Circle déjà protégé entre les cœurs** : `FF_FS_REENTRANT 1`
  (`addon/fatfs/ffconf.h`), un verrou par volume (`ffsystem.cpp`,
  `CGenericLock` = `CSpinLock` sans `NO_BUSY_WAIT`, réel avec
  `ARM_ALLOW_MULTI_CORE`). Chaque appel `f_*` prend le verrou de son
  volume. **Jamais exercé jusqu'ici** : le port n'a aucun accès simultané
  à la carte (tout passe entre deux trames). Premier risque à surveiller.
- Carte : pilote SDHOST (`USE_SDHOST`), attente active, déjà appelé depuis
  le cœur 1. Clés : DWHCI (verrous de canaux, `m_ChannelSpinLock`) et
  `usbmassdevice`, déjà lus par le cœur 1 (LOCI). Deux volumes (carte et
  clé, ou deux clés) peuvent alors être lus en même temps par deux
  cœurs : à vérifier dans DWHCI si deux clés sont utilisées à la fois.

## Conception

### Cœur 2 : exécuteur de travaux

- `CEmuCores::Run (2)` : boucle sur une case de travail (un seul travail à
  la fois suffit : les tâches réseau attendent leur tour), `WFE` quand il
  n'y a rien (pas de boucle active : chaleur, point 11 de CLAUDE.md),
  réveil par `SEV` du cœur 0 après dépôt.
- Travail : opération, chemin FatFs, tampon (réservé par le cœur 0),
  taille, résultat (`FRESULT`, octets), état (`libre` → `demandé` →
  `fini`) avec `DataMemBarrier` (un écrivain, un lecteur, comme les files
  de `netrelay.cpp`).
- Règles du cœur 2 : **FatFs directement (`f_*`), pas de stdio** (les
  structures de stdio de circle-newlib sont partagées sans verrou entre
  cœurs ; le cœur 1 et le journal du cœur 0 s'en servent), **pas de
  malloc**, pas d'ordonnanceur, pas de socket, pas de journal direct
  (résultat rendu au cœur 0, qui écrit dans le journal).
- Accès par **morceaux de 64 Ko** (un `f_read`/`f_write` par morceau, verrou
  rendu entre deux) : le cœur 1 (LOCI) ou le cœur 0 (journal toutes les
  2 s, F12) n'attendent au pire qu'un morceau (~25 ms en écriture). Taille
  à régler d'après les mesures (32 Ko si le journal ou le son en
  souffrent).
- Côté cœur 0 : la tâche réseau dépose le travail puis rend la main
  (`Yield` en boucle, ou `MsSleep (1)`) jusqu'à `fini` : l'ordonnanceur,
  la cadence des trames et le son continuent.

### Lecture (`CarteLire`, `CarteLireDans`)

- Disquette montée : `oric_machine_flush_disks ()` (pistes en attente)
  **reste entre deux trames sur le cœur 0** (lit la mémoire de
  l'émulation) ; court (pistes changées seulement).
- Puis lecture du fichier entier par le cœur 2, par morceaux.
- Point à régler : une disquette montée peut être réécrite par le cœur 1
  (`disquette.c`, 2 s après la dernière écriture de l'Oric) pendant que le
  cœur 2 la lit : copie mélangeant deux états. Proposé : compteur de
  réécritures dans `disquette.c`, relu avant et après ; s'il a changé,
  relire (ou, plus simple, lire une disquette montée en entier entre
  deux trames comme aujourd'hui : 1 Mo, le cas est rare).

### Écriture (`CarteEcrire`)

- Le gros du travail (écrire `x.nouveau` par morceaux) au cœur 2 :
  personne d'autre ne touche ce fichier.
- La fin **entre deux trames sur le cœur 0**, comme aujourd'hui (rapide) :
  disquette montée ? clé toujours là (même série) ? puis
  `x` → `x.ancien`, `x.nouveau` → `x`, effacer `x.ancien`. Le contrôle
  « disquette montée » se fait donc au dernier moment (LOCI a pu la monter
  pendant l'écriture) ; refus : `x.nouveau` effacé.
- `PAUSE_MS` de `web.cpp` devient inutile (à retirer ou réduire).
- TFTP : le dernier bloc est déjà acquitté avant l'écriture ; rien ne
  change pour le client (`carte.sh` vérifie par la liste).

### Listes

- `f_readdir` de 1000 entrées au plus : court, peut rester entre deux
  trames sur le cœur 0 (à mesurer ; sinon cœur 2 aussi).

### Clés USB

- Retrait d'une clé (`ClesUSBMiseAJour`, `UpdatePlugAndPlay`, cœur 0 entre
  deux trames) pendant que le cœur 2 la lit ou l'écrit : le périphérique
  serait libéré sous ses pieds. Le cœur 0 **reporte la prise en compte
  du branchement/retrait** tant qu'un travail du cœur 2 est en cours sur
  une clé (`UpdateKeyboard` attend la fin du travail). Clé arrachée
  physiquement : les transferts échouent proprement (« umsd: CBW transfer
  failed », vérifié pour le cœur 1, point 8), le travail rend une erreur.

### Arrêt et redémarrage

- Ctrl+Alt+Suppr, Ctrl+Alt+Fin, bouton de la page web : attendre la fin du
  travail du cœur 2 avant `oric_machine_shutdown`, `JournalFlush` et le
  démontage de la carte (`Cleanup`, `f_mount`).

## Ordre proposé

1. Mesurer la lecture (ci-dessus), pour avoir un point de départ.
2. Cœur 2 et case de travail, d'abord pour la **lecture** (TFTP `get`,
   `/f/`) : risque moindre (aucune écriture), mesure du gain.
3. **Écriture** de `x.nouveau` par le cœur 2, fin entre deux trames ;
   retirer `PAUSE_MS`.
4. Clés : report du branchement/retrait pendant un travail.
5. Documentation : `CLAUDE.md` (Points techniques : cœur 2, verrou de
   FatFs ; Carte SD : `tftp=`, `web=`), en-têtes de `carte.cpp`, `tftp.cpp`,
   `web.cpp`, `kernel.cpp`. Rien dans Phosphoric, rien pour
   `docs/phosphoric-f8.md`.

## Essais (sur la 3B+, Pierre)

Pas de banc natif possible (Circle) : `make` seulement sur la RPI 400.

- Envoi des 12 disquettes de CP/A par la page web (12 Mo) et par
  `carte.sh envoyer`, l'Oric jouant du son : plus de « trame en retard »
  de plus de quelques dizaines de ms dans le journal.
- `carte.sh lire` d'un gros fichier (20 Mo) pendant la même chose.
- Accès simultanés : sous CP/A, `DIR` et copies sur `oric/usb` (LOCI)
  pendant un envoi dans le même dossier ; vérifier les fichiers des deux
  côtés (tailles, contenu relu identique).
- Envoi d'une disquette montée : toujours refusé.
- Envoi sur une clé, clé retirée pendant l'envoi : erreur propre, pas de
  blocage, clé reconnue à son retour.
- Ctrl+Alt+Suppr pendant un envoi.
- Session longue (vitrine REMOTE plusieurs heures) avec des transferts de
  temps en temps : stabilité, température (le cœur 2 doit dormir quand il
  n'a rien à faire).
