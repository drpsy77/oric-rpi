# Plan : serveur WebDAV de la carte (suite du point 14, 22/11/2026)

But : la carte SD et les clés USB de la Pi vues
comme un dossier sur la RPI 400 (`dav://oric.local:81/` dans PCManFM, par
gvfs ; `davfs2` pour un point de montage), avec ce que la page web et TFTP
ne font pas : **effacer, renommer, déplacer, créer des dossiers**, et
glisser-déposer de nombreux fichiers.

Prérequis fait : lectures et écritures de fichiers par le cœur 2
(`carte.cpp`, `docs/plan-coeur2-carte.md`), sans arrêt de l'Oric.

## Constat

- `CHTTPDaemon` de Circle ne connaît que GET, HEAD et POST
  (`ParseMethod`, « Method Not Implemented » sinon) et garde la requête
  entière en mémoire. WebDAV demande OPTIONS, PROPFIND, PUT, DELETE,
  MKCOL, MOVE, COPY (et LOCK, UNLOCK, PROPPATCH selon les clients) :
  **serveur HTTP propre au port**, directement sur `CSocket` (Listen,
  Accept, Receive, Send ; une tâche du cœur 0 par connexion), comme
  `CHTTPDaemon` le fait (`lib/net/httpdaemon.cpp`, GPL-3.0 : structure
  reprise → `SOURCES.md`).
- RPI 400 : `gvfs-backends` installé (PCManFM, `gio mount dav://...`,
  libsoup) ; `davfs2` et `litmus` (suite d'essais WebDAV) non installés
  (`sudo apt install`, à Pierre de décider).
- FatFs : `FF_CODE_PAGE 850`, `FF_LFN_UNICODE 0` : noms en page 850 ;
  décidé au point 8 : noms ASCII seulement (un accent est illisible et
  impossible à taper sur l'Oric).
- `MAX_TASKS 20` (Circle) : tâches déjà prises par le réseau, NTP, mDNS,
  TFTP, la page web et ses connexions. gvfs ouvre plusieurs connexions à
  la fois : limiter (4 connexions WebDAV, les suivantes attendent dans
  `Listen`).
- **Taille du noyau** (avant l'étape 0) : `_end` à ~39 Ko de `MEM_KERNEL_END`
  (`KERNEL_MAX_SIZE` 4 Mo, `limite.ld`). Le code du serveur (20 à 40 Ko)
  ne tiendra pas. Le plus gros de la BSS est `s_emu` (`emulator_t` de
  Phosphoric, 2,6 Mo, statique dans `oric_machine.c`, code du port).
  **Étape 0** : le réserver par `calloc` dans `oric_machine_init` (pointeur
  `s_pemu`, `#define s_emu (*s_pemu)` pour ne pas toucher au reste du
  fichier) : 2,6 Mo de marge, sans reconstruire Circle. Autre voie, plus
  lourde : `./configure --kernel-max-size 8` (mrproper, ~20 min, pilote
  Wi-Fi à recompiler).

## Décisions (prises par Pierre le 22/11/2026)

1. **Port** : serveur WebDAV séparé, option `dav=81` (`dav=1` = 81), avec
   `tftp=` (même client autorisé) ; la page web reste sur `CHTTPDaemon`.
2. **PUT** : corps reçu en entier en mémoire puis `CarteEcrire`
   (`x.nouveau`, renommage), 64 Mo au plus par fichier. Plus tard,
   peut-être : écriture au fil de la réception.
3. **DELETE d'un dossier non vide : toujours refusé par le serveur (409)**,
   jamais d'effacement récursif sur la Pi. Pierre voulait une validation
   explicite avec alerte : le serveur ne peut pas en afficher, c'est le
   client qui confirme. gvfs refuse lui-même d'envoyer DELETE pour un
   dossier non vide (vérifié au banc : « Répertoire non vide », rien
   envoyé) ; PCManFM demande « supprimer définitivement ? » puis efface
   fichier par fichier. Protégés aussi : disquette montée, racine d'un
   volume, fichiers de démarrage à la racine de la carte
   (`kernel8-32.img`, `config.txt`, `cmdline.txt`, `bootcode.bin`,
   `start.elf`, `fixup.dat`) en DELETE et MOVE (source ou destination) ;
   PUT de `kernel8-32.img` permis.
4. **LOCK** : non géré (`DAV: 1`, LOCK et UNLOCK → 405) ; `davfs2` avec
   `use_locks 0`. Faux verrous seulement si un client l'exige.
5. **Noms** : ASCII imprimable seulement à la création (PUT, MKCOL, MOVE,
   COPY : 400 sinon, noté au journal) ; noms existants en page 850 rendus
   en UTF-8 (`ff_oem2uni` de FatFs, pas de table recopiée) et retrouvés
   depuis l'UTF-8 (`ff_uni2oem`).

## État (22/11/2026)

**Fait** (commits du 22/11/2026), noyau construit :

- Étape 0 : `s_emu` réservé par `calloc` à la première utilisation
  (`oric_emu ()`, `#define s_emu (*oric_emu ())` : les serveurs du cœur 0
  peuvent demander si une disquette est montée avant
  `oric_machine_init`). `_end` passe de ~4 Mo à ~1,4 Mo.
- `carte.cpp` : `CarteListe` (sortie de `web.cpp`), `CarteInfo`,
  `CarteEffacer`, `CarteCreerDossier`, `CarteDeplacer`, `CarteCopier`,
  `CarteEstProtegee`, codes `TCarteCode` (aussi en sortie de
  `CarteEcrire`). `disquette_montee_sous` (`disquette.c`) pour le
  déplacement d'un dossier.
- `dav.cpp` : serveur HTTP/1.1 persistant sur `CSocket`, méthodes du
  tableau ci-dessous ; 4 connexions au plus, la suivante attend et les
  connexions inactives se ferment alors (moins d'une seconde) ; tâches
  comptées avant chaque création (`MAX_TASKS` 20 de Circle : panique
  au-delà, 3 laissées libres).
- Entre la carte et une clé : déplacement d'un fichier = copie puis
  effacement ; d'un dossier : refusé (403). COPY d'un dossier : 403 (gvfs
  copie fichier par fichier). GET d'un dossier : texte d'aide. PROPPATCH :
  207 avec un seul refus (403) sans nommer les propriétés.

**Vérifié au banc natif** (`banc-dav/`, `dav.cpp` et `carte.cpp` compilés
pour Linux, FatFs sur des fichiers POSIX) : `gio mount`, `gio list`
(racine avec `USB:`, `café.txt` en page 850 affiché en UTF-8, dates),
`gio copy` dans les deux sens (3 Mo, `cmp` identique), remplacement,
`mkdir`, `rename` (fichier, dossier), `move`, `remove` ; `curl` pour
chaque refus (disquette montée : 423 ; protégé : 403 ; dossier non
vide : 409 ; accent ou « ? » : 400 ; dossier absent : 409 ;
`Overwrite: F` : 412 ; plus de 64 Mo : 413 avant l'envoi ; « .. » : 400 ;
`USB2:` absente : 404 ; Depth infinity : 403), PUT par morceaux
(`chunked`), `Expect: 100-continue`, HEAD, connexions persistantes, 6
connexions pour 4 places.

**Restent sur la 3B+** : voir « Essais » plus bas (PCManFM, durées,
journal, son pendant une copie, clé retirée). Non fait : `Range`, faux
verrous, écriture au fil de la réception, copie récursive côté serveur,
fichiers de plus de 64 Mo.

Limite connue : FatFs sans verrou des fichiers ouverts (`FF_FS_LOCK 0`) :
un fichier que l'Oric garde ouvert par l'API de fichiers de LOCI n'est
pas protégé (Phosphoric ne garde pas leur chemin), comme pour TFTP et la
page web. Les disquettes montées le sont.

## Conception

### Fichiers

- `dav.cpp/.h` : `COricDAV` (tâche d'écoute + tâches de connexion),
  analyse des requêtes, réponses, XML de PROPFIND.
- `carte.cpp/.h` (commun avec TFTP et la page web) : `LireDossier` et
  `TEntree` sortis de `web.cpp` ; nouvelles opérations, chacune avec
  `CarteReserver` / contrôles entre deux trames / cœur 2 pour le long :
  `CarteEffacer` (fichier, dossier récursif au cœur 2),
  `CarteCreerDossier` (`f_mkdir`, entre deux trames), `CarteDeplacer`
  (`f_rename` sur un même volume ; carte ↔ clé : copie par le cœur 2 puis
  effacement), `CarteCopier` (fichier par le cœur 2 ; dossier récursif à
  décider), `CarteInfo` (`f_stat`). Refus communs : disquette montée,
  clé absente ou changée, fichiers protégés (décision 3).
- `kernel.cpp` : option `dav=`, création comme `COricWeb` ; en-tête des
  options. `sdcard/oric.cfg` (modèle, commentaire), `CLAUDE.md`,
  `README.md`, `SOURCES.md`.

### HTTP

- HTTP/1.1, connexions persistantes (gvfs les garde) ; délai de silence
  30 s comme `web.cpp` (`DELAI_S`).
- En-têtes lus : `Content-Length`, `Transfer-Encoding: chunked` (libsoup
  peut l'utiliser pour PUT), `Expect: 100-continue` (répondre
  `100 Continue` avant le corps), `Depth`, `Destination` (URL complète :
  garder le chemin), `Overwrite`, `Range` (pas au début : 200 et fichier
  entier ; à ajouter si gvfs en abuse).
- Chemins : URL décodée (`%xx`), puis `CarteChemin` (déjà : « .. »,
  « \ », « : » refusés ; `USB:`, `USB2:` en tête). Racine `/` : dossier
  de la carte avec `USB:` et `USB2:` présents, comme la page web.
- Client : `CClientAutorise` (celui de `tftp=`), refus 403 sinon.
- Journal : une ligne par requête qui écrit ou refuse (méthode, chemin,
  code) ; pas les PROPFIND ni les GET (gvfs en fait beaucoup), sauf en
  erreur.

### Méthodes

| Méthode | Réponse | Carte |
|---|---|---|
| OPTIONS | 200, `DAV: 1`, `Allow:` | — |
| PROPFIND Depth 0/1 | 207 multistatus | liste (CarteReserver + entre deux trames) ; Depth infinity : 403 |
| GET, HEAD | 200 | `CarteLire` (cœur 2), 64 Mo au plus |
| PUT | 201 (créé) / 204 (remplacé) ; 423 disquette montée ; 409 dossier absent ; 507 carte pleine | `CarteEcrire` |
| DELETE | 204 ; 423 ; 403 protégé ; 409 dossier non vide | `CarteEffacer` (fichier ou dossier vide) |
| MKCOL | 201 ; 405 existe ; 409 parent absent | `CarteCreerDossier` |
| MOVE, COPY | 201 / 204 ; 412 si `Overwrite: F` et cible présente ; 403 : COPY d'un dossier, MOVE d'un dossier entre volumes | `CarteDeplacer`, `CarteCopier` |
| PROPPATCH | 207, chaque propriété refusée (403) | — |
| LOCK, UNLOCK | 405 au début (décision 4) | — |

PROPFIND : corps de la requête ignoré (toujours le même jeu de
propriétés) : `displayname`, `resourcetype` (`collection`),
`getcontentlength`, `getlastmodified` (RFC 1123 en GMT : date FatFs,
heure locale, moins le fuseau de `CTimer`), `creationdate` (même date,
ISO 8601), `getetag` (taille + date), `getcontenttype`. `href` encodés
(`%xx`), dossiers terminés par « / ».

### Cœur 2 et l'Oric

- Rien de nouveau pour les fichiers (lecture, écriture) ; effacement
  récursif et copie : nouvelles opérations du cœur 2 (une case, un
  travail à la fois, `CarteReserver` autour des étapes entre deux
  trames).
- Glisser 12 fichiers dans PCManFM = 12 PUT, plus des PROPFIND : chaque
  PUT ~30 ms entre deux trames (renommages), comme TFTP ; pas le coût de
  `GetMultipartFormPart` de la page web (~38 ms par Mo).

## Ordre proposé

0. `s_emu` par `calloc` (`[port]` dans `oric_machine.c`) ; vérifier
   `_end`, démarrage, CP/A, F8. Commit à part.
1. Serveur minimal : écoute, connexions (4 au plus), analyse, OPTIONS,
   PROPFIND, GET, HEAD. Essai : `curl -X PROPFIND -H "Depth: 1"`, puis
   `gio mount dav://oric.local:81/`, `gio list`, `gio copy` vers la
   RPI 400, PCManFM.
2. PUT (avec `100-continue` et `chunked`) : `gio copy` vers la Pi,
   glisser-déposer de PCManFM, relecture comparée (`cmp`).
3. DELETE, MKCOL.
4. MOVE, COPY (même volume, puis carte ↔ clé).
5. Selon les clients : faux LOCK, `Range`, PROPPATCH.
6. Documentation (voir Fichiers) ; `carte.sh` peut garder TFTP.

## Essais (RPI 400 par le réseau ; Pierre pour l'Oric et les clés)

- `gio` : `mount`, `list -a standard::size,time::modified`, `copy` dans
  les deux sens, `remove`, `move`, `mkdir` ; PCManFM : ouvrir
  `dav://oric.local:81/oric/usb`, copier, renommer, effacer, nouveau
  dossier.
- `curl` pour chaque code d'erreur (disquette montée, nom avec accent,
  fichier protégé, dossier absent, clé absente).
- Si installé : `davfs2` (`use_locks 0`), `litmus` (basic, copymove,
  props).
- Journal : pas de « trame en retard » de plus de quelques dizaines de ms
  pendant les copies.
- Pierre : son pendant une copie ; CP/A (`DIR` d'`oric/usb` pendant un
  glisser-déposer) ; clé USB retirée pendant une copie ; effacement
  refusé d'une disquette montée.
