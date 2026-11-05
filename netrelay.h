//
// netrelay.h — Réseau et fermetures de fichiers du cœur 1 exécutés sur le cœur 0
//
// La pile réseau de Circle et ses sockets passent par l'ordonnanceur
// coopératif, qui ne marche que sur le cœur 0 ; circle-newlib lui rend aussi
// la main à chaque fermeture de fichier (_close). Or le modem de Phosphoric
// (src/io/serial_picowifi.c) et LOCI appellent ces fonctions depuis
// l'émulation, sur le cœur 1. L'éditeur de liens (--wrap, voir Makefile) les
// détourne vers netrelay.cpp, qui les fait exécuter par le cœur 0.
// Phosphoric n'est pas modifié. Écrit pour le port.
//
#ifndef _netrelay_h
#define _netrelay_h

// Cœur 0, dans sa boucle : exécute l'appel en attente du cœur 1, s'il y en a un.
void NetRelayService (void);

// Cœur 0 : le réseau est prêt (CGlueNetworkInit fait). Avant, les sockets
// échouent proprement (ENETDOWN) : le modem répond NO CARRIER.
void NetRelaySetReady (void);

#endif
