//
// cles.cpp — Clés USB branchées sur la Pi, servies par LOCI
//
// Écrit pour le port. Voir cles.h.
//
// Circle crée le périphérique « umsdN » quand une clé est branchée et
// l'enlève quand elle est retirée (UpdatePlugAndPlay, cœur 0) ; le module
// diskio de FatFs (Circle addon/fatfs) met alors son volume « pas prêt »
// (RES_NOTRDY) : une lecture échoue sans planter. Ici, chaque changement
// est vu en cherchant à nouveau umsd1 et umsd2 : volume monté au
// branchement, démonté au retrait. Les fichiers que LOCI avait ouverts sur
// la clé deviennent invalides (FatFs : FR_INVALID_OBJECT), comme sur le
// vrai LOCI ; ceux ouverts après son retour marchent.
//
// FAT12/16/32 et exFAT (FF_FS_EXFAT, réglé par preparer_circle.sh). Une
// clé est reconnue par le numéro de série de son volume : une disquette
// montée depuis une clé n'est réécrite que sur la même clé (disquette.c,
// cles_usb_etat).
//
// statvfs (taille d'un volume, affichée par le menu de LOCI) : celui de
// circle-newlib rend toujours -1 ; __wrap_statvfs (--wrap, Makefile) donne
// la taille du volume FatFs (carte SD ou clé).
//
#include "cles.h"
#include "journal.h"
#include "oric_machine.h"

#include <circle/devicenameservice.h>
#include <fatfs/ff.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>

#define LIGNES_RACINE	20		// noms de la racine notés dans le journal

struct TCle
{
	const char *pDevice;		// nom de Circle
	const char *pVolume;		// volume de FatFs
	bool bPresente;
	bool bMontee;
	FATFS FileSystem;
	unsigned nSerie;		// numéro de série du volume
	unsigned nBranchement;		// change à chaque branchement
	char Texte[48];			// pour la liste des périphériques de LOCI
};

#define TEXTE_ABSENTE	"MSC no USB key"

static TCle s_Cles[CLES_MAX] =
{
	{ "umsd1", "USB:",  false, false, {}, 0, 0, TEXTE_ABSENTE },
	{ "umsd2", "USB2:", false, false, {}, 0, 0, TEXTE_ABSENTE },
};

// Nom et numéro de série du volume (FF_USE_LABEL, preparer_circle.sh)
static void Description (TCle *pCle, char *pNom, unsigned nTaille)
{
	char Nom[24];			// exFAT : 11 caractères, jusqu'à 2 octets chacun
	DWORD nSerie = 0;
	if (f_getlabel (pCle->pVolume, Nom, &nSerie) != FR_OK)
	{
		Nom[0] = '\0';
	}
	pCle->nSerie = nSerie;
	for (char *p = Nom; *p != '\0'; p++)
	{
		if (*p < ' ' || *p > '~')
		{
			*p = '?';
		}
	}
	snprintf (pNom, nTaille, "%s", Nom[0] != '\0' ? Nom : "sans nom");
}

static const char *Format (BYTE nType)
{
	switch (nType)
	{
	case FS_FAT12:	return "FAT12";
	case FS_FAT16:	return "FAT16";
	case FS_FAT32:	return "FAT32";
	case FS_EXFAT:	return "exFAT";
	default:	return "?";
	}
}

static void Branchee (unsigned i)
{
	TCle *pCle = &s_Cles[i];
	pCle->bPresente = true;
	pCle->nBranchement++;

	FRESULT Res = f_mount (&pCle->FileSystem, pCle->pVolume, 1);
	if (Res != FR_OK)
	{
		JournalPrintf ("Cle USB %u (%s) : branchee, illisible (%s)\n", i + 1, pCle->pDevice,
			       Res == FR_NO_FILESYSTEM ? "ni FAT ni exFAT : NTFS ?"
						       : "erreur de lecture");
		f_mount (0, pCle->pVolume, 0);
		snprintf (pCle->Texte, sizeof pCle->Texte, "MSC unreadable (%s)",
			  Res == FR_NO_FILESYSTEM ? "NTFS?" : "error");
		return;
	}
	pCle->bMontee = true;

	FATFS *fs = &pCle->FileSystem;
	unsigned long long nOctets =
		(unsigned long long) (fs->n_fatent - 2) * fs->csize * FF_MIN_SS;
	char Nom[24];
	Description (pCle, Nom, sizeof Nom);
	JournalPrintf ("Cle USB %u (%s) : branchee, %s, %s, %u.%u Go, serie %04X-%04X\n",
		       i + 1, pCle->pVolume, Nom, Format (fs->fs_type),
		       (unsigned) (nOctets >> 30), (unsigned) ((nOctets >> 20) % 1024 * 10 / 1024),
		       pCle->nSerie >> 16, pCle->nSerie & 0xFFFF);

	// Comme Phosphoric (loci_attach_usb_dir) : « MSC 14.6 GB ORIC »
	if (nOctets >= (1ULL << 30))
	{
		snprintf (pCle->Texte, sizeof pCle->Texte, "MSC %u.%u GB %s",
			  (unsigned) (nOctets >> 30),
			  (unsigned) ((nOctets >> 20) % 1024 * 10 / 1024), Nom);
	}
	else
	{
		snprintf (pCle->Texte, sizeof pCle->Texte, "MSC %u MB %s",
			  (unsigned) (nOctets >> 20), Nom);
	}

	// Racine, pour vérifier la lecture
	char Racine[8];
	snprintf (Racine, sizeof Racine, "%s/", pCle->pVolume);
	DIR Dir;
	if (f_opendir (&Dir, Racine) != FR_OK)
	{
		JournalPrintf ("  racine illisible\n");
		return;
	}
	FILINFO Info;
	unsigned n = 0;
	while (f_readdir (&Dir, &Info) == FR_OK && Info.fname[0] != '\0')
	{
		if (n++ < LIGNES_RACINE)
		{
			if (Info.fattrib & AM_DIR)
			{
				JournalPrintf ("  %s/\n", Info.fname);
			}
			else
			{
				JournalPrintf ("  %s (%llu octets)\n", Info.fname,
					       (unsigned long long) Info.fsize);
			}
		}
	}
	f_closedir (&Dir);
	if (n > LIGNES_RACINE)
	{
		JournalPrintf ("  ... %u entrees en tout\n", n);
	}
}

static void Retiree (unsigned i)
{
	TCle *pCle = &s_Cles[i];
	pCle->bPresente = false;
	snprintf (pCle->Texte, sizeof pCle->Texte, TEXTE_ABSENTE);
	if (pCle->bMontee)
	{
		f_mount (0, pCle->pVolume, 0);		// rien à écrire : FatFs n'a rien en attente
		pCle->bMontee = false;
	}
	JournalPrintf ("Cle USB %u (%s) : retiree\n", i + 1, pCle->pVolume);
}

void ClesUSBMiseAJour (bool bLOCI)
{
	CDeviceNameService *pNames = CDeviceNameService::Get ();
	for (unsigned i = 0; i < CLES_MAX; i++)
	{
		bool bPresente = pNames->GetDevice (s_Cles[i].pDevice, TRUE) != 0;
		if (bPresente == s_Cles[i].bPresente)
		{
			continue;
		}
		if (bPresente)
		{
			Branchee (i);
		}
		else
		{
			Retiree (i);
		}
		if (bLOCI)
		{
			oric_machine_loci_key_status (i, s_Cles[i].Texte);
		}
	}
}

const char *ClesUSBVolume (unsigned i)
{
	return i < CLES_MAX ? s_Cles[i].pVolume : 0;
}

const char *ClesUSBTexte (unsigned i)
{
	return i < CLES_MAX ? s_Cles[i].Texte : 0;
}

// Clé dont pPath est un chemin : « USB:... » ou « USB2:... » (casse libre)
static int Index (const char *pPath)
{
	for (unsigned i = 0; i < CLES_MAX; i++)
	{
		size_t n = strlen (s_Cles[i].pVolume);
		if (strncasecmp (pPath, s_Cles[i].pVolume, n) == 0)
		{
			return i;
		}
	}
	return -1;
}

int cles_usb_etat (const char *pPath, unsigned *pSerie, unsigned *pBranchement)
{
	int i = Index (pPath);
	if (i < 0)
	{
		return -1;
	}
	if (!s_Cles[i].bMontee)
	{
		return 0;
	}
	*pSerie = s_Cles[i].nSerie;
	*pBranchement = s_Cles[i].nBranchement;
	return 1;
}

// Taille du volume FatFs de pPath (clé, sinon carte SD) ; Phosphoric
// (oscompat_statvfs) ne lit que f_blocks et f_frsize
extern "C" int __wrap_statvfs (const char *pPath, struct statvfs *pInfo)
{
	int i = Index (pPath);
	const char *pVolume = i >= 0 ? s_Cles[i].pVolume : "SD:";
	DWORD nLibres;
	FATFS *fs;
	if (f_getfree (pVolume, &nLibres, &fs) != FR_OK)
	{
		return -1;
	}
	memset (pInfo, 0, sizeof *pInfo);
	pInfo->f_bsize = pInfo->f_frsize = (unsigned long) fs->csize * FF_MIN_SS;
	pInfo->f_blocks = fs->n_fatent - 2;
	pInfo->f_bfree = pInfo->f_bavail = nLibres;
	return 0;
}
