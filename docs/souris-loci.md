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
