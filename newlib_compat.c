/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * newlib_compat.c — Fonctions POSIX absentes de circle-newlib.
 *
 * access() : utilisée par Phosphoric src/cards/card_microdisc.c pour savoir
 * si le fichier .dsk est en lecture seule (onglet de protection). circle-newlib
 * ne la fournit pas ; on la réalise avec stat() (présent) et fopen().
 *
 * popen()/pclose() : le modem picowifi de Phosphoric (src/io/serial_picowifi.c)
 * interroge l'hôte (iwgetid, nmcli) pour le nom du réseau Wi-Fi. Pas de
 * shell ici : échec propre, le modem prend alors ses valeurs par défaut.
 *
 * getifaddrs()/freeifaddrs() (compat/ifaddrs.h) : même modem, adresse IP de
 * l'hôte. Pas de liste d'interfaces POSIX dans Circle : échec propre.
 *
 * Écrit pour le port (voir SOURCES.md).
 */
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ifaddrs.h>

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

FILE* popen(const char* command, const char* mode) {
    (void)command; (void)mode;
    errno = ENOSYS;
    return NULL;
}

int pclose(FILE* stream) {
    (void)stream;
    errno = ECHILD;
    return -1;
}

int getifaddrs(struct ifaddrs** ifap) {
    *ifap = NULL;
    errno = ENOSYS;
    return -1;
}

void freeifaddrs(struct ifaddrs* ifa) {
    (void)ifa;
}
