//
// dav.h — Serveur WebDAV de la carte SD (dav=<port>, avec tftp=<client>)
//
// La carte SD et les clés USB de la Pi vues comme un dossier depuis le
// client de tftp= : dav://oric.local:81/ dans PCManFM (gvfs), davfs2 pour
// un point de montage. Lire, envoyer, effacer, renommer, déplacer, créer
// des dossiers. Serveur HTTP propre au port (CHTTPDaemon de Circle ne
// connaît que GET, HEAD et POST) : une tâche d'écoute, une tâche par
// connexion (DAV_CONNEXIONS au plus), sur le cœur 0. Accès à la carte par
// carte.cpp (cœur 2 pour les fichiers, contrôles entre deux trames).
// Structure reprise de CHTTPDaemon (lib/net/httpdaemon.cpp, GPL-3.0,
// voir SOURCES.md). Écrit pour le port.
//
#ifndef _dav_h
#define _dav_h

#include <circle/sched/task.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/netdevice.h>
#include <circle/types.h>
#include "carte.h"

#define DAV_CONNEXIONS	4		// les suivantes attendent une place

class CTexteDAV;

class COricDAV : public CTask
{
public:
	// Écoute (pSocket == 0) : crée le client autorisé, partagé ensuite par
	// les tâches des connexions
	COricDAV (CNetSubSystem *pNet, u16 nPort, const char *pClient,
		  CSocket *pSocket = 0, CClientAutorise *pClientAutorise = 0);
	~COricDAV (void);

	void Run (void);

private:
	void Ecoute (void);
	void Connexion (void);
	bool Requete (void);			// faux : fermer la connexion

	// Réception
	bool Remplir (bool bEntreRequetes);
	int LireLigne (char *pLigne, unsigned nTaille, bool bEntreRequetes);
	bool Lire (u8 *pDest, unsigned nLongueur);
	bool LireCorps (u8 **ppCorps, unsigned *pnCorps, unsigned nMax, unsigned *pnStatut);
	void IgnorerCorps (void);

	// Réponses
	bool Repondre (unsigned nStatut, const char *pType = 0, const void *pCorps = 0,
		       unsigned nCorps = 0, const char *pEntetes = "", bool bSansCorps = false,
		       unsigned nLongueur = 0);
	bool Erreur (unsigned nStatut, const char *pTexte = 0);
	bool ErreurCarte (TCarteCode Code);

	// Méthodes
	bool Options (void);
	bool Get (const char *pChemin, bool bHead);
	bool Put (const char *pChemin);
	bool Delete (const char *pChemin);
	bool Mkcol (const char *pChemin);
	bool MoveCopy (const char *pChemin, bool bMove);
	bool PropFind (const char *pChemin);
	bool PropPatch (const char *pChemin);

	void Entree (CTexteDAV *pXML, const char *pRelatif, const TCarteEntree *pEntree);

private:
	CNetSubSystem *m_pNet;
	u16 m_nPort;
	const char *m_pClientNom;
	CSocket *m_pSocket;
	bool m_bEcoute;
	CClientAutorise *m_pClient;
	bool m_bAutorise;

	// Tampon de réception : Receive rend un segment entier, ou le perd
	u8 m_Recu[FRAME_BUFFER_SIZE];
	unsigned m_nRecuPos, m_nRecuFin;

	// Requête en cours
	char m_Methode[16];
	char m_Cible[2048];			// chemin de l'URL, non décodé
	char m_Destination[2048];		// MOVE, COPY
	unsigned m_nLongueur;			// Content-Length
	bool m_bLongueur;
	bool m_bChunked;
	bool m_bContinue;			// Expect: 100-continue
	int m_nProfondeur;			// Depth : 0, 1, -1 (infinity), -2 (absent)
	bool m_bEcraser;			// Overwrite
	bool m_bFermer;				// Connection: close, HTTP/1.0, corps non lu
};

#endif
