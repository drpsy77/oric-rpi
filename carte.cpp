//
// carte.cpp — Accès à la carte SD depuis le réseau (TFTP, page web, WebDAV)
//
// Écrit pour le port (repris de tftp.cpp, qui suivait Circle
// addon/tftpfileserver pour les appels de FatFs). Voir carte.h.
//
// Fichier reçu : écrit sous « nom.nouveau », puis l'ancien devient
// « nom.ancien », le nouveau prend son nom, l'ancien est effacé. Une coupure
// laisse au pire l'un des deux à côté, jamais un fichier à moitié écrit
// (kernel8-32.img compris).
//
// Noms : carte SD (« SD:/ ») ou clés USB de la Pi (« USB: », « USB2: »,
// cles.h) ; une clé absente est refusée avant FatFs (CarteChemin).
//
// Une disquette montée (Microdisc ou LOCI) ne peut pas être remplacée :
// l'Oric la garde en mémoire et la réécrirait par-dessus. Elle peut être
// lue : les pistes en attente sont d'abord réécrites (disquette.c).
//
// Lectures et écritures de fichiers entiers par le cœur 2 (voir plus bas) :
// l'Oric ne s'arrête plus le temps d'un transfert (~0,25 à 0,5 s par Mo
// écrit avant). Ne restent entre deux trames que les contrôles (disquette
// montée, clé) et les renommages.
//
#include "carte.h"
#include "cles.h"
#include "mdns.h"
#include "journal.h"
#include "oric_machine.h"

#include <circle/sched/scheduler.h>
#include <circle/string.h>
#include <circle/synchronize.h>
#include <fatfs/ff.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

CClientAutorise::CClientAutorise (CNetSubSystem *pNet, const char *pNom,
				  const char *pService)
:	m_pNet (pNet),
	m_pNom (pNom),
	m_pService (pService)
{
}

bool CClientAutorise::Accepte (const CIPAddress &rIP)
{
	// m_Adresse vide avant la première résolution (et comparer une adresse
	// vide est une assertion de Circle : panique)
	if (!m_Adresse.IsSet () || rIP != m_Adresse)
	{
		// Nom du client : adresse peut-être changée (ou pas encore connue)
		CIPAddress Adresse;
		if (   ResolveHostName (m_pNet, m_pNom, &Adresse)
		    && (!m_Adresse.IsSet () || Adresse != m_Adresse))
		{
			m_Adresse.Set (Adresse);
			CString Texte;
			m_Adresse.Format (&Texte);
			JournalPrintf ("%s : %s a l'adresse %s\n", m_pService, m_pNom,
				       (const char *) Texte);
		}
	}
	return m_Adresse.IsSet () && rIP == m_Adresse;
}

const TCarteErreur CarteNomInvalide = { "nom invalide", "nom invalide" };
const TCarteErreur CarteCleAbsente  = { "cle USB absente", "clé USB absente" };

static bool Refus (const TCarteErreur *pErreur, const TCarteErreur **ppErreur)
{
	if (ppErreur != 0)
	{
		*ppErreur = pErreur;
	}
	return false;
}

bool CarteChemin (const char *pNom, char *pChemin, unsigned nTaille, bool bDossier,
		  const TCarteErreur **ppErreur)
{
	while (*pNom == '/')
	{
		pNom++;
	}

	// Clé USB : « USB: » ou « USB2: » en tête
	const char *pVolume = "SD:";
	for (unsigned i = 0; i < CLES_MAX; i++)
	{
		const char *pCle = ClesUSBVolume (i);
		size_t n = strlen (pCle);
		if (strncasecmp (pNom, pCle, n) == 0)
		{
			unsigned nSerie, nBranchement;
			if (cles_usb_etat (pCle, &nSerie, &nBranchement) != 1)
			{
				return Refus (&CarteCleAbsente, ppErreur);
			}
			pVolume = pCle;
			pNom += n;
			while (*pNom == '/')
			{
				pNom++;
			}
			break;
		}
	}

	if (   (*pNom == '\0' && !bDossier)
	    || strchr (pNom, ':') != 0
	    || strstr (pNom, "..") != 0
	    || strchr (pNom, '\\') != 0)
	{
		return Refus (&CarteNomInvalide, ppErreur);
	}
	int n = snprintf (pChemin, nTaille, "%s/%s", pVolume, pNom);
	if (n < 0 || (unsigned) n >= nTaille)
	{
		return Refus (&CarteNomInvalide, ppErreur);
	}
	// dossier : sans « / » final (sauf la racine, « SD:/ »)
	int nRacine = strlen (pVolume) + 1;
	while (n > nRacine && pChemin[n - 1] == '/')
	{
		pChemin[--n] = '\0';
	}
	return true;
}

void CarteRelatif (const char *pChemin, char *pRelatif, unsigned nTaille)
{
	if (strncmp (pChemin, "SD:/", 4) == 0)
	{
		snprintf (pRelatif, nTaille, "%s", pChemin + 4);
		return;
	}
	snprintf (pRelatif, nTaille, "%s", pChemin);
	size_t n = strlen (pRelatif);
	if (n >= 2 && pRelatif[n - 1] == '/' && pRelatif[n - 2] == ':')
	{
		pRelatif[n - 1] = '\0';		// « USB:/ » → « USB: »
	}
}

// ── Cœur 2 : lecture et écriture des fichiers ───────────────────────────
//
// Une case de travail, un travail à la fois. La tâche du cœur 0 la prend
// (s_bOccupee : les tâches ne s'interrompent qu'en rendant la main), la
// remplit, passe nEtat à EtatDemande et réveille le cœur 2 (SEV), puis rend
// la main jusqu'à EtatFini : l'ordonnanceur, la cadence des trames et le
// son continuent. Une seule tâche écrit nEtat à la fois, le cœur 2 ne fait
// que passer de EtatDemande à EtatFini (comme les files de netrelay.cpp).
//
// Le cœur 2 n'utilise que FatFs (f_*), par morceaux de CARTE_MORCEAU : le
// verrou du volume (FF_FS_REENTRANT, ffsystem.cpp de Circle, verrou
// tournant entre les cœurs) est rendu entre deux, le cœur 1 (LOCI,
// disquettes) ou le cœur 0 (journal) n'attendent au pire qu'un morceau. Pas
// de stdio (ses structures, dans circle-newlib, n'ont pas de verrou), pas de
// journal ni d'ordonnanceur : le résultat revient au cœur 0. FatFs réserve un
// tampon pour chaque nom long (FF_USE_LFN 3) : malloc de Circle, verrou du
// tas.
//
// Tampon de l'appelant non aligné sur 4 octets (fichier d'un envoi de la
// page web, au milieu de la requête) : copié par morceaux dans s_pMorceau
// (sans lui, refusé).
// Sinon disk_read et disk_write (diskio.cpp de Circle) passeraient par leur
// propre tampon, statique, commun à tous les volumes et sans verrou, que le
// cœur 1 peut utiliser au même moment sur un autre volume. Les tampons de
// FatFs (FATFS.win, FIL.buf) sont alignés.

#define CARTE_MORCEAU	(64u << 10)	// un f_read ou f_write du cœur 2
#define LECTURE_ESSAIS	3		// disquette réécrite pendant la lecture

enum TEtat
{
	EtatLibre,
	EtatDemande,			// cœur 0 → cœur 2
	EtatFini			// cœur 2 → cœur 0
};

enum TOperation
{
	OpLire,
	OpEcrire,
	OpEffacer
};

enum TResultat
{
	ResultatOK,
	ResultatAbsent,			// lire : f_open refusé
	ResultatTropGros,		// lire : plus grand que le tampon
	ResultatIllisible,		// lire : f_read en erreur
	ResultatOuverture,		// écrire : f_open refusé (dossier absent ?)
	ResultatEcriture		// écrire : f_write ou f_close en erreur
};

struct TTravail
{
	volatile unsigned nEtat;	// TEtat
	TOperation Operation;
	const char *pChemin;		// lire, effacer : le fichier ; écrire : « x.nouveau »
	u8 *pTampon;			// lire : 0 pour la taille seulement
	unsigned nTaille;		// lire : taille du tampon ; écrire : des données
	TResultat Resultat;
	unsigned nFichier;		// lire : taille du fichier (0 si absent)
};

static TTravail s_Travail;
static bool s_bOccupee;				// case prise par une tâche du cœur 0
static void (*s_pAttenteTrame) (void);
// Réservé au démarrage, pas en mémoire statique : le noyau (code, données
// et BSS) doit tenir dans KERNEL_MAX_SIZE (Makefile, limite.ld)
static u8 *s_pMorceau;

void CarteInitialiser (void (*pAttenteTrame) (void))
{
	s_pAttenteTrame = pAttenteTrame;
	s_pMorceau = (u8 *) malloc (CARTE_MORCEAU);	// aligné (tas de Circle)
}

static bool Aligne (const void *p)
{
	return ((uintptr) p & 3) == 0;
}

static void Lire (TTravail *t)
{
	t->nFichier = 0;
	FIL File;
	if (f_open (&File, t->pChemin, FA_READ | FA_OPEN_EXISTING) != FR_OK)
	{
		t->Resultat = ResultatAbsent;
		return;
	}
	FSIZE_t nSize = f_size (&File);
	t->nFichier = nSize < 0xFFFFFFFFu ? (unsigned) nSize : 0xFFFFFFFFu;
	if (t->pTampon == 0)
	{
		t->Resultat = ResultatOK;		// taille seulement
	}
	else if (nSize > t->nTaille)
	{
		t->Resultat = ResultatTropGros;
	}
	else if (!Aligne (t->pTampon) && s_pMorceau == 0)
	{
		t->Resultat = ResultatIllisible;
	}
	else
	{
		t->Resultat = ResultatOK;
		bool bAligne = Aligne (t->pTampon);
		for (unsigned nPos = 0; nPos < nSize; )
		{
			unsigned n = nSize - nPos;
			if (n > CARTE_MORCEAU)
			{
				n = CARTE_MORCEAU;
			}
			u8 *p = bAligne ? t->pTampon + nPos : s_pMorceau;
			UINT nLu = 0;
			if (f_read (&File, p, n, &nLu) != FR_OK || nLu != n)
			{
				t->Resultat = ResultatIllisible;
				break;
			}
			if (!bAligne)
			{
				memcpy (t->pTampon + nPos, s_pMorceau, n);
			}
			nPos += n;
		}
	}
	f_close (&File);
}

static void Ecrire (TTravail *t)
{
	if (!Aligne (t->pTampon) && s_pMorceau == 0)
	{
		t->Resultat = ResultatEcriture;
		return;
	}
	FIL File;
	if (f_open (&File, t->pChemin, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		t->Resultat = ResultatOuverture;
		return;
	}
	bool bOK = true;
	bool bAligne = Aligne (t->pTampon);
	for (unsigned nPos = 0; nPos < t->nTaille; )
	{
		unsigned n = t->nTaille - nPos;
		if (n > CARTE_MORCEAU)
		{
			n = CARTE_MORCEAU;
		}
		const u8 *p = t->pTampon + nPos;
		if (!bAligne)
		{
			memcpy (s_pMorceau, p, n);
			p = s_pMorceau;
		}
		UINT nEcrit = 0;
		if (f_write (&File, p, n, &nEcrit) != FR_OK || nEcrit != n)
		{
			bOK = false;
			break;
		}
		nPos += n;
	}
	bOK = f_close (&File) == FR_OK && bOK;
	if (!bOK)
	{
		f_unlink (t->pChemin);
	}
	t->Resultat = bOK ? ResultatOK : ResultatEcriture;
}

static void Executer (TTravail *t)
{
	switch (t->Operation)
	{
	case OpLire:
		Lire (t);
		break;

	case OpEcrire:
		Ecrire (t);
		break;

	case OpEffacer:
		t->Resultat = f_unlink (t->pChemin) == FR_OK ? ResultatOK : ResultatEcriture;
		break;
	}
}

void CarteCoeur2 (void)
{
	while (true)
	{
		// WFE : réveillé par le SEV du cœur 0, ou par un autre (libération
		// d'un verrou tournant de Circle) : la case est relue à chaque fois
		while (s_Travail.nEtat != EtatDemande)
		{
			WaitForEvent ();
		}
		DataMemBarrier ();
		Executer (&s_Travail);
		DataMemBarrier ();
		s_Travail.nEtat = EtatFini;
	}
}

bool CarteCoeur2SurCle (void)
{
	return    s_Travail.nEtat == EtatDemande
	       && strncmp (s_Travail.pChemin, "SD:", 3) != 0;
}

void CarteCoeur2Attendre (void)
{
	while (s_Travail.nEtat == EtatDemande)
	{
		// fin du morceau en cours, puis du fichier
	}
	DataMemBarrier ();
}

// Tâche du cœur 0 : prend la case (attend en rendant la main qu'une autre
// tâche l'ait rendue)
static void Prendre (void)
{
	while (s_bOccupee)
	{
		CScheduler::Get ()->Yield ();
	}
	s_bOccupee = true;
}

static void Rendre (void)
{
	s_bOccupee = false;
}

void CarteReserver (void)
{
	Prendre ();
}

void CarteLiberer (void)
{
	Rendre ();
}

// Travail de la case exécuté par le cœur 2 (la tâche rend la main en
// attendant), ou sur place si !bCoeur2 (entre deux trames, sans rendre la
// main)
static void Faire (bool bCoeur2)
{
	if (!bCoeur2)
	{
		Executer (&s_Travail);
		return;
	}
	DataMemBarrier ();
	s_Travail.nEtat = EtatDemande;
	DataSyncBarrier ();
	SendEvent ();
	while (s_Travail.nEtat != EtatFini)
	{
		CScheduler::Get ()->Yield ();
	}
	DataMemBarrier ();
	s_Travail.nEtat = EtatLibre;
}

// Lecture d'un fichier entier par le cœur 2 : dans un tampon réservé ici
// (ppAlloue, CarteLire), sinon dans pTampon. nMax : taille la plus grande.
// Disquette montée : pistes en attente réécrites d'abord (entre deux
// trames) ; si le cœur 1 la réécrit pendant la lecture (compteur de
// disquette.c), elle est relue, et au dernier essai lue entre deux trames
// par le cœur 0, comme avant le cœur 2.
static bool Lecture (const char *pChemin, u8 **ppAlloue, u8 *pTampon, unsigned nMax,
		     unsigned *pTaille)
{
	*pTaille = 0;
	if (ppAlloue != 0)
	{
		*ppAlloue = 0;
	}

	Prendre ();
	(*s_pAttenteTrame) ();
	bool bMontee = oric_machine_disk_mounted (pChemin);
	if (bMontee)
	{
		oric_machine_flush_disks ();		// pistes en attente sur la carte
	}

	TTravail *t = &s_Travail;
	bool bOK = false;
	for (unsigned nEssai = 0; nEssai <= LECTURE_ESSAIS; nEssai++)
	{
		bool bCoeur2 = nEssai < LECTURE_ESSAIS;
		if (!bCoeur2)
		{
			(*s_pAttenteTrame) ();
		}
		unsigned nEcritures = oric_machine_disk_writes ();

		t->Operation = OpLire;
		t->pChemin = pChemin;
		t->pTampon = pTampon;
		t->nTaille = nMax;
		if (ppAlloue != 0)
		{
			// taille d'abord, pour réserver le tampon (sur le cœur 0)
			t->pTampon = 0;
			Faire (bCoeur2);
			*pTaille = t->nFichier;
			if (t->Resultat != ResultatOK || t->nFichier > nMax)
			{
				break;
			}
			free (*ppAlloue);
			*ppAlloue = 0;
			if (t->nFichier == 0)
			{
				bOK = true;			// fichier vide
				break;
			}
			*ppAlloue = (u8 *) malloc (t->nFichier);
			if (*ppAlloue == 0)
			{
				break;
			}
			t->pTampon = *ppAlloue;
			t->nTaille = t->nFichier;
		}
		Faire (bCoeur2);
		*pTaille = t->nFichier;
		bOK = t->Resultat == ResultatOK;
		if (!bOK || !bMontee || !bCoeur2)
		{
			break;
		}
		if ((nEcritures & 1) == 0 && oric_machine_disk_writes () == nEcritures)
		{
			break;				// aucune réécriture croisée
		}
		bOK = false;				// réécrite pendant la lecture : encore
	}
	Rendre ();

	if (!bOK && ppAlloue != 0)
	{
		free (*ppAlloue);
		*ppAlloue = 0;
	}
	return bOK;
}

bool CarteLire (const char *pChemin, u8 **ppDonnees, unsigned *pTaille, unsigned nMax)
{
	return Lecture (pChemin, ppDonnees, 0, nMax, pTaille);
}

bool CarteLireDans (const char *pChemin, u8 *pTampon, unsigned *pTaille)
{
	return Lecture (pChemin, 0, pTampon, *pTaille, pTaille);
}

// pRaison : sans accent (journal) ; pTexte : la même, pour la page web
static bool Echec (const char *pService, const char *pChemin, const char *pRaison,
		   const char *pTexte, char *pMessage, unsigned nMessage)
{
	JournalPrintf ("%s : %s non ecrit (%s)\n", pService, pChemin, pRaison);
	if (pMessage != 0)
	{
		snprintf (pMessage, nMessage, "non écrit (%s)", pTexte);
	}
	return false;
}

// Disquette montée ou clé retirée (ou remise : nBranchement a changé) ?
// Entre deux trames.
static bool Refuse (const char *pChemin, bool bCle, unsigned nBranchement,
		    const char **ppRaison, const char **ppTexte, TCarteCode *pCode)
{
	if (oric_machine_disk_mounted (pChemin))
	{
		*ppRaison = "disquette montee";
		*ppTexte = "disquette montée";
		*pCode = CarteMontee;
		return true;
	}
	unsigned nSerie, nBranchementActuel;
	if (bCle && (   cles_usb_etat (pChemin, &nSerie, &nBranchementActuel) != 1
		     || nBranchementActuel != nBranchement))
	{
		*ppRaison = CarteCleAbsente.pJournal;
		*ppTexte = CarteCleAbsente.pTexte;
		*pCode = CarteCle;
		return true;
	}
	return false;
}

bool CarteEcrire (const char *pChemin, const u8 *pDonnees, unsigned nTaille,
		  const char *pService, char *pMessage, unsigned nMessage,
		  TCarteCode *pCode)
{
	TCarteCode Code;
	if (pCode == 0)
	{
		pCode = &Code;
	}
	*pCode = CarteOK;

	char New[CARTE_MAX_CHEMIN + 10], Old[CARTE_MAX_CHEMIN + 10];
	snprintf (New, sizeof New, "%s.nouveau", pChemin);
	snprintf (Old, sizeof Old, "%s.ancien", pChemin);

	Prendre ();

	// Refus tout de suite, sans écrire (entre deux trames)
	(*s_pAttenteTrame) ();
	unsigned nSerie, nBranchement = 0;
	int nCle = cles_usb_etat (pChemin, &nSerie, &nBranchement);
	bool bCle = nCle >= 0;
	const char *pRaison, *pTexte;
	if (nCle == 0)		// retirée entre-temps
	{
		*pCode = CarteCle;
		Rendre ();
		return Echec (pService, pChemin, CarteCleAbsente.pJournal, CarteCleAbsente.pTexte,
			      pMessage, nMessage);
	}
	if (Refuse (pChemin, bCle, nBranchement, &pRaison, &pTexte, pCode))
	{
		Rendre ();
		return Echec (pService, pChemin, pRaison, pTexte, pMessage, nMessage);
	}

	// « x.nouveau » par le cœur 2, l'émulation continue
	TTravail *t = &s_Travail;
	t->Operation = OpEcrire;
	t->pChemin = New;
	t->pTampon = (u8 *) pDonnees;
	t->nTaille = nTaille;
	Faire (true);
	if (t->Resultat != ResultatOK)
	{
		Rendre ();
		*pCode = t->Resultat == ResultatOuverture ? CarteParentAbsent : CartePleine;
		return t->Resultat == ResultatOuverture
		       ? Echec (pService, pChemin, "dossier absent ?", "dossier absent ?", pMessage, nMessage)
		       : Echec (pService, pChemin, "carte pleine ?", "carte pleine ?", pMessage, nMessage);
	}

	// La fin entre deux trames, sans rendre la main : LOCI a pu monter la
	// disquette, la clé a pu être retirée pendant l'écriture
	(*s_pAttenteTrame) ();
	if (Refuse (pChemin, bCle, nBranchement, &pRaison, &pTexte, pCode))
	{
		f_unlink (New);
		Rendre ();
		return Echec (pService, pChemin, pRaison, pTexte, pMessage, nMessage);
	}

	FILINFO Info;
	bool bExists = f_stat (pChemin, &Info) == FR_OK;
	if (bExists)
	{
		f_unlink (Old);				// reste d'une coupure
		if (f_rename (pChemin, Old) != FR_OK)
		{
			f_unlink (New);
			Rendre ();
			*pCode = CarteErreur;
			return Echec (pService, pChemin, "fichier ouvert ?", "fichier ouvert ?", pMessage, nMessage);
		}
	}
	if (f_rename (New, pChemin) != FR_OK)
	{
		if (bExists)
		{
			f_rename (Old, pChemin);	// l'ancien reprend sa place
		}
		*pCode = CarteErreur;
		Rendre ();
		return Echec (pService, pChemin, "renommage impossible", "renommage impossible", pMessage, nMessage);
	}
	if (bExists)
	{
		// L'ancien effacé par le cœur 2 (FAT réécrite : ~1/3 de la fin
		// d'écriture), l'émulation reprise ; LOCI ne monte pas « x.ancien »
		t->Operation = OpEffacer;
		t->pChemin = Old;
		Faire (true);
	}
	Rendre ();

	bool bNoyau = strcmp (pChemin, "SD:/kernel8-32.img") == 0;
	JournalPrintf ("%s : %s ecrit (%u octets)%s\n", pService, pChemin, nTaille,
		       bNoyau ? ", pris en compte au prochain demarrage" : "");
	if (pMessage != 0)
	{
		snprintf (pMessage, nMessage, "écrit (%u octets)%s", nTaille,
			  bNoyau ? ", pris en compte au prochain démarrage" : "");
	}
	return true;
}

// ── Opérations du serveur WebDAV (dav.cpp) ───────────────────────────────
//
// Contrôles et opérations courtes (f_stat, f_unlink, f_mkdir, f_rename)
// entre deux trames, sans rendre la main, après la fin du travail du cœur 2
// (Prendre) : comme les listes de la page web. Les fichiers copiés sont
// lus et écrits par le cœur 2 (CarteLire, CarteEcrire).
//
// FatFs sans verrou des fichiers ouverts (FF_FS_LOCK 0) : un fichier que
// l'Oric garde ouvert (API de fichiers de LOCI) n'est pas protégé, comme
// pour TFTP et la page web. Les disquettes montées le sont (elles sont en
// mémoire et seraient réécrites par-dessus).

const char *CarteTexte (TCarteCode Code)
{
	switch (Code)
	{
	case CarteOK:		return "fait";
	case CarteAbsent:	return "introuvable";
	case CarteParentAbsent:	return "dossier de destination absent";
	case CarteExiste:	return "destination presente";
	case CarteMontee:	return "disquette montee";
	case CarteProtegee:	return "protege";
	case CarteNonVide:	return "dossier non vide";
	case CarteCle:		return CarteCleAbsente.pJournal;
	case CartePleine:	return "carte pleine ?";
	case CarteTropGros:	return "trop gros";
	case CarteDossier:	return "dossier refuse";
	case CarteErreur:	break;
	}
	return "refuse par FatFs";
}

static void Trame (void)
{
	(*s_pAttenteTrame) ();
}

static int CompareEntrees (const void *p1, const void *p2)
{
	const TCarteEntree *e1 = (const TCarteEntree *) p1, *e2 = (const TCarteEntree *) p2;
	if (e1->bDossier != e2->bDossier)
	{
		return e1->bDossier ? -1 : 1;
	}
	return strcasecmp (e1->Nom, e2->Nom);
}

// Repris de web.cpp (LireDossier)
FRESULT CarteListe (const char *pChemin, TCarteEntree *pEntrees, unsigned nMax,
		    unsigned *pnEntrees, bool *pbTronque)
{
	unsigned nEntrees = 0;
	*pbTronque = false;

	Prendre ();
	Trame ();

	unsigned nCles = 0;
	if (strcmp (pChemin, "SD:/") == 0)
	{
		for (unsigned i = 0; i < CLES_MAX && nCles < nMax; i++)
		{
			unsigned nSerie, nBranchement;
			if (cles_usb_etat (ClesUSBVolume (i), &nSerie, &nBranchement) == 1)
			{
				TCarteEntree *e = &pEntrees[nCles++];
				memset (e, 0, sizeof *e);
				snprintf (e->Nom, sizeof e->Nom, "%s", ClesUSBVolume (i));
				e->bDossier = true;
			}
		}
		nEntrees = nCles;
	}

	DIR Dir;
	FRESULT Res = f_opendir (&Dir, pChemin);
	if (Res == FR_OK)
	{
		bool bFinVolume = pChemin[strlen (pChemin) - 1] == '/';	// « SD:/ »
		FILINFO Info;
		while (f_readdir (&Dir, &Info) == FR_OK && Info.fname[0] != '\0')
		{
			if (nEntrees == nMax)
			{
				*pbTronque = true;
				break;
			}
			TCarteEntree *e = &pEntrees[nEntrees++];
			memcpy (e->Nom, Info.fname, sizeof e->Nom);	// même taille
			e->Nom[sizeof e->Nom - 1] = '\0';
			e->nTaille = Info.fsize;
			e->nDate = Info.fdate;
			e->nHeure = Info.ftime;
			e->bDossier = (Info.fattrib & AM_DIR) != 0;
			e->bMonte = false;
			if (!e->bDossier)
			{
				char Complet[CARTE_MAX_CHEMIN + FF_MAX_LFN + 2];
				snprintf (Complet, sizeof Complet, "%s%s%s", pChemin,
					  bFinVolume ? "" : "/", e->Nom);
				e->bMonte = oric_machine_disk_mounted (Complet);
			}
		}
		f_closedir (&Dir);
	}
	Rendre ();

	qsort (pEntrees + nCles, nEntrees - nCles, sizeof (TCarteEntree), CompareEntrees);
	*pnEntrees = nEntrees;
	return Res;
}

static bool EstRacine (const char *pChemin)
{
	size_t n = strlen (pChemin);
	return n > 0 && pChemin[n - 1] == '/';		// « SD:/ », « USB:/ »
}

bool CarteEstProtegee (const char *pChemin)
{
	static const char *const Demarrage[] =
	{
		"kernel8-32.img", "config.txt", "cmdline.txt",
		"bootcode.bin", "start.elf", "fixup.dat", 0
	};
	if (EstRacine (pChemin))
	{
		return true;
	}
	if (strncasecmp (pChemin, "SD:/", 4) == 0 && strchr (pChemin + 4, '/') == 0)
	{
		for (unsigned i = 0; Demarrage[i] != 0; i++)
		{
			if (strcasecmp (pChemin + 4, Demarrage[i]) == 0)
			{
				return true;
			}
		}
	}
	return false;
}

// Entre deux trames : état d'un chemin
static TCarteCode Stat (const char *pChemin, TCarteEntree *e)
{
	memset (e, 0, sizeof *e);
	unsigned nSerie, nBranchement;
	if (cles_usb_etat (pChemin, &nSerie, &nBranchement) == 0)
	{
		return CarteCle;
	}
	const char *pNom = strrchr (pChemin, '/');
	snprintf (e->Nom, sizeof e->Nom, "%s", pNom != 0 ? pNom + 1 : pChemin);
	if (EstRacine (pChemin))
	{
		e->bDossier = true;			// f_stat refuse la racine
		return CarteOK;
	}
	FILINFO Info;
	FRESULT Res = f_stat (pChemin, &Info);
	if (Res == FR_NO_FILE || Res == FR_NO_PATH)
	{
		return CarteAbsent;
	}
	if (Res != FR_OK)
	{
		return CarteErreur;
	}
	e->nTaille = Info.fsize;
	e->nDate = Info.fdate;
	e->nHeure = Info.ftime;
	e->bDossier = (Info.fattrib & AM_DIR) != 0;
	e->bMonte = !e->bDossier && oric_machine_disk_mounted (pChemin);
	return CarteOK;
}

static bool DossierVide (const char *pChemin)
{
	DIR Dir;
	if (f_opendir (&Dir, pChemin) != FR_OK)
	{
		return false;
	}
	FILINFO Info;
	bool bVide = f_readdir (&Dir, &Info) == FR_OK && Info.fname[0] == '\0';
	f_closedir (&Dir);
	return bVide;
}

static TCarteCode Note (TCarteCode Code, const char *pService, const char *pAction,
			const char *pChemin, const char *pDest = 0)
{
	JournalPrintf ("%s : %s %s%s%s : %s\n", pService, pAction, pChemin,
		       pDest != 0 ? " -> " : "", pDest != 0 ? pDest : "", CarteTexte (Code));
	return Code;
}

TCarteCode CarteInfo (const char *pChemin, TCarteEntree *pInfo)
{
	Prendre ();
	Trame ();
	TCarteCode Code = Stat (pChemin, pInfo);
	Rendre ();
	return Code;
}

TCarteCode CarteEffacer (const char *pChemin, const char *pService)
{
	if (CarteEstProtegee (pChemin))
	{
		return Note (CarteProtegee, pService, "effacer", pChemin);
	}
	Prendre ();
	Trame ();
	TCarteEntree e;
	TCarteCode Code = Stat (pChemin, &e);
	if (Code == CarteOK)
	{
		if (e.bMonte)
		{
			Code = CarteMontee;
		}
		else if (e.bDossier && !DossierVide (pChemin))
		{
			Code = CarteNonVide;
		}
		else if (f_unlink (pChemin) != FR_OK)
		{
			Code = CarteErreur;
		}
	}
	Rendre ();
	return Note (Code, pService, "effacer", pChemin);
}

TCarteCode CarteCreerDossier (const char *pChemin, const char *pService)
{
	if (EstRacine (pChemin))
	{
		return Note (CarteExiste, pService, "creer le dossier", pChemin);
	}
	Prendre ();
	Trame ();
	TCarteCode Code = CarteOK;
	unsigned nSerie, nBranchement;
	if (cles_usb_etat (pChemin, &nSerie, &nBranchement) == 0)
	{
		Code = CarteCle;
	}
	else
	{
		switch (f_mkdir (pChemin))
		{
		case FR_OK:		break;
		case FR_EXIST:		Code = CarteExiste;		break;
		case FR_NO_PATH:	Code = CarteParentAbsent;	break;
		case FR_DENIED:		Code = CartePleine;		break;
		default:		Code = CarteErreur;		break;
		}
	}
	Rendre ();
	return Note (Code, pService, "creer le dossier", pChemin);
}

static bool MemeVolume (const char *p1, const char *p2)
{
	const char *c1 = strchr (p1, ':'), *c2 = strchr (p2, ':');
	return    c1 != 0 && c2 != 0 && c1 - p1 == c2 - p2
	       && strncasecmp (p1, p2, c1 - p1) == 0;
}

// pDest est pSource, ou dans pSource (dossier déplacé dans lui-même) ?
static bool Dans (const char *pDest, const char *pSource)
{
	size_t n = strlen (pSource);
	return    strncasecmp (pDest, pSource, n) == 0
	       && (pDest[n] == '\0' || pDest[n] == '/');
}

// Contrôles communs aux déplacements et copies, entre deux trames :
// destination présente (*pbRemplace), à remplacer si bEcraser
static TCarteCode ControleDest (const char *pDest, bool bEcraser, bool bSourceDossier,
				bool *pbRemplace, TCarteEntree *pDestInfo)
{
	TCarteCode Code = Stat (pDest, pDestInfo);
	if (Code == CarteAbsent)
	{
		return CarteOK;
	}
	if (Code != CarteOK)
	{
		return Code;
	}
	*pbRemplace = true;
	if (!bEcraser)
	{
		return CarteExiste;
	}
	if (pDestInfo->bMonte)
	{
		return CarteMontee;
	}
	if (pDestInfo->bDossier)
	{
		// un dossier n'est remplacé que vide, et par un dossier
		if (!bSourceDossier)
		{
			return CarteDossier;
		}
		return DossierVide (pDest) ? CarteOK : CarteNonVide;
	}
	return bSourceDossier ? CarteDossier : CarteOK;
}

TCarteCode CarteCopier (const char *pSource, const char *pDest, bool bEcraser,
			bool *pbRemplace, unsigned nMax, const char *pService)
{
	*pbRemplace = false;
	if (CarteEstProtegee (pDest) || Dans (pDest, pSource))
	{
		return Note (CarteProtegee, pService, "copier", pSource, pDest);
	}

	Prendre ();
	Trame ();
	TCarteEntree Source, Dest;
	TCarteCode Code = Stat (pSource, &Source);
	if (Code == CarteOK && Source.bDossier)
	{
		Code = CarteDossier;		// le client copie fichier par fichier
	}
	if (Code == CarteOK)
	{
		Code = ControleDest (pDest, bEcraser, false, pbRemplace, &Dest);
	}
	Rendre ();
	if (Code != CarteOK)
	{
		return Note (Code, pService, "copier", pSource, pDest);
	}

	u8 *pDonnees;
	unsigned nTaille;
	if (!CarteLire (pSource, &pDonnees, &nTaille, nMax))
	{
		return Note (nTaille > nMax ? CarteTropGros : CarteErreur, pService, "copier",
			     pSource, pDest);
	}
	CarteEcrire (pDest, pDonnees, nTaille, pService, 0, 0, &Code);	// journal
	free (pDonnees);
	return Code;
}

TCarteCode CarteDeplacer (const char *pSource, const char *pDest, bool bEcraser,
			  bool *pbRemplace, unsigned nMax, const char *pService)
{
	*pbRemplace = false;
	// Même nom à la casse près : renommage du même fichier (FAT ne
	// distingue pas la casse), pas un déplacement dans lui-même
	bool bCasse = strcasecmp (pSource, pDest) == 0 && strcmp (pSource, pDest) != 0;
	if (   CarteEstProtegee (pSource) || CarteEstProtegee (pDest)
	    || (!bCasse && Dans (pDest, pSource)))
	{
		return Note (CarteProtegee, pService, "deplacer", pSource, pDest);
	}

	Prendre ();
	Trame ();
	TCarteEntree Source, Dest;
	TCarteCode Code = Stat (pSource, &Source);
	if (Code == CarteOK)
	{
		if (   Source.bMonte
		    || (Source.bDossier && oric_machine_disk_mounted_under (pSource)))
		{
			Code = CarteMontee;
		}
		else if (!MemeVolume (pSource, pDest) && Source.bDossier)
		{
			Code = CarteDossier;	// entre la carte et une clé : fichiers seulement
		}
	}
	if (Code != CarteOK || !MemeVolume (pSource, pDest))
	{
		Rendre ();
		if (Code != CarteOK)
		{
			return Note (Code, pService, "deplacer", pSource, pDest);
		}
		// Entre deux volumes : copie (journal de CarteEcrire), puis effacement
		Code = CarteCopier (pSource, pDest, bEcraser, pbRemplace, nMax, pService);
		return Code == CarteOK ? CarteEffacer (pSource, pService) : Code;
	}

	// Même volume : renommage. Fichier remplacé : mis de côté d'abord, remis
	// en place si le renommage échoue.
	Code = bCasse ? CarteOK : ControleDest (pDest, bEcraser, Source.bDossier, pbRemplace, &Dest);
	char Old[CARTE_MAX_CHEMIN + 10];
	snprintf (Old, sizeof Old, "%s.ancien", pDest);
	bool bMisDeCote = false;
	if (Code == CarteOK && *pbRemplace)
	{
		if (Dest.bDossier)
		{
			Code = f_unlink (pDest) == FR_OK ? CarteOK : CarteErreur;	// vide
		}
		else
		{
			f_unlink (Old);			// reste d'une coupure
			bMisDeCote = f_rename (pDest, Old) == FR_OK;
			Code = bMisDeCote ? CarteOK : CarteErreur;
		}
	}
	if (Code == CarteOK)
	{
		switch (f_rename (pSource, pDest))
		{
		case FR_OK:		break;
		case FR_NO_PATH:	Code = CarteParentAbsent;	break;
		case FR_EXIST:		Code = CarteExiste;		break;
		default:		Code = CarteErreur;		break;
		}
	}
	if (bMisDeCote)
	{
		if (Code == CarteOK)
		{
			f_unlink (Old);
		}
		else
		{
			f_rename (Old, pDest);
		}
	}
	Rendre ();
	return Note (Code, pService, "deplacer", pSource, pDest);
}
