/*
 * "ntfs" VFS for the PS Vita, backed by libntfs-3g.
 *
 * The vop/vfs-op contracts follow what 3.65 exfatfs.skprx does (addresses are
 * exfatfs function entries, see docs/RE-3.65.md):
 *   - vfs_mount   0x8100abe4: open the block device vnode mnt->mnt_vnode with
 *                 vfsAllocateFile + ksceVopOpen, bump its ref_count.
 *   - get_root    0x8100ad50 / lookup 0x81009ca0 / create 0x8100a69c /
 *     rename      0x8100a0dc: vfsGetNewVnode(mnt, default_vops, 0, &vp), lock it,
 *                 fill core fields, ref_count = 1 and hand it back still locked.
 *   - set_root    0x81008e30: lock, fill the given vnode as the root, unlock.
 *   - read/write/lseek move file->position themselves; pread/pwrite don't.
 *   - dread returns 1 per entry, 0 at the end.
 *   - devctl 0x3001 fills SceIoDevInfo (free space).
 * vnode->core.node_data holds the MFT record number and core.node_inf its
 * sequence number, so vnodes need no allocation.  Every libntfs-3g call runs
 * on the worker thread (worker.c).
 */
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "config.h"
#include "types.h"
#include "volume.h"
#include "inode.h"
#include "dir.h"
#include "attrib.h"
#include "unistr.h"
#include "device.h"
#include "layout.h"
#include "logging.h"

#include "ntfsfs.h"

#include <psp2kern/kernel/rtc.h>
#include <psp2kern/kernel/iofilemgr.h>

#define VNODE_TYPE_REG     SCE_VNODE_TYPE_REG
#define VNODE_TYPE_DIR     SCE_VNODE_TYPE_DIR
#define VNODE_TYPE_ROOTDIR SCE_VNODE_TYPE_ROOTDIR

#define ERR_NOTSUP ((int)0x80010030)
/* chstat bit 0x10000 (vshbridge only): set the attribute byte from st_attr */
#define CST_ATTR   0x10000

/*
 * libntfs-3g declares on-disk fields with enum types (INDEX_ROOT's
 * collation_rule, ATTR_DEF, ...), so it needs 4-byte enums.  The Vita
 * toolchain defaults to short enums; CMakeLists.txt passes -fno-short-enums.
 */
_Static_assert(sizeof(COLLATION_RULES) == 4, "build with -fno-short-enums");
_Static_assert(sizeof(INDEX_ROOT) == 32, "build with -fno-short-enums");
_Static_assert(sizeof(ATTR_DEF) == 160, "build with -fno-short-enums");

/*
 * A file deleted while open: iofilemgr renamed it to a hidden SCEDEL~ name
 * (whiteout fallback, see vop_rename); it is deleted when its last handle
 * closes, as exfatfs does in fd_release (0x81007ef8).
 */
typedef struct pending_del {
	MFT_REF mref;
	MFT_REF dref;
	char *name;
	SceSize name_len;
} pending_del;

typedef struct ntfsfs_mnt {
	ntfs_volume *vol;
	ntfsfs_blk blk;
	/* One cached open data attribute for the file being read/written. */
	ntfs_inode *c_ni;
	ntfs_attr *c_na;
	pending_del *pend;
	int npend;
	/* lookups ignore case; decode_path_elem upcases (set at mount) */
	int icase;
} ntfsfs_mnt;

typedef struct dent {
	MFT_REF mref;
	char *name;
} dent;

typedef struct ntfsfs_fh {
	struct ntfsfs_mnt *m;
	MFT_REF mref;
	int flags;
	/* directories: snapshot taken at dopen */
	dent *ents;
	int n, cap, idx;
} ntfsfs_fh;

extern const SceVopTable ntfsfs_vops;

/*
 * Open handles live in a table owned by the worker; SceVfsFile.fd holds
 * index + 1.  Only worker-side code touches the table.
 */
static ntfsfs_fh **g_fh;
static unsigned g_fh_cap;

static SceUInt32 fh_put(ntfsfs_fh *fh)
{
	unsigned i, cap;
	ntfsfs_fh **t;

	for (i = 0; i < g_fh_cap; i++)
		if (!g_fh[i])
			goto found;
	cap = g_fh_cap ? g_fh_cap * 2 : 32;
	t = realloc(g_fh, cap * sizeof(*t));
	if (!t)
		return 0;
	memset(t + g_fh_cap, 0, (cap - g_fh_cap) * sizeof(*t));
	g_fh = t;
	i = g_fh_cap;
	g_fh_cap = cap;
found:
	g_fh[i] = fh;
	return i + 1;
}

static ntfsfs_fh *fh_get(SceUInt32 fd)
{
	return fd && fd <= g_fh_cap ? g_fh[fd - 1] : NULL;
}

static int fh_open_count(ntfsfs_mnt *m, MFT_REF mref)
{
	unsigned i;
	int n = 0;

	for (i = 0; i < g_fh_cap; i++)
		if (g_fh[i] && g_fh[i]->m == m && MREF(g_fh[i]->mref) == MREF(mref))
			n++;
	return n;
}

static void fh_free(ntfsfs_fh *fh)
{
	int i;

	if (fh->ents) {
		for (i = 0; i < fh->n; i++)
			free(fh->ents[i].name);
		free(fh->ents);
	}
	free(fh);
}

/* ---- helpers (any thread) ----------------------------------------------- */

static ntfsfs_mnt *MNT(SceVfsVnode *vp)
{
	return vp->core.mnt ? vp->core.mnt->data : NULL;
}

static MFT_REF VREF(SceVfsVnode *vp)
{
	return MK_MREF((u64)(uintptr_t)vp->core.node_data, vp->core.node_inf);
}

static int sce_err(void)
{
	int e = errno ? errno : EIO;
	return NTFSFS_ERR(e);
}

static int is_rdonly(ntfsfs_mnt *m)
{
	return NVolReadOnly(m->vol);
}

/*
 * The FAT-style attribute byte exfatfs keeps in acl_data[0] and st_attr:
 * READONLY 1, HIDDEN 2, SYSTEM 4, DIRECTORY 0x10, ARCHIVE 0x20.  NTFS file
 * attributes use the same values; iofilemgr checks acl_data[0] with ACMgr.
 */
static unsigned fat_attr(unsigned ntfs_flags, int dir)
{
	return (ntfs_flags & 0x27) | (dir ? 0x10 : 0);
}

/* Fills a vnode the way exfatfs does for lookup/create results. */
static void fill_vnode(SceVfsVnode *vp, SceVfsMount *mnt, SceVfsVnode *dd,
		       MFT_REF mref, unsigned type, uint64_t size, unsigned attr)
{
	vp->core.mnt = mnt;
	vp->core.dd = dd;
	vp->core.node_data = (void *)(uintptr_t)MREF(mref);
	vp->core.node_inf = MSEQNO(mref);
	vp->core.state = SCE_VNODE_STATE_ACTIVE;
	vp->core.type = type;
	vp->core.size = size;
	vp->core.fid[0] = (SceUInt32)MREF(mref);
	vp->core.fid[1] = 0;
	vp->core.acl_data[0] = fat_attr(attr, type & SCE_VNODE_TYPE_DIR);
	vp->core.acl_data[1] = 0;
}

static int new_vnode(SceVfsMount *mnt, SceVfsVnode *dd, MFT_REF mref,
		     unsigned type, uint64_t size, unsigned attr, SceVfsVnode **out)
{
	SceVfsVnode *vp = NULL;
	int r;

	r = vfsGetNewVnode(mnt, (SceVopTable *)&ntfsfs_vops, 0, &vp);
	if (r < 0)
		return r;
	r = vfsLockVnode(vp);
	if (r < 0) {
		vfsFreeVnode(vp);
		return r;
	}
	fill_vnode(vp, mnt, dd, mref, type, size, attr);
	vp->core.ref_count = 1;
	*out = vp;   /* returned locked, as exfatfs does */
	return 0;
}

/* SceRtc ticks: microseconds since 0001-01-01. NTFS: 100 ns since 1601-01-01. */
#define RTC_TICK_1601 50491123200000000ULL
#define UNIX_TO_1601_SEC 11644473600ULL

static void ntfs_time_to_sce(sle64 t, SceDateTime *dt)
{
	SceRtcTick tick;
	s64 v = sle64_to_cpu(t);

	tick.tick = RTC_TICK_1601 + (v > 0 ? (u64)v / 10 : 0);
	memset(dt, 0, sizeof(*dt));
	ksceRtcConvertTickToDateTime(dt, &tick);
}

static int sce_time_to_ntfs(const SceDateTime *dt, sle64 *out)
{
	SceUInt64 unix_sec;
	int r = ksceRtcConvertDateTimeToUnixTime(dt, &unix_sec);

	if (r < 0)
		return r;
	*out = cpu_to_sle64((s64)((unix_sec + UNIX_TO_1601_SEC) * 10000000ULL
				  + dt->microsecond * 10ULL));
	return 0;
}

/* ---- worker side helpers ------------------------------------------------- */

static void cache_drop(ntfsfs_mnt *m)
{
	if (m->c_na)
		ntfs_attr_close(m->c_na);
	if (m->c_ni)
		ntfs_inode_close(m->c_ni);
	m->c_na = NULL;
	m->c_ni = NULL;
}

static ntfs_attr *cache_get(ntfsfs_mnt *m, MFT_REF mref)
{
	if (m->c_ni && m->c_ni->mft_no == MREF(mref))
		return m->c_na;
	cache_drop(m);
	m->c_ni = ntfs_inode_open(m->vol, mref);
	if (!m->c_ni)
		return NULL;
	m->c_na = ntfs_attr_open(m->c_ni, AT_DATA, AT_UNNAMED, 0);
	if (!m->c_na) {
		int e = errno;
		ntfs_inode_close(m->c_ni);
		m->c_ni = NULL;
		errno = e;
	}
	return m->c_na;
}

static int is_dir(ntfs_inode *ni)
{
	return !!(ni->mrec->flags & MFT_RECORD_IS_DIRECTORY);
}

/*
 * As exfatfs finfo_to_iostat (0x81001af8): st_attr is the attribute byte and
 * st_mode comes from it through ACMgr's attribute-to-mode table
 * (SceSblACMgrForDriver_3B356B98), plus the file type.
 */
static const unsigned short attr_mode[8] = {
	0x186, 0x106, 0x186, 0x106, 0x106, 0x104, 0x6, 0x4
};

static void ni_stat(ntfs_inode *ni, SceIoStat *st)
{
	int dir = is_dir(ni);
	/* the NTFS root is HIDDEN|SYSTEM; the Vita sees a plain directory */
	unsigned attr = ni->mft_no == FILE_root ? 0x10 :
			fat_attr(le32_to_cpu(ni->flags), dir);

	memset(st, 0, sizeof(*st));
	st->st_attr = attr;
	st->st_mode = attr_mode[attr & 7] | (dir ? SCE_S_IFDIR : SCE_S_IFREG);
	if (!dir)
		st->st_size = ni->data_size;
	ntfs_time_to_sce(ni->creation_time, &st->st_ctime);
	ntfs_time_to_sce(ni->last_access_time, &st->st_atime);
	ntfs_time_to_sce(ni->last_data_change_time, &st->st_mtime);
}

/* Converts a VFS path element (not NUL terminated) to NTFS UTF-16. */
static int to_uname(const char *name, SceSize len, ntfschar **uname)
{
	char tmp[256 * 4];
	int n;

	if (!len || len >= sizeof(tmp)) {
		errno = len ? ENAMETOOLONG : EINVAL;
		return -1;
	}
	memcpy(tmp, name, len);
	tmp[len] = 0;
	*uname = NULL;
	n = ntfs_mbstoucs(tmp, uname);
	if (n > 0 && n > NTFS_MAX_NAME_LEN) {
		free(*uname);
		*uname = NULL;
		errno = ENAMETOOLONG;
		return -1;
	}
	return n;
}

/* ---- jobs ---------------------------------------------------------------- */

typedef struct job {
	ntfsfs_mnt *m;
	MFT_REF ref;        /* target inode */
	MFT_REF dref;       /* parent directory */
	MFT_REF dref2;      /* rename: destination directory */
	const char *name;
	SceSize name_len;
	const char *name2;  /* rename: destination name */
	SceSize name2_len;
	SceUInt32 fd;       /* handle (index + 1) */
	void *buf;
	SceSize len;
	s64 off;
	int flags;
	SceIoStat *st;
	SceIoDirent *de;
	SceIoDevInfo *info;
	/* results */
	MFT_REF out_ref;
	int out_dir;
	u64 out_size;
	unsigned out_attr;
	s64 out_count;
} job;

static int job_lookup(void *arg)
{
	job *j = arg;
	ntfs_inode *dir, *ni;
	ntfschar *uname = NULL;
	int ulen, ret = 0;
	u64 mref;

	cache_drop(j->m);
	ulen = to_uname(j->name, j->name_len, &uname);
	if (ulen < 0)
		return sce_err();
	dir = ntfs_inode_open(j->m->vol, j->dref);
	if (!dir) {
		free(uname);
		return sce_err();
	}
	mref = ntfs_inode_lookup_by_name(dir, uname, ulen);
	ret = mref == (u64)-1 ? sce_err() : 0;
	ntfs_inode_close(dir);
	free(uname);
	if (ret)
		return ret;
	/* Hide the NTFS metadata files ($MFT, $Bitmap, ...). */
	if (MREF(mref) < FILE_first_user && MREF(mref) != FILE_root)
		return NTFSFS_ERR(ENOENT);
	ni = ntfs_inode_open(j->m->vol, mref);
	if (!ni)
		return sce_err();
	j->out_ref = MK_MREF(ni->mft_no, le16_to_cpu(ni->mrec->sequence_number));
	j->out_dir = is_dir(ni);
	j->out_size = j->out_dir ? 0 : (u64)ni->data_size;
	j->out_attr = le32_to_cpu(ni->flags);
	ntfs_inode_close(ni);
	return 0;
}

static int job_create(void *arg)
{
	job *j = arg;
	ntfs_inode *dir, *ni;
	ntfschar *uname = NULL;
	int ulen, ret = 0;

	cache_drop(j->m);
	if (is_rdonly(j->m))
		return NTFSFS_ERR(EROFS);
	ulen = to_uname(j->name, j->name_len, &uname);
	if (ulen < 0)
		return sce_err();
	dir = ntfs_inode_open(j->m->vol, j->dref);
	if (!dir) {
		free(uname);
		return sce_err();
	}
	if (ntfs_inode_lookup_by_name(dir, uname, ulen) != (u64)-1) {
		ret = NTFSFS_ERR(EEXIST);
	} else {
		ni = ntfs_create(dir, const_cpu_to_le32(0), uname, ulen,
				 j->out_dir ? S_IFDIR : S_IFREG);
		if (!ni) {
			ret = sce_err();
		} else {
			/* READONLY/HIDDEN/SYSTEM from the create mode (vop_create) */
			if (j->out_attr & NTFSFS_ATTR_MASK) {
				ni->flags |= cpu_to_le32(j->out_attr & NTFSFS_ATTR_MASK);
				ntfs_inode_mark_dirty(ni);
				NInoFileNameSetDirty(ni);
			}
			j->out_ref = MK_MREF(ni->mft_no,
					     le16_to_cpu(ni->mrec->sequence_number));
			j->out_attr = le32_to_cpu(ni->flags);
			if (ntfs_inode_close_in_dir(ni, dir))
				ret = sce_err();
		}
	}
	ntfs_inode_close(dir);
	free(uname);
	return ret;
}

/* want_dir: 1 directory, 0 file, -1 either */
static int delete_entry(ntfsfs_mnt *m, MFT_REF ref, MFT_REF dref, const char *name,
			SceSize name_len, int want_dir)
{
	ntfs_inode *dir, *ni;
	ntfschar *uname = NULL;
	int ulen;

	cache_drop(m);
	if (is_rdonly(m))
		return NTFSFS_ERR(EROFS);
	ulen = to_uname(name, name_len, &uname);
	if (ulen < 0)
		return sce_err();
	ni = ntfs_inode_open(m->vol, ref);
	if (!ni) {
		free(uname);
		return sce_err();
	}
	if (want_dir >= 0 && is_dir(ni) != want_dir) {
		int e = is_dir(ni) ? EISDIR : ENOTDIR;
		ntfs_inode_close(ni);
		free(uname);
		return NTFSFS_ERR(e);
	}
	dir = ntfs_inode_open(m->vol, dref);
	if (!dir) {
		int r = sce_err();
		ntfs_inode_close(ni);
		free(uname);
		return r;
	}
	/* ntfs_delete() closes both inodes, even on failure */
	ulen = ntfs_delete(m->vol, NULL, ni, dir, uname, ulen);
	free(uname);
	return ulen ? sce_err() : 0;
}

static int job_delete(void *arg)
{
	job *j = arg;

	/* j->out_dir: 1 = rmdir, 0 = remove */
	return delete_entry(j->m, j->ref, j->dref, j->name, j->name_len, j->out_dir);
}

/* Remember a renamed-away open file; delete it now if nobody has it open. */
static int pending_add(ntfsfs_mnt *m, MFT_REF ref, MFT_REF dref,
		       const char *name, SceSize len)
{
	pending_del *p;

	if (!fh_open_count(m, ref))
		return delete_entry(m, ref, dref, name, len, -1);
	p = realloc(m->pend, (m->npend + 1) * sizeof(*p));
	if (!p)
		return 0;	/* the hidden file just stays behind */
	m->pend = p;
	p += m->npend;
	p->name = malloc(len);
	if (!p->name)
		return 0;
	memcpy(p->name, name, len);
	p->name_len = len;
	p->mref = ref;
	p->dref = dref;
	m->npend++;
	return 0;
}

static void pending_close(ntfsfs_mnt *m, MFT_REF ref)
{
	int i;

	if (fh_open_count(m, ref))
		return;
	for (i = 0; i < m->npend; i++) {
		pending_del *p = &m->pend[i];

		if (MREF(p->mref) != MREF(ref))
			continue;
		delete_entry(m, p->mref, p->dref, p->name, p->name_len, -1);
		free(p->name);
		m->pend[i] = m->pend[--m->npend];
		return;
	}
}

static void pending_free(ntfsfs_mnt *m)
{
	int i;

	for (i = 0; i < m->npend; i++)
		free(m->pend[i].name);
	free(m->pend);
	m->pend = NULL;
	m->npend = 0;
}

static int job_rename(void *arg)
{
	job *j = arg;
	ntfs_volume *vol = j->m->vol;
	ntfschar *oname = NULL, *nname = NULL;
	ntfs_inode *ni = NULL, *nd = NULL, *od = NULL;
	int olen, nlen, ret = 0;
	u64 existing;

	cache_drop(j->m);
	if (is_rdonly(j->m))
		return NTFSFS_ERR(EROFS);
	olen = to_uname(j->name, j->name_len, &oname);
	nlen = to_uname(j->name2, j->name2_len, &nname);
	if (olen < 0 || nlen < 0) {
		ret = sce_err();
		goto out;
	}

	/*
	 * An existing destination is never replaced: vfsRename refuses it
	 * before calling us, and a delete-while-open retries with one more '~'
	 * on EEXIST (vfsRemove 0x81006600).  exfatfs rename_helper does the
	 * same.  Replacing would delete another open SCEDEL~ file.
	 */
	nd = ntfs_inode_open(vol, j->dref2);
	if (!nd) {
		ret = sce_err();
		goto out;
	}
	existing = ntfs_inode_lookup_by_name(nd, nname, nlen);
	if (existing != (u64)-1) {
		/* onto its own name: no-op; any other existing name: EEXIST */
		ret = MREF(existing) == MREF(j->ref) && olen == nlen &&
		      !memcmp(oname, nname, olen * 2) ? 0 : NTFSFS_ERR(EEXIST);
		goto out;
	}

	ni = ntfs_inode_open(vol, j->ref);
	if (!ni) {
		ret = sce_err();
		goto out;
	}
	if (ntfs_link(ni, nd, nname, nlen)) {
		ret = sce_err();
		goto out;
	}
	if (MREF(j->dref) == MREF(j->dref2)) {
		od = nd;
	} else {
		ntfs_inode_close(nd);
		od = ntfs_inode_open(vol, j->dref);
		if (!od) {
			nd = NULL;
			ret = sce_err();
			goto out;
		}
	}
	nd = NULL;
	/* closes ni and od */
	if (ntfs_delete(vol, NULL, ni, od, oname, olen))
		ret = sce_err();
	ni = NULL;
	if (!ret && j->flags)
		ret = pending_add(j->m, j->ref, j->dref2, j->name2, j->name2_len);
out:
	if (ni)
		ntfs_inode_close(ni);
	if (nd)
		ntfs_inode_close(nd);
	free(oname);
	free(nname);
	return ret;
}

static int job_open(void *arg)
{
	job *j = arg;
	ntfs_attr *na;
	ntfsfs_fh *fh;
	int wr = (j->flags & SCE_O_WRONLY) != 0;

	if (wr && is_rdonly(j->m))
		return NTFSFS_ERR(EROFS);
	na = cache_get(j->m, j->ref);
	if (!na) {
		/* a directory has no unnamed $DATA */
		if (j->m->c_ni == NULL && errno == ENOENT)
			return NTFSFS_ERR(EISDIR);
		return sce_err();
	}
	if (wr && (j->flags & SCE_O_TRUNC) && na->data_size) {
		if (ntfs_attr_truncate(na, 0))
			return sce_err();
		ntfs_inode_update_times(j->m->c_ni, NTFS_UPDATE_MCTIME);
	}
	fh = calloc(1, sizeof(*fh));
	if (!fh)
		return NTFSFS_ERR(ENOMEM);
	fh->m = j->m;
	fh->mref = j->ref;
	fh->flags = j->flags;
	j->fd = fh_put(fh);
	if (!j->fd) {
		free(fh);
		return NTFSFS_ERR(EMFILE);
	}
	j->out_size = na->data_size;
	return 0;
}

static int job_close(void *arg)
{
	job *j = arg;
	ntfsfs_fh *fh = fh_get(j->fd);

	if (!fh)
		return NTFSFS_ERR(EBADF);
	g_fh[j->fd - 1] = NULL;
	/* write metadata back once the writer is done */
	if ((fh->flags & SCE_O_WRONLY) && j->m && j->m->c_ni &&
	    j->m->c_ni->mft_no == MREF(fh->mref))
		cache_drop(j->m);
	if (fh->m && fh->m->npend)
		pending_close(fh->m, fh->mref);
	fh_free(fh);
	return 0;
}

static int job_pread(void *arg)
{
	job *j = arg;
	ntfsfs_fh *fh = fh_get(j->fd);
	ntfs_attr *na;
	s64 r;

	if (!fh || !(fh->flags & SCE_O_RDONLY))
		return NTFSFS_ERR(EBADF);
	na = cache_get(j->m, j->ref);
	if (!na)
		return sce_err();
	if (j->off >= na->data_size) {
		j->out_count = 0;
		return 0;
	}
	r = ntfs_attr_pread(na, j->off, j->len, j->buf);
	if (r < 0)
		return sce_err();
	j->out_count = r;
	return 0;
}

static int job_pwrite(void *arg)
{
	job *j = arg;
	ntfsfs_fh *fh = fh_get(j->fd);
	ntfs_attr *na;
	s64 r;

	if (!fh || !(fh->flags & SCE_O_WRONLY))
		return NTFSFS_ERR(EBADF);
	if (is_rdonly(j->m))
		return NTFSFS_ERR(EROFS);
	na = cache_get(j->m, j->ref);
	if (!na)
		return sce_err();
	/* j->flags is set only for write(), where O_APPEND applies */
	if (j->flags && (fh->flags & SCE_O_APPEND))
		j->off = na->data_size;
	r = ntfs_attr_pwrite(na, j->off, j->len, j->buf);
	if (r < 0)
		return sce_err();
	ntfs_inode_update_times(j->m->c_ni, NTFS_UPDATE_MCTIME);
	j->out_count = r;
	j->out_size = na->data_size;
	return 0;
}

static int job_stat(void *arg)
{
	job *j = arg;
	ntfs_inode *ni;

	/* reuse the cached inode so pending size/time updates are visible */
	if (j->m->c_ni && j->m->c_ni->mft_no == MREF(j->ref)) {
		ni_stat(j->m->c_ni, j->st);
		return 0;
	}
	ni = ntfs_inode_open(j->m->vol, j->ref);
	if (!ni)
		return sce_err();
	ni_stat(ni, j->st);
	ntfs_inode_close(ni);
	return 0;
}

static int job_chstat(void *arg)
{
	job *j = arg;
	SceIoStat *st = j->st;
	int bits = j->flags;
	ntfs_inode *ni;
	int r = 0;

	cache_drop(j->m);
	if (is_rdonly(j->m))
		return NTFSFS_ERR(EROFS);
	if (bits & SCE_CST_SIZE) {
		ntfs_attr *na = cache_get(j->m, j->ref);

		if (!na)
			return sce_err();
		if (ntfs_attr_truncate(na, st->st_size))
			return sce_err();
		ntfs_inode_update_times(j->m->c_ni, NTFS_UPDATE_MCTIME);
		j->out_size = na->data_size;
		cache_drop(j->m);
	}
	if (!(bits & (SCE_CST_MODE | CST_ATTR | SCE_CST_CT | SCE_CST_AT | SCE_CST_MT)))
		return 0;
	ni = ntfs_inode_open(j->m->vol, j->ref);
	if (!ni)
		return sce_err();
	if (bits & (SCE_CST_MODE | CST_ATTR)) {
		/* do_chstat got the new READONLY/HIDDEN/SYSTEM bits from ACMgr */
		ni->flags = (ni->flags & ~cpu_to_le32(NTFSFS_ATTR_MASK)) |
			    cpu_to_le32(j->out_attr & NTFSFS_ATTR_MASK);
	}
	if ((bits & SCE_CST_CT) && sce_time_to_ntfs(&st->st_ctime, &ni->creation_time) < 0)
		r = NTFSFS_ERR(EINVAL);
	if ((bits & SCE_CST_AT) && sce_time_to_ntfs(&st->st_atime, &ni->last_access_time) < 0)
		r = NTFSFS_ERR(EINVAL);
	if ((bits & SCE_CST_MT) && sce_time_to_ntfs(&st->st_mtime, &ni->last_data_change_time) < 0)
		r = NTFSFS_ERR(EINVAL);
	ntfs_inode_mark_dirty(ni);
	NInoFileNameSetDirty(ni);
	j->out_attr = le32_to_cpu(ni->flags);
	if (ntfs_inode_close(ni) && !r)
		r = sce_err();
	return r;
}

static int filldir(void *ctx, const ntfschar *name, const int name_len,
		   const int name_type, const s64 pos, const MFT_REF mref,
		   const unsigned dt_type)
{
	ntfsfs_fh *fh = ctx;
	char *utf8 = NULL;

	(void)pos; (void)dt_type;
	if (name_type == FILE_NAME_DOS)
		return 0;
	if (MREF(mref) < FILE_first_user && MREF(mref) != FILE_root)
		return 0;   /* $MFT, $Bitmap, ... */
	if (name_len == 1 && name[0] == const_cpu_to_le16('.'))
		return 0;
	if (name_len == 2 && name[0] == const_cpu_to_le16('.') &&
	    name[1] == const_cpu_to_le16('.'))
		return 0;
	if (ntfs_ucstombs(name, name_len, &utf8, 0) < 0)
		return 0;   /* skip names we cannot represent */
	/* files deleted while open, see vop_rename (exfatfs hides them too) */
	if (!strncmp(utf8, "SCEDEL~", 7)) {
		free(utf8);
		return 0;
	}
	if (fh->n == fh->cap) {
		int cap = fh->cap ? fh->cap * 2 : 64;
		dent *e = realloc(fh->ents, cap * sizeof(*e));

		if (!e) {
			free(utf8);
			return -1;
		}
		fh->ents = e;
		fh->cap = cap;
	}
	fh->ents[fh->n].mref = mref;
	fh->ents[fh->n].name = utf8;
	fh->n++;
	return 0;
}

static int job_dopen(void *arg)
{
	job *j = arg;
	ntfs_inode *ni;
	ntfsfs_fh *fh;
	s64 pos = 0;
	int r = 0;

	cache_drop(j->m);
	ni = ntfs_inode_open(j->m->vol, j->ref);
	if (!ni)
		return sce_err();
	if (!is_dir(ni)) {
		ntfs_inode_close(ni);
		return NTFSFS_ERR(ENOTDIR);
	}
	fh = calloc(1, sizeof(*fh));
	if (!fh) {
		ntfs_inode_close(ni);
		return NTFSFS_ERR(ENOMEM);
	}
	fh->m = j->m;
	fh->mref = j->ref;
	if (ntfs_readdir(ni, &pos, fh, filldir))
		r = sce_err();
	ntfs_inode_close(ni);
	if (r) {
		fh_free(fh);
		return r;
	}
	j->fd = fh_put(fh);
	if (!j->fd) {
		fh_free(fh);
		return NTFSFS_ERR(EMFILE);
	}
	return 0;
}

static int job_dread(void *arg)
{
	job *j = arg;
	ntfsfs_fh *fh = fh_get(j->fd);
	ntfs_inode *ni;

	if (!fh)
		return NTFSFS_ERR(EBADF);
	while (fh->idx < fh->n) {
		dent *e = &fh->ents[fh->idx++];

		ni = ntfs_inode_open(j->m->vol, e->mref);
		if (!ni)
			continue;   /* vanished since dopen */
		memset(j->de, 0, sizeof(*j->de));
		ni_stat(ni, &j->de->d_stat);
		ntfs_inode_close(ni);
		strncpy(j->de->d_name, e->name, sizeof(j->de->d_name) - 1);
		return 1;
	}
	return 0;
}

static int job_sync(void *arg)
{
	job *j = arg;
	struct ntfs_device *dev = j->m->vol->dev;

	if (j->m->c_ni && ntfs_inode_sync(j->m->c_ni))
		return sce_err();
	if (dev->d_ops->sync(dev))
		return sce_err();
	return 0;
}

static int job_devinfo(void *arg)
{
	job *j = arg;
	ntfs_volume *vol = j->m->vol;

	if (vol->free_clusters < 0 && ntfs_volume_get_free_space(vol))
		return sce_err();
	j->info->max_size = (SceOff)vol->nr_clusters * vol->cluster_size;
	j->info->free_size = (SceOff)(vol->free_clusters < 0 ? 0 : vol->free_clusters)
			     * vol->cluster_size;
	j->info->cluster_size = vol->cluster_size;
	j->info->unk = NULL;
	if (j->len == 0x28) {
		/* exfatfs devinfo_fill 0x81008a90: serial at 0x18, label at 0x1c */
		uint8_t *x = (uint8_t *)j->info;
		const char *label = vol->vol_name ? vol->vol_name : "";
		size_t n = strlen(label);

		memcpy(x + 0x18, &j->m->blk.serial, 4);
		memset(x + 0x1c, ' ', 11);
		memcpy(x + 0x1c, label, n > 11 ? 11 : n);
		x[0x27] = 0;
	}
	return 0;
}

static int job_mount(void *arg)
{
	job *j = arg;
	ntfsfs_mnt *m = j->m;
	struct ntfs_device *dev;
	int r;

	m->blk.bounce = malloc(NTFSFS_BOUNCE);
	if (!m->blk.bounce)
		return NTFSFS_ERR(ENOMEM);
	r = ntfsfs_probe(&m->blk);
	if (r < 0) {
		NTFSFS_LOG("no NTFS boot sector (0x%08X)\n", r);
		return r;
	}
	dev = ntfs_device_alloc("ntfsfs", 0, &ntfsfs_dev_ops, &m->blk);
	if (!dev)
		return sce_err();
	m->vol = ntfs_device_mount(dev, (m->blk.rdonly ? NTFS_MNT_RDONLY : 0)
				   | NTFS_MNT_MAY_RDONLY);
	if (!m->vol) {
		r = sce_err();
		NTFSFS_LOG("ntfs_device_mount failed (errno %d)\n", errno);
		/* ntfs_device_mount frees dev on failure */
		return r;
	}
	/* exFAT on the Vita is case-insensitive; software relies on it */
	m->icase = !ntfs_set_ignore_case(m->vol);
	if (!m->icase)
		NTFSFS_LOG("ignore_case unavailable, lookups are case-sensitive\n");
	ntfs_volume_get_free_space(m->vol);
	NTFSFS_LOG("mounted: %llu clusters of %u bytes at +0x%llx%s\n",
		   (unsigned long long)m->vol->nr_clusters, m->vol->cluster_size,
		   (unsigned long long)m->blk.part_off,
		   NVolReadOnly(m->vol) ? " (read-only)" : "");
	return 0;
}

static int job_umount(void *arg)
{
	job *j = arg;
	ntfsfs_mnt *m = j->m;
	int r = 0;

	cache_drop(m);
	pending_free(m);
	if (m->vol && ntfs_umount(m->vol, FALSE))
		r = sce_err();
	m->vol = NULL;
	free(m->blk.bounce);
	m->blk.bounce = NULL;
	return r;
}

static int job_alloc_mnt(void *arg)
{
	job *j = arg;

	j->m = calloc(1, sizeof(*j->m));
	return j->m ? 0 : NTFSFS_ERR(ENOMEM);
}

static int job_free_mnt(void *arg)
{
	free(((job *)arg)->m);
	return 0;
}

/* ---- vfs ops -------------------------------------------------------------- */

static void blk_close(ntfsfs_blk *b)
{
	if (!b->vp || b->file_uid < 0)
		return;
	if (vfsLockVnode(b->vp) >= 0) {
		ksceVopClose(b->vp, b->file);
		if (vfsFreeFile(b->vp, b->file_uid) >= 0)
			b->vp->core.ref_count--;
		vfsUnlockVnode(b->vp);
	}
	b->file = NULL;
	b->file_uid = -1;
}

static int vfs_mount(SceVfsOpMountArgs *a)
{
	SceVfsMount *mnt = a->mnt;
	SceVfsVnode *dvp = mnt->mnt_vnode;
	job j;
	int r, mode;

	if (mnt->data)
		return NTFSFS_ERR(EBUSY);
	memset(&j, 0, sizeof(j));
	r = ntfsfs_call(job_alloc_mnt, &j);
	if (r < 0)
		return r;
	j.m->blk.vp = dvp;
	j.m->blk.file_uid = -1;
	j.m->blk.rdonly = !!(mnt->mnt_flags & SCE_VFS_MOUNT_FLAG_RDONLY);

	/* open the block device, read-write if possible */
	r = vfsLockVnode(dvp);
	if (r < 0)
		goto fail;
	r = vfsAllocateFile(dvp, &j.m->blk.file, a->dev_file_path->name);
	if (r < 0) {
		vfsUnlockVnode(dvp);
		goto fail;
	}
	j.m->blk.file_uid = r;
	mode = j.m->blk.rdonly ? SCE_O_RDONLY : SCE_O_RDWR;
	j.m->blk.file->flags = mode;
	r = ksceVopOpen(dvp, a->dev_file_path, mode, j.m->blk.file);
	if (r < 0 && mode == SCE_O_RDWR) {
		j.m->blk.rdonly = 1;
		j.m->blk.file->flags = SCE_O_RDONLY;
		r = ksceVopOpen(dvp, a->dev_file_path, SCE_O_RDONLY, j.m->blk.file);
	}
	if (r < 0) {
		vfsFreeFile(dvp, j.m->blk.file_uid);
		j.m->blk.file_uid = -1;
		vfsUnlockVnode(dvp);
		goto fail;
	}
	dvp->core.ref_count++;
	vfsUnlockVnode(dvp);

	r = ntfsfs_call(job_mount, &j);
	if (r < 0) {
		ntfsfs_call(job_umount, &j);
		blk_close(&j.m->blk);
		goto fail;
	}
	mnt->data = j.m;
	/* exfatfs sets these two after a successful mount */
	mnt->available_entry_num = 0x40;
	mnt->default_io_cache_size = j.m->vol->cluster_size > 0x8000 ?
				     0x8000 : j.m->vol->cluster_size;
	return 0;
fail:
	ntfsfs_call(job_free_mnt, &j);
	return r;
}

static int vfs_umount(SceVfsOpUmountArgs *a)
{
	SceVfsMount *mnt = a->mnt;
	job j;
	int r;

	if (!mnt || !mnt->data)
		return NTFSFS_ERR(EINVAL);
	memset(&j, 0, sizeof(j));
	j.m = mnt->data;
	r = ntfsfs_call(job_umount, &j);
	if (r < 0 && !(a->flags & SCE_VFS_UMOUNT_FLAG_FORCE))
		NTFSFS_LOG("umount: 0x%08X\n", r);
	blk_close(&j.m->blk);
	mnt->data = NULL;
	ntfsfs_call(job_free_mnt, &j);
	return 0;
}

static void fill_root(SceVfsVnode *vp, SceVfsMount *mnt)
{
	fill_vnode(vp, mnt, NULL, MK_MREF(FILE_root, FILE_root),
		   VNODE_TYPE_ROOTDIR, 0, FILE_ATTR_DIRECTORY);
}

static int vfs_set_root(SceVfsOpSetRootArgs *a)
{
	if (!a->mnt->data)
		return NTFSFS_ERR(EINVAL);
	vfsLockVnode(a->vp);
	fill_root(a->vp, a->mnt);
	vfsUnlockVnode(a->vp);
	return 0;
}

static int vfs_get_root(SceVfsOpGetRootArgs *a)
{
	SceVfsVnode *vp;
	int r;

	*a->vpp = NULL;
	if (!a->mnt->data)
		return NTFSFS_ERR(EINVAL);
	r = new_vnode(a->mnt, NULL, MK_MREF(FILE_root, FILE_root),
		      VNODE_TYPE_ROOTDIR, 0, FILE_ATTR_DIRECTORY, &vp);
	if (r < 0)
		return r;
	*a->vpp = vp;
	return 0;
}

static int vfs_sync(SceVfsOpSyncArgs *a)
{
	job j;

	if (!a->mnt->data)
		return NTFSFS_ERR(EINVAL);
	memset(&j, 0, sizeof(j));
	j.m = a->mnt->data;
	return ntfsfs_call(job_sync, &j);
}

static int vfs_init(SceVfsOpInitArgs *a)
{
	(void)a;
	return 0;
}

static int vfs_fini(SceVfsOpFiniArgs *a)
{
	(void)a;
	return 0;
}

static int vfs_devctl(SceVfsOpDevctlArg *a)
{
	ntfsfs_mnt *m = a->mnt->data;
	job j;

	if (!m)
		return NTFSFS_ERR(EINVAL);
	switch (a->cmd) {
	case 0x3001:
		/*
		 * Free space (exfatfs vfs_devctl 0x8100978c): only callers that
		 * SceSblACMgr allows for this assign, else 0x80010030.
		 */
		if (!ntfsfs_acl_devinfo_ok(a->mnt->mnt_data ?
					   a->mnt->mnt_data->assign_name : NULL))
			return ERR_NOTSUP;
		if (a->buf_len != sizeof(SceIoDevInfo) && a->buf_len != 0x28)
			return NTFSFS_ERR(EINVAL);
		memset(a->buf, 0, a->buf_len);
		memset(&j, 0, sizeof(j));
		j.m = m;
		j.info = a->buf;
		j.len = a->buf_len;
		return ntfsfs_call(job_devinfo, &j);
	case 0x80000001:
		/* max file size: exFAT answers all ones, so does NTFS here */
		if (a->buf_len != 8)
			return NTFSFS_ERR(EINVAL);
		memset(a->buf, 0xff, 8);
		return 0;
	case 0x3802:
		/*
		 * vfsMount (0x81004a0c) sends 0x3802 (exfatfs: create and pin
		 * SceIoTrash) after mounting writable volumes, and unmounts
		 * again unless it succeeds or fails with 0x8001001C.  ntfsfs
		 * keeps files deleted while open itself (SCEDEL~ names), so
		 * there is nothing to create.
		 */
		return 0;
	case 0x3803:
		/*
		 * iofilemgr's idle daemon (0x810155ec) sends 0x3803 (exfatfs:
		 * purge one trash entry) to ux0:/grw0: and calls again every
		 * 100 us on any error except ENOENT, which drops the mount
		 * from its list.
		 */
		return NTFSFS_ERR(ENOENT);
	default:
		return ERR_NOTSUP;
	}
}

/*
 * iofilemgr canonicalises every path element with this before looking it up
 * in its name cache and before vop_lookup (iof_lookup_elem 0x81003c9c).
 * exfatfs (0x8100963c) returns the element upcased, so differently cased
 * names share one cache entry and one vnode.  ntfsfs does the same with the
 * volume's $UpCase table when lookups ignore case.  Create, rename and
 * remove still get the raw name.  Runs on the caller's thread and only reads
 * the immutable upcase table.
 */
static int utf8_get(const unsigned char *p, const unsigned char *end, unsigned *cp)
{
	unsigned c = p[0], n, i;

	if (c < 0x80) { *cp = c; return 1; }
	if ((c & 0xe0) == 0xc0) { n = 2; c &= 0x1f; }
	else if ((c & 0xf0) == 0xe0) { n = 3; c &= 0x0f; }
	else if ((c & 0xf8) == 0xf0) { n = 4; c &= 0x07; }
	else return -1;
	if (p + n > end)
		return -1;
	for (i = 1; i < n; i++) {
		if ((p[i] & 0xc0) != 0x80)
			return -1;
		c = c << 6 | (p[i] & 0x3f);
	}
	*cp = c;
	return n;
}

static int utf8_put(unsigned c, char *o, size_t room)
{
	if (c < 0x80) {
		if (room < 1) return -1;
		o[0] = c;
		return 1;
	}
	if (c < 0x800) {
		if (room < 2) return -1;
		o[0] = 0xc0 | c >> 6; o[1] = 0x80 | (c & 0x3f);
		return 2;
	}
	if (c < 0x10000) {
		if (room < 3) return -1;
		o[0] = 0xe0 | c >> 12; o[1] = 0x80 | ((c >> 6) & 0x3f);
		o[2] = 0x80 | (c & 0x3f);
		return 3;
	}
	if (room < 4) return -1;
	o[0] = 0xf0 | c >> 18; o[1] = 0x80 | ((c >> 12) & 0x3f);
	o[2] = 0x80 | ((c >> 6) & 0x3f); o[3] = 0x80 | (c & 0x3f);
	return 4;
}

static int vfs_decode_path_elem(SceVfsOpDecodePathElemArgs *a)
{
	ntfsfs_mnt *m = a->mnt ? a->mnt->data : NULL;
	const unsigned char *p, *e, *end;
	size_t out = 0;

	if (!m || !m->vol || !m->icase || !m->vol->upcase || !a->buf_len)
		return ERR_NOTSUP;       /* iofilemgr then uses the raw name */
	p = (const unsigned char *)a->path;
	while (*p == '/')
		p++;
	for (e = p; *e && *e != '/'; e++)
		;
	end = e;
	for (e = p; e < end; ) {
		unsigned c;
		int n = utf8_get(e, end, &c), k;

		if (n < 0)
			return ERR_NOTSUP;   /* not UTF-8: leave it to the raw name */
		if (c < m->vol->upcase_len)
			c = le16_to_cpu(m->vol->upcase[c]);
		k = utf8_put(c, a->buf + out, a->buf_len - out - 1);
		if (k < 0)
			return NTFSFS_ERR(ENAMETOOLONG);
		out += k;
		e += n;
	}
	a->buf[out] = 0;
	*a->path2 = (const char *)p;
	*a->path3 = (const char *)end;
	*a->decode_len = out;
	return 0;
}

static const SceVfsOpTable ntfsfs_vfs_ops = {
	.vfs_mount    = vfs_mount,
	.vfs_umount   = vfs_umount,
	.vfs_set_root = vfs_set_root,
	.vfs_get_root = vfs_get_root,
	.vfs_sync     = vfs_sync,
	.vfs_init     = vfs_init,
	.vfs_fini     = vfs_fini,
	.vfs_devctl   = vfs_devctl,
	.vfs_decode_path_elem = vfs_decode_path_elem,
};

/* ---- vops ----------------------------------------------------------------- */

static void job_for(job *j, SceVfsVnode *vp)
{
	memset(j, 0, sizeof(*j));
	j->m = MNT(vp);
	j->ref = VREF(vp);
}

static int vop_lookup(SceVopLookupArgs *a)
{
	job j;
	int r;

	*a->vpp = NULL;
	job_for(&j, a->dvp);
	if (!j.m)
		return NTFSFS_ERR(EINVAL);
	j.dref = j.ref;
	j.name = a->path->name;
	j.name_len = a->path->name_length;
	r = ntfsfs_call(job_lookup, &j);
	if (r < 0)
		return r;
	return new_vnode(a->dvp->core.mnt, a->dvp, j.out_ref,
			 j.out_dir ? VNODE_TYPE_DIR : VNODE_TYPE_REG,
			 j.out_size, j.out_attr, a->vpp);
}

static int vop_create(SceVopCreateArgs *a)
{
	job j;
	int r;

	*a->vpp = NULL;
	job_for(&j, a->dvp);
	if (!j.m)
		return NTFSFS_ERR(EINVAL);
	/* exfatfs vop_create 0x8100a69c: the attribute comes from the mode */
	r = ntfsfs_acl_from_mode(a->mode, &j.out_attr);
	if (r < 0)
		return r;
	j.dref = j.ref;
	j.name = a->path->name;
	j.name_len = a->path->name_length;
	j.out_dir = 0;
	r = ntfsfs_call(job_create, &j);
	if (r < 0)
		return r;
	return new_vnode(a->dvp->core.mnt, a->dvp, j.out_ref, VNODE_TYPE_REG,
			 0, j.out_attr, a->vpp);
}

static int vop_mkdir(SceVopMkdirArgs *a)
{
	job j;
	int r;

	*a->vpp = NULL;
	job_for(&j, a->dvp);
	if (!j.m)
		return NTFSFS_ERR(EINVAL);
	r = ntfsfs_acl_from_mode(a->mode, &j.out_attr);   /* vop_mkdir 0x8100a4c0 */
	if (r < 0)
		return r;
	j.dref = j.ref;
	j.name = a->path->name;
	j.name_len = a->path->name_length;
	j.out_dir = 1;
	r = ntfsfs_call(job_create, &j);
	if (r < 0)
		return r;
	return new_vnode(a->dvp->core.mnt, a->dvp, j.out_ref, VNODE_TYPE_DIR,
			 0, j.out_attr, a->vpp);
}

static int do_delete(SceVfsVnode *dvp, SceVfsVnode *vp, SceVfsPath *path, int dir)
{
	job j;

	job_for(&j, vp);
	if (!j.m)
		return NTFSFS_ERR(EINVAL);
	if (vp->core.type & SCE_VNODE_TYPE_ROOT)
		return NTFSFS_ERR(EBUSY);
	j.dref = VREF(dvp);
	j.name = path->name;
	j.name_len = path->name_length;
	j.out_dir = dir;
	return ntfsfs_call(job_delete, &j);
}

static int vop_remove(SceVopRemoveArgs *a)
{
	/*
	 * The last close of a file deleted while open calls remove with a NULL
	 * dvp when its name-cache entry has no parent (vfsClose, 0x81006248).
	 * pending_close has already deleted such a file.
	 */
	if (!a->dvp)
		return NTFSFS_ERR(ENOENT);
	return do_delete(a->dvp, a->vp, a->path, 0);
}

static int vop_rmdir(SceVopRmdirArgs *a)
{
	return do_delete(a->dvp, a->vp, a->path, 1);
}

static int vop_rename(SceVopRenameArgs *a)
{
	SceVfsVnode *p;
	job j;
	int r;

	SceVfsVnode *ndvp = a->ndvp ? a->ndvp : a->odvp;

	*a->nvpp = NULL;
	/*
	 * ndvp == NULL means "same directory": iofilemgr does this when a file
	 * deleted while open is moved to a hidden SCEDEL~ name (exfatfs treats
	 * it the same way, 0x8100a1c8). Across mounts exfatfs and iofilemgr
	 * (vfsRename) return 0x80010001.
	 */
	if (MNT(ndvp) != MNT(a->odvp))
		return NTFSFS_ERR(EPERM);
	job_for(&j, a->ovp);
	if (!j.m)
		return NTFSFS_ERR(EINVAL);
	if (a->ovp->core.type & SCE_VNODE_TYPE_ROOT)
		return NTFSFS_ERR(EBUSY);
	/* refuse moving a directory into its own subtree */
	for (p = ndvp; p; p = p->core.dd)
		if (p == a->ovp || (p->core.node_data == a->ovp->core.node_data))
			return NTFSFS_ERR(EINVAL);
	j.dref = VREF(a->odvp);
	j.dref2 = VREF(ndvp);
	j.name = a->old_path->name;
	j.name_len = a->old_path->name_length;
	j.name2 = a->new_path->name;
	j.name2_len = a->new_path->name_length;
	/* iofilemgr's whiteout fallback for files deleted while open */
	j.flags = !a->ndvp && j.name2_len >= 7 && !memcmp(j.name2, "SCEDEL~", 7);
	r = ntfsfs_call(job_rename, &j);
	if (r < 0)
		return r;
	/* exfatfs hands back a fresh vnode for the new name */
	return new_vnode(ndvp->core.mnt, ndvp, VREF(a->ovp),
			 a->ovp->core.type, a->ovp->core.size,
			 a->ovp->core.acl_data[0], a->nvpp);
}

static int vop_open(SceVopOpenArgs *a)
{
	job j;
	int r;

	job_for(&j, a->vp);
	if (!j.m)
		return NTFSFS_ERR(EINVAL);
	if (a->vp->core.type & SCE_VNODE_TYPE_DIR)
		return NTFSFS_ERR(EISDIR);
	j.flags = (a->flags & ~SCE_O_CREAT) | SCE_O_RDONLY;
	r = ntfsfs_call(job_open, &j);
	if (r < 0)
		return r;
	a->file->fd = j.fd;
	a->vp->core.size = j.out_size;
	return 0;
}

static int vop_close(SceVopCloseArgs *a)
{
	job j;

	if (!a->file->fd)
		return 0;
	job_for(&j, a->vp);
	j.fd = a->file->fd;
	a->file->fd = 0;
	return ntfsfs_call(job_close, &j);
}

static SceSSize do_read(SceVfsVnode *vp, SceVfsFile *file, void *buf, SceSize n, SceOff off)
{
	job j;
	int r;

	if (!n)
		return 0;
	job_for(&j, vp);
	if (!j.m || !file->fd)
		return NTFSFS_ERR(EBADF);
	j.fd = file->fd;
	j.buf = buf;
	j.len = n;
	j.off = off;
	r = ntfsfs_call(job_pread, &j);
	return r < 0 ? r : (SceSSize)j.out_count;
}

static SceSSize do_write(SceVfsVnode *vp, SceVfsFile *file, const void *buf, SceSize n,
			 SceOff off, SceOff *end)
{
	job j;
	int r;

	job_for(&j, vp);
	if (!j.m || !file->fd)
		return NTFSFS_ERR(EBADF);
	if (!n)
		return 0;
	j.fd = file->fd;
	j.buf = (void *)buf;
	j.len = n;
	j.off = off;
	j.flags = end != NULL;   /* O_APPEND only applies to write() */
	r = ntfsfs_call(job_pwrite, &j);
	if (r < 0)
		return r;
	vp->core.size = j.out_size;
	if (end)
		*end = j.off + j.out_count;
	return (SceSSize)j.out_count;
}

static SceSSize vop_read(SceVopReadArgs *a)
{
	SceSSize r = do_read(a->vp, a->file, a->buf, a->nbyte, a->file->position);

	if (r > 0)
		a->file->position += r;
	return r;
}

static SceSSize vop_write(SceVopWriteArgs *a)
{
	SceOff end;
	SceSSize r = do_write(a->vp, a->file, a->buf, a->nbyte, a->file->position, &end);

	if (r > 0)
		a->file->position = end;
	return r;
}

static SceSSize vop_pread(SceVopPreadArgs *a)
{
	return do_read(a->vp, a->file, a->buf, a->nbyte, a->offset);
}

static SceSSize vop_pwrite(SceVopPwriteArgs *a)
{
	return do_write(a->vp, a->file, a->buf, a->nbyte, a->offset, NULL);
}

static SceOff vop_lseek(SceVopLseekArgs *a)
{
	SceOff pos;

	switch (a->whence) {
	case SCE_SEEK_SET: pos = a->offset; break;
	case SCE_SEEK_CUR: pos = a->file->position + a->offset; break;
	case SCE_SEEK_END: pos = (SceOff)a->vp->core.size + a->offset; break;
	default: return NTFSFS_ERR(EINVAL);
	}
	if (pos < 0)
		return NTFSFS_ERR(EINVAL);
	a->file->position = pos;
	return pos;
}

static int vop_dopen(SceVopDopenAgrs *a)
{
	job j;
	int r;

	job_for(&j, a->vp);
	if (!j.m)
		return NTFSFS_ERR(EINVAL);
	r = ntfsfs_call(job_dopen, &j);
	if (r < 0)
		return r;
	a->file->fd = j.fd;
	return 0;
}

static int vop_dclose(SceVopDcloseArgs *a)
{
	return vop_close((SceVopCloseArgs *)a);
}

static int vop_dread(SceVopDreadArgs *a)
{
	job j;
	int r;

	job_for(&j, a->vp);
	if (!j.m || !a->file->fd)
		return NTFSFS_ERR(EBADF);
	j.fd = a->file->fd;
	j.de = a->dir;
	/* as exfatfs vop_dread (0x810093dc): skip entries ACMgr hides */
	do {
		r = ntfsfs_call(job_dread, &j);
	} while (r == 1 && ntfsfs_acl_check_read(a->dir->d_stat.st_attr) < 0);
	if (r != 1)
		memset(a->dir, 0, sizeof(*a->dir));
	return r;
}

static int do_getstat(SceVfsVnode *vp, SceIoStat *st)
{
	job j;
	int r;

	memset(st, 0, sizeof(*st));
	/* exfatfs vop_getstat (0x81009eac) checks read access first */
	r = ntfsfs_acl_check_read(vp->core.acl_data[0]);
	if (r < 0)
		return r;
	job_for(&j, vp);
	if (!j.m)
		return NTFSFS_ERR(EINVAL);
	j.st = st;
	return ntfsfs_call(job_stat, &j);
}

static int do_chstat(SceVfsVnode *vp, SceIoStat *st, int bits)
{
	job j;
	int r;

	job_for(&j, vp);
	if (!j.m)
		return NTFSFS_ERR(EINVAL);
	if ((bits & SCE_CST_SIZE) && (vp->core.type & SCE_VNODE_TYPE_DIR))
		return NTFSFS_ERR(EISDIR);
	/*
	 * exfatfs chstat_common (0x81009b18): MODE -> attribute through ACMgr
	 * (changing SYSTEM needs a privileged thread), bit 0x10000 -> from
	 * st_attr, the later one winning.
	 */
	if (bits & SCE_CST_MODE) {
		r = ntfsfs_acl_from_mode(st->st_mode, &j.out_attr);
		if (r < 0)
			return r;
	}
	if (bits & CST_ATTR) {
		r = ntfsfs_acl_inherit(st->st_attr, &j.out_attr);
		if (r < 0)
			return r;
	}
	j.st = st;
	j.flags = bits;
	j.out_size = vp->core.size;
	r = ntfsfs_call(job_chstat, &j);
	if (bits & SCE_CST_SIZE)
		vp->core.size = j.out_size;
	if (r >= 0 && (bits & (SCE_CST_MODE | CST_ATTR)))
		vp->core.acl_data[0] = fat_attr(j.out_attr, vp->core.type & SCE_VNODE_TYPE_DIR);
	return r;
}

static int vop_getstat(SceVopGetstatArgs *a)
{
	return do_getstat(a->vp, a->stat);
}

static int vop_fgetstat(SceVopFgetstatArgs *a)
{
	return do_getstat(a->vp, a->stat);
}

static int vop_chstat(SceVopChstatArgs *a)
{
	return do_chstat(a->vp, a->stat, a->bit);
}

static int vop_fchstat(SceVopFchstatArgs *a)
{
	return do_chstat(a->vp, a->stat, a->bit);
}

static int vop_sync(SceVopSyncArgs *a)
{
	job j;

	job_for(&j, a->vp);
	if (!j.m)
		return NTFSFS_ERR(EINVAL);
	return ntfsfs_call(job_sync, &j);
}

static int vop_inactive(SceVopInactiveArgs *a)
{
	/* nothing allocated per vnode */
	a->vp->core.node_data = NULL;
	return 0;
}

static int vop_cleanup(SceVopCleanupArgs *a)
{
	(void)a;
	return 0;
}

const SceVopTable ntfsfs_vops = {
	.vop_open     = vop_open,
	.vop_create   = vop_create,
	.vop_close    = vop_close,
	.vop_lookup   = vop_lookup,
	.vop_read     = vop_read,
	.vop_write    = vop_write,
	.vop_lseek    = vop_lseek,
	.vop_remove   = vop_remove,
	.vop_mkdir    = vop_mkdir,
	.vop_rmdir    = vop_rmdir,
	.vop_dopen    = vop_dopen,
	.vop_dclose   = vop_dclose,
	.vop_dread    = vop_dread,
	.vop_getstat  = vop_getstat,
	.vop_chstat   = vop_chstat,
	.vop_rename   = vop_rename,
	.vop_pread    = vop_pread,
	.vop_pwrite   = vop_pwrite,
	.vop_inactive = vop_inactive,
	.vop_sync     = vop_sync,
	.vop_fgetstat = vop_fgetstat,
	.vop_fchstat  = vop_fchstat,
	.vop_cleanup  = vop_cleanup,
	/* ioctl, link, unlink, whiteout, zerofill: unsupported (0x80010030) */
};

/* ---- registration --------------------------------------------------------- */

static SceVfsInfo g_vfs_info = {
	.vfs_ops = &ntfsfs_vfs_ops,
	.vfs_name = "ntfs",
	.vfs_name_len = sizeof("ntfs"),
	.type = 2,            /* same value exfatfs registers with */
	.default_vops = &ntfsfs_vops,
};

int ntfsfs_vfs_register(void)
{
	return ksceVfsAddVfs(&g_vfs_info);
}

int ntfsfs_vfs_unregister(void)
{
	SceVfsInfo *info = NULL;

	return ksceVfsDeleteVfs("ntfs", &info);
}
