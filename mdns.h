//
// mdns.h — Noms en .local (mDNS, Bonjour / Avahi) pour le port
//
// Circle ne résout les noms que par le serveur DNS de la box, qui ne
// connaît pas les noms en .local ; son démon mDNS (CmDNSDaemon) ne répond
// que pour le nom de la Pi, figé à « raspberrypi » par circle-stdlib.
// Cette tâche du cœur 0 fait les deux : elle répond pour « nom.local »
// (nom= ; défaut oric) et trouve l'adresse des autres machines en .local
// (tftp=, getaddrinfo du modem par netrelay.cpp, donc ATDT et HOSTS de
// CP/A). Écrit pour le port (format des messages : RFC 1035 et 6762 ; mise
// en place de la socket comme Circle lib/net/mdnsdaemon.cpp, GPL-3.0).
//
#ifndef _mdns_h
#define _mdns_h

#include <circle/sched/task.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/ipaddress.h>
#include <circle/net/socket.h>
#include <circle/types.h>

class COricMDNS : public CTask
{
public:
	COricMDNS (CNetSubSystem *pNet, const char *pName);
	~COricMDNS (void);

	void Run (void);

	// Adresse de « machine.local » ; cœur 0, depuis une tâche ou la boucle
	// principale (rend la main à l'ordonnanceur, 2 s au plus)
	static bool Resolve (const char *pName, CIPAddress *pAddress);

private:
	bool SendQuery (const char *pName);
	bool SendAnswer (void);
	void Parse (const u8 *pMsg, unsigned nLength);

private:
	CNetSubSystem *m_pNet;
	char m_Name[64];		// « oric.local »
	CSocket *m_pSocket;
	bool m_bRunning;

	static COricMDNS *s_pThis;
};

// Nom de machine -> adresse : chiffres, nom en .local (mDNS), sinon DNS de
// la box. Cœur 0 (rend la main à l'ordonnanceur).
bool ResolveHostName (CNetSubSystem *pNet, const char *pName, CIPAddress *pAddress);

// Vrai si pName finit par « .local »
bool IsLocalHostName (const char *pName);

#endif
