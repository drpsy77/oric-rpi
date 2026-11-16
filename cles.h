//
// cles.h — Clés USB branchées sur la Pi, servies par LOCI
//
// Écrit pour le port. Deux clés au plus : umsd1 → volume « USB: » de
// FatFs, umsd2 → « USB2: » (noms de Circle, addon/fatfs/diskio.cpp et
// ffconf.h). FAT et exFAT (preparer_circle.sh). Branchement et retrait à
// chaud, sans prévenir, comme sur le vrai LOCI : chaque clé a sa place
// dans la liste des périphériques de LOCI (oric_machine.c), « no USB
// key » sans clé.
//
// ClesUSBMiseAJour : cœur 0, seulement entre deux trames (le retrait
// d'une clé libère le périphérique que le cœur 1 pourrait lire).
//
#ifndef _cles_h
#define _cles_h

#define CLES_MAX	2

#ifdef __cplusplus
extern "C" {
#endif

// État de la clé dont pPath est un chemin (« USB:/Dsk/x.dsk ») :
//   -1 : pas un chemin de clé (carte SD) ; 0 : clé absente ; 1 : présente.
// Présente : *pSerie = numéro de série du volume, *pBranchement = compteur
// qui change à chaque branchement (clé retirée puis remise).
int cles_usb_etat (const char *pPath, unsigned *pSerie, unsigned *pBranchement);

#ifdef __cplusplus
}

// Volume FatFs de la clé i (« USB: ») et texte pour la liste de LOCI
const char *ClesUSBVolume (unsigned i);
const char *ClesUSBTexte (unsigned i);

// Clés branchées ou retirées depuis le dernier appel : montées, démontées,
// décrites dans le journal ; texte de LOCI mis à jour si bLOCI.
void ClesUSBMiseAJour (bool bLOCI);
#endif

#endif
