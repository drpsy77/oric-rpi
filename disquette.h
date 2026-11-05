/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * disquette.h — Réécriture des .dsk modifiés sur la carte SD (port Circle).
 *
 * Phosphoric garde chaque disquette en mémoire et ne la réécrit qu'à la
 * sortie de l'émulateur (jamais pour une MFM_DISK servie par LOCI) ; sur la
 * Pi on coupe le courant. Le port réécrit donc, peu après la dernière
 * écriture de l'Oric, les pistes modifiées, comme le vrai LOCI qui écrit sur
 * la clé. Voir disquette.c. Écrit pour le port.
 */
#ifndef DISQUETTE_H
#define DISQUETTE_H

#include "emulator.h"

/* Chemins des .dsk du Microdisc (disk=), Phosphoric ne les garde pas. */
void disquette_set_microdisc_path(int drive, const char* path);

/* Cœur 1, à la fin de chaque trame : note les écritures, réécrit les pistes
 * modifiées après DISQUETTE_DELAI_TRAMES trames sans écriture. */
void disquette_service(emulator_t* emu);

#endif /* DISQUETTE_H */
