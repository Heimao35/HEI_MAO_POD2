#pragma once
/* Host-only minimal FatFs shim for the video MJPEG regression.
 *
 * Production code now talks to FatFs directly so that FSIZE_t (32-bit
 * unsigned) can address files up to 4 GiB-1. The host build has no diskio or
 * ff.c; the test supplies stdio-backed implementations of these functions and
 * keeps exercising the real parser with the 460x460 fixture. */
#include <stdint.h>

typedef unsigned int UINT;
typedef uint32_t FSIZE_t;   /* matches DWORD when FF_FS_EXFAT == 0 */
typedef uint8_t BYTE;
typedef int FRESULT;

#define FR_OK 0
#define FA_READ 0x01U
#define AM_DIR 0x10U

typedef struct {
    void *fp;               /* stdio FILE * in the test double */
    FSIZE_t objsize;
} FIL;

typedef struct {
    FSIZE_t fsize;
    BYTE fattrib;
} FILINFO;

FRESULT f_open(FIL *fp, const char *path, BYTE mode);
FRESULT f_close(FIL *fp);
FRESULT f_read(FIL *fp, void *buf, UINT btr, UINT *br);
FRESULT f_lseek(FIL *fp, FSIZE_t ofs);
FRESULT f_stat(const char *path, FILINFO *fno);
FSIZE_t f_tell(FIL *fp);
/* matches the real FatFs inline macro on FILINFO/FIL.obj.objsize */
#define f_size(fp) ((fp)->objsize)
