/*
 * libntfs-3g device backend over a Vita block device vnode.
 *
 * exfatfs (3.65, os0:kd/exfatfs.skprx, blk_read 0x81012ea8 / blk_write
 * 0x8101315c) reads its device the same way: lock the device vnode,
 * ksceVopPread() of whole sectors, unlock; lseek + read when the device has no
 * pread.  A failed transfer is retried once after ksceVopSync(vp, file, 2), or
 * after ksceVopCleanup() when the sync fails and the cleanup returns 0 or is
 * not implemented.  sdstor needs offsets and lengths aligned to the device
 * sector (0x80010022 otherwise), which is 512 bytes, or 4096 on some USB
 * disks; ntfsfs_probe() finds out which.
 */
#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include "config.h"
#include "types.h"
#include "device.h"

#include "ntfsfs.h"

#define VOP_HAS(vp, member) ((vp)->core.ops && (vp)->core.ops->member)
#define ERR_NOTSUP   ((int)0x80010030)
#define ERR_UNALIGN  ((int)0x80010022)

static uint32_t SECT(const ntfsfs_blk *b)
{
	return b->sector ? b->sector : NTFSFS_SECTOR;
}

static int blk_xfer(ntfsfs_blk *b, uint64_t off, void *buf, uint32_t len, int wr)
{
	SceSize done = 0;
	int r, tries = 0;

	if ((off | len) & (SECT(b) - 1))
		return NTFSFS_ERR(EINVAL);
	r = vfsLockVnode(b->vp);
	if (r < 0)
		return r;
	for (;;) {
		done = 0;
		if (wr && VOP_HAS(b->vp, vop_pwrite)) {
			r = ksceVopPwrite(b->vp, b->file, buf, len, (SceOff)off, &done);
		} else if (!wr && VOP_HAS(b->vp, vop_pread)) {
			r = ksceVopPread(b->vp, b->file, buf, len, (SceOff)off, &done);
		} else {
			SceOff p = ksceVopLseek(b->vp, b->file, (SceOff)off, SCE_SEEK_SET);

			if (p != (SceOff)off)
				r = NTFSFS_ERR(EIO);
			else if (wr)
				r = ksceVopWrite(b->vp, b->file, buf, len, &done);
			else
				r = ksceVopRead(b->vp, b->file, buf, len, &done);
		}
		if ((r >= 0 && done == len) || r == ERR_UNALIGN || tries++)
			break;
		/* one retry, as exfatfs does */
		if (ksceVopSync(b->vp, b->file, 2) < 0) {
			int c = ksceVopCleanup(b->vp, b->file);

			if (c < 0 && c != ERR_NOTSUP)
				break;
		}
	}
	vfsUnlockVnode(b->vp);
	if (r < 0)
		return r;
	return done == len ? 0 : NTFSFS_ERR(EIO);
}

int ntfsfs_blk_read(ntfsfs_blk *b, uint64_t off, void *buf, uint32_t len)
{
	return blk_xfer(b, off, buf, len, 0);
}

int ntfsfs_blk_write(ntfsfs_blk *b, uint64_t off, const void *buf, uint32_t len)
{
	if (b->rdonly)
		return NTFSFS_ERR(EROFS);
	return blk_xfer(b, off, (void *)buf, len, 1);
}

/* ---- MBR / boot sector probe -------------------------------------------- */

static int is_ntfs_boot(const uint8_t *s)
{
	return !memcmp(s + 3, "NTFS    ", 8) && s[510] == 0x55 && s[511] == 0xaa;
}

static void got_boot(ntfsfs_blk *b, const uint8_t *s, uint64_t off)
{
	b->part_off = off;
	/* volume_serial_number (le64 at 0x48), reported by devctl 0x3001 */
	b->serial = s[0x48] | s[0x49] << 8 | s[0x4a] << 16 | (uint32_t)s[0x4b] << 24;
}

int ntfsfs_probe(ntfsfs_blk *b)
{
	uint8_t *s = b->bounce;
	int i, r;

	b->part_off = 0;
	/* sector 0 tells the device sector size: sdstor refuses misaligned reads */
	b->sector = NTFSFS_SECTOR;
	r = ntfsfs_blk_read(b, 0, s, b->sector);
	if (r == ERR_UNALIGN) {
		b->sector = 4096;
		r = ntfsfs_blk_read(b, 0, s, b->sector);
	}
	if (r < 0)
		return r;
	if (is_ntfs_boot(s)) {
		got_boot(b, s, 0);
		return 0;
	}
	if (s[510] != 0x55 || s[511] != 0xaa)
		return NTFSFS_ERR(EINVAL);

	/* Whole-disk device: look for an NTFS (type 0x07) MBR partition. */
	for (i = 0; i < 4; i++) {
		const uint8_t *e = s + 446 + i * 16;
		uint32_t lba = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
		uint64_t off = (uint64_t)lba * b->sector;

		if (e[4] != 0x07 || !lba)
			continue;
		r = ntfsfs_blk_read(b, off, s, b->sector);
		if (r < 0)
			return r;
		if (is_ntfs_boot(s)) {
			got_boot(b, s, off);
			return 0;
		}
		/* re-read the MBR for the next entry */
		r = ntfsfs_blk_read(b, 0, s, b->sector);
		if (r < 0)
			return r;
	}
	return NTFSFS_ERR(EINVAL);
}

/* ---- ntfs_device_operations --------------------------------------------- */

static ntfsfs_blk *BLK(struct ntfs_device *dev)
{
	return dev->d_private;
}

static int set_errno(int sce)
{
	errno = (sce & 0xffff0000) == 0x80010000 ? (sce & 0xffff) : EIO;
	return -1;
}

static int dev_open(struct ntfs_device *dev, int flags)
{
	ntfsfs_blk *b = BLK(dev);

	if ((flags & O_ACCMODE) != O_RDONLY && b->rdonly) {
		errno = EROFS;
		return -1;
	}
	if ((flags & O_ACCMODE) == O_RDONLY)
		NDevSetReadOnly(dev);
	NDevSetBlock(dev);
	NDevSetOpen(dev);
	b->pos = 0;
	return 0;
}

static int dev_close(struct ntfs_device *dev)
{
	NDevClearOpen(dev);
	return 0;
}

static s64 dev_pread(struct ntfs_device *dev, void *buf, s64 count, s64 offset)
{
	ntfsfs_blk *b = BLK(dev);
	uint8_t *out = buf;
	s64 total = 0;
	int r;

	if (offset < 0 || count < 0) {
		errno = EINVAL;
		return -1;
	}
	while (count > 0) {
		uint32_t sz = SECT(b);
		uint64_t abs = b->part_off + (uint64_t)offset;
		uint32_t head = abs & (sz - 1);

		if (!head && count >= sz) {
			/* aligned middle part straight into the caller's buffer */
			uint32_t n = (count > 0x100000 ? 0x100000 : (uint32_t)count)
				     & ~(sz - 1);
			r = ntfsfs_blk_read(b, abs, out, n);
			if (r < 0)
				return total ? total : set_errno(r);
			out += n; offset += n; count -= n; total += n;
		} else {
			uint64_t base = abs - head;
			uint32_t span = head + (uint32_t)(count > NTFSFS_BOUNCE ? NTFSFS_BOUNCE : count);
			uint32_t n;

			span = (span + sz - 1) & ~(sz - 1);
			if (span > NTFSFS_BOUNCE)
				span = NTFSFS_BOUNCE;
			r = ntfsfs_blk_read(b, base, b->bounce, span);
			if (r < 0)
				return total ? total : set_errno(r);
			n = span - head;
			if ((s64)n > count)
				n = (uint32_t)count;
			memcpy(out, b->bounce + head, n);
			out += n; offset += n; count -= n; total += n;
		}
	}
	return total;
}

static s64 dev_pwrite(struct ntfs_device *dev, const void *buf, s64 count, s64 offset)
{
	ntfsfs_blk *b = BLK(dev);
	const uint8_t *in = buf;
	s64 total = 0;
	int r;

	if (NDevReadOnly(dev) || b->rdonly) {
		errno = EROFS;
		return -1;
	}
	if (offset < 0 || count < 0) {
		errno = EINVAL;
		return -1;
	}
	NDevSetDirty(dev);
	while (count > 0) {
		uint32_t sz = SECT(b);
		uint64_t abs = b->part_off + (uint64_t)offset;
		uint32_t head = abs & (sz - 1);

		if (!head && count >= sz) {
			uint32_t n = (count > 0x100000 ? 0x100000 : (uint32_t)count)
				     & ~(sz - 1);
			r = ntfsfs_blk_write(b, abs, in, n);
			if (r < 0)
				return total ? total : set_errno(r);
			in += n; offset += n; count -= n; total += n;
		} else {
			/* read-modify-write the partial sector(s) */
			uint64_t base = abs - head;
			uint32_t n = NTFSFS_BOUNCE - head;
			uint32_t span;

			if ((s64)n > count)
				n = (uint32_t)count;
			span = (head + n + sz - 1) & ~(sz - 1);
			r = ntfsfs_blk_read(b, base, b->bounce, span);
			if (r < 0)
				return total ? total : set_errno(r);
			memcpy(b->bounce + head, in, n);
			r = ntfsfs_blk_write(b, base, b->bounce, span);
			if (r < 0)
				return total ? total : set_errno(r);
			in += n; offset += n; count -= n; total += n;
		}
	}
	return total;
}

static s64 dev_seek(struct ntfs_device *dev, s64 offset, int whence)
{
	ntfsfs_blk *b = BLK(dev);

	switch (whence) {
	case SEEK_SET: b->pos = offset; break;
	case SEEK_CUR: b->pos += offset; break;
	default:
		/* SEEK_END needs the device size, which nothing here depends on */
		errno = EINVAL;
		return -1;
	}
	return b->pos;
}

static s64 dev_read(struct ntfs_device *dev, void *buf, s64 count)
{
	s64 r = dev_pread(dev, buf, count, BLK(dev)->pos);

	if (r > 0)
		BLK(dev)->pos += r;
	return r;
}

static s64 dev_write(struct ntfs_device *dev, const void *buf, s64 count)
{
	s64 r = dev_pwrite(dev, buf, count, BLK(dev)->pos);

	if (r > 0)
		BLK(dev)->pos += r;
	return r;
}

static int dev_sync(struct ntfs_device *dev)
{
	ntfsfs_blk *b = BLK(dev);
	int r;

	if (NDevReadOnly(dev) || b->rdonly)
		return 0;
	r = vfsLockVnode(b->vp);
	if (r < 0)
		return set_errno(r);
	r = ksceVopSync(b->vp, b->file, 0);
	vfsUnlockVnode(b->vp);
	/* 0x80010030: device has no sync op */
	if (r < 0 && r != (int)0x80010030)
		return set_errno(r);
	NDevClearDirty(dev);
	return 0;
}

static int dev_stat(struct ntfs_device *dev, struct stat *st)
{
	(void)dev;
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFBLK;
	return 0;
}

static int dev_ioctl(struct ntfs_device *dev, unsigned long req, void *argp)
{
	(void)dev; (void)req; (void)argp;
	errno = ENOTTY;
	return -1;
}

struct ntfs_device_operations ntfsfs_dev_ops = {
	.open   = dev_open,
	.close  = dev_close,
	.seek   = dev_seek,
	.read   = dev_read,
	.write  = dev_write,
	.pread  = dev_pread,
	.pwrite = dev_pwrite,
	.sync   = dev_sync,
	.stat   = dev_stat,
	.ioctl  = dev_ioctl,
};
