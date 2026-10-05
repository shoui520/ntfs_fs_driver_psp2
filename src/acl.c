/*
 * File attribute permissions, the way 3.65 exfatfs.skprx does them.
 *
 * iofilemgr hands every fs_type-1 mount's vnode acl_data[0] to SceSblACMgr as
 * a FAT attribute byte ({type 3, &attr, 1}): READONLY 1, HIDDEN 2, SYSTEM 4.
 * NTFS uses the same bit values, so ntfsfs stores the low NTFS attribute bits
 * there and calls the same ACMgr functions exfatfs calls:
 *   SceSblACMgrForDriver_6210D745  attribute from SCE_S_* mode (create, mkdir,
 *                                  chstat)          exfatfs 0x81000000
 *   SceSblACMgrForDriver_B12CEAA8  attribute from st_attr (chstat 0x10000)
 *                                                   exfatfs 0x81000050
 *   SceSblACMgrForDriver_BE5667C5  read check (getstat, dread)
 *                                                   exfatfs 0x8100010c
 *   SceSblACMgrForDriver_D7AD8471  who may read free space (devctl 0x3001)
 *                                                   exfatfs 0x8100978c
 * They run on the calling thread: ACMgr looks at its TLS pid and access
 * level.  VitaSDK has no stubs for these NIDs, so they are resolved at start.
 * See docs/acmgr-3.65/README.md#filesystem-attributes.
 */
#include <string.h>

#include <psp2kern/kernel/iofilemgr.h>

#include "ntfsfs.h"

#ifndef NTFSFS_HOST
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid,
			   uint32_t funcnid, uintptr_t *func);
#endif

#define ACMGR_FOR_DRIVER 0x9AD8E213
#define ACM_ERR_INVAL    ((int)0x800F0916)

typedef struct acm_ctx {
	int type;      /* 3: FAT-style attribute byte */
	uint8_t *data;
	int count;
} acm_ctx;

static int (*p_attr_from_mode)(SceUID pid, acm_ctx *ctx, int mode);
static int (*p_inherit)(SceUID pid, acm_ctx *dst, acm_ctx *src);
static int (*p_check_access)(SceUID pid, acm_ctx *ctx, int req);
static int (*p_check_devinfo)(SceUID pid, const char *assign);

void ntfsfs_acl_init(void)
{
#ifndef NTFSFS_HOST
	static const struct { uint32_t nid; void *slot; } fn[] = {
		{ 0x6210D745, &p_attr_from_mode },
		{ 0xB12CEAA8, &p_inherit },
		{ 0xBE5667C5, &p_check_access },
		{ 0xD7AD8471, &p_check_devinfo },
	};
	unsigned i;

	for (i = 0; i < sizeof(fn) / sizeof(fn[0]); i++) {
		uintptr_t f = 0;

		if (module_get_export_func(KERNEL_PID, "SceSblACMgr", ACMGR_FOR_DRIVER,
					   fn[i].nid, &f) < 0)
			NTFSFS_LOG("SceSblACMgrForDriver_%08X not found\n", (unsigned)fn[i].nid);
		*(uintptr_t *)fn[i].slot = f;
	}
#endif
}

static SceUID tls_pid(void)
{
#ifndef NTFSFS_HOST
	return ksceKernelGetProcessIdFromTLS();
#else
	return 0;
#endif
}

/* ACMgr's "bad mode" is EINVAL to the caller, as in exfatfs */
static int acm_err(int r)
{
	return r == ACM_ERR_INVAL ? NTFSFS_ERR(22) : r;
}

int ntfsfs_acl_from_mode(int mode, unsigned *attr)
{
	uint8_t a = 0;
	acm_ctx c = { 3, &a, 1 };
	int r;

	if (!p_attr_from_mode) {
		/* without ACMgr: only the owner write bit, as READONLY */
		*attr = (mode & SCE_S_IWUSR) ? 0 : NTFSFS_ATTR_RO;
		return 0;
	}
	r = p_attr_from_mode(tls_pid(), &c, mode);
	if (r < 0)
		return acm_err(r);
	*attr = a & NTFSFS_ATTR_MASK;
	return 0;
}

int ntfsfs_acl_inherit(unsigned st_attr, unsigned *attr)
{
	uint8_t src = st_attr & 0xff, dst = 0;
	acm_ctx s = { 3, &src, 1 }, d = { 3, &dst, 1 };
	int r;

	if (!p_inherit) {
		*attr = st_attr & NTFSFS_ATTR_MASK;
		return 0;
	}
	r = p_inherit(tls_pid(), &d, &s);
	if (r < 0)
		return acm_err(r);
	*attr = dst & NTFSFS_ATTR_MASK;
	return 0;
}

int ntfsfs_acl_check_read(unsigned attr)
{
	uint8_t a = attr & NTFSFS_ATTR_MASK;
	acm_ctx c = { 3, &a, 1 };
	int r;

	if (!p_check_access)
		return 0;
	r = p_check_access(tls_pid(), &c, 1);
	return r < 0 ? r : 0;
}

int ntfsfs_acl_devinfo_ok(const char *assign)
{
	if (!p_check_devinfo || !assign)
		return 1;
	return p_check_devinfo(tls_pid(), assign) == 1;
}
