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
#include "carte.h"
#include "cles.h"
#include "mdns.h"
#include "journal.h"
#include "oric_machine.h"

#include <circle/string.h>
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

bool CarteLire (const char *pChemin, u8 **ppDonnees, unsigned *pTaille, unsigned nMax)
{
	*ppDonnees = 0;
	*pTaille = 0;

	if (oric_machine_disk_mounted (pChemin))
	{
		oric_machine_flush_disks ();	// pistes en attente sur la carte
	}

	FIL File;
	if (f_open (&File, pChemin, FA_READ | FA_OPEN_EXISTING) != FR_OK)
	{
		return false;
	}
	FSIZE_t nSize = f_size (&File);
	*pTaille = nSize;
	unsigned nRead = 0;
	bool bOK = nSize <= nMax;
	if (bOK && nSize > 0)
	{
		*ppDonnees = (u8 *) malloc (nSize);
		bOK =    *ppDonnees != 0
		      && f_read (&File, *ppDonnees, nSize, &nRead) == FR_OK
		      && nRead == nSize;
	}
	f_close (&File);

	if (!bOK)
	{
		free (*ppDonnees);
		*ppDonnees = 0;
	}
	return bOK;
}

bool CarteLireDans (const char *pChemin, u8 *pTampon, unsigned *pTaille)
{
	unsigned nMax = *pTaille;
	*pTaille = 0;

	if (oric_machine_disk_mounted (pChemin))
	{
		oric_machine_flush_disks ();
	}

	FIL File;
	if (f_open (&File, pChemin, FA_READ | FA_OPEN_EXISTING) != FR_OK)
	{
		return false;
	}
	FSIZE_t nSize = f_size (&File);
	*pTaille = nSize;
	unsigned nRead = 0;
	bool bOK =    nSize <= nMax
		   && (   nSize == 0
		       || (f_read (&File, pTampon, nSize, &nRead) == FR_OK && nRead == nSize));
	f_close (&File);
	return bOK;
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

bool CarteEcrire (const char *pChemin, const u8 *pDonnees, unsigned nTaille,
		  const char *pService, char *pMessage, unsigned nMessage)
{
	if (oric_machine_disk_mounted (pChemin))
	{
		return Echec (pService, pChemin, "disquette montee", "disquette montée", pMessage, nMessage);
	}
	unsigned nSerie, nBranchement;
	if (cles_usb_etat (pChemin, &nSerie, &nBranchement) == 0)	// retirée entre-temps
	{
		return Echec (pService, pChemin, CarteCleAbsente.pJournal, CarteCleAbsente.pTexte,
			      pMessage, nMessage);
	}

	char New[CARTE_MAX_CHEMIN + 10], Old[CARTE_MAX_CHEMIN + 10];
	snprintf (New, sizeof New, "%s.nouveau", pChemin);
	snprintf (Old, sizeof Old, "%s.ancien", pChemin);

	FIL File;
	unsigned nWritten = 0;
	if (f_open (&File, New, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		return Echec (pService, pChemin, "dossier absent ?", "dossier absent ?", pMessage, nMessage);
	}
	bool bOK =    (nTaille == 0 || f_write (&File, pDonnees, nTaille, &nWritten) == FR_OK)
		   && nWritten == nTaille;
	bOK = f_close (&File) == FR_OK && bOK;
	if (!bOK)
	{
		f_unlink (New);
		return Echec (pService, pChemin, "carte pleine ?", "carte pleine ?", pMessage, nMessage);
	}

	FILINFO Info;
	bool bExists = f_stat (pChemin, &Info) == FR_OK;
	if (bExists)
	{
		f_unlink (Old);				// reste d'une coupure
		if (f_rename (pChemin, Old) != FR_OK)
		{
			f_unlink (New);
			return Echec (pService, pChemin, "fichier ouvert ?", "fichier ouvert ?", pMessage, nMessage);
		}
	}
	if (f_rename (New, pChemin) != FR_OK)
	{
		if (bExists)
		{
			f_rename (Old, pChemin);	// l'ancien reprend sa place
		}
		return Echec (pService, pChemin, "renommage impossible", "renommage impossible", pMessage, nMessage);
	}
	if (bExists)
	{
		f_unlink (Old);
	}

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
