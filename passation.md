Voici le résumé, prêt à coller dans la nouvelle conversation :

---

**Passation — Oric sur Raspberry Pi (Circle + Phosphoric), état au 05/11/2026**

**Principe (inchangé).** Émulateur Oric en bare-metal avec Circle et circle-stdlib. Le cœur est Phosphoric, recopié sans modification par `importer_phosphoric.sh` (le dossier `phosphoric/` n'est pas versionné). Chaque reprise est notée dans `SOURCES.md`. Licence EUPL-1.2. Dépôt GitHub `drpsy77/oric-rpi`, branche unique `main`, dernier commit `9656e92`. Tout le travail se fait sur la RPI 400.

**Changement de cible.** Le Pi Zero est abandonné : trop juste en performances, gel avec LOCI. La Zero 2 W est en rupture pour des semaines. La cible est maintenant la **Pi 3B+**. Pour Circle, la Zero 2 W et la 3B+ sont la même cible (`-r 3`) : le même `kernel8-32.img` servira aux deux.

**Environnement sur la RPI 400, refait de zéro le 05/11 :**
- `/home/pi/Projets/circle-stdlib` : clone neuf (`git clone --recursive`), configuré `./configure -r 3`. C'est le seul circle-stdlib ; l'arborescence du Zero et la copie `rsync` `-rpi3` sont supprimées. Le Makefile le vise par défaut, il n'y a plus besoin de `CIRCLESTDLIB=`.
- `/home/pi/Projets/phosphoric` : clone au commit 675cb06.
- `/home/pi/Projets/oric-rpi` : construire avec `sh importer_phosphoric.sh /home/pi/Projets/phosphoric`, puis `make clean && make`, qui produit `kernel8-32.img`.

**Leçons du 05/11 :**
- La mire multicolore figée avec un clignotement normal de la LED veut dire que le noyau est chargé mais se bloque avant l'écran. La cause était la copie `rsync` de circle-stdlib : newlib cassé (les `str*` étaient introuvables, même pour `samples/02-stdio-hello`). Le clone neuf a réglé le problème.
- Les exemples de Circle seul (`libs/circle/sample/…`) ne compilent pas dans un arbre circle-stdlib. Pour tester un arbre, utiliser `circle-stdlib/samples/02-stdio-hello`.
- L'erreur « aucune règle pour fabriquer kernel.h » venait d'un fichier versionné effacé pendant le ménage. On le restaure avec `git restore .`.
- Carte SD : table MBR et première partition en FAT32. Le nom de la partition est sans importance.

**Carte SD de la 3B+, à la racine :**
- `bootcode.bin`, `start.elf`, `fixup.dat`, issus de `circle-stdlib/libs/circle/boot` après `make` ;
- `kernel8-32.img` ;
- `config.txt` : `arm_64bit=0` et `kernel=kernel8-32.img`, sans section `[...]` ;
- `cmdline.txt`, une seule ligne :
  - sans LOCI : `width=240 height=224 keymap=FR rom=oric/basic11b.rom`, plus `diskrom=oric/microdis.rom disk=oric/xxx.dsk` pour le Microdisc ;
  - avec LOCI : `width=240 height=224 keymap=FR loci=1`, sans `rom=` ;
- `oric/basic11b.rom` et `oric/microdis.rom`, avec les `.dsk`/`.tap` éventuels, pour le mode sans LOCI ;
- `oric/loci/` : flash interne « 0: ». Y mettre `locirom` (copié depuis `phosphoric/roms/loci/`), `basic11b.rom` et `microdis.rom` ;
- `oric/usb/` : clé USB « 1: ». C'est là que vont les `.dsk` et `.tap` pour LOCI.

**En cours :**
1. Confirmer que `kernel8-32.img` démarre sur la 3B+ et trouve ses ROM. En cas d'échec, relever le message exact `Failed to load ROM: …`.
2. Mesurer avec F12 (écrit `oric/perf.txt`) : BASIC, LOGO DEMO, puis avec LOCI. Surveiller un éventuel gel pendant 5 à 10 minutes. Sur le Zero, on avait 13,2 ms par trame en BASIC, 16 à 21 ms sur LOGO DEMO, et avec LOCI environ 23 ms puis un gel.

**À faire, avec l'accord de Pierre :**
- Mettre à jour `CLAUDE.md` : cible 3B+ / Zero 2 W, environnement unique en `-r 3`, leçons ci-dessus.
- Mettre à jour `sdcard/config.txt` (modèle pour la 3B+) et le `README.md`, section « Carte SD », avec `kernel8-32.img`.
- Simplifier le Makefile : une seule cible, plus besoin de `make zero` / `make pi3`.
- Ouverts : image probablement étirée en 16:9 ; pas de menu de choix des supports sans LOCI ; « LOCI allégé » pour CP/A.

---

Pour information, la copie du dépôt sur le Mac n'a pas été modifiée. Le seul changement est un fichier verrou Git renommé en `.git/stale-maintenance-lock`, que tu peux supprimer.