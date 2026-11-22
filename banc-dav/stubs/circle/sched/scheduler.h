#ifndef _banc_sched_h
#define _banc_sched_h
#include <circle/sched/task.h>
class CScheduler
{
public:
	static CScheduler *Get (void);
	void Yield (void);
	void MsSleep (unsigned ms);
	bool EnumerateTasks (bool (*pCallback) (CTask *, const char *, TTaskState, TTaskFlags, void *), void *pParam);
};
#endif
