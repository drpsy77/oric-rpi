# Plan : serveur WebDAV de la carte (suite du point 14, 22/11/2026)

À faire dans une nouvelle conversation, après accord de Pierre sur les
« Décisions » ci-dessous. But : la carte SD et les clés USB de la Pi vues
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
- **Taille du noyau** : `_end` à ~39 Ko de `MEM_KERNEL_END`
  (`KERNEL_MAX_SIZE` 4 Mo, `limite.ld`). Le code du serveur (20 à 40 Ko)
  ne tiendra pas. Le plus gros de la BSS est `s_emu` (`emulator_t` de
  Phosphoric, 2,6 Mo, statique dans `oric_machine.c`, code du port).
  **Étape 0** : le réserver par `calloc` dans `oric_machine_init` (pointeur
  `s_pemu`, `#define s_emu (*s_pemu)` pour ne pas toucher au reste du
  fichier) : 2,6 Mo de marge, sans reconstruire Circle. Autre voie, plus
  lourde : `./configure --kernel-max-size 8` (mrproper, ~20 min, pilote
  Wi-Fi à recompiler).

## Décisions (à Pierre, au début de la conversation)

1. **Port** : serveur WebDAV séparé, option `dav=81` (avec `tftp=`, même
   client autorisé), la page web restant sur `CHTTPDaemon` (port 80).
   Proposé. (Plus tard, éventuellement : un seul serveur pour les deux.)
2. **PUT** : corps reçu en entier en mémoire puis `CarteEcrire`
   (`x.nouveau` par le cœur 2, renommage entre deux trames : un envoi
   coupé ne remplace jamais le fichier), limite 64 Mo par fichier.
   Proposé pour commencer. Plus tard : écriture au fil de la réception
   (morceaux de 64 Ko au cœur 2), sans limite de mémoire.
3. **DELETE d'un dossier** : récursif (comme le demande WebDAV ; PCManFM
   envoie un seul DELETE pour un dossier), refusé s'il contient une
   disquette montée. Refus toujours : racine d'un volume, fichiers de
   démarrage à la racine de la carte (`kernel8-32.img`, `config.txt`,
   `cmdline.txt`, `bootcode.bin`, `start.elf`, `fixup.dat`) en DELETE et
   MOVE (un PUT de `kernel8-32.img` reste permis : mise à jour du noyau).
   À confirmer.
4. **LOCK** : classe 1 seulement au début (`DAV: 1`, suffit à gvfs) ;
   `davfs2` avec `use_locks 0`. Faux verrous (jeton rendu, rien
   d'imposé, comme beaucoup de petits serveurs) seulement si un client
   l'exige (Windows, macOS).
5. **Noms** : ASCII imprimable seulement à la création (PUT, MKCOL, MOVE,
   COPY : refus 400 sinon, noté au journal) ; noms existants en page 850
   rendus en UTF-8 dans les listes (table de 128 caractères).

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
| DELETE | 204 ; 423 ; 403 protégé | `CarteEffacer` |
| MKCOL | 201 ; 405 existe ; 409 parent absent | `CarteCreerDossier` |
| MOVE, COPY | 201 / 204 ; 412 si `Overwrite: F` et cible présente | `CarteDeplacer`, `CarteCopier` |
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
