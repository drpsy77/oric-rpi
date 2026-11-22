// Banc natif : FatFs remplacé par des fichiers POSIX (banc.cpp)
#ifndef _banc_ff_h
#define _banc_ff_h
#include <stdint.h>
#include <stdio.h>
typedef unsigned int UINT;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef uint16_t WCHAR;
typedef char TCHAR;
typedef uint64_t FSIZE_t;
#define FF_MAX_LFN 255
typedef enum { FR_OK = 0, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE, FR_NO_PATH,
	FR_INVALID_NAME, FR_DENIED, FR_EXIST, FR_INVALID_OBJECT } FRESULT;
typedef struct { FILE *fp; FSIZE_t taille; } FIL;
typedef struct { void *d; char chemin[600]; } FF_DIR;
#define DIR FF_DIR
typedef struct { FSIZE_t fsize; WORD fdate, ftime; BYTE fattrib; TCHAR fname[FF_MAX_LFN + 1]; } FILINFO;
#define AM_DIR 0x10
#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_OPEN_EXISTING 0x00
#define FA_CREATE_ALWAYS 0x08
#define f_size(f) ((f)->taille)
FRESULT f_open (FIL *, const TCHAR *, BYTE);
FRESULT f_close (FIL *);
FRESULT f_read (FIL *, void *, UINT, UINT *);
FRESULT f_write (FIL *, const void *, UINT, UINT *);
FRESULT f_opendir (DIR *, const TCHAR *);
FRESULT f_closedir (DIR *);
FRESULT f_readdir (DIR *, FILINFO *);
FRESULT f_stat (const TCHAR *, FILINFO *);
FRESULT f_unlink (const TCHAR *);
FRESULT f_rename (const TCHAR *, const TCHAR *);
FRESULT f_mkdir (const TCHAR *);
WCHAR ff_oem2uni (WCHAR oem, WORD cp);
WCHAR ff_uni2oem (DWORD uni, WORD cp);
#endif
