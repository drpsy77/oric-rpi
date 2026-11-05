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
// La pile TCP de Circle rend un segment entier à chaque lecture ; ce qui ne
// tient pas dans le tampon de l'appelant est perdu (circle/net/socket.h :
// « at least FRAME_BUFFER_SIZE, otherwise data may get lost »). Le modem lit
// par petits morceaux (la place libre de sa file). Le relais lit donc chaque
// segment dans un tampon par socket et le rend morceau par morceau ; poll()
// signale ce qui y reste.
//
#include "netrelay.h"

#include <circle/multicore.h>
#include <circle/netdevice.h>
#include <circle/synchronize.h>
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
static unsigned char s_FdFlags[MAX_FDS];	// SOCKET, NONBLOCK
#define FD_SOCKET	0x01
#define FD_NONBLOCK	0x02

static bool IsSocket (int fd)
{
	return fd >= 0 && fd < MAX_FDS && (s_FdFlags[fd] & FD_SOCKET);
}

// Segments reçus, rendus morceau par morceau (un seul lecteur : le cœur 1)
struct TRxBuffer
{
	unsigned nHead, nTail;
	unsigned char Data[FRAME_BUFFER_SIZE];
};
static TRxBuffer *s_pRxBuffer[MAX_FDS];

static unsigned RxPending (int fd)
{
	TRxBuffer *p = IsSocket (fd) ? s_pRxBuffer[fd] : 0;
	return p != 0 ? p->nTail - p->nHead : 0;
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

void NetRelayService (void)
{
	TRelayRequest *pRequest = s_pRequest;
	if (pRequest == 0)
	{
		return;
	}
	DataMemBarrier ();

	errno = 0;
	pRequest->nResult = pRequest->pCall (pRequest->pArgs);
	pRequest->nErrno = errno;

	s_pRequest = 0;
	DataMemBarrier ();
	s_bDone = true;
}

void NetRelaySetReady (void)
{
	s_bNetReady = true;
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
ssize_t __real_recv (int fd, void *buf, size_t len, int flags);
ssize_t __real_send (int fd, const void *buf, size_t len, int flags);
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
		s_FdFlags[fd] = FD_SOCKET;
		if (s_pRxBuffer[fd] == 0)
		{
			s_pRxBuffer[fd] = new TRxBuffer;
		}
		s_pRxBuffer[fd]->nHead = s_pRxBuffer[fd]->nTail = 0;
	}
	return fd;
}

struct TAddrArgs { int fd; const struct sockaddr *addr; socklen_t len; };
static long CallConnect (void *p)
{
	TAddrArgs *a = (TAddrArgs *) p;
	return __real_connect (a->fd, a->addr, a->len);
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
	return __real_accept (a->fd, a->addr, a->len);
}
int __wrap_accept (int fd, struct sockaddr *addr, socklen_t *len)
{
	TAddrOutArgs a = {fd, addr, len};
	int nNew = (int) Relay (CallAccept, &a);
	if (nNew >= 0 && nNew < MAX_FDS)
	{
		s_FdFlags[nNew] = FD_SOCKET;
		if (s_pRxBuffer[nNew] == 0)
		{
			s_pRxBuffer[nNew] = new TRxBuffer;
		}
		s_pRxBuffer[nNew]->nHead = s_pRxBuffer[nNew]->nTail = 0;
	}
	return nNew;
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

struct TIOArgs { int fd; void *buf; size_t len; int flags; };
static long CallRecv (void *p)
{
	TIOArgs *a = (TIOArgs *) p;
	return __real_recv (a->fd, a->buf, a->len, a->flags);
}
// Lecture d'une socket par le tampon : segment suivant si le tampon est vide
static ssize_t ReadSocket (int fd, void *buf, size_t len, int flags)
{
	TRxBuffer *p = s_pRxBuffer[fd];
	if (p == 0)
	{
		TIOArgs a = {fd, buf, len, flags};
		return Relay (CallRecv, &a);
	}

	if (p->nHead == p->nTail)
	{
		TIOArgs a = {fd, p->Data, sizeof p->Data, flags};
		ssize_t nResult = Relay (CallRecv, &a);
		if (nResult <= 0)
		{
			return nResult;		// rien (EWOULDBLOCK), fin, ou erreur
		}
		p->nHead = 0;
		p->nTail = nResult;
	}

	size_t nCopy = p->nTail - p->nHead;
	if (nCopy > len)
	{
		nCopy = len;
	}
	memcpy (buf, p->Data + p->nHead, nCopy);
	p->nHead += nCopy;

	return nCopy;
}

ssize_t __wrap_recv (int fd, void *buf, size_t len, int flags)
{
	if (!IsSocket (fd))
	{
		TIOArgs a = {fd, buf, len, flags};
		return Relay (CallRecv, &a);
	}
	if (s_FdFlags[fd] & FD_NONBLOCK)
	{
		flags |= MSG_DONTWAIT;
	}
	return ReadSocket (fd, buf, len, flags);
}
static long CallSend (void *p)
{
	TIOArgs *a = (TIOArgs *) p;
	return __real_send (a->fd, a->buf, a->len, a->flags);
}
ssize_t __wrap_send (int fd, const void *buf, size_t len, int flags)
{
	TIOArgs a = {fd, (void *) buf, len, flags};
	return Relay (CallSend, &a);
}

struct TPollArgs { struct pollfd *fds; nfds_t nfds; int timeout; };
static long CallPoll (void *p)
{
	TPollArgs *a = (TPollArgs *) p;
	return __real_poll (a->fds, a->nfds, a->timeout);
}
int __wrap_poll (struct pollfd fds[], nfds_t nfds, int timeout)
{
	// Données déjà dans un tampon : prêtes, sans demander à la pile TCP
	int nReady = 0;
	for (nfds_t i = 0; i < nfds; i++)
	{
		fds[i].revents = 0;
		if ((fds[i].events & POLLIN) && RxPending (fds[i].fd) > 0)
		{
			fds[i].revents = POLLIN;
			nReady++;
		}
	}
	if (nReady > 0)
	{
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
// sockets sont relayés, sauf _close, relayé pour tout descripteur (il rend
// la main à l'ordonnanceur).

struct TRWArgs { int fd; char *ptr; int len; };
int __wrap__read (int fd, char *ptr, int len)
{
	if (!IsSocket (fd))
	{
		return __real__read (fd, ptr, len);
	}
	return (int) ReadSocket (fd, ptr, len, (s_FdFlags[fd] & FD_NONBLOCK) ? MSG_DONTWAIT : 0);
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
	TRWArgs a = {fd, ptr, len};
	return (int) Relay (CallWrite, &a);
}

static long CallClose (void *p)
{
	return __real__close (*(int *) p);
}
int __wrap__close (int fd)
{
	if (fd >= 0 && fd < MAX_FDS)
	{
		s_FdFlags[fd] = 0;
		if (s_pRxBuffer[fd] != 0)
		{
			s_pRxBuffer[fd]->nHead = s_pRxBuffer[fd]->nTail = 0;
		}
	}
	return (int) Relay (CallClose, &fd);
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
