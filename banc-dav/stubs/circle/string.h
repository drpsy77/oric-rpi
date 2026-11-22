#ifndef _banc_string_h
#define _banc_string_h
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
class CString
{
public:
	CString (void) { m_s[0] = '\0'; }
	void Format (const char *f, ...) { va_list a; va_start (a, f); vsnprintf (m_s, sizeof m_s, f, a); va_end (a); }
	operator const char * (void) const { return m_s; }
	unsigned GetLength (void) const { return strlen (m_s); }
private:
	char m_s[1024];
};
#endif
