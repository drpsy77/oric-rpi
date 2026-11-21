# Plan : clés USB et listes par le réseau (accord de Pierre, 21/11/2026)

**Fait le 21/11/2026** (voir « Réalisation » à la fin) ; essais sur la
3B+ à faire. Trois parties, accord donné pour
les trois ; **pas de liste par TFTP** (décidé : TFTP ne fait que
transférer, la liste passe par la page web).

## Constat de départ

- Pierre a lancé `tftp -m binary oric.local -c put cpa-*.dsk` : rien n'est
  arrivé (aucune ligne « TFTP : » dans `oric/journal.txt` avant une
  lecture faite ensuite). Probablement aucun fichier ne correspondait dans
  le dossier courant. De toute façon, avec plusieurs fichiers, `put a b … z`
  de tftp-hpa prend le dernier nom pour un **dossier distant** : il faut un
  `put` par fichier.
- La racine de la carte n'est pas vue par l'Oric : les disquettes vont dans
  `oric/usb/` (« 1: ») ou sur une clé.
- Page web : les `.dsk` ne sont pas interdites (seules les disquettes
  montées le sont), mais l'envoi est limité à 4 Mo en tout (`MAX_ENVOI`) ;
  les 12 disquettes de `cpa-oric/build` font 12 Mo. Dillo 3.0.5 (navigateur
  de Pierre sur la RPI 400) n'a pas de JavaScript et ne prend sans doute
  qu'un fichier par envoi.
- Les clés USB n'apparaissent nulle part : FatFs n'a pas d'arborescence
  unique (volumes `SD:`, `USB:`, `USB2:` séparés, pas de point de montage),
  et `CarteChemin` (`carte.cpp`) colle toujours `SD:/` et refuse tout nom
  contenant `:`.

## 1. Clés USB dans la page web (`web.cpp`, `carte.cpp`)

- À la racine de la carte (`/?d=`), en tête de liste, les dossiers
  virtuels `USB:` et `USB2:`, seulement si la clé est présente
  (`cles_usb_etat ("USB:", ...) == 1`, `cles.h`). Choisi plutôt qu'un
  `/media` virtuel : un nom FAT ne peut pas contenir `:` (pas de conflit
  avec un vrai dossier), et c'est l'écriture du journal de LOCI
  (`USB:/dSK/cpa-exemples.dsk`).
- Adresses : `/?d=USB:/dSK`, `/f/USB:/dSK/x.dsk`, `/envoi?d=USB:/dSK`.
  `URL ()` code `:` en `%3A`, `Decode ()` le rend : rien à changer là.
- `CarteChemin` : accepter un préfixe `USB:` ou `USB2:` (casse libre, en
  tête seulement ; `:` toujours refusé ailleurs), le garder tel quel au
  lieu de `SD:/`. Clé absente : refus clair (« clé absente »), pas une
  erreur de FatFs. `..` et `\` toujours refusés. Ce sont TFTP et la page
  qui l'appellent : les deux gagnent les clés d'un coup.
- `web.cpp` : `SansVolume` suppose `SD:/` (4 caractères) → la
  généraliser (rendre le chemin relatif *avec* son volume pour les clés,
  `USB:/dSK`, et sans volume pour la carte, `oric/usb`). Fil d'Ariane :
  « carte / USB: / dSK ». Dossier parent de `USB:/dSK` → `USB:/`, et de
  `USB:/` → la racine de la carte (là où la clé est listée).
- Protections inchangées : `CarteEcrire` refuse une disquette montée
  (`disquette_montee` compare aussi les chemins `USB:/…` de LOCI,
  minuscules) ; écriture par `.nouveau`/`.ancien` ; carte et clé touchées
  seulement après `WaitIdle`, sans rendre la main. Vérifié : les transferts
  USB de Circle attendent en boucle (`NO_BUSY_WAIT` non défini dans
  `sysconfig.h`), donc pas de `Yield` pendant une lecture de clé, et
  `ClesUSBMiseAJour` (retrait) ne peut pas passer au milieu.
- Le test `bNoyau` de `CarteEcrire` (`SD:/kernel8-32.img`) reste propre à
  la carte.

## 2. Noms de clés pour TFTP (`tftp.cpp`)

- Rien de plus que `CarteChemin` étendu (`MakePath` l'appelle) : `put x.dsk
  USB:/dSK/x.dsk`, `get USB:/dSK/x.dsk`. Mettre à jour le commentaire de
  `MakePath` (« refus des autres volumes »).
- Pas de liste par TFTP.

## 3. Liste en texte simple et script `carte.sh`

- `web.cpp` : adresse `/l/<chemin>` (`/l/`, `/l/oric/usb`, `/l/USB:/dSK`)
  en `text/plain`, une ligne par entrée, facile à lire par un script :
  `d  -        -                nom/` pour un dossier, `f  taille  AAAA-MM-JJ hh:mm  nom`
  pour un fichier (taille en octets exacte), ` montee` en fin de ligne pour
  une disquette montée. À la racine, `USB:/` et `USB2:/` en dossiers si
  présentes. Erreur : code HTTP 404 et une ligne de texte. Même client
  autorisé que le reste.
- `carte.sh` à la racine du dépôt (sur la RPI 400, comme
  `installer_carte.sh`), en français, `ORIC=oric.local` par défaut
  (variable d'environnement) :
  - `carte.sh ls [dossier]` → `curl -s http://$ORIC/l/dossier` ;
  - `carte.sh envoyer dossier fichier...` → un `tftp -m binary $ORIC -c put
    fichier dossier/nom` par fichier, puis **vérification par la liste**
    (taille attendue) : le serveur acquitte le dernier bloc avant
    d'écrire, un refus ou un échec d'écriture ne se voit que dans le
    journal (`tftp.cpp`, en-tête). Dire « écrit » ou « non écrit (voir
    journal) » pour chaque fichier ; disquette montée : refus annoncé par
    tftp dès la demande ;
  - `carte.sh lire chemin [local]` → `tftp ... -c get` ;
  - `carte.sh journal` → lecture de `oric/journal.txt` (pratique pour voir
    les refus).
  Vérifier le code de retour de `tftp -c` (tftp-hpa 5.2) en cas d'erreur
  du serveur avant de s'y fier.
- Le noyau aussi : `carte.sh envoyer . kernel8-32.img` (racine de la
  carte) doit marcher.

## Docs à mettre à jour ensuite

`CLAUDE.md` (Carte SD : `tftp=`, `web=` ; tableau des fichiers du port :
`carte.sh`), en-têtes de `web.cpp`, `tftp.cpp`, `carte.cpp`. Pas de
Phosphoric touché, rien pour `docs/phosphoric-f8.md`.

## Essais

Banc : `make` (compile) ; la logique de `CarteChemin` et du format `/l/`
peut s'essayer à part (petit programme C sur la RPI 400). Sur la 3B+
(Pierre) : `carte.sh ls`, `carte.sh ls USB:/`, envoi des 12 disquettes de
`cpa-oric/build` dans `oric/usb`, envoi sur une clé, refus d'une disquette
montée, clé retirée puis liste.

## Réalisation (21/11/2026)

- `CarteChemin` (`carte.cpp`) : préfixe `USB:`/`USB2:` (casse libre, en
  tête), volume gardé (`USB:/dSK`), clé absente refusée ; raison rendue
  par `TCarteErreur` (`CarteNomInvalide`, `CarteCleAbsente` : texte du
  journal sans accent, texte de la page). `CarteRelatif` remplace
  `SansVolume` (« USB:/ » → « USB: », dont le parent est la racine).
  `CarteEcrire` refuse aussi une clé retirée entre la demande et
  l'écriture. Logique essayée à part (petit programme C++ : racine,
  clé absente, `..`, `:` ailleurs, « USB3: », « USBX/ »).
- `web.cpp` : `LireDossier` commun à la page et à `/l/` ; clés présentes en
  tête de la racine. Écart au plan : refus de `/l/` en une ligne
  `erreur : ...` avec le code **200**, pas 404 : CHTTPDaemon remplace le
  contenu de toute réponse non 200 par sa page générique (« Not Found »).
  Pour la même raison, les pages « Refusé » et « Fichier introuvable »
  (qui rendaient 400 ou 404 et ne s'affichaient donc jamais) sont en 200.
  Liste tronquée : dernière ligne `# liste arretee a 1000 entrees`.
- `tftp.cpp` : rien que `CarteChemin` étendu ; raison du refus au journal.
- `carte.sh` : constaté sur la Pi avec tftp-hpa 5.2 : `tftp -c` rend
  **toujours 0** (« Error code 1: File not found » compris) et crée ou
  vide le fichier local ; sans `-l`, « USB:/x » est pris pour la machine
  « USB ». Donc : `-l`, erreur = toute sortie de tftp, lecture dans un
  fichier temporaire renommé ensuite. Vérification d'un envoi : taille
  exacte **et** date ≥ heure de l'envoi moins une minute (les `.dsk` ont
  souvent la même taille que la version remplacée) ; heure de la Pi pas
  réglée : « non vérifié ».
