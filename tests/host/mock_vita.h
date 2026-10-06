#ifndef MOCK_VITA_H
#define MOCK_VITA_H

#include <psp2kern/vfs.h>
#include "ntfsfs.h"

extern int mock_img_fd;
extern long mock_dev_reads, mock_dev_writes;
extern SceVfsInfo *mock_vfs;
extern int mock_locked;
extern unsigned mock_sector;

/* the thread running the test (iofilemgr's caller) and the ntfsfs worker */
#define MOCK_THREAD_CALLER 0x40010001
#define MOCK_THREAD_WORKER 0x40010002
extern SceUID mock_thread;

SceVfsVnode *mock_blockdev(void);

#endif
