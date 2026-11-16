//
// web.h — Page web de la carte SD (web=<port>, avec tftp=<client>)
//
// Liste des dossiers de la carte, lecture et envoi de fichiers, état de la
// machine, redémarrage, depuis un navigateur. Sur CHTTPDaemon de Circle
// (lib/net/httpdaemon.cpp, GPL-3.0, voir SOURCES.md) ; même client autorisé
// et mêmes protections que le serveur TFTP (carte.cpp).
//
#ifndef _web_h
#define _web_h

#include <circle/net/httpdaemon.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/net/ipaddress.h>
#include <circle/string.h>
#include <circle/types.h>
#include "carte.h"

// Fournis par le noyau
struct TWebHooks
{
	void (*WaitIdle) (void);	// cœur 1 entre deux trames (voir tftp.cpp)
	void (*Status) (CString *pHTML);// lignes <tr> de l'état de la machine
	void (*Reboot) (void);		// comme Ctrl+Alt+Suppr, un peu plus tard
};

class COricWeb : public CHTTPDaemon
{
public:
	// Écoute (pSocket == 0) : crée le client autorisé, partagé ensuite par
	// les tâches de chaque connexion
	COricWeb (CNetSubSystem *pNetSubSystem, u16 nPort, const char *pClient,
		  const TWebHooks *pHooks,
		  CSocket *pSocket = 0, CClientAutorise *pClientAutorise = 0);
	~COricWeb (void);

	CHTTPDaemon *CreateWorker (CNetSubSystem *pNetSubSystem, CSocket *pSocket);

	THTTPStatus GetContent (const char *pPath, const char *pParams,
				const char *pFormData, u8 *pBuffer, unsigned *pLength,
				const char **ppContentType);

	void WriteAccessLog (const CIPAddress &rRemoteIP, THTTPRequestMethod RequestMethod,
			     const char *pRequestURI, THTTPStatus Status,
			     unsigned nContentLength);

private:
	THTTPStatus PageDossier (const char *pDossier, class CPage *pPage);
	THTTPStatus Fichier (const char *pNom, u8 *pBuffer, unsigned *pLength,
			     const char **ppContentType, class CPage *pPage);
	THTTPStatus Envoi (const char *pDossier, class CPage *pPage);
	THTTPStatus Redemarrer (const char *pFormData, class CPage *pPage);

private:
	CNetSubSystem *m_pNet;
	u16 m_nPort;
	const char *m_pClientNom;
	const TWebHooks *m_pHooks;
	bool m_bEcoute;
	CClientAutorise *m_pClient;
	CIPAddress m_ForeignIP;			// connexion : adresse du navigateur
};

#endif
