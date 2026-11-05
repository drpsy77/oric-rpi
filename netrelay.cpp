//
// netrelay.cpp — Réseau et fermetures de fichiers du cœur 1 exécutés sur le cœur 0
//
// Écrit pour le port (voir netrelay.h). Chaque fonction détournée (__wrap_x)
// appelle l'originale (__real_x) directement sur le cœur 0, ou la confie au
// cœur 0 depuis le cœur 1 : une seule demande à la fois (seul le cœur 1 en
// fait), le cœur 1 attend activement la réponse.
//
// circle-newlib ne gère pas O_NONBLOCK sur les sockets (fcntl répond
// ENOSYS) ; le modem en a besoin pour lire sans attendre. Le relais retient
// donc l'option pour chaque socket et lit alors par recv (MSG_DONTWAIT).
//
// Connexions TCP établies (connect, accept) : pas de relais pour les données.
// Sans cadence, l'ACIA interroge le modem toutes les 32 cycles (poll, read) :
// des dizaines de milliers d'allers-retours par seconde. Le cœur 0 lit donc
// lui-même chaque connexion (recv non bloquant) dans une file, et envoie ce
// que le cœur 1 a mis dans une autre ; le cœur 1 ne consulte que ces files
// (un écrivain, un lecteur, sans verrou). poll, read et write sur ces sockets
// ne quittent plus le cœur 1. La pile TCP de Circle rend un segment entier à
// chaque lecture, le reste étant perdu (circle/net/socket.h : « at least
// FRAME_BUFFER_SIZE ») : le cœur 0 ne lit que si la file a cette place. File
// pleine, il ne lit plus et la fenêtre TCP retient l'émetteur.
//
#include "netrelay.h"

#include <circle/multicore.h>
#include <circle/netdevice.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <string.h>

#define MAX_FDS		64

static volatile bool s_bNetReady;
static unsigned char s_FdFlags[MAX_FDS];	// SOCKET, NONBLOCK, STREAM
#define FD_SOCKET	0x01
#define FD_NONBLOCK	0x02
#define FD_STREAM	0x04	// SOCK_STREAM

static bool IsSocket (int fd)
{
	return fd >= 0 && fd < MAX_FDS && (s_FdFlags[fd] & FD_SOCKET);
}

// Files d'une connexion TCP. Compteurs libres (différence = remplissage).
#define RX_SIZE		8192	// puissances de 2
#define TX_SIZE		4096

struct TStream
{
	volatile bool bActive;		// le cœur 0 lit et envoie (connexion établie)

	// Réception : le cœur 0 écrit, le cœur 1 lit
	volatile unsigned nRxIn, nRxOut;
	volatile bool bRxEnd;		// plus rien ne viendra : nRxErrno (0 = fin)
	volatile int nRxErrno;
	unsigned char Rx[RX_SIZE];

	// Émission : le cœur 1 écrit, le cœur 0 envoie
	volatile unsigned nTxIn, nTxOut;
	volatile int nTxErrno;		// échec d'un envoi, rendu au write suivant
	unsigned char Tx[TX_SIZE];
};
static TStream *s_pStream[MAX_FDS];
static unsigned s_nActiveStreams;	// cœur 0

// La file de fd, pour un appel du cœur 1 (le cœur 0 passe par les originales)
static TStream *Stream (int fd)
{
	if (!IsSocket (fd) || CMultiCoreSupport::ThisCore () == 0)
	{
		return 0;
	}
	TStream *p = s_pStream[fd];
	return p != 0 && p->bActive ? p : 0;
}

// ── Demande au cœur 0 ────────────────────────────────────────────────────

struct TRelayRequest
{
	long (*pCall) (void *pArgs);
	void *pArgs;
	long nResult;
	int nErrno;
};

static TRelayRequest * volatile s_pRequest;
static volatile bool s_bDone;

static long Relay (long (*pCall) (void *), void *pArgs)
{
	if (CMultiCoreSupport::ThisCore () == 0)
	{
		return pCall (pArgs);
	}

	TRelayRequest Request = {pCall, pArgs, 0, 0};
	s_bDone = false;
	DataMemBarrier ();
	s_pRequest = &Request;
	while (!s_bDone)
	{
		// attente active : le cœur 0 exécute l'appel
	}
	DataMemBarrier ();

	errno = Request.nErrno;
	return Request.nResult;
}

static void ServiceStreams (void);

void NetRelayService (void)
{
	TRelayRequest *pRequest = s_pRequest;
	if (pRequest != 0)
	{
		DataMemBarrier ();

		errno = 0;
		pRequest->nResult = pRequest->pCall (pRequest->pArgs);
		pRequest->nErrno = errno;

		s_pRequest = 0;
		DataMemBarrier ();
		s_bDone = true;
	}

	if (s_nActiveStreams > 0)
	{
		ServiceStreams ();
	}
}

void NetRelaySetReady (void)
{
	s_bNetReady = true;
}

// ── Files des connexions TCP ─────────────────────────────────────────────

extern "C"
{
ssize_t __real_recv (int fd, void *buf, size_t len, int flags);
ssize_t __real_send (int fd, const void *buf, size_t len, int flags);
}

// Cœur 0 : connexion établie, début de la lecture anticipée
static void StreamStart (int fd)
{
	if (fd < 0 || fd >= MAX_FDS || !(s_FdFlags[fd] & FD_STREAM))
	{
		return;
	}
	TStream *p = s_pStream[fd];
	if (p == 0)
	{
		p = s_pStream[fd] = new TStream;
	}
	p->nRxIn = p->nRxOut = 0;
	p->bRxEnd = false;
	p->nRxErrno = 0;
	p->nTxIn = p->nTxOut = 0;
	p->nTxErrno = 0;
	DataMemBarrier ();
	p->bActive = true;
	s_nActiveStreams++;
}

// Cœur 0 : envoie ce que le cœur 1 a mis dans la file d'émission
static void StreamSend (int fd, TStream *p)
{
	while (p->nTxErrno == 0)
	{
		unsigned nOut = p->nTxOut;
		unsigned nCount = p->nTxIn - nOut;
		if (nCount == 0)
		{
			break;
		}
		DataMemBarrier ();
		unsigned nPos = nOut & (TX_SIZE-1);
		if (nCount > TX_SIZE - nPos)
		{
			nCount = TX_SIZE - nPos;
		}
		int nResult = __real_send (fd, p->Tx + nPos, nCount, 0);
		if (nResult < 0)
		{
			p->nTxErrno = errno != 0 ? errno : EPIPE;
			break;
		}
		DataMemBarrier ();
		p->nTxOut = nOut + nResult;
	}
}

// Cœur 0 : lit un segment s'il y a de la place pour lui
static void StreamReceive (int fd, TStream *p)
{
	static unsigned char Segment[FRAME_BUFFER_SIZE];

	if (p->bRxEnd || RX_SIZE - (p->nRxIn - p->nRxOut) < FRAME_BUFFER_SIZE)
	{
		return;
	}

	int nResult = __real_recv (fd, Segment, sizeof Segment, MSG_DONTWAIT);
	if (nResult > 0)
	{
		unsigned nIn = p->nRxIn;
		for (int i = 0; i < nResult; i++)
		{
			p->Rx[(nIn + i) & (RX_SIZE-1)] = Segment[i];
		}
		DataMemBarrier ();
		p->nRxIn = nIn + nResult;
	}
	else if (nResult == 0 || (errno != EWOULDBLOCK && errno != EAGAIN))
	{
		p->nRxErrno = nResult == 0 ? 0 : errno;
		DataMemBarrier ();
		p->bRxEnd = true;		// fin de la connexion ou erreur
	}
}

static void ServiceStreams (void)
{
	for (int fd = 0; fd < MAX_FDS; fd++)
	{
		TStream *p = s_pStream[fd];
		if (p != 0 && p->bActive)
		{
			StreamSend (fd, p);
			StreamReceive (fd, p);
		}
	}
}

// Cœur 0, avant la fermeture : envoie le reste, arrête la lecture
static void StreamStop (int fd)
{
	TStream *p = fd >= 0 && fd < MAX_FDS ? s_pStream[fd] : 0;
	if (p == 0 || !p->bActive)
	{
		return;
	}
	StreamSend (fd, p);
	p->bActive = false;
	s_nActiveStreams--;
}

// Cœur 1 : prêt à lire (données, fin ou erreur) ?
static bool StreamReadable (TStream *p)
{
	return p->nRxIn != p->nRxOut || p->bRxEnd;
}

// Cœur 1 : lecture dans la file ; attend si la socket est bloquante
static ssize_t StreamRead (int fd, TStream *p, void *buf, size_t len, int flags)
{
	if (len == 0)
	{
		return 0;
	}
	while (true)
	{
		bool bEnd = p->bRxEnd;		// avant les compteurs : fin posée après les données
		DataMemBarrier ();
		unsigned nOut = p->nRxOut;
		unsigned nCount = p->nRxIn - nOut;
		if (nCount > 0)
		{
			DataMemBarrier ();
			if (nCount > len)
			{
				nCount = len;
			}
			unsigned char *pBuf = (unsigned char *) buf;
			for (unsigned i = 0; i < nCount; i++)
			{
				pBuf[i] = p->Rx[(nOut + i) & (RX_SIZE-1)];
			}
			DataMemBarrier ();
			p->nRxOut = nOut + nCount;
			return nCount;
		}
		if (bEnd)
		{
			if (p->nRxErrno == 0)
			{
				return 0;
			}
			errno = p->nRxErrno;
			return -1;
		}
		if ((flags & MSG_DONTWAIT) || (s_FdFlags[fd] & FD_NONBLOCK))
		{
			errno = EWOULDBLOCK;
			return -1;
		}
		// socket bloquante : attente active, le cœur 0 remplit la file
	}
}

// Cœur 1 : écriture dans la file (attend la place) ; erreur d'un envoi
// précédent rendue ici, comme le ferait la pile TCP
static ssize_t StreamWrite (TStream *p, const void *buf, size_t len)
{
	const unsigned char *pBuf = (const unsigned char *) buf;
	size_t nDone = 0;
	while (nDone < len)
	{
		if (p->nTxErrno != 0)
		{
			if (nDone > 0)
			{
				break;
			}
			errno = p->nTxErrno;
			return -1;
		}
		unsigned nIn = p->nTxIn;
		unsigned nFree = TX_SIZE - (nIn - p->nTxOut);
		if (nFree == 0)
		{
			continue;		// file pleine : le cœur 0 envoie
		}
		DataMemBarrier ();
		if (nFree > len - nDone)
		{
			nFree = len - nDone;
		}
		for (unsigned i = 0; i < nFree; i++)
		{
			p->Tx[(nIn + i) & (TX_SIZE-1)] = pBuf[nDone + i];
		}
		DataMemBarrier ();
		p->nTxIn = nIn + nFree;
		nDone += nFree;
	}
	return nDone;
}

// ── Fonctions détournées ─────────────────────────────────────────────────

extern "C"
{

int __real_socket (int domain, int type, int protocol);
int __real_connect (int fd, const struct sockaddr *addr, socklen_t len);
int __real_bind (int fd, const struct sockaddr *addr, socklen_t len);
int __real_listen (int fd, int backlog);
int __real_accept (int fd, struct sockaddr *addr, socklen_t *len);
int __real_getsockname (int fd, struct sockaddr *addr, socklen_t *len);
int __real_getsockopt (int fd, int level, int name, void *value, socklen_t *len);
int __real_setsockopt (int fd, int level, int name, const void *value, socklen_t len);
int __real_poll (struct pollfd fds[], nfds_t nfds, int timeout);
int __real_getaddrinfo (const char *node, const char *service,
			const struct addrinfo *hints, struct addrinfo **res);
int __real__read (int fd, char *ptr, int len);
int __real__write (int fd, char *ptr, int len);
int __real__close (int fd);
int __real__fcntl (int fd, int cmd, ...);

// socket, accept : le descripteur obtenu est noté comme socket

struct TSocketArgs { int domain, type, protocol; };
static long CallSocket (void *p)
{
	TSocketArgs *a = (TSocketArgs *) p;
	return __real_socket (a->domain, a->type, a->protocol);
}
int __wrap_socket (int domain, int type, int protocol)
{
	if (!s_bNetReady)
	{
		errno = ENETDOWN;
		return -1;
	}
	TSocketArgs a = {domain, type, protocol};
	int fd = (int) Relay (CallSocket, &a);
	if (fd >= 0 && fd < MAX_FDS)
	{
		s_FdFlags[fd] = FD_SOCKET | (type == SOCK_STREAM ? FD_STREAM : 0);
	}
	return fd;
}

// connect, accept réussis : le cœur 0 commence la lecture anticipée
struct TAddrArgs { int fd; const struct sockaddr *addr; socklen_t len; };
static long CallConnect (void *p)
{
	TAddrArgs *a = (TAddrArgs *) p;
	int nResult = __real_connect (a->fd, a->addr, a->len);
	if (nResult == 0)
	{
		StreamStart (a->fd);
	}
	return nResult;
}
int __wrap_connect (int fd, const struct sockaddr *addr, socklen_t len)
{
	TAddrArgs a = {fd, addr, len};
	return (int) Relay (CallConnect, &a);
}

static long CallBind (void *p)
{
	TAddrArgs *a = (TAddrArgs *) p;
	return __real_bind (a->fd, a->addr, a->len);
}
int __wrap_bind (int fd, const struct sockaddr *addr, socklen_t len)
{
	TAddrArgs a = {fd, addr, len};
	return (int) Relay (CallBind, &a);
}

struct TListenArgs { int fd, backlog; };
static long CallListen (void *p)
{
	TListenArgs *a = (TListenArgs *) p;
	return __real_listen (a->fd, a->backlog);
}
int __wrap_listen (int fd, int backlog)
{
	TListenArgs a = {fd, backlog};
	return (int) Relay (CallListen, &a);
}

struct TAddrOutArgs { int fd; struct sockaddr *addr; socklen_t *len; };
static long CallAccept (void *p)
{
	TAddrOutArgs *a = (TAddrOutArgs *) p;
	int nNew = __real_accept (a->fd, a->addr, a->len);
	if (nNew >= 0 && nNew < MAX_FDS)
	{
		s_FdFlags[nNew] = FD_SOCKET | FD_STREAM;
		StreamStart (nNew);
	}
	return nNew;
}
int __wrap_accept (int fd, struct sockaddr *addr, socklen_t *len)
{
	TAddrOutArgs a = {fd, addr, len};
	return (int) Relay (CallAccept, &a);
}

static long CallGetSockName (void *p)
{
	TAddrOutArgs *a = (TAddrOutArgs *) p;
	return __real_getsockname (a->fd, a->addr, a->len);
}
int __wrap_getsockname (int fd, struct sockaddr *addr, socklen_t *len)
{
	TAddrOutArgs a = {fd, addr, len};
	return (int) Relay (CallGetSockName, &a);
}

struct TSockOptArgs { int fd, level, name; void *value; socklen_t len; socklen_t *plen; };
static long CallGetSockOpt (void *p)
{
	TSockOptArgs *a = (TSockOptArgs *) p;
	return __real_getsockopt (a->fd, a->level, a->name, a->value, a->plen);
}
int __wrap_getsockopt (int fd, int level, int name, void *value, socklen_t *len)
{
	TSockOptArgs a = {fd, level, name, value, 0, len};
	return (int) Relay (CallGetSockOpt, &a);
}
static long CallSetSockOpt (void *p)
{
	TSockOptArgs *a = (TSockOptArgs *) p;
	return __real_setsockopt (a->fd, a->level, a->name, a->value, a->len);
}
int __wrap_setsockopt (int fd, int level, int name, const void *value, socklen_t len)
{
	TSockOptArgs a = {fd, level, name, (void *) value, len, 0};
	return (int) Relay (CallSetSockOpt, &a);
}

// recv, send : par les files sur une connexion établie, sinon relayés
struct TIOArgs { int fd; void *buf; size_t len; int flags; };
static long CallRecv (void *p)
{
	TIOArgs *a = (TIOArgs *) p;
	return __real_recv (a->fd, a->buf, a->len, a->flags);
}
ssize_t __wrap_recv (int fd, void *buf, size_t len, int flags)
{
	TStream *pStream = Stream (fd);
	if (pStream != 0)
	{
		return StreamRead (fd, pStream, buf, len, flags);
	}
	if (IsSocket (fd) && (s_FdFlags[fd] & FD_NONBLOCK))
	{
		flags |= MSG_DONTWAIT;
	}
	TIOArgs a = {fd, buf, len, flags};
	return Relay (CallRecv, &a);
}
static long CallSend (void *p)
{
	TIOArgs *a = (TIOArgs *) p;
	return __real_send (a->fd, a->buf, a->len, a->flags);
}
ssize_t __wrap_send (int fd, const void *buf, size_t len, int flags)
{
	TStream *pStream = Stream (fd);
	if (pStream != 0)
	{
		return StreamWrite (pStream, buf, len);
	}
	TIOArgs a = {fd, (void *) buf, len, flags};
	return Relay (CallSend, &a);
}

// poll : réponse sur place si tous les descripteurs sont des connexions
// établies (cas du modem, qui scrute sa socket) ; sinon relayé
static bool PollStreams (struct pollfd fds[], nfds_t nfds, int *pReady)
{
	int nReady = 0;
	for (nfds_t i = 0; i < nfds; i++)
	{
		TStream *p = Stream (fds[i].fd);
		if (p == 0)
		{
			return false;
		}
		fds[i].revents = 0;
		if ((fds[i].events & POLLIN) && StreamReadable (p))
		{
			fds[i].revents |= POLLIN;
		}
		if ((fds[i].events & POLLOUT)
		    && (p->nTxErrno != 0 || p->nTxIn - p->nTxOut < TX_SIZE))
		{
			fds[i].revents |= POLLOUT;
		}
		if (p->nTxErrno != 0)
		{
			fds[i].revents |= POLLERR;
		}
		if (fds[i].revents != 0)
		{
			nReady++;
		}
	}
	*pReady = nReady;
	return true;
}

struct TPollArgs { struct pollfd *fds; nfds_t nfds; int timeout; };
static long CallPoll (void *p)
{
	TPollArgs *a = (TPollArgs *) p;
	return __real_poll (a->fds, a->nfds, a->timeout);
}
int __wrap_poll (struct pollfd fds[], nfds_t nfds, int timeout)
{
	int nReady;
	if (nfds > 0 && PollStreams (fds, nfds, &nReady))
	{
		unsigned nStart = CTimer::GetClockTicks ();
		while (nReady == 0
		       && (timeout < 0 || CTimer::GetClockTicks () - nStart < (unsigned) timeout * 1000))
		{
			PollStreams (fds, nfds, &nReady);	// attente active
		}
		return nReady;
	}

	TPollArgs a = {fds, nfds, timeout};
	return (int) Relay (CallPoll, &a);
}

struct TAddrInfoArgs { const char *node, *service; const struct addrinfo *hints; struct addrinfo **res; };
static long CallGetAddrInfo (void *p)
{
	TAddrInfoArgs *a = (TAddrInfoArgs *) p;
	return __real_getaddrinfo (a->node, a->service, a->hints, a->res);
}
int __wrap_getaddrinfo (const char *node, const char *service,
			const struct addrinfo *hints, struct addrinfo **res)
{
	if (!s_bNetReady)
	{
		return EAI_AGAIN;
	}
	TAddrInfoArgs a = {node, service, hints, res};
	return (int) Relay (CallGetAddrInfo, &a);
}

// read, write, close, fcntl de newlib (bas niveau _read...) : seuls les
// sockets sont relayés (read et write par les files sur une connexion
// établie), sauf _close, relayé pour tout descripteur (il rend la main à
// l'ordonnanceur).

struct TRWArgs { int fd; char *ptr; int len; };
int __wrap__read (int fd, char *ptr, int len)
{
	if (!IsSocket (fd))
	{
		return __real__read (fd, ptr, len);
	}
	return (int) __wrap_recv (fd, ptr, len, 0);
}

static long CallWrite (void *p)
{
	TRWArgs *a = (TRWArgs *) p;
	return __real__write (a->fd, a->ptr, a->len);
}
int __wrap__write (int fd, char *ptr, int len)
{
	if (!IsSocket (fd))
	{
		return __real__write (fd, ptr, len);
	}
	TStream *pStream = Stream (fd);
	if (pStream != 0)
	{
		return (int) StreamWrite (pStream, ptr, len);
	}
	TRWArgs a = {fd, ptr, len};
	return (int) Relay (CallWrite, &a);
}

// close : le cœur 0 envoie le reste de la file et arrête la lecture avant
static long CallClose (void *p)
{
	int fd = *(int *) p;
	StreamStop (fd);
	return __real__close (fd);
}
int __wrap__close (int fd)
{
	int nResult = (int) Relay (CallClose, &fd);
	if (fd >= 0 && fd < MAX_FDS)
	{
		s_FdFlags[fd] = 0;
	}
	return nResult;
}

int __wrap__fcntl (int fd, int cmd, ...)
{
	va_list args;
	va_start (args, cmd);
	int arg = va_arg (args, int);
	va_end (args);

	if (IsSocket (fd))
	{
		// Seuls F_GETFL et F_SETFL (O_NONBLOCK) sont utilisés par le modem
		if (cmd == F_GETFL)
		{
			return O_RDWR | ((s_FdFlags[fd] & FD_NONBLOCK) ? O_NONBLOCK : 0);
		}
		if (cmd == F_SETFL)
		{
			if (arg & O_NONBLOCK)
			{
				s_FdFlags[fd] |= FD_NONBLOCK;
			}
			else
			{
				s_FdFlags[fd] &= ~FD_NONBLOCK;
			}
			return 0;
		}
	}

	return __real__fcntl (fd, cmd, arg);
}

}
