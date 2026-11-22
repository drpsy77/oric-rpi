#define DataMemBarrier() __sync_synchronize ()
#define DataSyncBarrier() __sync_synchronize ()
void WaitForEvent (void);
void SendEvent (void);
