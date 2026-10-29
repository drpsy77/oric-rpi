/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * oric_machine.h — Interface minimale de la machine Oric pour le port Circle.
 *
 * API C opaque : n'inclut AUCUN en-tête de Phosphoric, pour que le kernel
 * C++ de Circle n'ait pas à compiler emulator.h (qui utilise le mot-clé C11
 * _Atomic, inconnu du C++). Implémentation : oric_machine.c (voir SOURCES.md).
 */
#ifndef ORIC_MACHINE_H
#define ORIC_MACHINE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORIC_MAX_DRIVES   4      /* = MICRODISC_MAX_DRIVES de Phosphoric */
#define ORIC_FRAME_US     20000  /* une trame PAL = 20 ms */

/* Supports à charger au démarrage. NULL = absent. Chemins newlib/FatFs
 * (circle-stdlib), relatifs à la racine de la carte SD. */
typedef struct {
    const char* rom_file;        /* ROM système (basic11b.rom ou basic10.rom) — obligatoire */
    const char* disk_rom_file;   /* microdis.rom — active le Microdisc */
    const char* disk_files[ORIC_MAX_DRIVES]; /* .dsk lecteurs A..D */
    const char* tape_file;       /* .tap lu par CLOAD"" (pièges ROM de Phosphoric) */
    const char* fdc_timing;      /* "fast" ou "real" (--fdc-timing) ; NULL = real */
    bool        loci;            /* cartouche LOCI (émulation de haut niveau) */
    const char* loci_flash_dir;  /* dossier = mémoire flash interne « 0: » */
    const char* loci_usb_dir;    /* dossier = clé USB « 1: » (NULL : aucune) */
} oric_media_t;

/* Initialise la machine, charge les supports, fait le reset CPU.
 * Retourne false en cas d'échec (le message est dans le journal). */
bool oric_machine_init(const oric_media_t* media);

/* Exécute une trame PAL complète (19 968 cycles = 20 ms d'Oric). */
void oric_machine_run_frame(void);

/* Reset (F5 de Phosphoric) et NMI (bouton sous l'Oric, Maj+F5). */
void oric_machine_reset(void);
void oric_machine_nmi(void);

/* LOCI : bouton Action (appui puis relâchement = menu LOCI) et clavier USB
 * vu par l'API de LOCI (rapport HID : modificateurs + 6 codes). */
void oric_machine_loci_button(bool down);
void oric_machine_loci_kbd_report(uint8_t modifiers, const uint8_t keys[6]);

/* Image de la dernière trame : RGB888, *w x *h pixels, lignes de 3*w octets. */
const uint8_t* oric_machine_framebuffer(int* w, int* h);

/* Matrice clavier, 8 colonnes, actif bas (bit à 0 = touche enfoncée). */
uint8_t* oric_machine_keyboard_matrix(void);
void oric_machine_keyboard_release_all(void);

/* Son de l'AY-3-8912 : n échantillons stéréo 16 bits entrelacés. */
int  oric_machine_audio_rate(void);              /* Hz (44 100) */
void oric_machine_audio(int16_t* stereo, int n);

/* Ne garder que les avertissements et erreurs dans le journal Phosphoric. */
void oric_machine_quiet_log(void);

/* Version de Phosphoric. */
const char* oric_machine_version(void);

#ifdef __cplusplus
}
#endif

#endif /* ORIC_MACHINE_H */
