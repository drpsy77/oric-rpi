#ifndef _banc_socket_h
#define _banc_socket_h
#include <circle/types.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/ipaddress.h>
class CSocket
{
public:
	CSocket (CNetSubSystem *, int);
	CSocket (int fd, const u8 *pIP);
	~CSocket (void);
	int Bind (u16 nPort);
	int Listen (unsigned nBackLog = 4);
	CSocket *Accept (CIPAddress *pIP, u16 *pPort);
	int Send (const void *p, unsigned n, int nFlags);
	int Receive (void *p, unsigned n, int nFlags);
	int SendTo (const void *, unsigned, int, const CIPAddress &, u16) { return -1; }
	int SetOptionReceiveTimeout (unsigned us);
	int SetOptionSendTimeout (unsigned us);
	const u8 *GetForeignIP (void) const { return m_IP; }
private:
	int m_fd;
	u8 m_IP[4];
};
#endif
