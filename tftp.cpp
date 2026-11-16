//
// tftp.cpp — Serveur TFTP de la carte SD (tftp=<adresse du client>)
//
// Écrit pour le port, sur CTFTPDaemon de Circle (lib/net/tftpdaemon.cpp,
// GPL-3.0) ; les appels de FatFs suivent addon/tftpfileserver de Circle
// (tftpfatfsfileserver.cpp). Voir SOURCES.md.
//
// Le démon est une tâche de l'ordonnanceur du cœur 0 : elle tourne pendant
// mScheduler.Yield () de la boucle principale, donc parfois pendant que le
// cœur 1 calcule une trame. Or le cœur 1 utilise la carte SD (disquettes,
// API fichiers de LOCI) : la carte n'est touchée ici qu'après pWaitIdle,
// et sans rendre la main (le cœur 0 ne demande pas de trame entre-temps).
// Un fichier est donc lu ou reçu en entier en mémoire (32 Mo au plus,
// limite de TFTP sans repli du numéro de bloc), puis écrit d'un coup.
//
// Fichier reçu : écrit sous « nom.nouveau », puis l'ancien devient
// « nom.ancien », le nouveau prend son nom, l'ancien est effacé. Une coupure
// laisse au pire l'un des deux à côté, jamais un fichier à moitié écrit
// (kernel8-32.img compris). Le dernier bloc est acquitté par Circle avant
// l'écriture : un échec n'est vu que dans le journal.
//
// Une disquette montée (Microdisc ou LOCI) ne peut pas être remplacée :
// l'Oric la garde en mémoire et la réécrirait par-dessus. Elle peut être
// lue : les pistes en attente sont d'abord réécrites (disquette.c).
//
#include "tftp.h"
#include "mdns.h"
#include "journal.h"
#include "oric_machine.h"

#include <circle/string.h>
#include <fatfs/ff.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILE_SIZE	(32u << 20)

COricTFTP::COricTFTP (CNetSubSystem *pNetSubSystem, const char *pClient,
		      void (*pWaitIdle) (void))
:	CTFTPDaemon (pNetSubSystem),
	m_pClientName (pClient),
	m_pWaitIdle (pWaitIdle),
	m_pData (0),
	m_nSize (0),
	m_nAlloc (0),
	m_nPos (0),
	m_bWriting (false)
{
	m_Path[0] = '\0';
}

COricTFTP::~COricTFTP (void)
{
	Release ();
}

void COricTFTP::Release (void)
{
	free (m_pData);
	m_pData = 0;
	m_nSize = m_nAlloc = m_nPos = 0;
	m_bWriting = false;
}

// « SD:/ » + nom ; refus des autres volumes et de « .. »
bool COricTFTP::MakePath (const char *pFileName)
{
	while (*pFileName == '/')
	{
		pFileName++;
	}
	if (   *pFileName == '\0'
	    || strchr (pFileName, ':') != 0
	    || strstr (pFileName, "..") != 0
	    || strchr (pFileName, '\\') != 0)
	{
		return false;
	}
	snprintf (m_Path, sizeof m_Path, "SD:/%s", pFileName);
	return true;
}

boolean COricTFTP::IsAccessAllowed (const CIPAddress *pForeignIP,
				    const char *pFileName, boolean bWriteRequest)
{
	CString IP;
	pForeignIP->Format (&IP);

	// m_Client vide avant la première résolution (et comparer une adresse
	// vide est une assertion de Circle : panique)
	if (!m_Client.IsSet () || *pForeignIP != m_Client)
	{
		// Nom du client : adresse peut-être changée (ou pas encore connue)
		CIPAddress Address;
		if (   ResolveHostName (m_pNetSubSystem, m_pClientName, &Address)
		    && (!m_Client.IsSet () || Address != m_Client))
		{
			m_Client.Set (Address);
			CString New;
			m_Client.Format (&New);
			JournalPrintf ("TFTP : %s a l'adresse %s\n", m_pClientName,
				       (const char *) New);
		}
	}
	if (!m_Client.IsSet () || *pForeignIP != m_Client)
	{
		JournalPrintf ("TFTP : %s refuse (client %s, %s attendu)\n", pFileName,
			       (const char *) IP, m_pClientName);
		return FALSE;
	}
	if (!MakePath (pFileName))
	{
		JournalPrintf ("TFTP : %s refuse (nom invalide)\n", pFileName);
		return FALSE;
	}
	if (bWriteRequest)
	{
		(*m_pWaitIdle) ();
		if (oric_machine_disk_mounted (m_Path))
		{
			JournalPrintf ("TFTP : %s refuse (disquette montee)\n", m_Path);
			return FALSE;
		}
	}
	JournalPrintf ("TFTP : %s de %s demande par %s\n",
		       bWriteRequest ? "envoi" : "lecture", m_Path, (const char *) IP);
	return TRUE;
}

// Lecture : le fichier entier en mémoire, entre deux trames
boolean COricTFTP::FileOpen (const char *pFileName)
{
	Release ();
	if (!MakePath (pFileName))
	{
		return FALSE;
	}

	(*m_pWaitIdle) ();
	if (oric_machine_disk_mounted (m_Path))
	{
		oric_machine_flush_disks ();	// pistes en attente sur la carte
	}

	FIL File;
	if (f_open (&File, m_Path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
	{
		JournalPrintf ("TFTP : %s introuvable\n", m_Path);
		return FALSE;
	}
	FSIZE_t nSize = f_size (&File);
	unsigned nRead = 0;
	bool bOK = nSize <= MAX_FILE_SIZE;
	if (bOK && nSize > 0)
	{
		m_pData = (u8 *) malloc (nSize);
		bOK =    m_pData != 0
		      && f_read (&File, m_pData, nSize, &nRead) == FR_OK
		      && nRead == nSize;
	}
	f_close (&File);

	if (!bOK)
	{
		JournalPrintf ("TFTP : %s illisible (%u octets)\n", m_Path, (unsigned) nSize);
		Release ();
		return FALSE;
	}
	m_nSize = nSize;
	return TRUE;
}

int COricTFTP::FileRead (void *pBuffer, unsigned nCount)
{
	unsigned nLeft = m_nSize - m_nPos;
	if (nCount > nLeft)
	{
		nCount = nLeft;
	}
	memcpy (pBuffer, m_pData + m_nPos, nCount);
	m_nPos += nCount;
	return nCount;
}

// Envoi : reçu en mémoire, écrit à la fin (UpdateStatus, StatusWriteCompleted)
boolean COricTFTP::FileCreate (const char *pFileName)
{
	Release ();
	if (!MakePath (pFileName))
	{
		return FALSE;
	}
	m_bWriting = true;
	return TRUE;
}

int COricTFTP::FileWrite (const void *pBuffer, unsigned nCount)
{
	if (m_nSize + nCount > MAX_FILE_SIZE)
	{
		JournalPrintf ("TFTP : %s refuse (plus de %u Mo)\n", m_Path, MAX_FILE_SIZE >> 20);
		return -1;
	}
	if (m_nSize + nCount > m_nAlloc)
	{
		unsigned nAlloc = m_nAlloc == 0 ? 256 * 1024 : m_nAlloc * 2;
		while (nAlloc < m_nSize + nCount)
		{
			nAlloc *= 2;
		}
		u8 *pData = (u8 *) realloc (m_pData, nAlloc);
		if (pData == 0)
		{
			return -1;
		}
		m_pData = pData;
		m_nAlloc = nAlloc;
	}
	memcpy (m_pData + m_nSize, pBuffer, nCount);
	m_nSize += nCount;
	return nCount;
}

// Appelé à la fin d'un transfert réussi ou non ; UpdateStatus suit et dit
// lequel : rien n'est écrit ici.
boolean COricTFTP::FileClose (void)
{
	if (!m_bWriting)
	{
		Release ();
	}
	return TRUE;
}

void COricTFTP::UpdateStatus (TStatus Status, const char *pFileName)
{
	switch (Status)
	{
	case StatusReadCompleted:
		JournalPrintf ("TFTP : %s lu\n", m_Path);
		break;

	case StatusReadAborted:
		JournalPrintf ("TFTP : lecture de %s interrompue\n", m_Path);
		break;

	case StatusWriteCompleted:
		if (m_bWriting)
		{
			Commit ();
			Release ();
		}
		break;

	case StatusWriteAborted:
		if (m_bWriting)
		{
			JournalPrintf ("TFTP : envoi de %s interrompu (%u octets recus), fichier inchange\n",
				       m_Path, m_nSize);
			Release ();
		}
		break;

	default:
		break;
	}
}

// Fichier reçu : remplace l'ancien, entre deux trames, sans rendre la main
bool COricTFTP::Commit (void)
{
	(*m_pWaitIdle) ();
	if (oric_machine_disk_mounted (m_Path))		// montée pendant l'envoi
	{
		JournalPrintf ("TFTP : %s non ecrit (disquette montee pendant l'envoi)\n", m_Path);
		return false;
	}

	char New[sizeof m_Path + 10], Old[sizeof m_Path + 10];
	snprintf (New, sizeof New, "%s.nouveau", m_Path);
	snprintf (Old, sizeof Old, "%s.ancien", m_Path);

	FIL File;
	unsigned nWritten = 0;
	if (f_open (&File, New, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		JournalPrintf ("TFTP : %s non ecrit (dossier absent ?)\n", m_Path);
		return false;
	}
	bool bOK =    (m_nSize == 0 || f_write (&File, m_pData, m_nSize, &nWritten) == FR_OK)
		   && nWritten == m_nSize;
	bOK = f_close (&File) == FR_OK && bOK;
	if (!bOK)
	{
		f_unlink (New);
		JournalPrintf ("TFTP : %s non ecrit (carte pleine ?)\n", m_Path);
		return false;
	}

	FILINFO Info;
	bool bExists = f_stat (m_Path, &Info) == FR_OK;
	if (bExists)
	{
		f_unlink (Old);				// reste d'une coupure
		if (f_rename (m_Path, Old) != FR_OK)
		{
			f_unlink (New);
			JournalPrintf ("TFTP : %s non remplace (fichier ouvert ?)\n", m_Path);
			return false;
		}
	}
	if (f_rename (New, m_Path) != FR_OK)
	{
		if (bExists)
		{
			f_rename (Old, m_Path);		// l'ancien reprend sa place
		}
		JournalPrintf ("TFTP : %s non remplace\n", m_Path);
		return false;
	}
	if (bExists)
	{
		f_unlink (Old);
	}

	JournalPrintf ("TFTP : %s ecrit (%u octets)%s\n", m_Path, m_nSize,
		       strcmp (m_Path, "SD:/kernel8-32.img") == 0
		       ? ", pris en compte au prochain demarrage (Ctrl+Alt+Suppr)" : "");
	return true;
}
