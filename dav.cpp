//
// dav.cpp — Serveur WebDAV de la carte SD (dav=<port>, avec tftp=<client>)
//
// Écrit pour le port. Écoute et tâches de connexion reprises de CHTTPDaemon
// de Circle (lib/net/httpdaemon.cpp, GPL-3.0, © R. Stange) ; le reste
// (HTTP/1.1 persistant, méthodes WebDAV) est propre au port. Voir dav.h,
// SOURCES.md et docs/plan-webdav.md.
//
// WebDAV classe 1 (RFC 4918), sans verrous :
//   OPTIONS              DAV: 1
//   PROPFIND Depth 0, 1  207 ; corps ignoré, toujours les mêmes propriétés
//                        (Depth infinity refusé : 403)
//   GET, HEAD            fichier entier (DAV_MAX_FICHIER au plus), sans Range
//   PUT                  corps reçu en mémoire puis CarteEcrire (x.nouveau,
//                        renommage : un envoi coupé ne remplace rien)
//   DELETE               fichier ou dossier vide (dossier non vide : 409, le
//                        client efface son contenu après sa confirmation)
//   MKCOL, MOVE, COPY    COPY : fichiers seulement (le client copie un
//                        dossier fichier par fichier)
//   PROPPATCH            207, propriétés refusées (403)
//   LOCK, UNLOCK         405 (davfs2 : use_locks 0)
//
// Noms : FatFs les tient en page 850 ; rendus en UTF-8 dans les listes,
// reçus en UTF-8 et convertis (ff_uni2oem). Un nom créé (PUT, MKCOL, MOVE,
// COPY) doit être en ASCII imprimable, sans les caractères refusés par FAT :
// un accent est illisible et impossible à taper sur l'Oric (point 8).
//
// Racine « / » : la carte, avec « USB: » et « USB2: » (clés présentes),
// comme la page web ; leurs adresses sont « /USB%3A/... ».
//
// Seul le client de tftp= est servi (403 sinon). Journal : refus et
// écritures (carte.cpp) ; pas les lectures ni les listes réussies (gvfs en
// fait beaucoup), ni les 404 de lecture (gvfs cherche .hidden, etc.).
//
#include "dav.h"
#include "journal.h"

#include <circle/net/in.h>
#include <circle/net/error.h>
#include <circle/sched/scheduler.h>
#include <circle/sysconfig.h>
#include <circle/timer.h>
#include <circle/string.h>
#include <fatfs/ff.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define DAV_MAX_FICHIER	(64u << 20)	// PUT, GET, COPY : fichier en mémoire
#define DAV_MAX_ENTREES	1000		// lignes d'un dossier (PROPFIND)
#define DAV_DELAI_S	30		// silence du client : fin de la connexion
#define DAV_CORPS_LU	(64u << 10)	// corps ignoré lu jusqu'à cette taille
#define DAV_TACHES	3		// tâches laissées libres (MAX_TASKS : panique)
#define PAGE_DE_CODE	850		// FF_CODE_PAGE
#define HTTP_LIGNE	2048		// ligne de requête ou d'en-tête

static unsigned s_nConnexions;		// tâches de connexion
static volatile bool s_bAttente;	// une connexion attend une place

// ── Texte qui grandit (XML de PROPFIND) ──────────────────────────────────

class CTexteDAV
{
public:
	CTexteDAV (void) : m_p (0), m_n (0), m_nTaille (0), m_bErreur (false) {}
	~CTexteDAV (void)	{ free (m_p); }

	void Ajoute (const char *p, unsigned n)
	{
		if (m_bErreur)
		{
			return;
		}
		if (m_n + n > m_nTaille)
		{
			unsigned nTaille = m_nTaille != 0 ? m_nTaille : 4096;
			while (nTaille < m_n + n)
			{
				nTaille *= 2;
			}
			char *pNouveau = (char *) realloc (m_p, nTaille);
			if (pNouveau == 0)
			{
				m_bErreur = true;
				return;
			}
			m_p = pNouveau;
			m_nTaille = nTaille;
		}
		memcpy (m_p + m_n, p, n);
		m_n += n;
	}

	void Ajoute (const char *p)	{ Ajoute (p, strlen (p)); }

	void Printf (const char *pFormat, ...)
	{
		char Ligne[512];
		va_list Args;
		va_start (Args, pFormat);
		vsnprintf (Ligne, sizeof Ligne, pFormat, Args);
		va_end (Args);
		Ajoute (Ligne);
	}

	// texte UTF-8 dans du XML
	void XML (const char *p)
	{
		for (; *p != '\0'; p++)
		{
			switch (*p)
			{
			case '<':	Ajoute ("&lt;");	break;
			case '>':	Ajoute ("&gt;");	break;
			case '&':	Ajoute ("&amp;");	break;
			case '"':	Ajoute ("&quot;");	break;
			default:	Ajoute (p, 1);		break;
			}
		}
	}

	// chemin UTF-8 dans une adresse (« / » gardé, « : » codé)
	void URL (const char *p)
	{
		for (; *p != '\0'; p++)
		{
			unsigned char c = *p;
			if (   (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			    || (c >= '0' && c <= '9') || strchr ("-_.~/", c) != 0)
			{
				Ajoute ((const char *) &c, 1);
			}
			else
			{
				Printf ("%%%02X", c);
			}
		}
	}

	const char *Donnees (void) const	{ return m_p; }
	unsigned Longueur (void) const		{ return m_n; }
	bool Erreur (void) const		{ return m_bErreur; }

private:
	char *m_p;
	unsigned m_n, m_nTaille;
	bool m_bErreur;
};

// ── Noms, dates, nombres ─────────────────────────────────────────────────

// Nom en page 850 (FatFs) → UTF-8
static void VersUTF8 (const char *p, char *pDest, unsigned nTaille)
{
	unsigned n = 0;
	for (; *p != '\0'; p++)
	{
		unsigned c = (unsigned char) *p;
		if (c >= 0x80)
		{
			c = ff_oem2uni (c, PAGE_DE_CODE);
			if (c == 0)
			{
				c = '?';
			}
		}
		char Octets[3];
		unsigned k;
		if (c < 0x80)
		{
			Octets[0] = c;
			k = 1;
		}
		else if (c < 0x800)
		{
			Octets[0] = 0xC0 | (c >> 6);
			Octets[1] = 0x80 | (c & 0x3F);
			k = 2;
		}
		else
		{
			Octets[0] = 0xE0 | (c >> 12);
			Octets[1] = 0x80 | ((c >> 6) & 0x3F);
			Octets[2] = 0x80 | (c & 0x3F);
			k = 3;
		}
		if (n + k >= nTaille)
		{
			break;
		}
		memcpy (pDest + n, Octets, k);
		n += k;
	}
	pDest[n] = '\0';
}

// Chemin d'une adresse (« /oric/usb/x%20y.dsk », ou URL complète de
// Destination) → nom en page 850 : %xx décodés, requête (« ? ») ôtée, UTF-8
// converti. Faux si invalide, trop long, ou caractère absent de la page 850.
static bool DecodeChemin (const char *pURL, char *pNom, unsigned nTaille)
{
	// « http://oric.local:81/chemin » → « /chemin »
	const char *p;
	if (   strncasecmp (pURL, "http://", 7) == 0
	    || strncasecmp (pURL, "https://", 8) == 0)
	{
		p = strchr (strstr (pURL, "://") + 3, '/');
		pURL = p != 0 ? p : "/";
	}

	// %xx
	char Octets[CARTE_MAX_CHEMIN * 3];
	unsigned n = 0;
	for (p = pURL; *p != '\0' && *p != '?' && *p != '#'; p++)
	{
		char c = *p;
		if (c == '%')
		{
			char Hex[3] = { p[1], p[2], '\0' };
			char *pFin;
			c = (char) strtoul (Hex, &pFin, 16);
			if (pFin != Hex + 2)
			{
				return false;
			}
			p += 2;
		}
		if (c == '\0' || n + 1 >= sizeof Octets)
		{
			return false;
		}
		Octets[n++] = c;
	}
	Octets[n] = '\0';

	// UTF-8 → page 850
	unsigned k = 0;
	for (unsigned i = 0; i < n; )
	{
		unsigned c = (unsigned char) Octets[i++];
		if (c >= 0x80)
		{
			unsigned nSuite;
			if ((c & 0xE0) == 0xC0)		{ c &= 0x1F; nSuite = 1; }
			else if ((c & 0xF0) == 0xE0)	{ c &= 0x0F; nSuite = 2; }
			else if ((c & 0xF8) == 0xF0)	{ c &= 0x07; nSuite = 3; }
			else				return false;
			for (; nSuite > 0; nSuite--)
			{
				if (i >= n || (Octets[i] & 0xC0) != 0x80)
				{
					return false;
				}
				c = (c << 6) | (Octets[i++] & 0x3F);
			}
			c = ff_uni2oem (c, PAGE_DE_CODE);
			if (c == 0 || c > 0xFF)
			{
				return false;
			}
		}
		if (k + 1 >= nTaille)
		{
			return false;
		}
		pNom[k++] = (char) c;
	}
	pNom[k] = '\0';
	return true;
}

// Nom créé (dernier élément du chemin) : ASCII imprimable, sans les
// caractères refusés par FAT, ni point ou espace final (FatFs les ôterait)
static bool NomPermis (const char *pChemin)
{
	const char *pNom = strrchr (pChemin, '/');
	pNom = pNom != 0 ? pNom + 1 : pChemin;
	size_t n = strlen (pNom);
	if (n == 0 || pNom[n - 1] == '.' || pNom[n - 1] == ' ')
	{
		return false;
	}
	for (const char *p = pNom; *p != '\0'; p++)
	{
		if (*p < 0x20 || *p > 0x7E || strchr ("\"*/:<>?\\|", *p) != 0)
		{
			return false;
		}
	}
	return true;
}

// Dossier parent d'un chemin de FatFs : « SD:/oric/x » → « SD:/oric »,
// « SD:/x » → « SD:/ »
static void Parent (const char *pChemin, char *pParent, unsigned nTaille)
{
	snprintf (pParent, nTaille, "%s", pChemin);
	char *pSlash = strrchr (pParent, '/');
	if (pSlash != 0)
	{
		if (pSlash > pParent && pSlash[-1] == ':')
		{
			pSlash[1] = '\0';
		}
		else
		{
			*pSlash = '\0';
		}
	}
}

// Nombre en texte : le printf de circle-newlib ne connaît pas %llu
static const char *Nombre (unsigned long long n, char *pTampon, unsigned nTaille)
{
	char *p = pTampon + nTaille;
	*--p = '\0';
	do
	{
		*--p = '0' + n % 10;
		n /= 10;
	}
	while (n != 0);
	return p;
}

// Date FatFs (heure locale de CTimer) → temps universel ; faux sans date
static bool DateUTC (WORD nDate, WORD nHeure, struct tm *pTm)
{
	if (nDate == 0)
	{
		return false;
	}
	// jours depuis le 1/1/1970 (calendrier grégorien, H. Hinnant)
	int y = 1980 + (nDate >> 9);
	unsigned m = (nDate >> 5) & 15, d = nDate & 31;
	if (m < 1 || m > 12 || d < 1)
	{
		return false;
	}
	y -= m <= 2;
	int era = y / 400;
	unsigned yoe = y - era * 400;
	unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	long long nJours = (long long) era * 146097 + doe - 719468;

	time_t t = (time_t) (  nJours * 86400 + (nHeure >> 11) * 3600
			     + ((nHeure >> 5) & 63) * 60 + (nHeure & 31) * 2
			     - CTimer::Get ()->GetTimeZone () * 60);
	return gmtime_r (&t, pTm) != 0;
}

static const char *TypeContenu (const char *pNom)
{
	const char *pExt = strrchr (pNom, '.');
	if (pExt != 0
	    && (   strcasecmp (pExt, ".txt") == 0 || strcasecmp (pExt, ".cfg") == 0
		|| strcasecmp (pExt, ".conf") == 0 || strcasecmp (pExt, ".log") == 0
		|| strcasecmp (pExt, ".md") == 0 || strcasecmp (pExt, ".kma") == 0))
	{
		return "text/plain; charset=utf-8";
	}
	return "application/octet-stream";
}

static const char *TexteStatut (unsigned nStatut)
{
	switch (nStatut)
	{
	case 200:	return "OK";
	case 201:	return "Created";
	case 204:	return "No Content";
	case 207:	return "Multi-Status";
	case 400:	return "Bad Request";
	case 403:	return "Forbidden";
	case 404:	return "Not Found";
	case 405:	return "Method Not Allowed";
	case 409:	return "Conflict";
	case 411:	return "Length Required";
	case 412:	return "Precondition Failed";
	case 413:	return "Payload Too Large";
	case 415:	return "Unsupported Media Type";
	case 423:	return "Locked";
	case 431:	return "Request Header Fields Too Large";
	case 500:	return "Internal Server Error";
	case 501:	return "Not Implemented";
	case 505:	return "HTTP Version Not Supported";
	case 507:	return "Insufficient Storage";
	default:	return "Error";
	}
}

static unsigned StatutCarte (TCarteCode Code)
{
	switch (Code)
	{
	case CarteOK:		return 200;
	case CarteAbsent:	return 404;
	case CarteParentAbsent:	return 409;
	case CarteExiste:	return 412;
	case CarteMontee:	return 423;
	case CarteProtegee:	return 403;
	case CarteNonVide:	return 409;
	case CarteCle:		return 404;
	case CartePleine:	return 507;
	case CarteTropGros:	return 507;
	case CarteDossier:	return 403;
	case CarteErreur:	break;
	}
	return 500;
}

static const char Allow[] =
	"Allow: OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, MOVE, COPY, PROPFIND, PROPPATCH\r\n";

// ── Tâches ───────────────────────────────────────────────────────────────

COricDAV::COricDAV (CNetSubSystem *pNet, u16 nPort, const char *pClient,
		    CSocket *pSocket, CClientAutorise *pClientAutorise)
:	CTask (TASK_STACK_SIZE),
	m_pNet (pNet),
	m_nPort (nPort),
	m_pClientNom (pClient),
	m_pSocket (pSocket),
	m_bEcoute (pSocket == 0),
	m_pClient (pClientAutorise),
	m_bAutorise (false),
	m_nRecuPos (0),
	m_nRecuFin (0)
{
	if (m_bEcoute)
	{
		m_pClient = new CClientAutorise (pNet, pClient, "DAV");
		SetName ("dav");
	}
	else
	{
		s_nConnexions++;
	}
}

COricDAV::~COricDAV (void)
{
	delete m_pSocket;			// ferme la connexion
	m_pSocket = 0;
	if (m_bEcoute)
	{
		delete m_pClient;
	}
	else
	{
		s_nConnexions--;
	}
	m_pClient = 0;
}

void COricDAV::Run (void)
{
	if (m_bEcoute)
	{
		Ecoute ();
	}
	else
	{
		Connexion ();
	}
}

static boolean CompteTache (CTask *pTask, const char *pName, TTaskState State,
			    TTaskFlags Flags, void *pParam)
{
	(*(unsigned *) pParam)++;
	return TRUE;
}

static unsigned Taches (void)
{
	unsigned n = 0;
	CScheduler::Get ()->EnumerateTasks (CompteTache, &n);
	return n;
}

void COricDAV::Ecoute (void)
{
	m_pSocket = new CSocket (m_pNet, IPPROTO_TCP);
	if (   m_pSocket->Bind (m_nPort) < 0
	    || m_pSocket->Listen (2 * DAV_CONNEXIONS) < 0)
	{
		JournalPrintf ("DAV : port %u indisponible, serveur arrete\n", m_nPort);
		delete m_pSocket;
		m_pSocket = 0;
		return;
	}

	while (true)
	{
		CIPAddress IP;
		u16 nPort;
		CSocket *pConnexion = m_pSocket->Accept (&IP, &nPort);
		if (pConnexion == 0)
		{
			continue;
		}

		// Une place : DAV_CONNEXIONS tâches au plus, et MAX_TASKS de Circle
		// jamais atteint (panique). En attendant, les connexions inactives
		// (gvfs les garde ouvertes) se ferment (s_bAttente).
		unsigned nAttente = 0;
		while (   s_nConnexions >= DAV_CONNEXIONS
		       || Taches () + DAV_TACHES >= MAX_TASKS)
		{
			s_bAttente = true;
			CScheduler::Get ()->MsSleep (50);
			if (++nAttente == 20 * DAV_DELAI_S)
			{
				JournalPrintf ("DAV : aucune place (%u connexions, %u taches)\n",
					       s_nConnexions, Taches ());
			}
		}
		s_bAttente = false;

		new COricDAV (m_pNet, m_nPort, m_pClientNom, pConnexion, m_pClient);
	}
}

void COricDAV::Connexion (void)
{
	m_pSocket->SetOptionReceiveTimeout (1000000);		// Remplir : 1 s
	m_pSocket->SetOptionSendTimeout (DAV_DELAI_S * 1000000);

	const u8 *pIP = m_pSocket->GetForeignIP ();
	if (pIP == 0)
	{
		return;
	}
	CIPAddress IP (pIP);
	m_bAutorise = m_pClient->Accepte (IP);
	if (!m_bAutorise)
	{
		CString Texte;
		IP.Format (&Texte);
		JournalPrintf ("DAV : client %s refuse (%s attendu)\n", (const char *) Texte,
			       m_pClientNom);
	}

	while (Requete ())
	{
		// connexion persistante (HTTP/1.1)
	}
}

// ── Réception ────────────────────────────────────────────────────────────

// Segment suivant. Faux : connexion fermée, client silencieux DAV_DELAI_S,
// ou, entre deux requêtes, une autre connexion attend une place.
bool COricDAV::Remplir (bool bEntreRequetes)
{
	for (unsigned nSilence = 0; nSilence < DAV_DELAI_S; nSilence++)
	{
		int n = m_pSocket->Receive (m_Recu, sizeof m_Recu, 0);
		if (n > 0)
		{
			m_nRecuPos = 0;
			m_nRecuFin = n;
			return true;
		}
		if (n != -NET_ERROR_WOULD_BLOCK)
		{
			return false;
		}
		if (bEntreRequetes && s_bAttente)
		{
			return false;
		}
	}
	return false;
}

// 1 : ligne lue (sans CR LF) ; 0 : connexion finie ; -1 : trop longue
int COricDAV::LireLigne (char *pLigne, unsigned nTaille, bool bEntreRequetes)
{
	unsigned n = 0;
	bool bTrop = false;
	while (true)
	{
		if (m_nRecuPos == m_nRecuFin && !Remplir (bEntreRequetes && n == 0))
		{
			return 0;
		}
		char c = m_Recu[m_nRecuPos++];
		if (c == '\n')
		{
			break;
		}
		if (c == '\r')
		{
			continue;
		}
		if (n + 1 < nTaille)
		{
			pLigne[n++] = c;
		}
		else
		{
			bTrop = true;
		}
	}
	pLigne[n] = '\0';
	return bTrop ? -1 : 1;
}

bool COricDAV::Lire (u8 *pDest, unsigned nLongueur)
{
	while (nLongueur > 0)
	{
		if (m_nRecuPos == m_nRecuFin && !Remplir (false))
		{
			return false;
		}
		unsigned n = m_nRecuFin - m_nRecuPos;
		if (n > nLongueur)
		{
			n = nLongueur;
		}
		if (pDest != 0)
		{
			memcpy (pDest, m_Recu + m_nRecuPos, n);
			pDest += n;
		}
		m_nRecuPos += n;
		nLongueur -= n;
	}
	return true;
}

// Corps entier en mémoire (malloc, à libérer), nMax octets au plus, avec
// Content-Length ou par morceaux (chunked). Faux : *pnStatut (0 : connexion
// perdue), la connexion est alors fermée après la réponse.
bool COricDAV::LireCorps (u8 **ppCorps, unsigned *pnCorps, unsigned nMax, unsigned *pnStatut)
{
	*ppCorps = 0;
	*pnCorps = 0;
	*pnStatut = 0;

	if (!m_bChunked)
	{
		if (m_nLongueur > nMax)
		{
			*pnStatut = 413;
			m_bFermer = true;
			return false;
		}
		if (m_nLongueur == 0)
		{
			return true;
		}
		u8 *p = (u8 *) malloc (m_nLongueur);
		if (p == 0)
		{
			*pnStatut = 507;
			m_bFermer = true;
			return false;
		}
		if (!Lire (p, m_nLongueur))
		{
			free (p);
			m_bFermer = true;
			return false;
		}
		*ppCorps = p;
		*pnCorps = m_nLongueur;
		return true;
	}

	// « taille en hexadécimal[;extension] », données, CR LF ; 0 à la fin,
	// puis des en-têtes éventuels et une ligne vide
	u8 *p = 0;
	unsigned n = 0, nTaille = 0;
	char Ligne[256];
	while (true)
	{
		if (LireLigne (Ligne, sizeof Ligne, false) != 1)
		{
			break;
		}
		char *pFin;
		unsigned long nMorceau = strtoul (Ligne, &pFin, 16);
		if (pFin == Ligne)
		{
			*pnStatut = 400;
			break;
		}
		if (nMorceau == 0)
		{
			int r;
			while ((r = LireLigne (Ligne, sizeof Ligne, false)) == 1 && Ligne[0] != '\0')
			{
				// en-têtes de fin, ignorés
			}
			if (r != 1)
			{
				break;
			}
			*ppCorps = p;
			*pnCorps = n;
			return true;
		}
		if (nMorceau > nMax - n)
		{
			*pnStatut = 413;
			break;
		}
		if (n + nMorceau > nTaille)
		{
			unsigned nNouvelle = nTaille != 0 ? nTaille : 65536;
			while (nNouvelle < n + nMorceau)
			{
				nNouvelle = nNouvelle <= nMax / 2 ? nNouvelle * 2 : nMax;
			}
			u8 *pNouveau = (u8 *) realloc (p, nNouvelle);
			if (pNouveau == 0)
			{
				*pnStatut = 507;
				break;
			}
			p = pNouveau;
			nTaille = nNouvelle;
		}
		if (!Lire (p + n, nMorceau) || LireLigne (Ligne, sizeof Ligne, false) != 1)
		{
			break;
		}
		n += nMorceau;
	}
	free (p);
	m_bFermer = true;
	return false;
}

// Corps d'une requête qui n'en a pas l'usage : lu s'il est petit, sinon
// la connexion sera fermée après la réponse
void COricDAV::IgnorerCorps (void)
{
	if (m_bChunked)
	{
		u8 *p;
		unsigned n, nStatut;
		if (LireCorps (&p, &n, DAV_CORPS_LU, &nStatut))
		{
			free (p);
		}
	}
	else if (m_nLongueur > DAV_CORPS_LU || !Lire (0, m_nLongueur))
	{
		m_bFermer = true;
	}
	m_nLongueur = 0;
	m_bChunked = false;
}

// ── Réponses ─────────────────────────────────────────────────────────────

// pEntetes : lignes en plus, chacune finie par CR LF. bSansCorps (HEAD) :
// Content-Length vaut nLongueur, rien n'est envoyé après les en-têtes.
bool COricDAV::Repondre (unsigned nStatut, const char *pType, const void *pCorps,
			 unsigned nCorps, const char *pEntetes, bool bSansCorps,
			 unsigned nLongueur)
{
	char Entete[1024];
	char Type[100] = "", Longueur[40] = "";
	if (pType != 0)
	{
		snprintf (Type, sizeof Type, "Content-Type: %s\r\n", pType);
	}
	if (nStatut != 204)		// interdit pour 204 (RFC 9110)
	{
		snprintf (Longueur, sizeof Longueur, "Content-Length: %u\r\n",
			  bSansCorps ? nLongueur : nCorps);
	}
	int n = snprintf (Entete, sizeof Entete,
			  "HTTP/1.1 %u %s\r\n"
			  "Server: Oric-DAV (Circle)\r\n"
			  "%s%s%s"
			  "Connection: %s\r\n"
			  "\r\n",
			  nStatut, TexteStatut (nStatut), Type, pEntetes, Longueur,
			  m_bFermer ? "close" : "keep-alive");
	if (n < 0 || (unsigned) n >= sizeof Entete)
	{
		return false;
	}

	// petite réponse : un seul envoi
	if (!bSansCorps && nCorps > 0 && nCorps <= sizeof Entete - n)
	{
		u8 Tout[sizeof Entete * 2];
		memcpy (Tout, Entete, n);
		memcpy (Tout + n, pCorps, nCorps);
		return m_pSocket->Send (Tout, n + nCorps, 0) == (int) (n + nCorps);
	}
	if (m_pSocket->Send (Entete, n, 0) != n)
	{
		return false;
	}
	// Send attend (MSG_DONTWAIT absent) quand 64 Ko sont en file
	return    bSansCorps || nCorps == 0
	       || m_pSocket->Send (pCorps, nCorps, 0) == (int) nCorps;
}

bool COricDAV::Erreur (unsigned nStatut, const char *pTexte)
{
	char Texte[200];
	snprintf (Texte, sizeof Texte, "%u %s%s%s\n", nStatut, TexteStatut (nStatut),
		  pTexte != 0 ? " : " : "", pTexte != 0 ? pTexte : "");
	const char *pEntetes = nStatut == 405 || nStatut == 501 ? Allow : "";
	return Repondre (nStatut, "text/plain; charset=utf-8", Texte, strlen (Texte), pEntetes);
}

bool COricDAV::ErreurCarte (TCarteCode Code)
{
	return Erreur (StatutCarte (Code), CarteTexte (Code));
}

// ── Requête ──────────────────────────────────────────────────────────────

// pMot dans pTexte, casse libre
static bool Contient (const char *pTexte, const char *pMot)
{
	size_t n = strlen (pMot);
	for (; *pTexte != '\0'; pTexte++)
	{
		if (strncasecmp (pTexte, pMot, n) == 0)
		{
			return true;
		}
	}
	return false;
}

static bool Champ (const char *pLigne, const char *pNom, const char **ppValeur)
{
	size_t n = strlen (pNom);
	if (strncasecmp (pLigne, pNom, n) != 0 || pLigne[n] != ':')
	{
		return false;
	}
	const char *p = pLigne + n + 1;
	while (*p == ' ' || *p == '\t')
	{
		p++;
	}
	*ppValeur = p;
	return true;
}

bool COricDAV::Requete (void)
{
	m_Methode[0] = '\0';
	m_Cible[0] = '\0';
	m_Destination[0] = '\0';
	m_nLongueur = 0;
	m_bLongueur = false;
	m_bChunked = false;
	m_bContinue = false;
	m_nProfondeur = -2;
	m_bEcraser = true;
	m_bFermer = false;

	// Ligne de requête (lignes vides avant : permises, RFC 9112)
	char Ligne[HTTP_LIGNE];
	int r;
	do
	{
		r = LireLigne (Ligne, sizeof Ligne, true);
	}
	while (r == 1 && Ligne[0] == '\0');
	if (r == 0)
	{
		return false;
	}
	unsigned nErreur = r < 0 ? 431 : 0;

	char Version[16] = "";
	if (   nErreur == 0
	    && sscanf (Ligne, "%15s %2047s %15s", m_Methode, m_Cible, Version) != 3)
	{
		nErreur = 400;
	}
	if (nErreur == 0 && strncmp (Version, "HTTP/1.", 7) != 0)
	{
		nErreur = 505;
	}
	if (strcmp (Version, "HTTP/1.0") == 0)
	{
		m_bFermer = true;		// pas de connexion persistante
	}

	// En-têtes
	while ((r = LireLigne (Ligne, sizeof Ligne, false)) != 0)
	{
		if (r < 0)
		{
			nErreur = 431;
			continue;
		}
		if (Ligne[0] == '\0')
		{
			break;
		}
		const char *pValeur;
		if (Champ (Ligne, "Content-Length", &pValeur))
		{
			char *pFin;
			unsigned long n = strtoul (pValeur, &pFin, 10);
			m_nLongueur = n > 0xFFFFFFF0UL ? 0xFFFFFFF0U : (unsigned) n;
			m_bLongueur = pFin != pValeur;
		}
		else if (Champ (Ligne, "Transfer-Encoding", &pValeur))
		{
			m_bChunked = Contient (pValeur, "chunked");
		}
		else if (Champ (Ligne, "Expect", &pValeur))
		{
			m_bContinue = strcasecmp (pValeur, "100-continue") == 0;
		}
		else if (Champ (Ligne, "Depth", &pValeur))
		{
			m_nProfondeur =   strcmp (pValeur, "0") == 0 ? 0
					: strcmp (pValeur, "1") == 0 ? 1 : -1;
		}
		else if (Champ (Ligne, "Destination", &pValeur))
		{
			snprintf (m_Destination, sizeof m_Destination, "%s", pValeur);
		}
		else if (Champ (Ligne, "Overwrite", &pValeur))
		{
			m_bEcraser = !(pValeur[0] == 'F' || pValeur[0] == 'f');
		}
		else if (Champ (Ligne, "Connection", &pValeur))
		{
			if (Contient (pValeur, "close"))
			{
				m_bFermer = true;
			}
		}
	}
	if (r == 0)
	{
		return false;			// connexion perdue au milieu
	}

	if (nErreur != 0)
	{
		m_bFermer = true;
		Erreur (nErreur);
		return false;
	}
	if (!m_bAutorise)
	{
		IgnorerCorps ();
		Erreur (403, "client non autorise (tftp= de cmdline.txt)");
		return false;
	}

	// Chemin de FatFs (« SD:/oric/usb », « USB:/dSK ») ; « * » d'OPTIONS
	char Chemin[CARTE_MAX_CHEMIN];
	bool bOptions = strcmp (m_Methode, "OPTIONS") == 0;
	if (!bOptions || strcmp (m_Cible, "*") != 0)
	{
		char Nom[CARTE_MAX_CHEMIN];
		const TCarteErreur *pErreur = &CarteNomInvalide;
		if (   !DecodeChemin (m_Cible, Nom, sizeof Nom)
		    || !CarteChemin (Nom, Chemin, sizeof Chemin, true, &pErreur))
		{
			JournalPrintf ("DAV : %s %s refuse (%s)\n", m_Methode, m_Cible,
				       pErreur->pJournal);
			IgnorerCorps ();
			Erreur (pErreur == &CarteCleAbsente ? 404 : 400, pErreur->pJournal);
			return !m_bFermer;
		}
	}

	bool bOK;
	if (bOptions)					bOK = Options ();
	else if (strcmp (m_Methode, "PROPFIND") == 0)	bOK = PropFind (Chemin);
	else if (strcmp (m_Methode, "GET") == 0)	bOK = Get (Chemin, false);
	else if (strcmp (m_Methode, "HEAD") == 0)	bOK = Get (Chemin, true);
	else if (strcmp (m_Methode, "PUT") == 0)	bOK = Put (Chemin);
	else if (strcmp (m_Methode, "DELETE") == 0)	bOK = Delete (Chemin);
	else if (strcmp (m_Methode, "MKCOL") == 0)	bOK = Mkcol (Chemin);
	else if (strcmp (m_Methode, "MOVE") == 0)	bOK = MoveCopy (Chemin, true);
	else if (strcmp (m_Methode, "COPY") == 0)	bOK = MoveCopy (Chemin, false);
	else if (strcmp (m_Methode, "PROPPATCH") == 0)	bOK = PropPatch (Chemin);
	else
	{
		JournalPrintf ("DAV : methode %s refusee\n", m_Methode);
		IgnorerCorps ();
		bool bConnue =    strcmp (m_Methode, "LOCK") == 0
			       || strcmp (m_Methode, "UNLOCK") == 0;
		bOK = Erreur (bConnue ? 405 : 501);
	}
	return bOK && !m_bFermer;
}

// ── Méthodes ─────────────────────────────────────────────────────────────

bool COricDAV::Options (void)
{
	IgnorerCorps ();
	char Entetes[300];
	snprintf (Entetes, sizeof Entetes, "DAV: 1\r\nMS-Author-Via: DAV\r\n%s", Allow);
	return Repondre (200, 0, 0, 0, Entetes);
}

// Chemin pour une adresse (« oric/usb », « USB:/dSK », « » à la racine),
// en UTF-8
static void Relatif (const char *pChemin, char *pRelatif, unsigned nTaille)
{
	char Page850[CARTE_MAX_CHEMIN];
	CarteRelatif (pChemin, Page850, sizeof Page850);
	VersUTF8 (Page850, pRelatif, nTaille);
}

static void EnTetesFichier (const TCarteEntree *e, char *pEntetes, unsigned nTaille)
{
	char Chiffres[24];
	const char *pTaille = Nombre (e->nTaille, Chiffres, sizeof Chiffres);
	struct tm Tm;
	char Date[64] = "";
	if (DateUTC (e->nDate, e->nHeure, &Tm))
	{
		char Texte[40];
		strftime (Texte, sizeof Texte, "%a, %d %b %Y %H:%M:%S GMT", &Tm);
		snprintf (Date, sizeof Date, "Last-Modified: %s\r\n", Texte);
	}
	snprintf (pEntetes, nTaille, "%sETag: \"%s-%x-%x\"\r\n", Date, pTaille,
		  e->nDate, e->nHeure);
}

bool COricDAV::Get (const char *pChemin, bool bHead)
{
	IgnorerCorps ();
	TCarteEntree e;
	TCarteCode Code = CarteInfo (pChemin, &e);
	if (Code != CarteOK)
	{
		if (Code != CarteAbsent)
		{
			JournalPrintf ("DAV : %s %s : %s\n", m_Methode, pChemin, CarteTexte (Code));
		}
		return ErreurCarte (Code);
	}
	if (e.bDossier)
	{
		static const char Texte[] =
			"Dossier de la carte de l'Oric : l'ouvrir avec un client WebDAV "
			"(dav://oric.local:81/ dans le gestionnaire de fichiers),\n"
			"ou passer par la page web (web=).\n";
		return Repondre (200, "text/plain; charset=utf-8", Texte, sizeof Texte - 1, "",
				 bHead, sizeof Texte - 1);
	}
	if (e.nTaille > DAV_MAX_FICHIER)
	{
		JournalPrintf ("DAV : %s %s : plus de %u Mo\n", m_Methode, pChemin,
			       DAV_MAX_FICHIER >> 20);
		return Erreur (403, "fichier trop gros (64 Mo au plus par WebDAV)");
	}

	char Entetes[200];
	EnTetesFichier (&e, Entetes, sizeof Entetes);
	if (bHead)
	{
		return Repondre (200, TypeContenu (pChemin), 0, 0, Entetes, true,
				 (unsigned) e.nTaille);
	}

	u8 *pDonnees;
	unsigned nTaille;
	if (!CarteLire (pChemin, &pDonnees, &nTaille, DAV_MAX_FICHIER))
	{
		JournalPrintf ("DAV : GET %s : illisible\n", pChemin);
		return Erreur (nTaille == 0 ? 404 : 500);
	}
	bool bOK = Repondre (200, TypeContenu (pChemin), pDonnees, nTaille, Entetes);
	free (pDonnees);
	return bOK;
}

bool COricDAV::Put (const char *pChemin)
{
	// Refus avant de recevoir le corps (connexion fermée ensuite : le
	// client ne l'a peut-être pas encore envoyé)
	unsigned nStatut = 0;
	const char *pRaison = 0;
	TCarteEntree e;
	TCarteCode Code = CarteOK;
	bool bExiste = false;
	if (!NomPermis (pChemin))
	{
		nStatut = 400;
		pRaison = "nom refuse (ASCII seulement)";
	}
	else if (m_bLongueur && !m_bChunked && m_nLongueur > DAV_MAX_FICHIER)
	{
		nStatut = 413;
		pRaison = "plus de 64 Mo";
	}
	else if ((Code = CarteInfo (pChemin, &e)) == CarteOK)
	{
		bExiste = true;
		if (e.bDossier)
		{
			nStatut = 405;
			pRaison = "c'est un dossier";
		}
		else if (e.bMonte)
		{
			nStatut = 423;
			pRaison = CarteTexte (CarteMontee);
		}
	}
	else if (Code == CarteAbsent)
	{
		char Dossier[CARTE_MAX_CHEMIN];
		Parent (pChemin, Dossier, sizeof Dossier);
		TCarteEntree d;
		if (CarteInfo (Dossier, &d) != CarteOK || !d.bDossier)
		{
			nStatut = 409;
			pRaison = CarteTexte (CarteParentAbsent);
		}
	}
	else
	{
		nStatut = StatutCarte (Code);
		pRaison = CarteTexte (Code);
	}
	if (nStatut != 0)
	{
		JournalPrintf ("DAV : PUT %s refuse (%s)\n", pChemin, pRaison);
		if (m_bChunked || m_nLongueur > 0)
		{
			m_bFermer = true;
		}
		return Erreur (nStatut, pRaison);
	}

	if (m_bContinue)
	{
		static const char Continue[] = "HTTP/1.1 100 Continue\r\n\r\n";
		if (m_pSocket->Send (Continue, sizeof Continue - 1, 0) < 0)
		{
			return false;
		}
	}
	u8 *pCorps;
	unsigned nCorps;
	if (!LireCorps (&pCorps, &nCorps, DAV_MAX_FICHIER, &nStatut))
	{
		JournalPrintf ("DAV : PUT %s : envoi incomplet\n", pChemin);
		return nStatut != 0 && Erreur (nStatut);
	}

	// x.nouveau par le cœur 2, puis renommages (journal de CarteEcrire)
	CarteEcrire (pChemin, pCorps, nCorps, "DAV", 0, 0, &Code);
	free (pCorps);
	if (Code != CarteOK)
	{
		return ErreurCarte (Code);
	}
	return Repondre (bExiste ? 204 : 201);
}

bool COricDAV::Delete (const char *pChemin)
{
	IgnorerCorps ();
	TCarteCode Code = CarteEffacer (pChemin, "DAV");
	return Code == CarteOK ? Repondre (204) : ErreurCarte (Code);
}

bool COricDAV::Mkcol (const char *pChemin)
{
	if (m_bChunked || m_nLongueur > 0)
	{
		IgnorerCorps ();
		return Erreur (415);		// corps de MKCOL : non pris en charge
	}
	if (!NomPermis (pChemin))
	{
		JournalPrintf ("DAV : MKCOL %s refuse (nom)\n", pChemin);
		return Erreur (400, "nom refuse (ASCII seulement)");
	}
	TCarteCode Code = CarteCreerDossier (pChemin, "DAV");
	if (Code == CarteExiste)
	{
		return Erreur (405, CarteTexte (Code));
	}
	return Code == CarteOK ? Repondre (201) : ErreurCarte (Code);
}

bool COricDAV::MoveCopy (const char *pChemin, bool bMove)
{
	IgnorerCorps ();
	char Nom[CARTE_MAX_CHEMIN], Dest[CARTE_MAX_CHEMIN];
	const TCarteErreur *pErreur = &CarteNomInvalide;
	if (   m_Destination[0] == '\0'
	    || !DecodeChemin (m_Destination, Nom, sizeof Nom)
	    || !CarteChemin (Nom, Dest, sizeof Dest, true, &pErreur))
	{
		JournalPrintf ("DAV : %s %s -> %s refuse (%s)\n", m_Methode, pChemin,
			       m_Destination, pErreur->pJournal);
		return Erreur (pErreur == &CarteCleAbsente ? 409 : 400, pErreur->pJournal);
	}
	if (!NomPermis (Dest))
	{
		JournalPrintf ("DAV : %s %s -> %s refuse (nom)\n", m_Methode, pChemin, Dest);
		return Erreur (400, "nom refuse (ASCII seulement)");
	}

	bool bRemplace;
	TCarteCode Code = bMove
		? CarteDeplacer (pChemin, Dest, m_bEcraser, &bRemplace, DAV_MAX_FICHIER, "DAV")
		: CarteCopier (pChemin, Dest, m_bEcraser, &bRemplace, DAV_MAX_FICHIER, "DAV");
	if (Code != CarteOK)
	{
		return ErreurCarte (Code);
	}
	return Repondre (bRemplace ? 204 : 201);
}

// Un élément <D:response> : pRelatif en UTF-8 (adresse sans « / » initial)
void COricDAV::Entree (CTexteDAV *pXML, const char *pRelatif, const TCarteEntree *e)
{
	pXML->Ajoute ("<D:response><D:href>/");
	pXML->URL (pRelatif);
	if (e->bDossier && *pRelatif != '\0')
	{
		pXML->Ajoute ("/");
	}
	pXML->Ajoute ("</D:href><D:propstat><D:prop><D:displayname>");
	const char *pNom = strrchr (pRelatif, '/');
	pNom = pNom != 0 ? pNom + 1 : pRelatif;
	pXML->XML (*pNom != '\0' ? pNom : "carte");
	pXML->Ajoute ("</D:displayname>");
	if (e->bDossier)
	{
		pXML->Ajoute ("<D:resourcetype><D:collection/></D:resourcetype>");
	}
	else
	{
		char Chiffres[24];
		const char *pTaille = Nombre (e->nTaille, Chiffres, sizeof Chiffres);
		pXML->Printf ("<D:resourcetype/><D:getcontentlength>%s</D:getcontentlength>"
			      "<D:getcontenttype>%s</D:getcontenttype>"
			      "<D:getetag>\"%s-%x-%x\"</D:getetag>",
			      pTaille, TypeContenu (pNom), pTaille, e->nDate, e->nHeure);
	}
	struct tm Tm;
	if (DateUTC (e->nDate, e->nHeure, &Tm))
	{
		char Texte[40];
		strftime (Texte, sizeof Texte, "%a, %d %b %Y %H:%M:%S GMT", &Tm);
		pXML->Printf ("<D:getlastmodified>%s</D:getlastmodified>", Texte);
		strftime (Texte, sizeof Texte, "%Y-%m-%dT%H:%M:%SZ", &Tm);
		pXML->Printf ("<D:creationdate>%s</D:creationdate>", Texte);
	}
	pXML->Ajoute ("</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>\n");
}

bool COricDAV::PropFind (const char *pChemin)
{
	IgnorerCorps ();			// toujours les mêmes propriétés
	if (m_nProfondeur == -1)
	{
		return Erreur (403, "Depth: infinity refuse");
	}

	TCarteEntree e;
	TCarteCode Code = CarteInfo (pChemin, &e);
	if (Code != CarteOK)
	{
		if (Code != CarteAbsent)
		{
			JournalPrintf ("DAV : PROPFIND %s : %s\n", pChemin, CarteTexte (Code));
		}
		return ErreurCarte (Code);
	}

	CTexteDAV XML;
	XML.Ajoute ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
		    "<D:multistatus xmlns:D=\"DAV:\">\n");
	char Rel[CARTE_MAX_CHEMIN * 2];
	Relatif (pChemin, Rel, sizeof Rel);
	Entree (&XML, Rel, &e);

	// Depth absent : 1 (RFC 4918 : infinity, refusé ici ; curl l'omet)
	if (e.bDossier && m_nProfondeur != 0)
	{
		TCarteEntree *pEntrees =
			(TCarteEntree *) malloc (DAV_MAX_ENTREES * sizeof (TCarteEntree));
		if (pEntrees == 0)
		{
			return Erreur (507);
		}
		unsigned nEntrees;
		bool bTronque;
		if (CarteListe (pChemin, pEntrees, DAV_MAX_ENTREES, &nEntrees, &bTronque) == FR_OK)
		{
			if (bTronque)
			{
				JournalPrintf ("DAV : %s : liste arretee a %u entrees\n", pChemin,
					       DAV_MAX_ENTREES);
			}
			char Enfant[CARTE_MAX_CHEMIN * 2 + FF_MAX_LFN * 3 + 2];
			for (unsigned i = 0; i < nEntrees; i++)
			{
				char Nom[FF_MAX_LFN * 3 + 1];
				VersUTF8 (pEntrees[i].Nom, Nom, sizeof Nom);
				// clé à la racine : « USB: » ; dans « USB: » : « USB:/x »
				snprintf (Enfant, sizeof Enfant, "%s%s%s", Rel,
					  *Rel != '\0' ? "/" : "", Nom);
				Entree (&XML, Enfant, &pEntrees[i]);
			}
		}
		free (pEntrees);
	}
	XML.Ajoute ("</D:multistatus>\n");
	if (XML.Erreur ())
	{
		return Erreur (507);
	}
	return Repondre (207, "application/xml; charset=utf-8", XML.Donnees (), XML.Longueur ());
}

// Propriétés non modifiables : chacune refusée. Le corps n'est pas lu en
// détail, une seule réponse 403 sans propriété nommée.
bool COricDAV::PropPatch (const char *pChemin)
{
	IgnorerCorps ();
	char Rel[CARTE_MAX_CHEMIN * 2];
	Relatif (pChemin, Rel, sizeof Rel);
	CTexteDAV XML;
	XML.Ajoute ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
		    "<D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>/");
	XML.URL (Rel);
	XML.Ajoute ("</D:href><D:propstat><D:prop/><D:status>HTTP/1.1 403 Forbidden"
		    "</D:status></D:propstat></D:response></D:multistatus>\n");
	if (XML.Erreur ())
	{
		return Erreur (507);
	}
	return Repondre (207, "application/xml; charset=utf-8", XML.Donnees (), XML.Longueur ());
}
