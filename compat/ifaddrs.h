/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * compat/ifaddrs.h — <ifaddrs.h> absent de circle-newlib.
 *
 * Utilisé par Phosphoric src/io/serial_picowifi.c pour afficher l'adresse IP
 * de l'hôte (réponse du modem). Déclarations POSIX/BSD habituelles ;
 * getifaddrs() est réalisée dans newlib_compat.c. Écrit pour le port.
 */
#ifndef COMPAT_IFADDRS_H
#define COMPAT_IFADDRS_H

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ifaddrs {
    struct ifaddrs*  ifa_next;
    char*            ifa_name;
    unsigned int     ifa_flags;
    struct sockaddr* ifa_addr;
    struct sockaddr* ifa_netmask;
    struct sockaddr* ifa_dstaddr;
    void*            ifa_data;
};

int  getifaddrs(struct ifaddrs** ifap);
void freeifaddrs(struct ifaddrs* ifa);

#ifdef __cplusplus
}
#endif

#endif /* COMPAT_IFADDRS_H */
