/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * newlib_compat.c — Fonctions POSIX absentes de circle-newlib.
 *
 * access() : utilisée par Phosphoric src/cards/card_microdisc.c pour savoir
 * si le fichier .dsk est en lecture seule (onglet de protection). circle-newlib
 * ne la fournit pas ; on la réalise avec stat() (présent) et fopen().
 * Écrit pour le port (voir SOURCES.md).
 */
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int access(const char* path, int mode) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;                       /* errno positionné par stat() */
    if (mode & W_OK) {
        FILE* f = fopen(path, "r+");     /* ouverture en écriture sans tronquer */
        if (!f) {
            errno = EACCES;
            return -1;
        }
        fclose(f);
    }
    return 0;
}
