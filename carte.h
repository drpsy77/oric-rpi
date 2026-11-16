//
// carte.h — Accès à la carte SD depuis le réseau (TFTP, page web)
//
// Écrit pour le port. Commun à tftp.cpp et web.cpp : client autorisé
// (tftp=), noms de fichiers, lecture d'un fichier entier, remplacement
// d'un fichier sans le laisser à moitié écrit.
//
// Toutes ces fonctions touchent la carte SD : elles ne s'appellent que
// depuis une tâche du cœur 0, après pWaitIdle (cœur 1 entre deux trames),
// sans rendre la main à l'ordonnanceur entre-temps.
//
#ifndef _carte_h
#define _carte_h

#include <circle/net/netsubsystem.h>
#include <circle/net/ipaddress.h>
#include <circle/types.h>

#define CARTE_MAX_CHEMIN	300		// « SD:/ » + nom

// Seul client autorisé : adresse ou nom (cyberlab.local par mDNS, sinon DNS
// de la box), résolu à nouveau si une autre adresse se présente (bail DHCP
// changé). pService : « TFTP », « Web »... pour le journal.
class CClientAutorise
{
public:
	CClientAutorise (CNetSubSystem *pNet, const char *pNom, const char *pService);

	bool Accepte (const CIPAddress &rIP);

	const char *GetNom (void) const	{ return m_pNom; }

private:
	CNetSubSystem *m_pNet;
	const char *m_pNom;
	const char *m_pService;
	CIPAddress m_Adresse;			// sa dernière adresse connue
};

// « SD:/ » + nom (sans « / » au début) ; refus des autres volumes, de « .. »
// et de « \ ». Nom vide permis seulement si bDossier (racine).
bool CarteChemin (const char *pNom, char *pChemin, unsigned nTaille, bool bDossier = false);

// Fichier entier en mémoire (malloc, à libérer par free) ; pistes en attente
// réécrites avant si c'est une disquette montée. Faux si absent, illisible
// ou plus grand que nMax (*pTaille dit alors sa taille, 0 si absent).
bool CarteLire (const char *pChemin, u8 **ppDonnees, unsigned *pTaille, unsigned nMax);

// Même chose dans un tampon donné : *pTaille = taille du tampon en entrée,
// du fichier en sortie (0 si absent) ; faux s'il n'y tient pas.
bool CarteLireDans (const char *pChemin, u8 *pTampon, unsigned *pTaille);

// Remplace (ou crée) pChemin : écrit sous « x.nouveau », l'ancien devient
// « x.ancien », le nouveau prend son nom, l'ancien est effacé. Refusé pour
// une disquette montée. Résultat dans le journal (préfixe pService) et,
// en clair, dans pMessage (peut être 0).
bool CarteEcrire (const char *pChemin, const u8 *pDonnees, unsigned nTaille,
		  const char *pService, char *pMessage = 0, unsigned nMessage = 0);

#endif
