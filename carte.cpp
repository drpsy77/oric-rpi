//
// carte.cpp — Accès à la carte SD depuis le réseau (TFTP, page web)
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
		    const char **ppRaison, const char **ppTexte)
{
	if (oric_machine_disk_mounted (pChemin))
	{
		*ppRaison = "disquette montee";
		*ppTexte = "disquette montée";
		return true;
	}
	unsigned nSerie, nBranchementActuel;
	if (bCle && (   cles_usb_etat (pChemin, &nSerie, &nBranchementActuel) != 1
		     || nBranchementActuel != nBranchement))
	{
		*ppRaison = CarteCleAbsente.pJournal;
		*ppTexte = CarteCleAbsente.pTexte;
		return true;
	}
	return false;
}

bool CarteEcrire (const char *pChemin, const u8 *pDonnees, unsigned nTaille,
		  const char *pService, char *pMessage, unsigned nMessage)
{
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
		Rendre ();
		return Echec (pService, pChemin, CarteCleAbsente.pJournal, CarteCleAbsente.pTexte,
			      pMessage, nMessage);
	}
	if (Refuse (pChemin, bCle, nBranchement, &pRaison, &pTexte))
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
		return t->Resultat == ResultatOuverture
		       ? Echec (pService, pChemin, "dossier absent ?", "dossier absent ?", pMessage, nMessage)
		       : Echec (pService, pChemin, "carte pleine ?", "carte pleine ?", pMessage, nMessage);
	}

	// La fin entre deux trames, sans rendre la main : LOCI a pu monter la
	// disquette, la clé a pu être retirée pendant l'écriture
	(*s_pAttenteTrame) ();
	if (Refuse (pChemin, bCle, nBranchement, &pRaison, &pTexte))
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
			return Echec (pService, pChemin, "fichier ouvert ?", "fichier ouvert ?", pMessage, nMessage);
		}
	}
	if (f_rename (New, pChemin) != FR_OK)
	{
		if (bExists)
		{
			f_rename (Old, pChemin);	// l'ancien reprend sa place
		}
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
