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
// Noms : chemin de la carte (« oric/usb/x.dsk »), ou d'une clé USB de la
// Pi (« USB:/dSK/x.dsk », « USB2:/... » ; tftp-hpa prend « USB: » pour un
// nom de machine sans -l). Pas de liste des dossiers : page web (/l/).
// Remplacement d'un fichier, disquettes montées, client autorisé : carte.cpp
// (commun avec la page web). Le dernier bloc est acquitté par Circle avant
// l'écriture : un échec n'est vu que dans le journal.
//
#include "tftp.h"
#include "carte.h"
#include "journal.h"
#include "oric_machine.h"

#include <circle/string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILE_SIZE	(32u << 20)

COricTFTP::COricTFTP (CNetSubSystem *pNetSubSystem, const char *pClient,
		      void (*pWaitIdle) (void))
:	CTFTPDaemon (pNetSubSystem),
	m_Client (pNetSubSystem, pClient, "TFTP"),
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

// « SD:/ » + nom, ou clé USB (« USB:/dSK/x.dsk ») ; refus de « .. » et
// d'une clé absente (carte.cpp)
bool COricTFTP::MakePath (const char *pFileName, const TCarteErreur **ppErreur)
{
	return CarteChemin (pFileName, m_Path, sizeof m_Path, false, ppErreur);
}

boolean COricTFTP::IsAccessAllowed (const CIPAddress *pForeignIP,
				    const char *pFileName, boolean bWriteRequest)
{
	CString IP;
	pForeignIP->Format (&IP);

	if (!m_Client.Accepte (*pForeignIP))
	{
		JournalPrintf ("TFTP : %s refuse (client %s, %s attendu)\n", pFileName,
			       (const char *) IP, m_Client.GetNom ());
		return FALSE;
	}
	const TCarteErreur *pErreur = &CarteNomInvalide;
	if (!MakePath (pFileName, &pErreur))
	{
		JournalPrintf ("TFTP : %s refuse (%s)\n", pFileName, pErreur->pJournal);
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
	if (!CarteLire (m_Path, &m_pData, &m_nSize, MAX_FILE_SIZE))
	{
		if (m_nSize == 0)
		{
			JournalPrintf ("TFTP : %s introuvable\n", m_Path);
		}
		else
		{
			JournalPrintf ("TFTP : %s illisible (%u octets)\n", m_Path, m_nSize);
		}
		Release ();
		return FALSE;
	}
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
			// entre deux trames, sans rendre la main
			(*m_pWaitIdle) ();
			CarteEcrire (m_Path, m_pData, m_nSize, "TFTP");
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
