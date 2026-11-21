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

#define CARTE_MAX_CHEMIN	300		// « SD:/ » ou « USB2:/ » + nom

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

// Chemin de FatFs d'un nom reçu du réseau (« / » au début sans effet) :
//   « oric/usb/x.dsk »  → « SD:/oric/usb/x.dsk » (carte SD)
//   « USB:/dSK/x.dsk »  → « USB:/dSK/x.dsk » (clé USB, cles.h : « USB: » ou
//                         « USB2: », casse libre, en tête seulement)
// Refus de « : » ailleurs, de « .. » et de « \ », et d'une clé absente.
// Nom vide (ou volume seul) permis seulement si bDossier (racine). Dossier :
// sans « / » final, sauf la racine (« SD:/ », « USB:/ »). Refus : raison
// dans *ppErreur, si ppErreur (CarteNomInvalide, CarteCleAbsente).
struct TCarteErreur
{
	const char *pJournal;		// sans accent
	const char *pTexte;		// pour la page web
};
extern const TCarteErreur CarteNomInvalide, CarteCleAbsente;

bool CarteChemin (const char *pNom, char *pChemin, unsigned nTaille, bool bDossier = false,
		  const TCarteErreur **ppErreur = 0);

// Chemin pour la page web : « SD:/oric/usb » → « oric/usb » (carte, « » à la
// racine) ; « USB:/dSK » → « USB:/dSK », « USB:/ » → « USB: » (clé)
void CarteRelatif (const char *pChemin, char *pRelatif, unsigned nTaille);

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
