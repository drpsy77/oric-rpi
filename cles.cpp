//
// cles.cpp — Clés USB branchées sur la Pi (étape 1 : détection, journal)
//
// Écrit pour le port. Voir cles.h.
//
// Circle crée le périphérique « umsdN » quand une clé est branchée et
// l'enlève quand elle est retirée (UpdatePlugAndPlay, cœur 0) ; le module
// diskio de FatFs (Circle addon/fatfs) met alors son volume « pas prêt »
// (RES_NOTRDY) : une lecture échoue sans planter. Ici, chaque changement
// est vu en cherchant à nouveau umsd1 et umsd2 : volume monté au
// branchement, démonté au retrait.
//
// FAT12/16/32 et exFAT (FF_FS_EXFAT, réglé par preparer_circle.sh). Une
// clé est reconnue par le numéro de série de son volume, qui servira à l'étape suivante : réécrire les disquettes
// montées seulement sur la même clé.
//
#include "cles.h"
#include "journal.h"

#include <circle/devicenameservice.h>
#include <fatfs/ff.h>
#include <stdio.h>
#include <string.h>

#define LIGNES_RACINE	20		// noms de la racine notés dans le journal

struct TCle
{
	const char *pDevice;		// nom de Circle
	const char *pVolume;		// volume de FatFs
	bool bPresente;
	bool bMontee;
	FATFS FileSystem;
	unsigned nSerie;		// numéro de série du volume
};

static TCle s_Cles[CLES_MAX] =
{
	{ "umsd1", "USB:",  false, false, {}, 0 },
	{ "umsd2", "USB2:", false, false, {}, 0 },
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

	FRESULT Res = f_mount (&pCle->FileSystem, pCle->pVolume, 1);
	if (Res != FR_OK)
	{
		JournalPrintf ("Cle USB %u (%s) : branchee, illisible (%s)\n", i + 1, pCle->pDevice,
			       Res == FR_NO_FILESYSTEM ? "ni FAT ni exFAT : NTFS ?"
						       : "erreur de lecture");
		f_mount (0, pCle->pVolume, 0);
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
	if (pCle->bMontee)
	{
		f_mount (0, pCle->pVolume, 0);		// rien à écrire : FatFs n'a rien en attente
		pCle->bMontee = false;
	}
	JournalPrintf ("Cle USB %u (%s) : retiree\n", i + 1, pCle->pVolume);
}

void ClesUSBMiseAJour (void)
{
	CDeviceNameService *pNames = CDeviceNameService::Get ();
	for (unsigned i = 0; i < CLES_MAX; i++)
	{
		bool bPresente = pNames->GetDevice (s_Cles[i].pDevice, TRUE) != 0;
		if (bPresente && !s_Cles[i].bPresente)
		{
			Branchee (i);
		}
		else if (!bPresente && s_Cles[i].bPresente)
		{
			Retiree (i);
		}
	}
}
