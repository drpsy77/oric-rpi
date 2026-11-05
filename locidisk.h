/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * locidisk.h — Démarrage direct sur une disquette de LOCI (port Circle).
 * Voir locidisk.c. Écrit pour le port.
 */
#ifndef LOCIDISK_H
#define LOCIDISK_H

#include "emulator.h"

/* Monte guest (chemin vu par LOCI, ex. "0:/cpa.dsk") dans le lecteur A: de
 * LOCI, écrit la configuration du menu en XRAM et démarre le Microdisc (BASIC
 * 1.1 + microdis.rom de 0:), comme le menu puis Échap. LOCI doit être prêt
 * (étape CARD_STAGE_LOCI faite, ROM du menu chargée). false : rien démarré
 * ou démarrage incomplet (message dans le journal). */
bool locidisk_boot(emulator_t* emu, const char* guest, bool modem);

#endif /* LOCIDISK_H */
