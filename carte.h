//
// carte.h — Accès à la carte SD depuis le réseau (TFTP, page web, WebDAV)
//
// Écrit pour le port. Commun à tftp.cpp, web.cpp et dav.cpp : client autorisé
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
#include <fatfs/ff.h>

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

// Résultat des opérations de la carte (WebDAV : code HTTP de la réponse)
enum TCarteCode
{
	CarteOK,
	CarteAbsent,		// fichier ou dossier introuvable
	CarteParentAbsent,	// dossier de destination introuvable
	CarteExiste,		// destination présente (remplacement non demandé)
	CarteMontee,		// disquette montée (ou dossier qui en contient une)
	CarteProtegee,		// racine d'un volume, fichier de démarrage, dossier dans lui-même
	CarteNonVide,		// dossier non vide
	CarteCle,		// clé USB absente ou retirée
	CartePleine,		// écriture impossible (carte pleine ?)
	CarteTropGros,		// plus grand que la limite donnée
	CarteDossier,		// refusé pour un dossier (copie, déplacement entre volumes)
	CarteErreur		// autre refus de FatFs
};

// Texte d'un code, sans accent (journal)
const char *CarteTexte (TCarteCode Code);

// Entrée d'un dossier, ou d'un chemin (CarteInfo)
struct TCarteEntree
{
	char Nom[FF_MAX_LFN + 1];	// en page 850 (FatFs)
	FSIZE_t nTaille;
	WORD nDate, nHeure;		// FatFs (0 : sans date), heure locale
	bool bDossier;
	bool bMonte;			// disquette montée
};

// Entrées de pChemin (chemin de FatFs, CarteChemin), nMax au plus, triées :
// dossiers puis fichiers. À la racine de la carte (« SD:/ »), les clés USB
// présentes d'abord (« USB: », dossier). Faite entre deux trames, après la
// fin du travail du cœur 2.
FRESULT CarteListe (const char *pChemin, TCarteEntree *pEntrees, unsigned nMax,
		    unsigned *pnEntrees, bool *pbTronque);

// Un fichier ou un dossier (racine d'un volume comprise : dossier sans date)
TCarteCode CarteInfo (const char *pChemin, TCarteEntree *pInfo);

// Racine d'un volume, ou fichier de démarrage à la racine de la carte
// (kernel8-32.img, config.txt, cmdline.txt, bootcode.bin, start.elf,
// fixup.dat) : ni effacé, ni déplacé, ni remplacé par un déplacement ou une
// copie (CarteEcrire, envoi d'un fichier, reste permis : nouveau noyau).
bool CarteEstProtegee (const char *pChemin);

// Les opérations suivantes (WebDAV) notent leur résultat dans le journal,
// préfixe pService. Contrôles et opérations courtes entre deux trames.
//
// Fichier, ou dossier vide (un dossier non vide est refusé : le client
// efface d'abord son contenu, après sa propre confirmation).
TCarteCode CarteEffacer (const char *pChemin, const char *pService);

TCarteCode CarteCreerDossier (const char *pChemin, const char *pService);

// Déplacement ou renommage. Même volume : f_rename (fichier ou dossier).
// Entre la carte et une clé : fichier seulement, copié (CarteCopier) puis
// effacé. Destination présente : remplacée si bEcraser (*pbRemplace),
// sauf un dossier non vide. Refus : source ou destination protégée,
// disquette montée (pour un dossier : une disquette montée dedans).
TCarteCode CarteDeplacer (const char *pSource, const char *pDest, bool bEcraser,
			  bool *pbRemplace, unsigned nMax, const char *pService);

// Copie d'un fichier (lu en mémoire, nMax octets au plus, puis CarteEcrire)
TCarteCode CarteCopier (const char *pSource, const char *pDest, bool bEcraser,
			bool *pbRemplace, unsigned nMax, const char *pService);

// Remplace (ou crée) pChemin : écrit sous « x.nouveau » par le cœur 2, puis,
// entre deux trames, l'ancien devient « x.ancien », le nouveau prend son
// nom, l'ancien est effacé. Refusé pour une disquette montée (vérifié avant
// et après l'écriture de « x.nouveau » : LOCI a pu la monter entre-temps). Résultat dans le journal (préfixe pService) et,
// en clair, dans pMessage (peut être 0).
// Raison d'un refus dans *pCode (peut être 0).
bool CarteEcrire (const char *pChemin, const u8 *pDonnees, unsigned nTaille,
		  const char *pService, char *pMessage = 0, unsigned nMessage = 0,
		  TCarteCode *pCode = 0);

#endif
