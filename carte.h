//
// carte.h — Accès à la carte SD depuis le réseau (TFTP, page web)
//
// Écrit pour le port. Commun à tftp.cpp et web.cpp : client autorisé
// (tftp=), noms de fichiers, lecture d'un fichier entier, remplacement
// d'un fichier sans le laisser à moitié écrit.
//
// Elles s'appellent depuis une tâche du cœur 0. Le gros d'une lecture ou
// d'une écriture (CarteLire, CarteLireDans, CarteEcrire) est fait par le
// cœur 2, pendant que la tâche rend la main : l'émulation continue. Le
// reste (disquette montée ? renommages) est fait entre deux trames, par
// l'attente donnée à CarteInitialiser. CarteChemin, CarteRelatif : pas
// d'accès à la carte.
//
#ifndef _carte_h
#define _carte_h

#include <circle/net/netsubsystem.h>
#include <circle/net/ipaddress.h>
#include <circle/types.h>

#define CARTE_MAX_CHEMIN	300		// « SD:/ » ou « USB2:/ » + nom

// Cœur 0, au démarrage : pAttenteTrame rend la main quand le cœur 1 est
// entre deux trames (il y reste tant que la tâche ne rend pas la main).
void CarteInitialiser (void (*pAttenteTrame) (void));

// Cœur 2 : exécute les lectures et écritures demandées, dort (WFE) entre
// deux. Ne revient jamais.
void CarteCoeur2 (void);

// Cœur 0 : un travail du cœur 2 en cours sur une clé USB ? (branchements
// reportés : le retrait libère le périphérique qu'il lit)
bool CarteCoeur2SurCle (void);

// Cœur 0, arrêt ou redémarrage : attend la fin du travail en cours
void CarteCoeur2Attendre (void);

// Tâche du cœur 0 qui accède elle-même à la carte entre deux trames (listes
// de la page web) : CarteReserver attend, en rendant la main, la fin du
// travail du cœur 2 et en empêche un autre jusqu'à CarteLiberer. Sans cela,
// chaque appel de FatFs attendrait le verrou du volume pendant un morceau
// écrit par le cœur 2, émulation arrêtée. Pas de CarteLire, CarteLireDans ni
// CarteEcrire entre les deux (elles réservent elles-mêmes).
void CarteReserver (void);
void CarteLiberer (void);

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

// Fichier entier en mémoire (malloc, à libérer par free), lu par le cœur 2 ;
// pistes en attente réécrites avant si c'est une disquette montée (relue si
// le cœur 1 la réécrit pendant la lecture). Faux si absent, illisible
// ou plus grand que nMax (*pTaille dit alors sa taille, 0 si absent).
bool CarteLire (const char *pChemin, u8 **ppDonnees, unsigned *pTaille, unsigned nMax);

// Même chose dans un tampon donné : *pTaille = taille du tampon en entrée,
// du fichier en sortie (0 si absent) ; faux s'il n'y tient pas.
bool CarteLireDans (const char *pChemin, u8 *pTampon, unsigned *pTaille);

// Remplace (ou crée) pChemin : écrit sous « x.nouveau » par le cœur 2, puis,
// entre deux trames, l'ancien devient « x.ancien », le nouveau prend son
// nom, l'ancien est effacé. Refusé pour une disquette montée (vérifié avant
// et après l'écriture de « x.nouveau » : LOCI a pu la monter entre-temps). Résultat dans le journal (préfixe pService) et,
// en clair, dans pMessage (peut être 0).
bool CarteEcrire (const char *pChemin, const u8 *pDonnees, unsigned nTaille,
		  const char *pService, char *pMessage = 0, unsigned nMessage = 0);

#endif
