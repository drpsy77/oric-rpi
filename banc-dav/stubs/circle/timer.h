#ifndef _banc_timer_h
#define _banc_timer_h
class CTimer
{
public:
	static CTimer *Get (void);
	int GetTimeZone (void) const;
};
#endif
