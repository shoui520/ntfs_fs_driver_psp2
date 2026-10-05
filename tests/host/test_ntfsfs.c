/*
 * Drives the ntfsfs VFS ops exactly as iofilemgr would, against an image file
 * (or raw block device) given on the command line.
 *
 * usage: test_ntfsfs <image> [ro]
 *   expects the image to contain /hello.txt with "hello from ntfsprogs\n"
 *   (see run.sh) unless "ro" is given, in which case it only lists and reads.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* glibc (host test build) defines these as macros; SceIoStat uses the names */
#undef st_atime
#undef st_ctime
#undef st_mtime

#include <psp2kern/vfs.h>
#include <psp2kern/kernel/iofilemgr.h>

#include "mock_vita.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CHECK_R(r) do { int _r = (r); if (_r < 0) { fprintf(stderr, "FAIL %s:%d: %s = 0x%08X\n", __FILE__, __LINE__, #r, _r); fails++; } } while (0)

static const SceVopTable *V;
static SceVfsMount mnt;
static SceVfsVnode *root;

static SceVfsPath P(const char *name)
{
	SceVfsPath p = { name, (SceSize)strlen(name), name };
	return p;
}

static SceVfsVnode *lookup(SceVfsVnode *dir, const char *name, int *err)
{
	SceVfsVnode *vp = NULL;
	SceVfsPath p = P(name);
	SceVopLookupArgs a = { dir, &vp, &p, 0 };
	int r = V->vop_lookup(&a);

	if (err)
		*err = r;
	if (r < 0)
		return NULL;
	vfsUnlockVnode(vp);
	return vp;
}

static SceVfsVnode *mk(SceVfsVnode *dir, const char *name, int is_dir)
{
	SceVfsVnode *vp = NULL;
	SceVfsPath p = P(name);
	int r;

	if (is_dir) {
		SceVopMkdirArgs a = { dir, &vp, &p, 0777 };
		r = V->vop_mkdir(&a);
	} else {
		SceVopCreateArgs a = { dir, &vp, &p, SCE_O_WRONLY | SCE_O_CREAT, 0666 };
		r = V->vop_create(&a);
	}
	CHECK_R(r);
	if (r < 0)
		return NULL;
	vfsUnlockVnode(vp);
	return vp;
}

static SceVfsFile *fopen_(SceVfsVnode *vp, int flags)
{
	SceVfsFile *f = calloc(1, sizeof(*f));
	SceVfsPath p = P("x");
	SceVopOpenArgs a = { vp, &p, flags, f };
	int r = V->vop_open(&a);

	f->vp = vp;
	if (r < 0) {
		free(f);
		return NULL;
	}
	return f;
}

static void fclose_(SceVfsVnode *vp, SceVfsFile *f)
{
	SceVopCloseArgs a = { vp, f };

	CHECK_R(V->vop_close(&a));
	free(f);
}

static SceSSize fwrite_(SceVfsVnode *vp, SceVfsFile *f, const void *b, SceSize n)
{
	SceVopWriteArgs a = { vp, f, b, n };
	return V->vop_write(&a);
}

static SceSSize fread_(SceVfsVnode *vp, SceVfsFile *f, void *b, SceSize n)
{
	SceVopReadArgs a = { vp, f, b, n };
	return V->vop_read(&a);
}

static SceSSize fpread_(SceVfsVnode *vp, SceVfsFile *f, void *b, SceSize n, SceOff off)
{
	SceVopPreadArgs a = { vp, f, b, n, off };
	return V->vop_pread(&a);
}

static int getstat(SceVfsVnode *vp, SceIoStat *st)
{
	SceVfsPath p = P("x");
	SceVopGetstatArgs a = { vp, &p, st };
	return V->vop_getstat(&a);
}

static int listdir(SceVfsVnode *vp, const char *want, int verbose)
{
	SceVfsFile f;
	SceVfsPath p = P("x");
	SceVopDopenAgrs o = { vp, &p, &f };
	SceIoDirent de;
	SceVopDreadArgs d = { vp, &f, &de };
	SceVopDcloseArgs c = { vp, &f };
	int found = 0, r;

	memset(&f, 0, sizeof(f));
	CHECK_R(V->vop_dopen(&o));
	while ((r = V->vop_dread(&d)) > 0) {
		if (verbose)
			printf("  %c %10lld %04d-%02d-%02d %s\n",
			       SCE_S_ISDIR(de.d_stat.st_mode) ? 'd' : '-',
			       (long long)de.d_stat.st_size, de.d_stat.st_mtime.year,
			       de.d_stat.st_mtime.month, de.d_stat.st_mtime.day, de.d_name);
		if (want && !strcmp(de.d_name, want))
			found = 1;
	}
	CHECK(r == 0);
	CHECK_R(V->vop_dclose(&c));
	return found;
}

static void fill(unsigned char *b, size_t n, unsigned seed)
{
	size_t i;

	for (i = 0; i < n; i++) {
		seed = seed * 1103515245u + 12345u;
		b[i] = seed >> 16;
	}
}

static void test_rw(void)
{
	enum { BIG = 3 * 1024 * 1024 + 777 };
	unsigned char *src = malloc(BIG), *dst = malloc(BIG);
	SceVfsVnode *dir, *vp, *vp2;
	SceVfsFile *f;
	SceIoStat st;
	SceSSize n;
	size_t off;
	int err;

	/* reading a file created by ntfsprogs */
	vp = lookup(root, "hello.txt", &err);
	CHECK_R(err);
	if (vp) {
		char buf[64] = { 0 };
		f = fopen_(vp, SCE_O_RDONLY);
		CHECK(f);
		n = fread_(vp, f, buf, sizeof(buf));
		CHECK(n == 21 && !memcmp(buf, "hello from ntfsprogs\n", 21));
		fclose_(vp, f);
	}
	/* case-insensitive lookup, like Windows */
	CHECK(lookup(root, "HELLO.TXT", NULL) != NULL);
	CHECK(lookup(root, "missing", &err) == NULL && err == (int)0x80010002);
	/* metadata files are hidden */
	CHECK(lookup(root, "$MFT", &err) == NULL);
	/* stat as exfatfs: mode from the attribute table, attribute byte */
	vp = lookup(root, "hello.txt", NULL);
	if (vp) {
		CHECK_R(getstat(vp, &st));
		CHECK(st.st_mode == (SCE_S_IFREG | 0x186));
		CHECK((st.st_attr & 0x17) == 0);
	}
	CHECK_R(getstat(root, &st));
	CHECK(st.st_mode == (SCE_S_IFDIR | 0x186) && (st.st_attr & 0x10));

	dir = mk(root, "dir", 1);
	CHECK(dir && dir->core.type == SCE_VNODE_TYPE_DIR);
	vp = mk(dir, "big.bin", 0);
	CHECK(vp);
	{
		SceVfsVnode *dup = NULL;
		SceVfsPath p = P("big.bin");
		SceVopCreateArgs a = { dir, &dup, &p, SCE_O_WRONLY | SCE_O_CREAT, 0666 };
		CHECK(V->vop_create(&a) == (int)0x80010011);
	}

	/* write in odd-sized chunks */
	fill(src, BIG, 42);
	f = fopen_(vp, SCE_O_WRONLY);
	CHECK(f);
	for (off = 0; off < BIG; ) {
		size_t c = (off / 7 % 5) * 12345 + 1;
		if (c > BIG - off)
			c = BIG - off;
		n = fwrite_(vp, f, src + off, c);
		CHECK(n == (SceSSize)c);
		if (n <= 0)
			break;
		off += n;
	}
	CHECK(f->position == BIG);
	CHECK(vp->core.size == BIG);
	fclose_(vp, f);

	/* fresh lookup sees the size; read back in odd chunks */
	vp2 = lookup(dir, "big.bin", &err);
	CHECK(vp2 && vp2->core.size == BIG);
	f = fopen_(vp2, SCE_O_RDONLY);
	memset(dst, 0, BIG);
	for (off = 0; off < BIG; ) {
		size_t c = 4093 + off % 3;
		n = fread_(vp2, f, dst + off, c);
		if (n <= 0)
			break;
		off += n;
	}
	CHECK(off == BIG && !memcmp(src, dst, BIG));
	CHECK(fread_(vp2, f, dst, 10) == 0);   /* EOF */
	/* pread does not move the position */
	n = fpread_(vp2, f, dst, 1000, 1234567);
	CHECK(n == 1000 && !memcmp(dst, src + 1234567, 1000));
	CHECK(f->position == BIG);
	{
		SceVopLseekArgs a = { vp2, f, -100, SCE_SEEK_END };
		CHECK(V->vop_lseek(&a) == BIG - 100);
	}
	/* writing through a read-only handle fails */
	CHECK(fwrite_(vp2, f, src, 10) < 0);
	fclose_(vp2, f);

	/* O_APPEND */
	f = fopen_(vp2, SCE_O_WRONLY | SCE_O_APPEND);
	CHECK(fwrite_(vp2, f, "tail", 4) == 4);
	fclose_(vp2, f);
	CHECK_R(getstat(vp2, &st));
	CHECK(st.st_size == BIG + 4 && SCE_S_ISREG(st.st_mode));
	CHECK(st.st_mtime.year >= 2024);

	/* truncate via chstat, then O_TRUNC */
	{
		SceVfsPath p = P("x");
		SceIoStat s2;
		SceVopChstatArgs a = { vp2, &p, &s2, SCE_CST_SIZE };
		memset(&s2, 0, sizeof(s2));
		s2.st_size = 1000;
		CHECK_R(V->vop_chstat(&a));
		CHECK_R(getstat(vp2, &st));
		CHECK(st.st_size == 1000);
	}
	f = fopen_(vp2, SCE_O_WRONLY | SCE_O_TRUNC);
	CHECK(fwrite_(vp2, f, "abc", 3) == 3);
	fclose_(vp2, f);
	CHECK_R(getstat(vp2, &st));
	CHECK(st.st_size == 3);

	/* chstat times */
	{
		SceVfsPath p = P("x");
		SceIoStat s2;
		SceVopChstatArgs a = { vp2, &p, &s2, SCE_CST_MT };
		memset(&s2, 0, sizeof(s2));
		s2.st_mtime.year = 2011; s2.st_mtime.month = 12; s2.st_mtime.day = 17;
		s2.st_mtime.hour = 9;
		CHECK_R(V->vop_chstat(&a));
		CHECK_R(getstat(vp2, &st));
		CHECK(st.st_mtime.year == 2011 && st.st_mtime.month == 12 &&
		      st.st_mtime.day == 17 && st.st_mtime.hour == 9);
	}

	/*
	 * rename across directories: an existing target is refused (EEXIST,
	 * as exfatfs and iofilemgr), a free name works
	 */
	{
		SceVfsVnode *other = mk(root, "taken.txt", 0), *nvp = NULL;
		SceVfsPath op = P("big.bin"), tp = P("taken.txt"), np = P("victim.txt");
		SceVopRenameArgs t = { dir, vp2, &op, root, &nvp, &tp };
		SceVopRenameArgs a = { dir, vp2, &op, root, &nvp, &np };

		CHECK(other);
		CHECK(V->vop_rename(&t) == (int)0x80010011);
		CHECK(lookup(root, "taken.txt", NULL) && lookup(dir, "big.bin", NULL));
		CHECK_R(V->vop_rename(&a));
		CHECK(nvp && nvp->core.dd == root);
		if (nvp)
			vfsUnlockVnode(nvp);
		CHECK(lookup(dir, "big.bin", NULL) == NULL);
		vp = lookup(root, "victim.txt", &err);
		CHECK(vp && vp->core.size == 3);
	}

	/* rename a directory into a subdirectory of itself is refused */
	{
		SceVfsVnode *sub = mk(dir, "sub", 1), *nvp = NULL;
		SceVfsPath op = P("dir"), np = P("dir2");
		SceVopRenameArgs a = { root, dir, &op, sub, &nvp, &np };
		CHECK(sub);
		sub->core.dd = dir;
		CHECK(V->vop_rename(&a) < 0);
	}

	/* rmdir non-empty fails, remove + rmdir succeed */
	{
		SceVfsPath p = P("dir");
		SceVopRmdirArgs a = { root, dir, &p };
		SceVfsVnode *sub = lookup(dir, "sub", NULL);
		SceVfsPath sp = P("sub");
		SceVopRmdirArgs sa = { dir, sub, &sp };

		CHECK(V->vop_rmdir(&a) == NTFSFS_ERR(ENOTEMPTY));
		CHECK_R(V->vop_rmdir(&sa));
		CHECK_R(V->vop_rmdir(&a));
		CHECK(lookup(root, "dir", NULL) == NULL);
	}
	{
		SceVfsPath p = P("victim.txt");
		SceVopRemoveArgs a = { root, vp, &p, 0 };
		SceVopRmdirArgs ra = { root, vp, &p };
		CHECK(V->vop_rmdir(&ra) < 0);   /* not a directory */
		CHECK_R(V->vop_remove(&a));
		CHECK(lookup(root, "victim.txt", NULL) == NULL);
	}

	/* a rename never replaces: a second SCEDEL~ name collision gets EEXIST */
	{
		SceVfsVnode *x = mk(root, "keep.txt", 0), *y = mk(root, "move.txt", 0), *nvp = NULL;
		SceVfsPath op = P("move.txt"), np = P("keep.txt");
		SceVopRenameArgs a = { root, y, &op, NULL, &nvp, &np };
		CHECK(x && y);
		CHECK(V->vop_rename(&a) == (int)0x80010011);
		CHECK(lookup(root, "keep.txt", NULL) && lookup(root, "move.txt", NULL));
	}
	/* mode without the write bit: READONLY (no ACMgr in the host build) */
	{
		SceVfsVnode *r = mk(root, "keep.txt2", 0);
		SceIoStat cs;
		memset(&cs, 0, sizeof(cs));
		cs.st_mode = SCE_S_IRUSR;
		if (r) {
			SceVfsPath p = P("keep.txt2");
			SceVopChstatArgs ca = { r, &p, &cs, SCE_CST_MODE };
			CHECK_R(V->vop_chstat(&ca));
			CHECK((r->core.acl_data[0] & 7) == 1);
			CHECK_R(getstat(r, &cs));
			CHECK(cs.st_mode == (SCE_S_IFREG | 0x106) && (cs.st_attr & 1));
		}
	}

	/* delete-while-open: iofilemgr renames to SCEDEL~ with ndvp == NULL */
	{
		SceVfsVnode *o = mk(root, "open.txt", 0), *nvp = NULL;
		SceVfsFile *of = fopen_(o, SCE_O_RDWR);
		SceVfsPath op = P("open.txt"), np = P("SCEDEL~~~~~~~~");
		SceVopRenameArgs a = { root, o, &op, NULL, &nvp, &np };
		char b[8] = { 0 };

		CHECK(fwrite_(o, of, "still", 5) == 5);
		CHECK_R(V->vop_rename(&a));
		CHECK(nvp && nvp->core.dd == root);
		if (nvp)
			vfsUnlockVnode(nvp);
		CHECK(lookup(root, "open.txt", NULL) == NULL);
		CHECK(!listdir(root, "SCEDEL~~~~~~~~", 0));
		/* the open handle keeps working on the moved file */
		CHECK(fpread_(nvp, of, b, 5, 0) == 5 && !memcmp(b, "still", 5));
		/* still listed under its hidden name while open */
		CHECK(lookup(root, "SCEDEL~~~~~~~~", NULL) != NULL);
		fclose_(nvp, of);
		/* last close deletes it, as exfatfs does */
		CHECK(lookup(root, "SCEDEL~~~~~~~~", NULL) == NULL);
		/* then vfsClose may call remove on it with a NULL dvp */
		{
			SceVopRemoveArgs ra = { NULL, nvp, &np, 0 };
			CHECK(V->vop_remove(&ra) < 0);
		}
	}

	/* leave something behind for the external check */
	vp = mk(root, "from_vita.txt", 0);
	f = fopen_(vp, SCE_O_WRONLY);
	CHECK(fwrite_(vp, f, "written by ntfsfs\n", 17) == 17);
	fclose_(vp, f);
	{
		SceVfsVnode *d = mk(root, "日本語フォルダ", 1);
		CHECK(d);
		CHECK(lookup(root, "日本語フォルダ", NULL) != NULL);
	}

	free(src);
	free(dst);
}

int main(int argc, char **argv)
{
	SceVfsVnode *blk;
	SceVfsPath devpath = P("sdstor0:uma-lp-act-entire");
	SceVfsOpMountArgs ma;
	SceVfsOpGetRootArgs ga;
	SceVfsOpUmountArgs ua;
	SceIoDevInfo info;
	SceVfsOpDevctlArg dc;
	int ro = argc > 2 && !strcmp(argv[2], "ro");
	struct timespec t0, t1;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <image> [ro]\n", argv[0]);
		return 2;
	}
	if (getenv("NTFSFS_TEST_SECTOR"))
		mock_sector = atoi(getenv("NTFSFS_TEST_SECTOR"));
	mock_img_fd = open(argv[1], ro ? O_RDONLY : O_RDWR);
	if (mock_img_fd < 0) {
		perror(argv[1]);
		return 2;
	}
	CHECK_R(ntfsfs_heap_init());
	CHECK_R(ntfsfs_vfs_register());
	CHECK(mock_vfs && !strcmp(mock_vfs->vfs_name, "ntfs"));
	V = mock_vfs->default_vops;

	blk = mock_blockdev();
	mnt.mnt_vnode = blk;
	mnt.mnt_vfs_inf = mock_vfs;
	mnt.mnt_flags = SCE_VFS_MOUNT_TYPE_FSROOT | (ro ? SCE_VFS_MOUNT_FLAG_RDONLY : 0);
	ma.mnt = &mnt;
	ma.dev_file_path = &devpath;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	CHECK_R(mock_vfs->vfs_ops->vfs_mount(&ma));
	if (!mnt.data) {
		fprintf(stderr, "mount failed\n");
		return 1;
	}
	CHECK(blk->core.ref_count == 1);

	ga.mnt = &mnt;
	ga.vpp = &root;
	CHECK_R(mock_vfs->vfs_ops->vfs_get_root(&ga));
	CHECK(root && root->core.type == SCE_VNODE_TYPE_ROOTDIR);
	vfsUnlockVnode(root);

	memset(&dc, 0, sizeof(dc));
	dc.mnt = &mnt;
	dc.cmd = 0x3001;
	dc.buf = &info;
	dc.buf_len = sizeof(info);
	CHECK_R(mock_vfs->vfs_ops->vfs_devctl(&dc));
	printf("size %lld free %lld cluster %u\n", (long long)info.max_size,
	       (long long)info.free_size, info.cluster_size);
	CHECK(info.max_size > 0 && info.free_size > 0 && info.free_size <= info.max_size);
	/* the idle daemon's 0x3803 must get ENOENT (see vfs_devctl) */
	dc.cmd = 0x3803;
	dc.buf = NULL;
	dc.buf_len = 0;
	CHECK(mock_vfs->vfs_ops->vfs_devctl(&dc) == (int)0x80010002);
	{
		/* max file size (exfatfs: all ones for exFAT) */
		unsigned long long max = 0;
		dc.cmd = 0x80000001;
		dc.buf = &max;
		dc.buf_len = 8;
		CHECK_R(mock_vfs->vfs_ops->vfs_devctl(&dc));
		CHECK(max == ~0ULL);
	}
	{
		/* 0x28 form: serial at 0x18, label at 0x1c (as exfatfs) */
		unsigned char big[0x28];
		dc.cmd = 0x3001;
		dc.buf = big;
		dc.buf_len = sizeof(big);
		CHECK_R(mock_vfs->vfs_ops->vfs_devctl(&dc));
		CHECK(!memcmp(big + 0x1c, "ntfsfs-test", 11) || !memcmp(big + 0x1c, "a9         ", 11));
		CHECK(big[0x27] == 0);
	}
	{
		/* decode_path_elem: upcased element, like exfatfs */
		char buf[0x400];
		const char *p2 = NULL, *p3 = NULL;
		SceSize len = 0;
		const char *path = "Hello.txt/rest";
		SceVfsOpDecodePathElemArgs d = { &mnt, path, &p2, &p3, buf, sizeof(buf), &len };
		CHECK_R(mock_vfs->vfs_ops->vfs_decode_path_elem(&d));
		CHECK(len == 9 && !memcmp(buf, "HELLO.TXT", 9) && p2 == path && p3 == path + 9);
		path = "\xc3\xa9t\xc3\xa9";   /* e-acute t e-acute */
		d.path = path;
		CHECK_R(mock_vfs->vfs_ops->vfs_decode_path_elem(&d));
		CHECK(len == 5 && !memcmp(buf, "\xc3\x89T\xc3\x89", 5));
	}

	printf("root:\n");
	listdir(root, NULL, 1);
	if (!ro) {
		test_rw();
		CHECK(listdir(root, "from_vita.txt", 0));
		CHECK(!listdir(root, "$MFT", 0));
	}

	ua.mnt = &mnt;
	ua.flags = 0;
	CHECK_R(mock_vfs->vfs_ops->vfs_umount(&ua));
	CHECK(blk->core.ref_count == 0);
	CHECK(mock_locked == 0);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("device reads %ld writes %ld, %.3f s\n", mock_dev_reads, mock_dev_writes,
	       (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
	printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
	return fails ? 1 : 0;
}
