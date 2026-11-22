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

/* Réécrit tout de suite ce qui a changé (avant un redémarrage). L'émulation
 * doit être arrêtée (entre deux trames). */
void disquette_flush(emulator_t* emu);

/* Même chose, seulement si des pistes attendent (écriture de l'Oric depuis
 * la dernière réécriture, clé absente) : sinon les fichiers sont à jour, et
 * disquette_flush chargerait chaque disquette jamais réécrite pour la
 * comparer (~0,5 s par Mo, émulation arrêtée). */
void disquette_flush_attente(emulator_t* emu);

/* Vrai si path (relatif à la racine de la carte, « SD:/ » permis) est une
 * disquette montée, Microdisc ou LOCI. Émulation entre deux trames. */
bool disquette_montee(emulator_t* emu, const char* path);

/* Vrai si une disquette montée est dans le dossier dir ou plus bas (même
 * forme de chemin). Émulation entre deux trames. */
bool disquette_montee_sous(emulator_t* emu, const char* dir);

/* Compteur des réécritures (impair : une réécriture en cours), lisible
 * depuis n'importe quel cœur : une lecture faite pendant que la valeur est
 * restée la même et paire n'a croisé aucune réécriture. */
unsigned disquette_reecritures(void);

#endif /* DISQUETTE_H */
