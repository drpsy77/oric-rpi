//
// banc.cpp — Banc natif du serveur WebDAV (dav.cpp), sans la Pi
//
// dav.cpp et carte.cpp du port, compilés pour Linux : Circle (tâches,
// sockets) et FatFs remplacés ici par des threads, des sockets et des
// fichiers POSIX. Un verrou global n'est rendu que là où Circle rend la main
// (Yield, MsSleep, attente d'un socket) : même ordonnancement coopératif que
// sur le cœur 0. Le « cœur 2 » de carte.cpp est un thread sans le verrou.
//
//   ./banc-dav PORT RACINE
//     RACINE/sd   : la carte (« SD:/ »)
//     RACINE/usb  : la clé « USB: » (« USB2: » absente)
//   disquette montée simulée : oric/usb/monte.dsk de la carte
//   client autorisé : 127.0.0.1 (dav://127.0.0.1:PORT/)
//
// Écrit pour le port, pour les essais ; pas dans le noyau.
//
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <iconv.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <thread>
#include <stdlib.h>

#include "../dav.h"
#include "../carte.h"
#include "../cles.h"
#include "../mdns.h"
#include "../journal.h"
#include "../oric_machine.h"
#include <circle/sched/scheduler.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/net/error.h>

static pthread_mutex_t s_Verrou = PTHREAD_MUTEX_INITIALIZER;
static int s_nTaches = 8;		// réseau, NTP, mDNS, TFTP, web... sur la Pi
static const char *s_pRacine;

static void Prendre (void)	{ pthread_mutex_lock (&s_Verrou); }
static void Rendre (void)	{ pthread_mutex_unlock (&s_Verrou); }

// ── Tâches ──
CTask::CTask (unsigned, bool)
{
	s_nTaches++;
	CTask *pTache = this;
	std::thread ([pTache] {
		usleep (20000);		// constructeur dérivé fini
		Prendre ();
		pTache->Run ();
		delete pTache;		// Circle efface une tâche finie
		Rendre ();
	}).detach ();
}

CTask::~CTask (void)	{ s_nTaches--; }

CScheduler *CScheduler::Get (void)	{ static CScheduler s; return &s; }
void CScheduler::Yield (void)		{ Rendre (); usleep (100); Prendre (); }
void CScheduler::MsSleep (unsigned ms)	{ Rendre (); usleep (ms * 1000); Prendre (); }
bool CScheduler::EnumerateTasks (bool (*pCallback) (CTask *, const char *, TTaskState, TTaskFlags, void *),
				 void *pParam)
{
	for (int i = 0; i < s_nTaches; i++)
	{
		(*pCallback) (0, "", TaskStateReady, TaskFlagNone, pParam);
	}
	return true;
}

void WaitForEvent (void)	{ usleep (100); }
void SendEvent (void)		{}

CTimer *CTimer::Get (void)	{ static CTimer t; return &t; }
int CTimer::GetTimeZone (void) const
{
	time_t t = time (0);
	struct tm Tm;
	localtime_r (&t, &Tm);
	return Tm.tm_gmtoff / 60;
}

// ── Sockets ──
CSocket::CSocket (CNetSubSystem *, int) : m_fd (socket (AF_INET, SOCK_STREAM, 0)) {}
CSocket::CSocket (int fd, const u8 *pIP) : m_fd (fd) { memcpy (m_IP, pIP, 4); }
CSocket::~CSocket (void)	{ close (m_fd); }

int CSocket::Bind (u16 nPort)
{
	int n = 1;
	setsockopt (m_fd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);
	struct sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_port = htons (nPort);
	return bind (m_fd, (struct sockaddr *) &a, sizeof a);
}

int CSocket::Listen (unsigned n)	{ return listen (m_fd, n); }

CSocket *CSocket::Accept (CIPAddress *pIP, u16 *pPort)
{
	struct sockaddr_in a;
	socklen_t n = sizeof a;
	Rendre ();
	int fd = accept (m_fd, (struct sockaddr *) &a, &n);
	Prendre ();
	if (fd < 0)
	{
		return 0;
	}
	pIP->Set ((const u8 *) &a.sin_addr);
	*pPort = ntohs (a.sin_port);
	return new CSocket (fd, (const u8 *) &a.sin_addr);
}

int CSocket::Send (const void *p, unsigned n, int)
{
	Rendre ();
	unsigned nFait = 0;
	while (nFait < n)
	{
		ssize_t r = send (m_fd, (const char *) p + nFait, n - nFait, MSG_NOSIGNAL);
		if (r <= 0)
		{
			Prendre ();
			return -NET_ERROR_CONNECTION_RESET;
		}
		nFait += r;
	}
	Prendre ();
	return n;
}

int CSocket::Receive (void *p, unsigned n, int)
{
	if (n > FRAME_BUFFER_SIZE)
	{
		n = FRAME_BUFFER_SIZE;		// un segment, comme Circle
	}
	Rendre ();
	ssize_t r = recv (m_fd, p, n, 0);
	Prendre ();
	if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
	{
		return -NET_ERROR_WOULD_BLOCK;
	}
	return r > 0 ? (int) r : -NET_ERROR_CONNECTION_RESET;
}

static int Delai (int fd, int nOption, unsigned us)
{
	struct timeval tv = { (time_t) (us / 1000000), (suseconds_t) (us % 1000000) };
	return setsockopt (fd, SOL_SOCKET, nOption, &tv, sizeof tv);
}
int CSocket::SetOptionReceiveTimeout (unsigned us)	{ return Delai (m_fd, SO_RCVTIMEO, us); }
int CSocket::SetOptionSendTimeout (unsigned us)		{ return Delai (m_fd, SO_SNDTIMEO, us); }

// ── Réseau, journal, clés, Oric ──
bool ResolveHostName (CNetSubSystem *, const char *, CIPAddress *pAddress)
{
	static const u8 Local[4] = { 127, 0, 0, 1 };
	pAddress->Set (Local);
	return true;
}

void JournalPrintf (const char *pFormat, ...)
{
	va_list Args;
	va_start (Args, pFormat);
	printf ("[journal] ");
	vprintf (pFormat, Args);
	va_end (Args);
	fflush (stdout);
}

const char *ClesUSBVolume (unsigned i)	{ return i == 0 ? "USB:" : "USB2:"; }

extern "C" int cles_usb_etat (const char *pPath, unsigned *pSerie, unsigned *pBranchement)
{
	if (strncasecmp (pPath, "USB:", 4) == 0)
	{
		*pSerie = 1234;
		*pBranchement = 1;
		return 1;
	}
	return strncasecmp (pPath, "USB2:", 5) == 0 ? 0 : -1;
}

static const char Montee[] = "oric/usb/monte.dsk";

static const char *SansSD (const char *p)
{
	if (strncasecmp (p, "SD:", 3) == 0)
	{
		p += 3;
	}
	while (*p == '/')
	{
		p++;
	}
	return p;
}

extern "C" bool oric_machine_disk_mounted (const char *path)
{
	return strncasecmp (path, "USB", 3) != 0 && strcasecmp (SansSD (path), Montee) == 0;
}

extern "C" bool oric_machine_disk_mounted_under (const char *dir)
{
	if (strncasecmp (dir, "USB", 3) == 0)
	{
		return false;
	}
	const char *p = SansSD (dir);
	size_t n = strlen (p);
	return n == 0 || (strncasecmp (Montee, p, n) == 0 && Montee[n] == '/');
}

extern "C" void oric_machine_flush_disks (void)		{}
extern "C" unsigned oric_machine_disk_writes (void)	{ return 0; }

// ── FatFs sur des fichiers POSIX ──
static void Hote (const char *p, char *pHote, unsigned n)
{
	const char *pDossier = "sd";
	if (strncasecmp (p, "SD:", 3) == 0)
	{
		p += 3;
	}
	else if (strncasecmp (p, "USB:", 4) == 0)
	{
		p += 4;
		pDossier = "usb";
	}
	else if (strncasecmp (p, "USB2:", 5) == 0)
	{
		p += 5;
		pDossier = "usb2";
	}
	snprintf (pHote, n, "%s/%s/%s", s_pRacine, pDossier, p);
}

static FRESULT Absent (const char *pHote)
{
	char Parent[700];
	snprintf (Parent, sizeof Parent, "%s", pHote);
	char *pSlash = strrchr (Parent, '/');
	if (pSlash != 0)
	{
		*pSlash = '\0';
	}
	struct stat st;
	return stat (Parent, &st) == 0 && S_ISDIR (st.st_mode) ? FR_NO_FILE : FR_NO_PATH;
}

static void Date (time_t t, WORD *pDate, WORD *pHeure)
{
	struct tm Tm;
	localtime_r (&t, &Tm);
	*pDate = ((Tm.tm_year - 80) << 9) | ((Tm.tm_mon + 1) << 5) | Tm.tm_mday;
	*pHeure = (Tm.tm_hour << 11) | (Tm.tm_min << 5) | (Tm.tm_sec / 2);
}

FRESULT f_open (FIL *f, const TCHAR *p, BYTE nMode)
{
	char h[700];
	Hote (p, h, sizeof h);
	f->fp = fopen (h, nMode & FA_WRITE ? "wb" : "rb");
	if (f->fp == 0)
	{
		return Absent (h);
	}
	struct stat st;
	fstat (fileno (f->fp), &st);
	f->taille = st.st_size;
	return FR_OK;
}

FRESULT f_close (FIL *f)	{ return fclose (f->fp) == 0 ? FR_OK : FR_DISK_ERR; }

FRESULT f_read (FIL *f, void *p, UINT n, UINT *pn)
{
	*pn = fread (p, 1, n, f->fp);
	return FR_OK;
}

FRESULT f_write (FIL *f, const void *p, UINT n, UINT *pn)
{
	*pn = fwrite (p, 1, n, f->fp);
	return *pn == n ? FR_OK : FR_DISK_ERR;
}

FRESULT f_opendir (DIR *d, const TCHAR *p)
{
	Hote (p, d->chemin, sizeof d->chemin);
	d->d = opendir (d->chemin);
	return d->d != 0 ? FR_OK : Absent (d->chemin);
}

FRESULT f_closedir (DIR *d)
{
	closedir ((struct __dirstream *) d->d);
	return FR_OK;
}

FRESULT f_readdir (DIR *d, FILINFO *pInfo)
{
	struct dirent *e;
	do
	{
		e = readdir ((struct __dirstream *) d->d);
	}
	while (e != 0 && (strcmp (e->d_name, ".") == 0 || strcmp (e->d_name, "..") == 0));
	if (e == 0)
	{
		pInfo->fname[0] = '\0';
		return FR_OK;
	}
	char h[1000];
	snprintf (h, sizeof h, "%s/%s", d->chemin, e->d_name);
	struct stat st;
	stat (h, &st);
	snprintf (pInfo->fname, sizeof pInfo->fname, "%s", e->d_name);
	pInfo->fsize = S_ISDIR (st.st_mode) ? 0 : st.st_size;
	pInfo->fattrib = S_ISDIR (st.st_mode) ? AM_DIR : 0;
	Date (st.st_mtime, &pInfo->fdate, &pInfo->ftime);
	return FR_OK;
}

FRESULT f_stat (const TCHAR *p, FILINFO *pInfo)
{
	char h[700];
	Hote (p, h, sizeof h);
	struct stat st;
	if (stat (h, &st) != 0)
	{
		return Absent (h);
	}
	const char *pNom = strrchr (p, '/');
	snprintf (pInfo->fname, sizeof pInfo->fname, "%s", pNom != 0 ? pNom + 1 : p);
	pInfo->fsize = S_ISDIR (st.st_mode) ? 0 : st.st_size;
	pInfo->fattrib = S_ISDIR (st.st_mode) ? AM_DIR : 0;
	Date (st.st_mtime, &pInfo->fdate, &pInfo->ftime);
	return FR_OK;
}

FRESULT f_unlink (const TCHAR *p)
{
	char h[700];
	Hote (p, h, sizeof h);
	struct stat st;
	if (stat (h, &st) != 0)
	{
		return Absent (h);
	}
	if (S_ISDIR (st.st_mode))
	{
		return rmdir (h) == 0 ? FR_OK : FR_DENIED;
	}
	return unlink (h) == 0 ? FR_OK : FR_DENIED;
}

FRESULT f_rename (const TCHAR *pAncien, const TCHAR *pNouveau)
{
	char a[700], n[700];
	Hote (pAncien, a, sizeof a);
	Hote (pNouveau, n, sizeof n);
	struct stat st;
	if (stat (a, &st) != 0)
	{
		return Absent (a);
	}
	if (stat (n, &st) == 0 && strcasecmp (a, n) != 0)
	{
		return FR_EXIST;		// FatFs ne remplace pas
	}
	if (rename (a, n) != 0)
	{
		return errno == ENOENT ? FR_NO_PATH : FR_DENIED;
	}
	return FR_OK;
}

FRESULT f_mkdir (const TCHAR *p)
{
	char h[700];
	Hote (p, h, sizeof h);
	if (mkdir (h, 0755) == 0)
	{
		return FR_OK;
	}
	return errno == EEXIST ? FR_EXIST : errno == ENOENT ? FR_NO_PATH : FR_DENIED;
}

// Page 850 par iconv
static unsigned Conversion (const char *pDe, const char *pVers, unsigned c, unsigned nOctets)
{
	iconv_t ic = iconv_open (pVers, pDe);
	char Entree[4], Sortie[4] = { 0, 0, 0, 0 };
	if (nOctets == 1)
	{
		Entree[0] = c;
	}
	else
	{
		memcpy (Entree, &c, 4);		// UCS-4LE (ARM, x86)
	}
	char *pe = Entree, *ps = Sortie;
	size_t ne = nOctets, ns = sizeof Sortie;
	size_t r = iconv (ic, &pe, &ne, &ps, &ns);
	iconv_close (ic);
	if (r == (size_t) -1)
	{
		return 0;
	}
	if (nOctets == 1)
	{
		unsigned u;
		memcpy (&u, Sortie, 4);
		return u;
	}
	return (unsigned char) Sortie[0];
}

WCHAR ff_oem2uni (WCHAR oem, WORD)	{ return Conversion ("CP850", "UCS-4LE", oem, 1); }
WCHAR ff_uni2oem (DWORD uni, WORD)	{ return Conversion ("UCS-4LE", "CP850", uni, 4); }

// ── Lancement ──
static void Attente (void) {}		// « entre deux trames » : toujours

int main (int argc, char **argv)
{
	if (argc != 3)
	{
		fprintf (stderr, "usage : %s PORT RACINE\n", argv[0]);
		return 1;
	}
	s_pRacine = argv[2];
	static CNetSubSystem Net;
	CarteInitialiser (Attente);
	std::thread (CarteCoeur2).detach ();
	Prendre ();
	new COricDAV (&Net, (u16) atoi (argv[1]), "localhost");
	Rendre ();
	printf ("banc-dav : dav://127.0.0.1:%s/ , carte %s/sd, cle %s/usb\n",
		argv[1], s_pRacine, s_pRacine);
	fflush (stdout);
	while (true)
	{
		pause ();
	}
}
