#ifndef _banc_ip_h
#define _banc_ip_h
#include <circle/types.h>
#include <circle/string.h>
#include <string.h>
class CIPAddress
{
public:
	CIPAddress (void) : m_bSet (false) {}
	CIPAddress (const u8 *p) : m_bSet (true) { memcpy (m_a, p, 4); }
	void Set (const u8 *p) { memcpy (m_a, p, 4); m_bSet = true; }
	void Set (const CIPAddress &r) { *this = r; }
	bool IsSet (void) const { return m_bSet; }
	bool operator== (const CIPAddress &r) const { return memcmp (m_a, r.m_a, 4) == 0; }
	bool operator!= (const CIPAddress &r) const { return !(*this == r); }
	void Format (CString *p) const { p->Format ("%u.%u.%u.%u", m_a[0], m_a[1], m_a[2], m_a[3]); }
	const u8 *Get (void) const { return m_a; }
private:
	u8 m_a[4];
	bool m_bSet;
};
#endif
