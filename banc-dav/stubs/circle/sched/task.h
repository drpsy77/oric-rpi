#ifndef _banc_task_h
#define _banc_task_h
enum TTaskState { TaskStateReady, TaskStateTerminated };
enum TTaskFlags { TaskFlagNone = 0 };
class CTask
{
public:
	CTask (unsigned nStackSize = 0x8000, bool bSuspended = false);
	virtual ~CTask (void);
	virtual void Run (void) {}
	void SetName (const char *) {}
	const char *GetName (void) const { return "tache"; }
};
#endif
