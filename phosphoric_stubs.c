/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * phosphoric_stubs.c — Bouche-trous pour les composants de Phosphoric
 * volontairement NON repris dans le port Circle (étape 1).
 *
 * Pourquoi ce fichier : le cœur de Phosphoric (io_bus.c, card_modules.c,
 * memory.c, tape_patches.c, disk.c) référence ces symboles, mais ne les
 * appelle que si la carte correspondante est présente (emu->card_on[...]),
 * ce qui n'est jamais le cas ici. Les fichiers de Phosphoric restent donc
 * NON MODIFIÉS ; seul ce fichier est ajouté.
 *
 * Signatures recopiées des en-têtes de Phosphoric (voir SOURCES.md).
 *
 * LOCI (émulation de haut niveau) est repris : plus de stub pour lui.
 *
 * Pour réintégrer un composant plus tard : copier son .c depuis Phosphoric
 * et supprimer le stub correspondant ci-dessous.
 */
#include <stdbool.h>
#include "emulator.h"
#include "card_module.h"
#include "storage/disk_http.h"

/* Cartes absentes — src/cards/card_<id>.c non repris.
 * Module vide = sans menu, sans option, sans bus, sans setup (testé NULL). */
const card_module_t card_acia    = {0};   /* ACIA 6551 / modem   */
const card_module_t card_dtl2000 = {0};   /* Digitelec DTL 2000  */
const card_module_t card_mageco  = {0};   /* Mageco MIDI         */
const card_module_t card_sp0256  = {0};   /* synthèse vocale     */
const card_module_t card_mea8000 = {0};   /* synthèse vocale     */

/* Ticks des cartes (include/card_ticks.h), appelés seulement si card_on. */
void card_acia_tick(emulator_t* e, int c)    { (void)e; (void)c; }
void card_dtl2000_tick(emulator_t* e, int c) { (void)e; (void)c; }
void card_mageco_tick(emulator_t* e, int c)  { (void)e; (void)c; }
void card_sp0256_tick(emulator_t* e, int c)  { (void)e; (void)c; }
void card_mea8000_tick(emulator_t* e, int c) { (void)e; (void)c; }

/* Disquettes via HTTP — src/storage/disk_http.c non repris (pas de réseau). */
long disk_http_get(const char* u, long o, long l, uint8_t* out, long cap) {
    (void)u; (void)o; (void)l; (void)out; (void)cap; return -1;
}
