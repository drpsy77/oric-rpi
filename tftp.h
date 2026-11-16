//
// tftp.h — Serveur TFTP de la carte SD (tftp=<client : adresse ou nom>)
//
// Mise à jour du noyau, des disquettes, lecture du journal sans retirer la
// carte. Sur CTFTPDaemon de Circle (lib/net/tftpdaemon.cpp, GPL-3.0, voir
// SOURCES.md), comme addon/tftpfileserver ; écrit pour le port : fichiers
// gardés en mémoire pendant le transfert, carte SD touchée seulement quand
// l'émulation est entre deux trames, remplacement d'un fichier sans le
// laisser à moitié écrit, disquettes montées protégées.
//
#ifndef _tftp_h
#define _tftp_h

#include <circle/net/tftpdaemon.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/ipaddress.h>
#include <circle/types.h>
#include "carte.h"

class COricTFTP : public CTFTPDaemon
{
public:
	// pClient : seul client autorisé, adresse ou nom (cyberlab.local par
	// mDNS, sinon DNS de la box), résolu à nouveau si une autre adresse
	// se présente (bail DHCP changé).
	// pWaitIdle : rend la main quand le cœur 1 est entre deux trames (il le
	// reste tant que cette tâche ne rend pas la main à l'ordonnanceur)
	COricTFTP (CNetSubSystem *pNetSubSystem, const char *pClient,
		   void (*pWaitIdle) (void));
	~COricTFTP (void);

	boolean FileOpen (const char *pFileName);
	boolean FileCreate (const char *pFileName);
	boolean FileClose (void);
	int FileRead (void *pBuffer, unsigned nCount);
	int FileWrite (const void *pBuffer, unsigned nCount);

	boolean IsAccessAllowed (const CIPAddress *pForeignIP,
				 const char *pFileName, boolean bWriteRequest);

	void UpdateStatus (TStatus Status, const char *pFileName);

private:
	bool MakePath (const char *pFileName);
	void Release (void);

private:
	CClientAutorise m_Client;
	void (*m_pWaitIdle) (void);

	char m_Path[CARTE_MAX_CHEMIN];		// « SD:/ » + nom
	u8 *m_pData;				// fichier entier, en mémoire
	unsigned m_nSize;			// octets reçus ou à envoyer
	unsigned m_nAlloc;
	unsigned m_nPos;			// lecture : prochain octet envoyé
	bool m_bWriting;
};

#endif
