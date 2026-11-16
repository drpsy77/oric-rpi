//
// cles.h — Clés USB branchées sur la Pi (étape 1 : détection, journal)
//
// Écrit pour le port. Deux clés au plus : umsd1 → volume « USB: » de
// FatFs, umsd2 → « USB2: » (noms de Circle, addon/fatfs/diskio.cpp et
// ffconf.h). Branchement et retrait à chaud, sans prévenir, comme sur le
// vrai LOCI. Pour l'instant, la clé n'est que montée et décrite dans le
// journal (format, taille, nom et numéro de série du volume, racine) ;
// elle n'est pas encore servie à LOCI.
//
// Cœur 0 seulement (le cœur 1 ne touche pas encore aux clés).
//
#ifndef _cles_h
#define _cles_h

#define CLES_MAX	2

// Après chaque UpdatePlugAndPlay () qui a vu un changement sur l'USB
void ClesUSBMiseAJour (void);

#endif
