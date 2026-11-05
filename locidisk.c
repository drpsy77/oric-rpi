/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * locidisk.c — Démarrage direct sur une disquette de LOCI (port Circle).
 *
 * Écrit pour le port. Phosphoric n'a pas d'option pour monter une disquette
 * dans LOCI au démarrage : il faut passer par le menu (choisir l'image, puis
 * Échap). locidisk_boot fait ce que font alors le menu et le firmware, avec
 * les fonctions de Phosphoric :
 *   1. montage dans le lecteur A: de LOCI, comme op_mount (loci_fs.c) ;
 *   2. configuration du menu dans la XRAM, comme loci-rom (persist.c, mise en
 *      page relevée par cpa-oric, docs/usb.md) : le menu (F8) montre l'image
 *      et CP/A accepte le lecteur (loci_abs) ;
 *   3. démarrage comme op_mia_boot (loci_boot.c) avec LOCI_BOOT_FDC et
 *      LOCI_BOOT_B11 : BASIC 1.1 en $C000, microdis.rom en $A000 (overlay).
 */
#include "locidisk.h"
#include "io/loci_glue.h"
#include "io/loci_internal.h"
#include "utils/logging.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Configuration du menu en XRAM (loci-rom src/persist.h) */
#define CFG_BASE    0x8000      /* "LOCI" */
#define CFG_FDC_ON  0x04
#define CFG_B11_ON  0x08
#define CFG_SER_ON  0x09
#define CFG_MOUNTS  0x0B        /* bit n : lecteur n, 4 cassette, 5 ROM */
#define CFG_PATH    0x0D        /* répertoire du menu, 256 octets */
#define CFG_NAMES   0x10D       /* + 35 x n : nom seul, 32 caractères au plus */
#define CFG_NAME_SZ 35

static bool resolve_rom(loci_t* loci, const char* name, char* out, size_t outsz) {
    char guest[32];
    snprintf(guest, sizeof(guest), "0:%s", name);
    return resolve_path(loci, guest, out, outsz);
}

bool locidisk_boot(emulator_t* emu, const char* guest, bool modem) {
    loci_t* loci = &emu->loci;
    char host[512];

    /* 1. Montage (op_mount) */
    if (!resolve_path(loci, guest, host, sizeof(host)) || access(host, R_OK) != 0) {
        log_error("[port] locidisk: %s not found", guest);
        return false;
    }
    if (!loci_dsk_open(loci, 0, host)) {
        log_error("[port] locidisk: cannot mount %s", host);
        return false;
    }
    loci->mnt_mounted[0] = true;
    snprintf(loci->mnt_paths[0], sizeof(loci->mnt_paths[0]), "%s", guest);

    /* 2. Configuration du menu : Microdisc, BASIC 1.1, A: monté */
    uint8_t* cfg = &loci->xram[CFG_BASE];
    memset(cfg, 0, CFG_NAMES + CFG_NAME_SZ * LOCI_MNT_MAX);
    memcpy(cfg, "LOCI", 4);
    cfg[CFG_FDC_ON] = 1;
    cfg[CFG_B11_ON] = 1;
    cfg[CFG_SER_ON] = modem ? 1 : 0;
    cfg[CFG_MOUNTS] = 0x01;
    const char* name = strrchr(guest, '/');
    name = name ? name + 1 : (guest[0] && guest[1] == ':' ? guest + 2 : guest);
    size_t dir = (size_t)(name - guest);
    if (dir > 1 && guest[dir - 1] == '/' && guest[dir - 2] != ':') dir--;   /* "1:/jeux/x" → "1:/jeux" */
    if (dir > 255) dir = 255;
    memcpy(&cfg[CFG_PATH], guest, dir);
    snprintf((char*)&cfg[CFG_NAMES], 33, "%s", name);

    /* 3. Démarrage (op_mia_boot, FDC + BASIC 1.1) */
    char rom[512], disc[512];
    if (!resolve_rom(loci, "basic11b.rom", rom, sizeof(rom)) ||
        !resolve_rom(loci, "microdis.rom", disc, sizeof(disc))) {
        log_error("[port] locidisk: ROM path too long");
        return false;
    }
    loci->boot_settings = LOCI_BOOT_FDC | LOCI_BOOT_B11;
    if (!loci_rom_swap_cb(emu, rom, 0xC000) || !loci_rom_swap_cb(emu, disc, 0xA000)) {
        log_error("[port] locidisk: cannot load %s or %s", rom, disc);
        return false;
    }
    log_info("[port] locidisk: %s in A:, booting the Microdisc", guest);
    return true;
}
