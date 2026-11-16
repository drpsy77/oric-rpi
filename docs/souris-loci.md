# Souris USB de LOCI (16/11/2026)

But : une souris pour CP/A (éditeur d'écran HIRES prévu), comme une souris
USB branchée sur le vrai LOCI.

## Côté port

Une souris USB branchée sur la Pi (`mouse1` de Circle, branchement et
retrait à chaud) est vue par l'Oric comme celle de la cartouche, avec
`loci=1` seulement. Cœur 0 (interruption USB) : boutons et déplacements
cumulés (`MouseStatusHandler`, `kernel.cpp`) ; cœur 1, en début de
trame : la différence depuis la trame précédente est passée à
`loci_mou_report` de Phosphoric (`oric_machine_loci_mou_report`), comme
le fait `sdl_mouse_event` de `main.c`. Rien n'est modifié dans Phosphoric.

## Côté Oric (API de LOCI)

1. Le programme choisit 5 octets en XRAM et en donne l'adresse à LOCI :
   `XREG` (opération $01), périphérique 0, canal 0, registre 1.
2. LOCI y tient ensuite, à chaque mouvement :

   | Octet | Contenu |
   |---|---|
   | 0 | boutons : bit 0 gauche, 1 droit, 2 milieu |
   | 1 | X cumulé (modulo 256, vers la droite +) |
   | 2 | Y cumulé (modulo 256, vers le bas +) |
   | 3 | molette cumulée |
   | 4 | défilement horizontal cumulé (toujours 0 sur la Pi) |

   Le programme garde la valeur lue précédente et prend la différence
   (signée, sur 8 bits) : il ne doit pas laisser passer plus de 127 points
   de mouvement entre deux lectures (une lecture par trame suffit).
3. Lecture : `ADDR0` (`$03A6`/`$03A7`), `STEP0` (`$03A5`) à 1,
   puis `RW0` (`$03A4`) avance d'un octet à chaque lecture.

Appel de `XREG`, d'après `op_pix_xreg` (`src/io/loci_core.c`) :
empiler sur `$03AC` dans cet ordre périphérique, canal, registre, octet
haut puis octet bas de l'adresse ; écrire l'opération en `$03AF` ;
`JSR $03B0` (attend la fin, rend A et X : 0 = bon).

**Piège** : empiler par `STA abs` (ou `STA abs,X` sans changement de
page). `STA (zp),Y`, donc le `POKE` du BASIC, fait d'abord une lecture
fantôme de l'adresse, comme le vrai 6502 ; lire `$03AC` dépile : chaque
`POKE 940,v` dépile puis empile, la pile n'avance pas. Les lectures de
`RW0` par `PEEK` (`LDA (zp),Y`) sont justes.

## Pour un programme : du compteur au curseur

Les octets 1 à 3 ne sont pas une position mais des compteurs qui tournent
(255 + 1 = 0, comme un compteur kilométrique). Une souris courante en
donne environ 100 par centimètre (essai sur la 3B+ : 0 à 255 en 2 à
3 cm). Le programme tient lui-même la position du curseur :

1. **Au départ**, juste après `XREG` : lire les compteurs et les garder
   comme « anciens » (ils ne valent pas forcément 0).
2. **À chaque trame** (le mieux : dans l'interruption, pour ne jamais
   laisser passer plus de 127 pas entre deux lectures, soit environ
   60 cm/s) : `d = nouveau - ancien` sur 8 bits, pris signé ; puis
   `ancien = nouveau`. Le passage de 255 à 0 se règle tout seul.
3. **Sensibilité** : `s = reste + d` ; déplacement en points
   `q = s >> n` (décalage *arithmétique*, signe gardé) ; `reste = s AND
   (2^n - 1)`. Rien n'est perdu : les petits mouvements lents
   s'additionnent. `n` est le réglage : 2 (division par 4) donne environ
   10 cm pour traverser les 240 points du HIRES avec une souris à
   100 pas/cm ; 1 ou 3 pour d'autres souris (réglage de CP/A).
4. **Curseur** : `x = x + q` borné à 0..239, `y = y + q` borné à 0..199
   (Y vers le bas : même sens que les lignes de l'écran).
5. **Boutons** (octet 0) : un clic est le passage de 0 à 1 d'un bit entre
   deux lectures ; le relâchement, de 1 à 0 (tracé à main levée tant que
   le bit est à 1).
6. **Molette** (octet 3) : même différence que l'étape 2, sans division
   (un cran = 1).

Lecture dans l'interruption : prendre la fenêtre 1 (`ADDR1` `$03AA`/`$03AB`,
`STEP1` `$03A9`, `RW1` `$03A8`) et la laisser à la souris, pour ne pas
déplacer la fenêtre 0 que le programme principal utiliserait au même moment.
`XREG` se fait une seule fois, hors interruption.

Étapes 2 et 3 pour X, avec `n = 2`, en 6502 (`ANCX` ancien compteur,
`RESX` reste 0..3, `CURX` position) ; |d| doit rester sous 125 (`s` tient
sur 8 bits signés) :

```
        LDA $03A8       ; compteur X (RW1, fenêtre sur l'octet 1)
        TAY
        SEC
        SBC ANCX        ; A = d, signé
        STY ANCX
        CLC
        ADC RESX        ; s = d + reste
        TAX
        AND #3
        STA RESX        ; nouveau reste (0..3)
        TXA
        CMP #$80        ; décalage arithmétique : C = bit de signe
        ROR A
        CMP #$80
        ROR A           ; A = q, signé (une paire CMP/ROR par bit de n)
        BMI MOINS
        CLC
        ADC CURX
        BCS MAXI        ; dépasse 255
        CMP #240
        BCC RANGE
MAXI    LDA #239
        BNE RANGE
MOINS   CLC
        ADC CURX        ; ajouter un négatif : C = 1 si pas sous 0
        BCS RANGE
        LDA #0
RANGE   STA CURX
```

Exemples : `d = -1`, reste 0 : `s = $FF`, reste 3, `q = -1` (−1 = −4 + 3) ;
`d = 5`, reste 0 : reste 1, `q = 1`.

## Programme d'essai (BASIC, Atmos)

Vérifié sur le banc natif (copie de Phosphoric, souris simulée par une
variable d'environnement) : XREG rend 0, boutons, X, Y et molette suivent.
La routine en `$0400` fait l'appel de XREG (adresse `$FF00`), le
résultat en `$0480`.

```
10 REM SOURIS DE LOCI, RAPPORT EN XRAM #FF00
20 FOR I=0 TO 34:READ V:POKE 1024+I,V:NEXT:CLS
30 CALL 1024:PLOT 1,2,"XREG"+STR$(PEEK(1152))
40 R=932:POKE 933,1
50 POKE 934,0:POKE 935,255
60 B=PEEK(R):X=PEEK(R):Y=PEEK(R):W=PEEK(R)
70 PLOT 1,4,"BOUTONS"+STR$(B)+"   "
80 PLOT 1,5,"X"+STR$(X)+" Y"+STR$(Y)+" MOLETTE"+STR$(W)+"   "
90 GOTO 50
100 DATA 169,0,141,172,3,141,172,3,169,1,141,172,3
110 DATA 169,255,141,172,3,169,0,141,172,3
120 DATA 169,1,141,175,3,32,176,3,141,128,4,96
```

En assembleur, la routine des lignes 100 à 120 :

```
        LDA #0
        STA $03AC      ; périphérique 0
        STA $03AC      ; canal 0
        LDA #1
        STA $03AC      ; registre 1 : souris
        LDA #$FF
        STA $03AC      ; adresse, octet haut
        LDA #0
        STA $03AC      ; octet bas : $FF00
        LDA #1
        STA $03AF      ; XREG
        JSR $03B0      ; attend, A = résultat
        STA $0480
        RTS
```
