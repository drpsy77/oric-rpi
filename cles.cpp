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
// Une clé est reconnue par le numéro de série de son volume (secteur de
// démarrage), qui servira à l'étape suivante : réécrire les disquettes
// montées seulement sur la même clé.
//
#include "cles.h"
#include "journal.h"

#include <circle/devicenameservice.h>
#include <fatfs/ff.h>
#include <fatfs/diskio.h>
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

static unsigned Lit32 (const u8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (unsigned) p[3] << 24;
}

// Nom et numéro de série dans le secteur de démarrage du volume
static void Description (TCle *pCle, char *pNom, unsigned nTaille)
{
	static u8 Secteur[FF_MAX_SS] __attribute__ ((aligned (4)));
	FATFS *fs = &pCle->FileSystem;
	snprintf (pNom, nTaille, "?");
	pCle->nSerie = 0;
	if (disk_read (fs->pdrv, Secteur, fs->volbase, 1) != RES_OK)
	{
		return;
	}
	unsigned nBase = fs->fs_type == FS_FAT32 ? 0x43 : 0x27;	// BS_VolID
	pCle->nSerie = Lit32 (&Secteur[nBase]);
	char Nom[12];
	memcpy (Nom, &Secteur[nBase + 4], 11);			// BS_VolLab
	Nom[11] = '\0';
	for (int i = 10; i >= 0 && Nom[i] == ' '; i--)
	{
		Nom[i] = '\0';
	}
	for (char *p = Nom; *p != '\0'; p++)
	{
		if (*p < ' ' || *p > '~')
		{
			*p = '?';
		}
	}
	snprintf (pNom, nTaille, "%s", Nom[0] != '\0' ? Nom : "sans nom");
}

static void Branchee (unsigned i)
{
	TCle *pCle = &s_Cles[i];
	pCle->bPresente = true;

	FRESULT Res = f_mount (&pCle->FileSystem, pCle->pVolume, 1);
	if (Res != FR_OK)
	{
		JournalPrintf ("Cle USB %u (%s) : branchee, illisible (%s)\n", i + 1, pCle->pDevice,
			       Res == FR_NO_FILESYSTEM ? "ni FAT16 ni FAT32 : exFAT, NTFS ?"
						       : "erreur de lecture");
		f_mount (0, pCle->pVolume, 0);
		return;
	}
	pCle->bMontee = true;

	FATFS *fs = &pCle->FileSystem;
	unsigned long long nOctets =
		(unsigned long long) (fs->n_fatent - 2) * fs->csize * FF_MIN_SS;
	char Nom[16];
	Description (pCle, Nom, sizeof Nom);
	JournalPrintf ("Cle USB %u (%s) : branchee, %s, FAT%s, %u.%u Go, serie %04X-%04X\n",
		       i + 1, pCle->pVolume, Nom,
		       fs->fs_type == FS_FAT32 ? "32" : fs->fs_type == FS_FAT16 ? "16" : "12",
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
				JournalPrintf ("  %s (%u octets)\n", Info.fname, (unsigned) Info.fsize);
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
