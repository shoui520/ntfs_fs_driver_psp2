/*
 * ntfsfs: NTFS for PS Vita storage, via libntfs-3g.
 *
 * Registers an "ntfs" VFS and hooks ksceIoMount().  When the stock exfat mount
 * of a removable device fails, the hook points that device's iofilemgr mount
 * table entry at "ntfs" and retries, so the normal mount path (mount daemon,
 * waiters, notifications) runs unchanged; afterwards the entry is restored so
 * exFAT media keep working.
 *
 * iofilemgr (3.65) keeps that table in its data segment: 32 entries of 0x38
 * bytes, { id, SceVfsMountParam (root_path, blockdev, fs_type|opt<<16,
 * mnt_flags, vfs_name, data, misc, vops), ... } at 0x81022b30, read by the
 * ksceIoMount worker at 0x81017ea8.  See docs/RE-3.65.md.
 */
#include <errno.h>
#include <string.h>

#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/iofilemgr.h>
#include <taihen.h>

#include "ntfsfs.h"

/* taihenModuleUtils (taiHEN >= 0.11) */
int module_get_offset(SceUID pid, SceUID modid, int segidx, size_t offset, uintptr_t *addr);

#define IOFILEMGR_TEXT_SIZE  0x20980 /* 3.65 */
#define IOFILEMGR_DATA_SIZE  0x2238  /* 3.65 */
#define MOUNT_ENTRY_SIZE     0x38
#define MOUNT_ENTRY_VFS_NAME 0x14
#define MOUNT_ENTRY_MISC     0x1c /* SceVfsMountData: assign, fs, blockdev, no_part, id */

/* Removable devices whose exfat mount may be retried as NTFS. */
static const int g_targets[] = {
	0xF00, /* uma0: USB mass storage */
	0x100, /* sd0:  external SD (dev units / PSTV) */
	0xA00, /* grw0: game card slot (SD2Vita) */
};

static tai_hook_ref_t g_mount_ref;
static SceUID g_mount_hook = -1;
static SceUID g_hook_lock = -1;

static uintptr_t g_text, g_data;
static char g_ntfs_name[] = "ntfs";

static int in_text(uintptr_t p, size_t n)
{
	return p >= g_text && p + n <= g_text + IOFILEMGR_TEXT_SIZE;
}

/*
 * Returns the vfs_name slot of the mount table entry for id, or NULL; *whole
 * gets the entry's whole-device blockdev (SceVfsMountData.blockdev_name_no_part,
 * e.g. "sdstor0:uma-lp-act-entire") when it has one.
 */
static const char **find_vfs_name_slot(int id, const char **whole)
{
	uintptr_t off;

	if (!g_data)
		return NULL;
	for (off = 0; off + MOUNT_ENTRY_SIZE <= IOFILEMGR_DATA_SIZE; off += 4) {
		uint32_t *e = (uint32_t *)(g_data + off);
		const char *root, *name;

		if (e[0] != (uint32_t)id)
			continue;
		root = (const char *)e[1];
		name = (const char *)e[MOUNT_ENTRY_VFS_NAME / 4];
		if (!in_text((uintptr_t)root, 2) || !in_text((uintptr_t)name, 6))
			continue;
		if (root[0] != '/' || strcmp(name, "exfat") != 0)
			continue;
		if (whole) {
			const uint32_t *misc = (const uint32_t *)e[MOUNT_ENTRY_MISC / 4];

			*whole = NULL;
			if (in_text((uintptr_t)misc, 0x14) && in_text(misc[3], 9))
				*whole = (const char *)misc[3];
		}
		return (const char **)&e[MOUNT_ENTRY_VFS_NAME / 4];
	}
	return NULL;
}

static int is_target(int id)
{
	unsigned i;

	for (i = 0; i < sizeof(g_targets) / sizeof(g_targets[0]); i++)
		if (g_targets[i] == id)
			return 1;
	return 0;
}

/*
 * Retries a failed mount of id with the "ntfs" VFS, with the arguments of the
 * original ksceIoMount call.  path, when given, is the block device to mount
 * (ksceIoMount takes one for ids >= 0x100, e.g. from _vshIoMount); otherwise
 * the mount table entry's own blockdev is used.  Returns
 * NTFSFS_ERR_NOT_NTFS when the device holds no NTFS volume or id has no
 * mount table entry.
 */
int ntfsfs_try_mount(int id, const char *path, int permission, int a4, int a5, int a6)
{
	const char **slot, *orig, *whole = NULL;
	int r, r2;

	slot = find_vfs_name_slot(id, &whole);
	if (!slot) {
		NTFSFS_LOG("no mount table entry for 0x%X\n", id);
		return NTFSFS_ERR_NOT_NTFS;
	}
	ksceKernelLockMutex(g_hook_lock, 1, NULL);
	orig = *slot;
	*slot = g_ntfs_name;
	r = TAI_CONTINUE(int, g_mount_ref, id, path, permission, a4, a5, a6);
	/*
	 * With a valid MBR sdstor exposes only partition 1 ("pp-act-a") and
	 * the whole device, and iofilemgr falls back to the whole device only
	 * when partition 1 does not exist (vfsMount 0x81004a0c).  NTFS in a
	 * later partition is found by mounting the whole device, whose MBR
	 * ntfsfs_probe() reads.  A block device the caller named is used as
	 * it is.
	 */
	if (r < 0 && r != (int)0x80010011 && !path && whole) {
		r2 = TAI_CONTINUE(int, g_mount_ref, id, whole, permission, a4, a5, a6);
		/* keep the result of the attempt that found NTFS */
		if (r2 >= 0 || r == NTFSFS_ERR_NOT_NTFS || r2 != NTFSFS_ERR_NOT_NTFS)
			r = r2;
	}
	*slot = orig;
	ksceKernelUnlockMutex(g_hook_lock, 1);
	return r;
}

static int ksceIoMount_hook(int id, const char *path, int permission, int a4, int a5, int a6)
{
	int r, r2;

	r = TAI_CONTINUE(int, g_mount_ref, id, path, permission, a4, a5, a6);
	/* 0x80010011: already mounted */
	if (r >= 0 || r == (int)0x80010011 || !is_target(id))
		return r;
	r2 = ntfsfs_try_mount(id, path, permission, a4, a5, a6);
	if (r2 >= 0) {
		NTFSFS_LOG("mounted 0x%X%s%s as NTFS\n", id, path ? " from " : "", path ? path : "");
		return r2;
	}
	NTFSFS_LOG("mount 0x%X: exfat 0x%08X, ntfs 0x%08X\n", id, r, r2);
	/*
	 * exfat fails an NTFS volume with 0x80010005 (its -0x22: no FAT sector
	 * count), which says nothing about why NTFS failed.  Its error only
	 * stands when there is no NTFS on the device.
	 */
	return r2 == NTFSFS_ERR_NOT_NTFS ? r : r2;
}

static int locate_iofilemgr(void)
{
	tai_module_info_t info;
	int r;

	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	r = taiGetModuleInfoForKernel(KERNEL_PID, "SceIofilemgr", &info);
	if (r < 0)
		return r;
	r = module_get_offset(KERNEL_PID, info.modid, 0, 0, &g_text);
	if (r < 0)
		return r;
	return module_get_offset(KERNEL_PID, info.modid, 1, 0, &g_data);
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp)
{
	unsigned i;
	int r;

	(void)args; (void)argp;
	if ((r = ntfsfs_heap_init()) < 0)
		goto fail;
	if ((r = ntfsfs_worker_start()) < 0)
		goto fail;
	g_hook_lock = ksceKernelCreateMutex("SceNtfsfsHook", 0, 0, NULL);
	if (g_hook_lock < 0) {
		r = g_hook_lock;
		goto fail;
	}
	ntfsfs_acl_init();
	if ((r = locate_iofilemgr()) < 0) {
		NTFSFS_LOG("cannot locate SceIofilemgr segments: 0x%08X\n", r);
		goto fail;
	}
	if ((r = ntfsfs_vfs_register()) < 0) {
		NTFSFS_LOG("ksceVfsAddVfs: 0x%08X\n", r);
		goto fail;
	}
	g_mount_hook = taiHookFunctionExportForKernel(KERNEL_PID, &g_mount_ref,
			"SceIofilemgr", 0x40FD29C7, 0xD070BC48, ksceIoMount_hook);
	if (g_mount_hook < 0) {
		r = g_mount_hook;
		NTFSFS_LOG("hook ksceIoMount: 0x%08X\n", r);
		ntfsfs_vfs_unregister();
		goto fail;
	}
	/* Devices that were already present and failed their exfat mount. */
	for (i = 0; i < sizeof(g_targets) / sizeof(g_targets[0]); i++)
		if (ntfsfs_try_mount(g_targets[i], NULL, 0, 0, 0, 0) >= 0)
			NTFSFS_LOG("mounted 0x%X as NTFS\n", g_targets[i]);
	NTFSFS_LOG("ready\n");
	return SCE_KERNEL_START_SUCCESS;
fail:
	NTFSFS_LOG("start failed: 0x%08X\n", r);
	ntfsfs_worker_stop();
	ntfsfs_heap_fini();
	return SCE_KERNEL_START_FAILED;
}

int module_stop(SceSize args, void *argp)
{
	(void)args; (void)argp;
	/* Unloading with live NTFS mounts would leave dangling vop tables. */
	return SCE_KERNEL_STOP_CANCEL;
}
