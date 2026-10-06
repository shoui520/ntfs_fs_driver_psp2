#ifndef NTFSFS_H
#define NTFSFS_H

#include <stdint.h>

/* glibc (host test build) defines these as macros; SceIoStat uses the names */
#undef st_atime
#undef st_ctime
#undef st_mtime
#include <psp2kern/types.h>
#include <psp2kern/vfs.h>

#include "ntfsfs_log.h"

/* smallest device sector; the real one (512 or 4096) is probed at mount */
#define NTFSFS_SECTOR 512

/* Block device opened by vfs_mount; all I/O goes through ksceVopPread/Pwrite. */
typedef struct ntfsfs_blk {
	SceVfsVnode *vp;      /* block device vnode (mnt->mnt_vnode) */
	SceVfsFile *file;     /* VFS file opened on it */
	SceUID file_uid;
	int rdonly;
	uint32_t sector;      /* device sector size, I/O alignment */
	uint32_t serial;      /* NTFS volume serial number (low 32 bits) */
	uint64_t part_off;    /* byte offset of the NTFS partition */
	int64_t pos;          /* for the seek/read/write device ops */
	uint8_t *bounce;      /* NTFSFS_BOUNCE bytes, sector aligned I/O */
} ntfsfs_blk;

#define NTFSFS_BOUNCE 0x10000

struct ntfs_device_operations;
extern struct ntfs_device_operations ntfsfs_dev_ops;

/* Raw sector access on the block device (offset/len in bytes, sector-aligned). */
int ntfsfs_blk_read(ntfsfs_blk *b, uint64_t off, void *buf, uint32_t len);
int ntfsfs_blk_write(ntfsfs_blk *b, uint64_t off, const void *buf, uint32_t len);

/* Finds the NTFS boot sector: at offset 0 or behind an MBR partition entry. */
int ntfsfs_probe(ntfsfs_blk *b);

/* Runs fn(arg) on the ntfsfs worker thread and returns its result. */
typedef int (*ntfsfs_job_fn)(void *arg);
int ntfsfs_worker_start(void);
void ntfsfs_worker_stop(void);
int ntfsfs_call(ntfsfs_job_fn fn, void *arg);

/*
 * The thread blocked in ntfsfs_call() on the job the worker is running, or -1.
 * Locks it holds cannot be taken by the worker until the job returns.
 */
SceUID ntfsfs_caller_thread(void);

int ntfsfs_heap_init(void);
void ntfsfs_heap_fini(void);

/* VFS registration (vfs.c) */
int ntfsfs_vfs_register(void);
int ntfsfs_vfs_unregister(void);

/* Mount helper used by the ksceIoMount hook (main.c) */
int ntfsfs_try_mount(int id, const char *path, int permission, int a4, int a5, int a6);

/*
 * File attributes as SceSblACMgr sees them (acl.c): the FAT bits READONLY,
 * HIDDEN and SYSTEM, which NTFS shares.
 */
#define NTFSFS_ATTR_RO   0x01
#define NTFSFS_ATTR_MASK 0x07
void ntfsfs_acl_init(void);
int ntfsfs_acl_from_mode(int mode, unsigned *attr);
int ntfsfs_acl_inherit(unsigned st_attr, unsigned *attr);
int ntfsfs_acl_check_read(unsigned attr);
int ntfsfs_acl_devinfo_ok(const char *assign);

/* errno -> SCE error code (SCE_ERROR_ERRNO_* share newlib's numbering) */
#define NTFSFS_ERR(e) ((int)(0x80010000u | ((unsigned)(e) & 0xffff)))

/*
 * ntfsfs_probe() found no NTFS volume on the device.  The ksceIoMount hook
 * then reports the stock exfat error instead of this one.
 */
#ifndef EFTYPE
#define EFTYPE 79 /* newlib */
#endif
#define NTFSFS_ERR_NOT_NTFS NTFSFS_ERR(EFTYPE)

#endif
