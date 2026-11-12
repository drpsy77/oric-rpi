## Oric sur Raspberry Pi — v0.1

Première version prête à l'emploi : le contenu de la carte SD pour transformer une **Raspberry Pi 3B+** ou une **Raspberry Pi Zero 2 W** en Oric Atmos, sans Linux (bare-metal, Circle).

### Installation

1. Carte micro-SD, partition FAT32 (table MBR).
2. Décompresser `oric-rpi-carte-sd-v0.1.zip` **à la racine** de la carte.
3. Ajouter vos ROM (non fournies, droits d'auteur) : `oric/loci/basic11b.rom` (obligatoire) et `oric/loci/microdis.rom` (disquettes).
4. Disquettes `.dsk` et cassettes `.tap` dans `oric/usb/`.
5. HDMI + clavier USB, alimentation : le menu LOCI s'affiche (Échap pour démarrer l'Oric).

Arrêt : **Ctrl+Alt+Fin**, puis débrancher quand le message l'indique. Détails dans `LISEZMOI.txt` de l'archive.

### Contenu

- Phosphoric 2.31.15 (acfeacc) : BASIC 1.1, HIRES, son HDMI, clavier AZERTY, cassette, Microdisc, cartouche LOCI (menu, API fichiers), modem Wi-Fi de LOCI (`modem=1`).
- Firmware Raspberry Pi (révision du 20/08/2025), arbre matériel de la Zero 2 W, micrologiciels Wi-Fi des deux cartes.
- ROM du menu LOCI v0.3.0 (Sodiumlightbaby, BSD-3-Clause).

### Zero 2 W

Même SoC (BCM2710) et même noyau `kernel8-32.img` que la 3B+. À prévoir : un adaptateur OTG micro-USB pour le clavier et un câble mini-HDMI. Le processeur tourne à 1 GHz au lieu de 1,4 GHz : environ 10 ms par trame de 20 ms attendus.

### Construit avec

circle-stdlib 4fbab62 (Circle Step51.1.1), `-r 3 -o ARM_ALLOW_MULTI_CORE`, pilote Wi-Fi recompilé avec les en-têtes de circle-newlib ; arm-none-eabi-gcc 13.2.1.

SHA-256 de l'archive : `b482751e042623d11bc13ed02d420f6278f7e5e2bc295008b7d6436ed843563b`

Licences : oric-rpi et Phosphoric EUPL-1.2, Circle et circle-stdlib GPL-3.0 (textes dans `licences/`).

Merci à **bmarty** (Phosphoric) et **Sodiumlightbaby** (LOCI).
