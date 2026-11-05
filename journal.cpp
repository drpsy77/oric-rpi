//
// journal.cpp — Journal de Phosphoric dans un fichier de la carte SD
//
// Écrit pour le port. Flux stdio maison (funopen de newlib) qui range le
// texte dans un tampon en mémoire, protégé par un verrou entre les cœurs ;
// JournalFlush l'ajoute au fichier. Si le tampon déborde entre deux
// écritures, le surplus est perdu et le nombre d'octets perdus est noté.
//
#include "journal.h"

#include <circle/spinlock.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define JOURNAL_SIZE	32768

static char s_Buffer[JOURNAL_SIZE];
static unsigned s_nUsed;			// octets en attente dans s_Buffer
static unsigned s_nLost;			// octets perdus (tampon plein)
static CSpinLock s_Lock (TASK_LEVEL);
static const char *s_pFileName;

static int JournalWrite (void *pCookie, const char *pBuf, int nLength)
{
	s_Lock.Acquire ();
	unsigned nCopy = nLength;
	if (nCopy > JOURNAL_SIZE - s_nUsed)
	{
		nCopy = JOURNAL_SIZE - s_nUsed;
		s_nLost += nLength - nCopy;
	}
	memcpy (s_Buffer + s_nUsed, pBuf, nCopy);
	s_nUsed += nCopy;
	s_Lock.Release ();

	return nLength;		// jamais d'erreur : Phosphoric ne doit pas s'arrêter
}

void JournalPrintf (const char *pFormat, ...)
{
	char Line[256];
	va_list Args;
	va_start (Args, pFormat);
	int nLength = vsnprintf (Line, sizeof Line, pFormat, Args);
	va_end (Args);
	if (nLength > (int) sizeof Line - 1)
	{
		nLength = sizeof Line - 1;
	}
	if (nLength > 0)
	{
		JournalWrite (0, Line, nLength);
	}
}

FILE *JournalOpen (const char *pFileName, const char *pPrevName)
{
	s_pFileName = pFileName;

	remove (pPrevName);
	rename (pFileName, pPrevName);
	FILE *pFile = fopen (pFileName, "w");
	if (pFile == 0)
	{
		return 0;
	}
	fclose (pFile);

	return funopen (0, 0, JournalWrite, 0, 0);
}

void JournalFlush (void)
{
	static char Pending[JOURNAL_SIZE];

	s_Lock.Acquire ();
	unsigned nPending = s_nUsed;
	unsigned nLost = s_nLost;
	memcpy (Pending, s_Buffer, nPending);
	s_nUsed = s_nLost = 0;
	s_Lock.Release ();

	if (nPending == 0 && nLost == 0)
	{
		return;
	}

	FILE *pFile = fopen (s_pFileName, "a");
	if (pFile == 0)
	{
		return;
	}
	fwrite (Pending, 1, nPending, pFile);
	if (nLost != 0)
	{
		fprintf (pFile, "[journal : %u octets perdus, tampon plein]\n", nLost);
	}
	fclose (pFile);
}
