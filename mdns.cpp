//
// mdns.cpp — Noms en .local (mDNS, Bonjour / Avahi) pour le port
//
// Écrit pour le port. Messages DNS (RFC 1035), en multicast sur
// 224.0.0.251:5353 (RFC 6762) ; socket ouverte comme CmDNSDaemon de Circle
// (lib/net/mdnsdaemon.cpp, GPL-3.0, voir SOURCES.md) : Bind (5353),
// Connect (groupe), SetOptionAddMembership.
//
// - Question d'une autre machine pour « nom.local » (type A ou ANY) :
//   réponse en multicast avec l'adresse de la Pi.
// - Réponses reçues (enregistrements A) : gardées dans une petite table,
//   où Resolve () trouve l'adresse demandée (question renvoyée à 0, 0,3 et
//   1 s ; 2 s au plus).
// Pas de recherche de conflit de nom : choisir nom= unique sur le réseau.
//
#include "mdns.h"
#include "journal.h"

#include <circle/net/in.h>
#include <circle/net/dnsclient.h>
#include <circle/sched/scheduler.h>
#include <circle/timer.h>
#include <circle/string.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define MDNS_PORT	5353
#define MAX_MESSAGE	1600		// au moins FRAME_BUFFER_SIZE (CSocket::Receive)
#define TTL_ANSWER	120		// s, durée de validité donnée aux autres
#define CACHE_SIZE	8
#define CACHE_MAX_AGE	60		// s, réponse réutilisée sans nouvelle question

static const u8 s_Group[] = {224, 0, 0, 251};

COricMDNS *COricMDNS::s_pThis = 0;

// Réponses reçues : nom (minuscules, sans point final), adresse, heure
static struct
{
	char Name[64];
	u8 Address[4];
	unsigned nTicks;		// CTimer::GetClockTicks (), 0 : vide
} s_Cache[CACHE_SIZE];

bool IsLocalHostName (const char *pName)
{
	size_t n = strlen (pName);
	if (n > 0 && pName[n - 1] == '.')
	{
		n--;
	}
	return n > 6 && strncasecmp (pName + n - 6, ".local", 6) == 0;
}

// Nom en minuscules, sans point final
static void CopyName (char *pOut, size_t nSize, const char *pIn)
{
	size_t k = 0;
	for (; *pIn && k + 1 < nSize; pIn++)
	{
		char c = *pIn;
		pOut[k++] = (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
	}
	if (k > 0 && pOut[k - 1] == '.')
	{
		k--;
	}
	pOut[k] = '\0';
}

// Nom du message à l'offset *pOffset (étiquettes, pointeurs de compression)
// -> « a.b.local » en minuscules ; *pOffset passe après le nom.
static bool ReadName (const u8 *pMsg, unsigned nLength, unsigned *pOffset,
		      char *pOut, size_t nSize)
{
	unsigned nPos = *pOffset;
	bool bJumped = false;
	unsigned nJumps = 0;
	size_t k = 0;

	while (true)
	{
		if (nPos >= nLength)
		{
			return false;
		}
		u8 ucLen = pMsg[nPos];
		if ((ucLen & 0xC0) == 0xC0)		// pointeur
		{
			if (nPos + 1 >= nLength || ++nJumps > 16)
			{
				return false;
			}
			if (!bJumped)
			{
				*pOffset = nPos + 2;
				bJumped = true;
			}
			nPos = ((ucLen & 0x3F) << 8) | pMsg[nPos + 1];
			continue;
		}
		if (ucLen & 0xC0)
		{
			return false;
		}
		nPos++;
		if (ucLen == 0)
		{
			break;
		}
		if (nPos + ucLen > nLength)
		{
			return false;
		}
		if (k > 0 && k + 1 < nSize)
		{
			pOut[k++] = '.';
		}
		for (unsigned i = 0; i < ucLen; i++)
		{
			char c = pMsg[nPos + i];
			if (k + 1 < nSize)
			{
				pOut[k++] = (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
			}
		}
		nPos += ucLen;
	}
	pOut[k] = '\0';
	if (!bJumped)
	{
		*pOffset = nPos;
	}
	return true;
}

// « a.b.local » -> étiquettes ; retourne la longueur écrite (0 : trop long)
static unsigned WriteName (u8 *pOut, unsigned nSize, const char *pName)
{
	unsigned k = 0;
	while (*pName)
	{
		const char *pDot = strchr (pName, '.');
		unsigned nLabel = pDot ? (unsigned) (pDot - pName) : strlen (pName);
		if (nLabel == 0 || nLabel > 63 || k + 1 + nLabel + 1 > nSize)
		{
			return 0;
		}
		pOut[k++] = nLabel;
		memcpy (pOut + k, pName, nLabel);
		k += nLabel;
		pName += nLabel;
		if (*pName == '.')
		{
			pName++;
		}
	}
	pOut[k++] = 0;
	return k;
}

static u16 Get16 (const u8 *p)
{
	return p[0] << 8 | p[1];
}

static void Put16 (u8 *p, u16 v)
{
	p[0] = v >> 8;
	p[1] = v;
}

COricMDNS::COricMDNS (CNetSubSystem *pNet, const char *pName)
:	m_pNet (pNet),
	m_pSocket (0),
	m_bRunning (false)
{
	char Name[48];
	CopyName (Name, sizeof Name, pName);
	snprintf (m_Name, sizeof m_Name, "%s.local", Name);
	s_pThis = this;
	SetName ("mdns");
}

COricMDNS::~COricMDNS (void)
{
	delete m_pSocket;
	s_pThis = 0;
}

void COricMDNS::Run (void)
{
	CIPAddress Group (s_Group);
	m_pSocket = new CSocket (m_pNet, IPPROTO_UDP);
	if (   m_pSocket->Bind (MDNS_PORT) < 0
	    || m_pSocket->Connect (Group, MDNS_PORT) < 0
	    || m_pSocket->SetOptionAddMembership (Group) < 0)
	{
		JournalPrintf ("mDNS : port 5353 ou groupe multicast indisponible\n");
		delete m_pSocket;
		m_pSocket = 0;
		return;
	}
	m_bRunning = true;

	CString IP;
	m_pNet->GetConfig ()->GetIPAddress ()->Format (&IP);
	JournalPrintf ("mDNS : la Pi repond au nom %s (%s)\n", m_Name, (const char *) IP);
	SendAnswer ();				// annonce

	static u8 Message[MAX_MESSAGE];
	while (true)
	{
		int nLength = m_pSocket->Receive (Message, sizeof Message, 0);
		if (nLength > 0)
		{
			Parse (Message, nLength);
		}
		else if (nLength < 0)
		{
			CScheduler::Get ()->MsSleep (100);
		}
	}
}

void COricMDNS::Parse (const u8 *pMsg, unsigned nLength)
{
	if (nLength < 12)
	{
		return;
	}
	u16 usFlags = Get16 (pMsg + 2);
	unsigned nQuestions = Get16 (pMsg + 4);
	unsigned nRecords = Get16 (pMsg + 6) + Get16 (pMsg + 8) + Get16 (pMsg + 10);
	unsigned nOffset = 12;
	char Name[128];

	if (!(usFlags & 0x8000))		// question
	{
		if (usFlags & 0x7800)		// pas une requête standard
		{
			return;
		}
		for (unsigned i = 0; i < nQuestions; i++)
		{
			if (   !ReadName (pMsg, nLength, &nOffset, Name, sizeof Name)
			    || nOffset + 4 > nLength)
			{
				return;
			}
			u16 usType = Get16 (pMsg + nOffset);
			nOffset += 4;
			if ((usType == 1 || usType == 255) && strcmp (Name, m_Name) == 0)
			{
				SendAnswer ();
				return;
			}
		}
		return;
	}

	// Réponse : questions sautées, enregistrements A gardés
	for (unsigned i = 0; i < nQuestions; i++)
	{
		if (!ReadName (pMsg, nLength, &nOffset, Name, sizeof Name))
		{
			return;
		}
		nOffset += 4;
	}
	for (unsigned i = 0; i < nRecords; i++)
	{
		if (   !ReadName (pMsg, nLength, &nOffset, Name, sizeof Name)
		    || nOffset + 10 > nLength)
		{
			return;
		}
		u16 usType = Get16 (pMsg + nOffset);
		u16 usClass = Get16 (pMsg + nOffset + 2) & 0x7FFF;
		unsigned nData = Get16 (pMsg + nOffset + 8);
		nOffset += 10;
		if (nOffset + nData > nLength)
		{
			return;
		}
		if (   usType == 1 && usClass == 1 && nData == 4
		    && strlen (Name) < sizeof s_Cache[0].Name
		    && strcmp (Name, m_Name) != 0)
		{
			// même nom, sinon la plus ancienne entrée
			unsigned nSlot = 0;
			for (unsigned j = 0; j < CACHE_SIZE; j++)
			{
				if (strcmp (s_Cache[j].Name, Name) == 0)
				{
					nSlot = j;
					break;
				}
				if (s_Cache[j].nTicks < s_Cache[nSlot].nTicks)
				{
					nSlot = j;
				}
			}
			strcpy (s_Cache[nSlot].Name, Name);
			memcpy (s_Cache[nSlot].Address, pMsg + nOffset, 4);
			s_Cache[nSlot].nTicks = CTimer::GetClockTicks () | 1;
		}
		nOffset += nData;
	}
}

bool COricMDNS::SendQuery (const char *pName)
{
	u8 Message[300];
	memset (Message, 0, 12);
	Put16 (Message + 4, 1);			// une question
	unsigned n = WriteName (Message + 12, sizeof Message - 16, pName);
	if (n == 0)
	{
		return false;
	}
	n += 12;
	Put16 (Message + n, 1);			// type A
	Put16 (Message + n + 2, 1);		// classe IN, réponse en multicast
	n += 4;
	return m_pSocket->Send (Message, n, MSG_DONTWAIT) == (int) n;
}

bool COricMDNS::SendAnswer (void)
{
	const CIPAddress *pIP = m_pNet->GetConfig ()->GetIPAddress ();
	if (pIP->IsNull ())
	{
		return false;
	}
	u8 Message[128];
	memset (Message, 0, 12);
	Put16 (Message + 2, 0x8400);		// réponse, faisant autorité
	Put16 (Message + 6, 1);			// un enregistrement
	unsigned n = WriteName (Message + 12, sizeof Message - 26, m_Name);
	if (n == 0)
	{
		return false;
	}
	n += 12;
	Put16 (Message + n, 1);			// type A
	Put16 (Message + n + 2, 0x8001);	// classe IN, « cache flush »
	Put16 (Message + n + 4, 0);		// durée de validité
	Put16 (Message + n + 6, TTL_ANSWER);
	Put16 (Message + n + 8, 4);
	pIP->CopyTo (Message + n + 10);
	n += 14;
	return m_pSocket->Send (Message, n, MSG_DONTWAIT) == (int) n;
}

bool COricMDNS::Resolve (const char *pName, CIPAddress *pAddress)
{
	COricMDNS *pThis = s_pThis;
	if (pThis == 0 || !pThis->m_bRunning)
	{
		return false;
	}
	char Name[64];
	CopyName (Name, sizeof Name, pName);

	unsigned nStart = CTimer::GetClockTicks ();
	unsigned nNextQuery = 0;			// ms depuis le début
	static const unsigned Retries[] = {300, 1000, ~0u};
	unsigned nRetry = 0;
	bool bFirst = true;

	while (true)
	{
		unsigned nNow = CTimer::GetClockTicks ();
		for (unsigned j = 0; j < CACHE_SIZE; j++)
		{
			if (   s_Cache[j].nTicks != 0
			    && strcmp (s_Cache[j].Name, Name) == 0
			    && (   (int) (s_Cache[j].nTicks - nStart) >= 0
				|| nNow - s_Cache[j].nTicks < CACHE_MAX_AGE * 1000000u))
			{
				pAddress->Set (s_Cache[j].Address);
				return true;
			}
		}

		unsigned nElapsed = (nNow - nStart) / 1000;
		if (nElapsed >= 2000)
		{
			return false;
		}
		if (bFirst || nElapsed >= nNextQuery)
		{
			pThis->SendQuery (Name);
			nNextQuery = Retries[nRetry++];
			bFirst = false;
		}
		CScheduler::Get ()->MsSleep (10);
	}
}

bool ResolveHostName (CNetSubSystem *pNet, const char *pName, CIPAddress *pAddress)
{
	if (IsLocalHostName (pName))
	{
		return COricMDNS::Resolve (pName, pAddress);
	}
	CDNSClient DNS (pNet);
	return DNS.Resolve (pName, pAddress);
}
