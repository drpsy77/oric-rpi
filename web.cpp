//
// web.cpp — Page web de la carte SD (web=<port>, avec tftp=<client>)
//
// Écrit pour le port, sur CHTTPDaemon de Circle (lib/net/httpdaemon.cpp,
// GPL-3.0 ; dérivé comme Circle sample/21-webserver). Voir SOURCES.md.
//
// Adresses :
//   /?d=oric/usb          état de la machine, liste du dossier, envoi
//   /f/oric/journal.txt   lecture d'un fichier (texte affiché, le reste
//                         téléchargé)
//   /envoi?d=oric/usb     (POST, multipart) fichiers envoyés dans le dossier
//   /redemarrer           (POST) comme Ctrl+Alt+Suppr
//
// Comme le serveur TFTP : chaque connexion est une tâche de l'ordonnanceur
// du cœur 0, qui ne touche la carte qu'après WaitIdle (cœur 1 entre deux
// trames) et sans rendre la main entre-temps ; seul le client de tftp= est
// servi ; une disquette montée peut être lue, pas remplacée ; un fichier
// reçu remplace l'ancien par renommage (carte.cpp).
//
// CHTTPDaemon prépare chaque réponse en entier en mémoire, dans un tampon
// réservé à la création de chaque connexion (MAX_CONTENU), et reçoit un
// envoi en entier avant de l'écrire (MAX_ENVOI) : plus gros, passer par
// TFTP (32 Mo).
//
#include "web.h"
#include "journal.h"
#include "oric_machine.h"

#include <fatfs/ff.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_CONTENU	(4u << 20)		// fichier lu, page
#define MAX_ENVOI	((4u << 20) + 16384)	// fichiers envoyés en une fois
#define MAX_ENTREES	1000			// lignes d'un dossier
#define DELAI_S		30			// réception sans nouvelles : abandon

// ── Page en construction, dans le tampon de CHTTPDaemon ──────────────────
class CPage
{
public:
	CPage (u8 *pBuffer, unsigned nSize)
	:	m_pBuffer ((char *) pBuffer), m_nSize (nSize), m_nLength (0) {}

	void Texte (const char *p)
	{
		while (*p != '\0' && m_nLength < m_nSize)
		{
			m_pBuffer[m_nLength++] = *p++;
		}
	}

	void Printf (const char *pFormat, ...)
	{
		char Ligne[512];
		va_list Args;
		va_start (Args, pFormat);
		vsnprintf (Ligne, sizeof Ligne, pFormat, Args);
		va_end (Args);
		Texte (Ligne);
	}

	// texte dans du HTML (noms de fichiers)
	void HTML (const char *p)
	{
		for (; *p != '\0'; p++)
		{
			switch (*p)
			{
			case '<':	Texte ("&lt;");		break;
			case '>':	Texte ("&gt;");		break;
			case '&':	Texte ("&amp;");	break;
			case '"':	Texte ("&quot;");	break;
			case '\'':	Texte ("&#39;");	break;
			default:
				{
					char c[2] = { *p, '\0' };
					Texte (c);
				}
				break;
			}
		}
	}

	// chemin dans une adresse (« / » gardé)
	void URL (const char *p)
	{
		for (; *p != '\0'; p++)
		{
			unsigned char c = *p;
			if (   (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			    || (c >= '0' && c <= '9') || strchr ("-_.~/", c) != 0)
			{
				char s[2] = { (char) c, '\0' };
				Texte (s);
			}
			else
			{
				Printf ("%%%02X", c);
			}
		}
	}

	unsigned Longueur (void) const	{ return m_nLength; }

private:
	char *m_pBuffer;
	unsigned m_nSize;
	unsigned m_nLength;
};

static const char Style[] =
	"<style>"
	":root{--fond:#f6f5f1;--texte:#1d1d1b;--doux:#6b6a64;--ligne:#dddbd3;"
	"--lien:#1f5fae;--accent:#b8321f;--carte:#fff}"
	"@media(prefers-color-scheme:dark){:root{--fond:#1b1b1a;--texte:#e9e7e0;"
	"--doux:#9a988f;--ligne:#3a3936;--lien:#7fb0ef;--accent:#ef7a62;--carte:#242422}}"
	"body{margin:0;padding:16px;background:var(--fond);color:var(--texte);"
	"font:15px/1.45 system-ui,sans-serif}"
	"main{max-width:900px;margin:auto}"
	"h1{font-size:1.4em;margin:0 0 12px}h2{font-size:1.1em;margin:0 0 8px}"
	"section{background:var(--carte);border:1px solid var(--ligne);border-radius:8px;"
	"padding:12px 14px;margin-bottom:14px;overflow-x:auto}"
	"table{border-collapse:collapse;width:100%}"
	"td,th{padding:4px 8px;text-align:left;border-bottom:1px solid var(--ligne);"
	"white-space:nowrap}"
	"th{color:var(--doux);font-weight:600}.n{text-align:right;font-variant-numeric:tabular-nums}.etat th{width:1%}"
	"a{color:var(--lien);text-decoration:none}a:hover{text-decoration:underline}"
	".doux{color:var(--doux)}.monte{color:var(--accent);font-size:.85em}"
	"button{font:inherit;padding:4px 12px;cursor:pointer}"
	"form{margin:10px 0 0}"
	"</style>";

static void Debut (CPage *pPage, const char *pTitre)
{
	pPage->Texte ("<!DOCTYPE html><html lang=\"fr\"><head><meta charset=\"utf-8\">"
		      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
		      "<title>");
	pPage->HTML (pTitre);
	pPage->Texte ("</title>");
	pPage->Texte (Style);
	pPage->Texte ("</head><body><main>");
}

static void Fin (CPage *pPage)
{
	pPage->Texte ("</main></body></html>\n");
}

// Page courte : un titre, un message, un lien de retour
static void Message (CPage *pPage, const char *pTitre, const char *pMessage,
		     const char *pDossier)
{
	Debut (pPage, pTitre);
	pPage->Texte ("<h1>");
	pPage->HTML (pTitre);
	pPage->Texte ("</h1><section><p>");
	pPage->HTML (pMessage);
	pPage->Texte ("</p><p><a href=\"/?d=");
	pPage->URL (pDossier != 0 ? pDossier : "");
	pPage->Texte ("\">Retour à la carte</a></p></section>");
	Fin (pPage);
}

// %XX (et « + » dans les paramètres) ; faux si trop long
static bool Decode (const char *p, unsigned nLong, char *pDest, unsigned nTaille, bool bPlus)
{
	unsigned n = 0;
	for (unsigned i = 0; i < nLong && p[i] != '\0'; i++)
	{
		char c = p[i];
		if (c == '%' && i + 2 < nLong)
		{
			char Hex[3] = { p[i + 1], p[i + 2], '\0' };
			c = (char) strtoul (Hex, 0, 16);
			i += 2;
		}
		else if (c == '+' && bPlus)
		{
			c = ' ';
		}
		if (n + 1 >= nTaille)
		{
			return false;
		}
		pDest[n++] = c;
	}
	pDest[n] = '\0';
	return true;
}

// Valeur du paramètre pNom (« d » de « ?d=oric/usb »), décodée ; "" si absent
static bool Parametre (const char *pParams, const char *pNom, char *pDest, unsigned nTaille)
{
	size_t nNom = strlen (pNom);
	const char *p = pParams;
	while (p != 0 && *p != '\0')
	{
		const char *pFin = strchr (p, '&');
		size_t nLong = pFin != 0 ? (size_t) (pFin - p) : strlen (p);
		if (nLong > nNom && strncmp (p, pNom, nNom) == 0 && p[nNom] == '=')
		{
			return Decode (p + nNom + 1, nLong - nNom - 1, pDest, nTaille, true);
		}
		p = pFin != 0 ? pFin + 1 : 0;
	}
	*pDest = '\0';
	return true;
}

// « SD:/oric/usb » → « oric/usb »
static const char *SansVolume (const char *pChemin)
{
	return pChemin + 4;
}

// ── Connexions ───────────────────────────────────────────────────────────

COricWeb::COricWeb (CNetSubSystem *pNetSubSystem, u16 nPort, const char *pClient,
		    const TWebHooks *pHooks, CSocket *pSocket,
		    CClientAutorise *pClientAutorise)
:	CHTTPDaemon (pNetSubSystem, pSocket, pSocket != 0 ? MAX_CONTENU : 0, nPort,
		     pSocket != 0 ? MAX_ENVOI : 0, DELAI_S),
	m_pNet (pNetSubSystem),
	m_nPort (nPort),
	m_pClientNom (pClient),
	m_pHooks (pHooks),
	m_bEcoute (pSocket == 0),
	m_pClient (pClientAutorise)
{
	if (m_bEcoute)
	{
		m_pClient = new CClientAutorise (pNetSubSystem, pClient, "Web");
	}
	else
	{
		const u8 *pIP = pSocket->GetForeignIP ();
		if (pIP != 0)
		{
			m_ForeignIP.Set (pIP);
		}
	}
}

COricWeb::~COricWeb (void)
{
	if (m_bEcoute)
	{
		delete m_pClient;
	}
	m_pClient = 0;
}

CHTTPDaemon *COricWeb::CreateWorker (CNetSubSystem *pNetSubSystem, CSocket *pSocket)
{
	return new COricWeb (pNetSubSystem, m_nPort, m_pClientNom, m_pHooks, pSocket, m_pClient);
}

void COricWeb::WriteAccessLog (const CIPAddress &rRemoteIP, THTTPRequestMethod RequestMethod,
			       const char *pRequestURI, THTTPStatus Status,
			       unsigned nContentLength)
{
	if (strcmp (pRequestURI, "/favicon.ico") == 0)
	{
		return;
	}
	CString IP;
	rRemoteIP.Format (&IP);
	JournalPrintf ("Web : %s %s -> %u (%u octets) par %s\n",
		       RequestMethod == HTTPRequestMethodPost ? "POST" : "GET",
		       pRequestURI, (unsigned) Status, nContentLength, (const char *) IP);
}

THTTPStatus COricWeb::GetContent (const char *pPath, const char *pParams,
				  const char *pFormData, u8 *pBuffer, unsigned *pLength,
				  const char **ppContentType)
{
	CPage Page (pBuffer, *pLength);
	*ppContentType = "text/html; charset=utf-8";

	if (strcmp (pPath, "/favicon.ico") == 0)
	{
		return HTTPNotFound;
	}

	if (!m_ForeignIP.IsSet () || !m_pClient->Accepte (m_ForeignIP))
	{
		CString IP;
		if (m_ForeignIP.IsSet ())
		{
			m_ForeignIP.Format (&IP);
		}
		JournalPrintf ("Web : %s refuse (client %s, %s attendu)\n", pPath,
			       (const char *) IP, m_pClientNom);
		char Texte[200];
		snprintf (Texte, sizeof Texte, "Cette page est réservée à %s (tftp= de cmdline.txt).",
			  m_pClientNom);
		Message (&Page, "Accès refusé", Texte, 0);
		*pLength = Page.Longueur ();
		return HTTPOK;
	}

	char Dossier[CARTE_MAX_CHEMIN];
	if (!Parametre (pParams, "d", Dossier, sizeof Dossier))
	{
		return HTTPBadRequest;
	}

	THTTPStatus Status;
	if (strcmp (pPath, "/") == 0 || strcmp (pPath, "/index.html") == 0)
	{
		Status = PageDossier (Dossier, &Page);
	}
	else if (strncmp (pPath, "/f/", 3) == 0)
	{
		char Nom[CARTE_MAX_CHEMIN];
		if (!Decode (pPath + 3, strlen (pPath + 3), Nom, sizeof Nom, false))
		{
			return HTTPBadRequest;
		}
		Status = Fichier (Nom, pBuffer, pLength, ppContentType, &Page);
		if (Status == HTTPOK && *ppContentType != 0
		    && strncmp (*ppContentType, "text/html", 9) != 0)
		{
			return HTTPOK;		// fichier : *pLength déjà rempli
		}
	}
	else if (strcmp (pPath, "/envoi") == 0)
	{
		Status = Envoi (Dossier, &Page);
	}
	else if (strcmp (pPath, "/redemarrer") == 0)
	{
		Status = Redemarrer (pFormData, &Page);
	}
	else
	{
		return HTTPNotFound;
	}

	*pLength = Page.Longueur ();
	return Status;
}

// ── Liste d'un dossier ───────────────────────────────────────────────────

struct TEntree
{
	char Nom[FF_MAX_LFN + 1];
	FSIZE_t nTaille;
	WORD nDate, nHeure;
	bool bDossier;
	bool bMonte;
};

static int Compare (const void *p1, const void *p2)
{
	const TEntree *e1 = (const TEntree *) p1, *e2 = (const TEntree *) p2;
	if (e1->bDossier != e2->bDossier)
	{
		return e1->bDossier ? -1 : 1;
	}
	return strcasecmp (e1->Nom, e2->Nom);
}

static void Taille (CPage *pPage, FSIZE_t n)
{
	if (n < 10000)
	{
		pPage->Printf ("%u o", (unsigned) n);
	}
	else if (n < 10000 * 1024)
	{
		pPage->Printf ("%u Ko", (unsigned) ((n + 512) / 1024));
	}
	else
	{
		pPage->Printf ("%u Mo", (unsigned) ((n + 512 * 1024) / (1024 * 1024)));
	}
}

THTTPStatus COricWeb::PageDossier (const char *pDossier, CPage *pPage)
{
	char Chemin[CARTE_MAX_CHEMIN];
	if (!CarteChemin (pDossier, Chemin, sizeof Chemin, true))
	{
		Message (pPage, "Nom invalide", pDossier, 0);
		return HTTPBadRequest;
	}
	const char *pRel = SansVolume (Chemin);		// « oric/usb », « » pour la racine

	TEntree *pEntrees = (TEntree *) malloc (MAX_ENTREES * sizeof (TEntree));
	if (pEntrees == 0)
	{
		return HTTPInternalServerError;
	}

	// Carte : entre deux trames, sans rendre la main
	(*m_pHooks->WaitIdle) ();
	unsigned nEntrees = 0;
	bool bTronque = false;
	DIR Dir;
	FRESULT Res = f_opendir (&Dir, Chemin);
	if (Res == FR_OK)
	{
		FILINFO Info;
		while (f_readdir (&Dir, &Info) == FR_OK && Info.fname[0] != '\0')
		{
			if (nEntrees == MAX_ENTREES)
			{
				bTronque = true;
				break;
			}
			TEntree *e = &pEntrees[nEntrees++];
			memcpy (e->Nom, Info.fname, sizeof e->Nom);	// même taille
			e->Nom[sizeof e->Nom - 1] = '\0';
			e->nTaille = Info.fsize;
			e->nDate = Info.fdate;
			e->nHeure = Info.ftime;
			e->bDossier = (Info.fattrib & AM_DIR) != 0;
			e->bMonte = false;
			if (!e->bDossier)
			{
				char Complet[CARTE_MAX_CHEMIN + FF_MAX_LFN + 2];
				snprintf (Complet, sizeof Complet, "%s/%s",
					  *pRel != '\0' ? Chemin : "SD:", e->Nom);
				e->bMonte = oric_machine_disk_mounted (Complet);
			}
		}
		f_closedir (&Dir);
	}
	qsort (pEntrees, nEntrees, sizeof (TEntree), Compare);

	CString Etat;
	(*m_pHooks->Status) (&Etat);

	Debut (pPage, "Oric : carte SD");
	pPage->Texte ("<h1>Oric : carte SD</h1>");

	// État et redémarrage
	pPage->Texte ("<section><h2>État</h2><table class=\"etat\">");
	pPage->Texte (Etat);
	pPage->Texte ("</table><form method=\"post\" action=\"/redemarrer\" "
		      "onsubmit=\"return confirm('Redémarrer la Pi ? (comme Ctrl+Alt+Suppr : "
		      "disquettes réécrites, puis redémarrage)')\">"
		      "<input type=\"hidden\" name=\"ok\" value=\"1\">"
		      "<button>Redémarrer</button> <span class=\"doux\">"
		      "comme Ctrl+Alt+Suppr (après l'envoi d'un noyau)</span></form></section>");

	// Fil d'Ariane : carte / oric / usb
	pPage->Texte ("<section><h2><a href=\"/?d=\">carte</a>");
	{
		char Partiel[CARTE_MAX_CHEMIN];
		const char *p = pRel;
		while (*p != '\0')
		{
			const char *pFin = strchr (p, '/');
			size_t n = pFin != 0 ? (size_t) (pFin - p) : strlen (p);
			size_t nAvant = p - pRel;
			snprintf (Partiel, sizeof Partiel, "%.*s", (int) (nAvant + n), pRel);
			char Morceau[FF_MAX_LFN + 1];
			snprintf (Morceau, sizeof Morceau, "%.*s", (int) n, p);
			pPage->Texte (" / <a href=\"/?d=");
			pPage->URL (Partiel);
			pPage->Texte ("\">");
			pPage->HTML (Morceau);
			pPage->Texte ("</a>");
			p += n;
			while (*p == '/')
			{
				p++;
			}
		}
	}
	pPage->Texte ("</h2>");

	if (Res != FR_OK)
	{
		pPage->Texte ("<p>Dossier introuvable.</p>");
	}
	else
	{
		pPage->Texte ("<table><tr><th>Nom</th><th class=\"n\">Taille</th>"
			      "<th>Modifié</th></tr>");
		if (*pRel != '\0')
		{
			// dossier parent
			char Parent[CARTE_MAX_CHEMIN];
			snprintf (Parent, sizeof Parent, "%s", pRel);
			char *pSlash = strrchr (Parent, '/');
			if (pSlash != 0)
			{
				*pSlash = '\0';
			}
			else
			{
				Parent[0] = '\0';
			}
			pPage->Texte ("<tr><td><a href=\"/?d=");
			pPage->URL (Parent);
			pPage->Texte ("\">..</a></td><td></td><td></td></tr>");
		}
		for (unsigned i = 0; i < nEntrees; i++)
		{
			TEntree *e = &pEntrees[i];
			pPage->Texte ("<tr><td><a href=\"");
			pPage->Texte (e->bDossier ? "/?d=" : "/f/");
			if (*pRel != '\0')
			{
				pPage->URL (pRel);
				pPage->Texte ("/");
			}
			pPage->URL (e->Nom);
			pPage->Texte ("\">");
			pPage->HTML (e->Nom);
			pPage->Texte (e->bDossier ? "/</a>" : "</a>");
			if (e->bMonte)
			{
				pPage->Texte (" <span class=\"monte\">montée</span>");
			}
			pPage->Texte ("</td><td class=\"n\">");
			if (!e->bDossier)
			{
				Taille (pPage, e->nTaille);
			}
			pPage->Texte ("</td><td class=\"doux\">");
			if (e->nDate != 0)
			{
				pPage->Printf ("%04u-%02u-%02u %02u:%02u",
					       1980 + (e->nDate >> 9), (e->nDate >> 5) & 15,
					       e->nDate & 31, e->nHeure >> 11, (e->nHeure >> 5) & 63);
			}
			pPage->Texte ("</td></tr>");
		}
		pPage->Texte ("</table>");
		if (bTronque)
		{
			pPage->Printf ("<p class=\"doux\">Liste arrêtée à %u entrées.</p>", MAX_ENTREES);
		}

		// Envoi de fichiers dans ce dossier
		pPage->Texte ("<form method=\"post\" enctype=\"multipart/form-data\" action=\"/envoi?d=");
		pPage->URL (pRel);
		pPage->Printf ("\" onsubmit=\"var t=0,f=this.fichier.files;"
			       "for(var i=0;i<f.length;i++)t+=f[i].size;"
			       "if(!f.length)return false;"
			       "if(t>%u){alert('Plus de %u Ko en tout : passer par TFTP.');return false}"
			       "this.querySelector('button').disabled=true;return true\">"
			       "<input type=\"file\" name=\"fichier\" multiple> "
			       "<button>Envoyer ici</button> <span class=\"doux\">"
			       "remplace un fichier du même nom ; %u Ko au plus</span></form>",
			       MAX_ENVOI - 16384, (MAX_ENVOI - 16384) / 1024,
			       (MAX_ENVOI - 16384) / 1024);
	}
	pPage->Texte ("</section>");
	Fin (pPage);

	free (pEntrees);
	return HTTPOK;
}

// ── Lecture d'un fichier ─────────────────────────────────────────────────

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
	return "application/octet-stream";	// téléchargé sous son nom
}

THTTPStatus COricWeb::Fichier (const char *pNom, u8 *pBuffer, unsigned *pLength,
			       const char **ppContentType, CPage *pPage)
{
	char Chemin[CARTE_MAX_CHEMIN];
	if (!CarteChemin (pNom, Chemin, sizeof Chemin))
	{
		Message (pPage, "Nom invalide", pNom, 0);
		return HTTPBadRequest;
	}

	// Dossier sous /f/ : sa liste
	(*m_pHooks->WaitIdle) ();
	FILINFO Info;
	if (f_stat (Chemin, &Info) == FR_OK && (Info.fattrib & AM_DIR))
	{
		return PageDossier (pNom, pPage);
	}

	unsigned nTaille = *pLength;
	if (!CarteLireDans (Chemin, pBuffer, &nTaille))
	{
		char Dossier[CARTE_MAX_CHEMIN];
		snprintf (Dossier, sizeof Dossier, "%s", SansVolume (Chemin));
		char *pSlash = strrchr (Dossier, '/');
		*(pSlash != 0 ? pSlash : Dossier) = '\0';
		if (nTaille == 0)
		{
			Message (pPage, "Fichier introuvable", SansVolume (Chemin), Dossier);
			return HTTPNotFound;
		}
		char Texte[CARTE_MAX_CHEMIN + 100];
		snprintf (Texte, sizeof Texte,
			  "%s : %u octets, la page web en lit %u au plus. Passer par TFTP.",
			  SansVolume (Chemin), nTaille, *pLength);
		Message (pPage, "Fichier trop gros", Texte, Dossier);
		return HTTPOK;
	}
	*pLength = nTaille;
	*ppContentType = TypeContenu (Chemin);
	return HTTPOK;
}

// ── Envoi de fichiers ────────────────────────────────────────────────────

THTTPStatus COricWeb::Envoi (const char *pDossier, CPage *pPage)
{
	char Dossier[CARTE_MAX_CHEMIN];
	if (!CarteChemin (pDossier, Dossier, sizeof Dossier, true))
	{
		Message (pPage, "Nom invalide", pDossier, 0);
		return HTTPBadRequest;
	}
	const char *pRel = SansVolume (Dossier);

	Debut (pPage, "Envoi");
	pPage->Texte ("<h1>Envoi</h1><section><table>");

	unsigned nFichiers = 0;
	const char *pEntete;
	const u8 *pDonnees;
	unsigned nTaille;
	while (GetMultipartFormPart (&pEntete, &pDonnees, &nTaille))
	{
		// Content-Disposition: form-data; name="fichier"; filename="x.dsk"
		const char *p = strstr (pEntete, "filename=\"");
		if (p == 0)
		{
			continue;
		}
		p += 10;
		const char *pFin = strchr (p, '"');
		if (pFin == 0 || pFin == p)
		{
			continue;		// aucun fichier choisi
		}
		char Nom[FF_MAX_LFN + 1];
		snprintf (Nom, sizeof Nom, "%.*s", (int) (pFin - p), p);
		// navigateurs anciens : chemin complet du poste
		const char *pBase = Nom;
		for (const char *q = Nom; *q != '\0'; q++)
		{
			if (*q == '/' || *q == '\\')
			{
				pBase = q + 1;
			}
		}

		nFichiers++;
		char Relatif[CARTE_MAX_CHEMIN], Chemin[CARTE_MAX_CHEMIN];
		int nRel = snprintf (Relatif, sizeof Relatif, "%s%s%s", pRel,
				     *pRel != '\0' ? "/" : "", pBase);
		char Resultat[120];
		if (   *pBase == '\0' || nRel < 0 || (unsigned) nRel >= sizeof Relatif
		    || !CarteChemin (Relatif, Chemin, sizeof Chemin))
		{
			snprintf (Resultat, sizeof Resultat, "refusé (nom invalide)");
			JournalPrintf ("Web : %s refuse (nom invalide)\n", Relatif);
		}
		else
		{
			// entre deux trames, sans rendre la main
			(*m_pHooks->WaitIdle) ();
			CarteEcrire (Chemin, pDonnees, nTaille, "Web", Resultat, sizeof Resultat);
		}
		pPage->Texte ("<tr><td>");
		pPage->HTML (pBase);
		pPage->Texte ("</td><td>");
		pPage->HTML (Resultat);
		pPage->Texte ("</td></tr>");
	}
	if (nFichiers == 0)
	{
		pPage->Texte ("<tr><td>Aucun fichier reçu.</td></tr>");
	}
	pPage->Texte ("</table><p><a href=\"/?d=");
	pPage->URL (pRel);
	pPage->Texte ("\">Retour au dossier</a></p></section>");
	Fin (pPage);
	return HTTPOK;
}

// ── Redémarrage ──────────────────────────────────────────────────────────

THTTPStatus COricWeb::Redemarrer (const char *pFormData, CPage *pPage)
{
	if (strstr (pFormData, "ok=1") == 0)		// GET, ou formulaire incomplet
	{
		Message (pPage, "Redémarrage", "Utiliser le bouton de la page.", 0);
		return HTTPOK;
	}
	JournalPrintf ("Web : redemarrage demande\n");
	(*m_pHooks->Reboot) ();

	Debut (pPage, "Redémarrage");
	pPage->Texte ("<meta http-equiv=\"refresh\" content=\"30;url=/\">"
		      "<h1>Redémarrage</h1><section><p>Disquettes réécrites, puis "
		      "redémarrage de la Pi. Cette page se recharge dans 30 s "
		      "(le temps que le réseau revienne).</p>"
		      "<p><a href=\"/\">Retour à la carte</a></p></section>");
	Fin (pPage);
	return HTTPOK;
}
