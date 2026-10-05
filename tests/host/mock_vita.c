/*
 * Host-side stand-ins for the iofilemgr VFS API, so src/vfs.c and src/devio.c
 * can run against an NTFS image file.  The block device enforces the same
 * 512-byte alignment rule as a real Vita block device.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <psp2kern/vfs.h>
#include <psp2kern/kernel/rtc.h>

#include "mock_vita.h"

int mock_img_fd = -1;
long mock_dev_reads, mock_dev_writes;
SceVfsInfo *mock_vfs;
int mock_locked;
unsigned mock_sector = 512;  /* device sector size (NTFSFS_TEST_SECTOR) */

/* ---- block device ------------------------------------------------------- */

static int blk_dummy(void *a) { (void)a; return 0; }

static const SceVopTable blk_vops = {
	.vop_pread  = (void *)blk_dummy,
	.vop_pwrite = (void *)blk_dummy,
};

SceVfsVnode *mock_blockdev(void)
{
	SceVfsVnode *vp = calloc(1, sizeof(*vp));

	vp->core.ops = (SceVopTable *)&blk_vops;
	vp->core.type = SCE_VNODE_TYPE_CHRDEV;
	return vp;
}

int ksceVopOpen(SceVfsVnode *vp, SceVfsPath *path, int flags, SceVfsFile *file)
{
	(void)vp; (void)path; (void)flags; (void)file;
	return 0;
}

int ksceVopClose(SceVfsVnode *vp, SceVfsFile *file)
{
	(void)vp; (void)file;
	return 0;
}

int ksceVopPread(SceVfsVnode *vp, SceVfsFile *file, void *data, SceSize nbyte,
		 SceOff offset, SceSize *res)
{
	(void)file;
	assert(vp->core.ops == &blk_vops);
	assert(mock_locked > 0);
	if ((offset | nbyte) & 511) {
		fprintf(stderr, "unaligned device read %lld+%u\n", (long long)offset, nbyte);
		abort();
	}
	/* sdstor: offsets and lengths must be whole device sectors */
	if ((offset | nbyte) & (mock_sector - 1))
		return (int)0x80010022;
	mock_dev_reads++;
	*res = pread(mock_img_fd, data, nbyte, offset);
	return 0;
}

int ksceVopPwrite(SceVfsVnode *vp, SceVfsFile *file, const void *data, SceSize nbyte,
		  SceOff offset, SceSize *res)
{
	(void)file;
	assert(vp->core.ops == &blk_vops);
	assert(mock_locked > 0);
	if ((offset | nbyte) & 511) {
		fprintf(stderr, "unaligned device write %lld+%u\n", (long long)offset, nbyte);
		abort();
	}
	/* sdstor: offsets and lengths must be whole device sectors */
	if ((offset | nbyte) & (mock_sector - 1))
		return (int)0x80010022;
	mock_dev_writes++;
	*res = pwrite(mock_img_fd, data, nbyte, offset);
	return 0;
}

int ksceVopRead(SceVfsVnode *vp, SceVfsFile *f, void *d, SceSize n, SceSize *r)
{
	(void)vp; (void)f; (void)d; (void)n; (void)r;
	abort();
}

int ksceVopWrite(SceVfsVnode *vp, SceVfsFile *f, const void *d, SceSize n, SceSize *r)
{
	(void)vp; (void)f; (void)d; (void)n; (void)r;
	abort();
}

SceOff ksceVopLseek(SceVfsVnode *vp, SceVfsFile *f, SceOff o, int w)
{
	(void)vp; (void)f; (void)o; (void)w;
	abort();
}

int ksceVopCleanup(SceVfsVnode *vp, SceVfsFile *file)
{
	(void)vp; (void)file;
	return (int)0x80010030;
}

int ksceVopSync(SceVfsVnode *vp, SceVfsFile *file, int flags)
{
	(void)vp; (void)file; (void)flags;
	return fsync(mock_img_fd);
}

/* ---- vnodes / files ------------------------------------------------------- */

int vfsLockVnode(SceVfsVnode *vp)
{
	vp->vdlock.recursive_count++;
	mock_locked++;
	return 0;
}

int vfsUnlockVnode(SceVfsVnode *vp)
{
	assert(vp->vdlock.recursive_count > 0);
	vp->vdlock.recursive_count--;
	mock_locked--;
	return 0;
}

int vfsGetNewVnode(SceVfsMount *mnt, SceVopTable *vops, int unk, SceVfsVnode **vpp)
{
	SceVfsVnode *vp = calloc(1, sizeof(*vp));

	(void)unk;
	vp->core.ops = vops;
	vp->core.mnt = mnt;
	*vpp = vp;
	return 0;
}

int vfsFreeVnode(SceVfsVnode *vp)
{
	free(vp);
	return 0;
}

SceUID vfsAllocateFile(SceVfsVnode *vp, SceVfsFile **file, const char *name)
{
	(void)name;
	*file = calloc(1, sizeof(**file));
	(*file)->vp = vp;
	return 0x1234;
}

int vfsFreeFile(SceVfsVnode *vp, SceUID fd)
{
	(void)vp; (void)fd;
	return 0;
}

int ksceVfsAddVfs(SceVfsInfo *info)
{
	mock_vfs = info;
	return 0;
}

int ksceVfsDeleteVfs(const char *name, SceVfsInfo **info)
{
	(void)name;
	if (info)
		*info = mock_vfs;
	mock_vfs = NULL;
	return 0;
}

/* ---- misc kernel services -------------------------------------------------- */

int ksceKernelPrintf(const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vfprintf(stderr, fmt, ap);
	va_end(ap);
	return r;
}

int ntfsfs_call(ntfsfs_job_fn fn, void *arg)
{
	return fn(arg);
}

/* RTC tick: microseconds since 0001-01-01 (proleptic Gregorian). */
#define TICK_UNIX_EPOCH 62135596800000000ULL

int ksceRtcConvertTickToDateTime(SceDateTime *dst, SceRtcTick *src)
{
	time_t t = (time_t)((src->tick - TICK_UNIX_EPOCH) / 1000000ULL);
	struct tm tm;

	gmtime_r(&t, &tm);
	dst->year = tm.tm_year + 1900;
	dst->month = tm.tm_mon + 1;
	dst->day = tm.tm_mday;
	dst->hour = tm.tm_hour;
	dst->minute = tm.tm_min;
	dst->second = tm.tm_sec;
	dst->microsecond = (src->tick - TICK_UNIX_EPOCH) % 1000000ULL;
	return 0;
}

int ksceRtcConvertDateTimeToUnixTime(const SceDateTime *src, SceUInt64 *dst)
{
	struct tm tm;

	memset(&tm, 0, sizeof(tm));
	tm.tm_year = src->year - 1900;
	tm.tm_mon = src->month - 1;
	tm.tm_mday = src->day;
	tm.tm_hour = src->hour;
	tm.tm_min = src->minute;
	tm.tm_sec = src->second;
	*dst = (SceUInt64)timegm(&tm);
	return 0;
}

/* libc.c is not used on the host; glibc's allocator backs libntfs-3g. */
int ntfsfs_heap_init(void) { return 0; }
void ntfsfs_heap_fini(void) { }
