# exfatfs.skprx (retail 3.65), reverse engineered

`os0:kd/exfatfs.skprx` (module `SceExfatfs`) is the PS Vita's only FAT-family
filesystem driver. Despite the name it implements **FAT12, FAT16, FAT32 and
exFAT**; internally it is Sony's "UVFAT" engine (the `UVFAT12 ` and `UVFAT`
boot-sector labels it recognises) wrapped in an iofilemgr VFS front end.

Every one of the module's 282 functions is named and described in
[reference.md](reference.md), together with the layouts of its data structures.
Each function body was read in full, not just its head. Where Ghidra's
decompilation was unreliable (dropped blocks, NEON-vectorised loops, a switch
split into fragments) the disassembly was read instead. All other bytes of
`.text` are literal pools, padding, `tbb` jump tables or the import stubs.
This page explains how the pieces fit together.

Firmware: retail 3.65 `PSP2UPDAT.PUP` (133754368 bytes). Addresses are link
addresses (text at `0x81000000`, data at `0x8101d000`). Everything here is from
static analysis of that binary; nothing was observed on hardware.

## Files here

| file | contents |
|---|---|
| [reference.md](reference.md) | structures and the full function index (generated) |
| [exfat-names.tsv](exfat-names.tsv) | address, name, prototype, notes for every function |
| [exfat-types.spec](exfat-types.spec) | structure layouts and globals |
| [tools/](tools) | Ghidra scripts that apply the two files above to a Ghidra project, and `gendoc.py` that renders reference.md |

To reproduce the annotated database: import the decrypted module with
VitaLoaderRedux, run `CreateFuncs.java` with the vfs/vop table entries (it also
creates every `bl` target that Ghidra missed), then
`python3 tools/genspec.py exfat-types.spec exfat-names.tsv > gen.spec` and run
`ApplyTypes.java gen.spec`.

## Layers

```
 iofilemgr ──► VFS front end      vfs_* / vop_* (0x81008c88-0x8100b0f4)
                │                 vnode <-> fnode, file->fd = fd index, ACL checks
                ▼
              UVFAT API           uvfat_open/read/write/dread/chstat/... (0x81003384-0x81006a1c)
                │                 fd and fnode tables, path parsing, trash, quota
                ▼
              fnode / fd layer    fnode_get/_release, fd_reserve/_release (0x81006bb0-0x810086d8)
                │
                ▼
              directory engine    exFAT entry sets, FAT LFN/8.3, create/lookup (0x8100e11c-0x810123f0)
              cluster engine      seek, extend, extent cache, free (0x8100bc80-0x8100e0e8)
              allocation          FAT windows, exFAT bitmap, upcase (0x8100b0f4-0x8100bc38, 0x81013378-0x81014f44)
                │
                ▼
              block I/O           blk_read / blk_write on the device vnode (0x81012ea8, 0x8101315c)
```

Helpers at the bottom of the address space: SceSblACMgr wrappers
(`0x81000000-0x81000348`), OEM codepage conversion (`0x81000348-0x81000c94`),
the `SceExfatfsCommon` heap and path buffers (`0x81000c94-0x81000e34`),
UTF-8/UTF-16 and name parsing (`0x81000e34-0x81001c50`).

## Global state

One `uvfat_global` of 0x295D60 bytes lives in `.bss` at `0x8101d080`:

| offset | contents |
|---|---|
| `+0x0` | 15 drives × 0x430. A drive is one mounted volume: device name, boot-sector geometry, FAT window cache, exFAT bitmap and upcase state, a drive-private directory iterator, a root fnode and a fast mutex (`SceExfatfsRoot<ASSIGN>`) |
| `+0x3ed0` | 15 × 0x200 sector buffers (sector 0 of each drive) |
| `+0x5cd0` | pointer to the fd pool: 0x38-byte open handles, 512 at a time |
| `+0x5cd8` | 1024 fnodes × 0x290 (one per vnode), growable to 0x1000 |
| `+0x295cd8` | 15 drive pins `{active, drive*}` |

Drives are found by mount context with a 15-way comparison that the compiler
inlined into dozens of functions; that is the long `if` chain seen everywhere
in the decompilation.

The per-mount context `exfat_mnt` (0x230 bytes, `mnt->data`) holds the mount
flags, the assign name, a fast mutex (`SceExfatfsDrive<ASSIGN>`) that every
vop takes, `mnt_data`, and the block device vnode and file opened at mount.

## Mounting

`module_start` registers the VFS (`SceVfsInfo` at `0x8101d00c`, name `exfat`,
type 2), initialises the engine and mounts `os0:`, then queues
`exfat_init_thread` on a work queue to mount `vs0 sa0 vd0 tm0 ud0 pd0 ur0` and,
outside manufacturing mode, `ux0:`. Manufacturing and special boot modes mount
`sd0:`, `ux0:`, `gro0:` and `grw0:` directly.

`vfs_mount` → `drive_mount` → `uvfat_drive_mount` → `uvfat_volume_mount`:

1. The block device is opened read/write unless the mount is `RDONLY` or the
   device reports write protection (devctl 1, then `0x8001001E`).
2. Mount options: `1` read-only, `2` read/write, `|0x400` cluster (extent)
   cache for every device except `sd0 os0 tm0 sa0 pd0`, `|0x200` whole-FAT
   caching for `sa0:`.
3. Sector 0 must end in `0x55AA`. exFAT is detected by zero BPB bytes at
   `0x0b-0x0c`. It takes BytesPerSectorShift 9-12 (shift sum ≤ 25), records
   ClusterHeapOffset, the root cluster and the active FAT, then loads the
   Allocation Bitmap (`0x81`) and Up-case Table (`0x82`) entries from the root.
   The up-case checksum is verified, and `0xFFFF` identity runs are kept as
   extents. The cluster count is derived from VolumeLength and
   ClusterHeapOffset; the ClusterCount field is ignored. FAT needs 512-byte
   sectors. FAT32 is chosen when BPB bytes `0x11-0x13` are zero (RootEntCnt and
   the low byte of TotSec16). FAT12 is recognised only by the `FAT12   ` or
   `UVFAT12 ` label, not by cluster count; anything else is FAT16. The cluster
   count is clamped to what the FAT can address. The io block size is always
   512.
4. The FAT width is also stored in `exfat_mnt.flags` (`0x1000` 32-bit,
   `0x8000` FAT16). The ACL wrappers use those bits to pick the permission
   class: 2 for FAT16, 3 for everything else (FAT12 sets neither bit).

`vfs_mount` then sets `available_entry_num = 0x40` and
`default_io_cache_size = min(cluster, 0x8000)`.

## Objects: fnodes and fds

- An **fnode** (0x290) is the node behind a vnode (`vnode->node_data`). It
  stores the full UTF-16 path, which is also the lookup key: `fnode_get` returns
  the existing fnode for a drive+path pair or allocates one. It also holds a
  copy of the directory entry (`finfo` at `+0x210`: mode, type, size, times,
  first cluster, attributes, exFAT flags), the parent directory's location
  (`+0x250..+0x26c`), a 42-entry extent cache, a reference count (`+0x270`)
  and an open-fd count (`+0x272`).
- An **fd** (0x38) is an open handle (`SceVfsFile.fd` holds its index). It has
  a mode (1 read, 2 write, 6 read/write, 3 directory), a 64-bit position, the
  current cluster, and a one-block buffer (the drive's io size, ≤ 512 bytes)
  with a dirty flag. Several fds on one fnode keep their buffers coherent
  (`fd_share_block`).

## Reading and writing

`uvfat_read` and `uvfat_write` go through `fd_read`/`fd_write`. Unaligned edges
use the fd's block buffer. Aligned runs that are physically contiguous
(`fd_contig_read` for reads, `fd_contig_span` for writes) are transferred
straight between the caller's buffer and the device. Evicting a dirty buffer
block of a file also stamps its mtime and rewrites its entry. Every block flush
marks the owning fnode's mtime (`fd_flush_block` → `fnode_touch`).

Seeking resolves a byte position to a cluster with the extent cache first,
then by walking the FAT chain (`fd_seek`). exFAT files flagged NoFatChain are
contiguous by definition and resolve arithmetically. Extending a file
(`fd_extend`) allocates clusters with `cluster_find_free`, which uses the exFAT
bitmap from a lowest-free hint or scans FAT windows. A NoFatChain file that
cannot stay contiguous is converted to a real FAT chain (`fat_set_run`).
Writing past the end first zero-fills the gap (`fd_write_at`). Growing a file
with chstat instead uses an allocate-only `fd_write` (flag `0x100`), which
copies nothing and does not dirty partial blocks, so the new tail is not zeroed
on disk. FAT refuses files beyond 4 GiB (`0x8001001B`).

Block I/O (`blk_read`/`blk_write`) locks the device vnode and calls
`ksceVopPread`/`ksceVopPwrite`, or lseek plus read/write. Transfers must be
whole 512-byte sectors. On failure it calls `ksceVopSync(…, 2)` or
`ksceVopCleanup` and retries once.

Metadata is written back lazily. An fnode marked dirty (`+0x26d & 4`) is
rewritten by `fnode_writeback`. On exFAT it updates the File and Stream
Extension entries and recomputes SetChecksum. ValidDataLength is always
written equal to DataLength, so there is no separate valid length. On FAT it
rewrites the 8.3 entry, keeping its 10 ms byte. FAT
windows, the exFAT bitmap sector and the exFAT boot sector are flushed by
`drive_flush`, which also forces PercentInUse to `0xFF`. This happens on
close, fsync and vfs sync.

## Directories and names

- exFAT lookups hash the upcased name (`exfat_name_hash`), compare NameHash,
  then compare the File Name entries, verifying SetChecksum along the way.
  Creation (`dir_create_entry`) looks for a run of entries with InUse clear,
  or the end marker, and writes File + Stream Extension + ⌈len/15⌉ File Name
  entries with `dir_write_raw`. Writing past the end grows the directory a
  cluster at a time (`fd_extend`). Only the buffered block of a new cluster is
  zero-filled; the rest of it is not written. The parent's own entry is then
  updated with the new size. The File Name entries keep the name's original
  case, and the UTC offset bytes are written as `0x80` (UTC, valid).
- Deleting an exFAT entry (`dir_delete_entry`) clears InUse on the File entry
  (type `0x05`) and overwrites every secondary entry with a copy of that File
  entry retyped `0x40`. The Stream and File Name data are destroyed rather than
  only marked unused.
- FAT lookups match either the LFN (fragments validated against the alias
  checksum; the ordinal sequence is not checked) or the 8.3 name. `.` and `..`
  entries are skipped. A name that is already upper-case 8.3 gets only an SFN.
  Any other name gets LFN entries. A lossy alias also gets a `~N` tail: spaces
  skipped, surrogates become `_`, and other invalid characters become a
  *random* `[0-9A-Z]` character. N starts at 1 and is bumped while a
  case-sensitive `strncmp` of the formatted alias finds a collision. The 10 ms
  creation byte is written as 0 and ignored when read. Deleting clears the
  8.3 entry and the LFN entries directly before it. Short names use the OEM
  codepage. On 3.65 that is
  always CP932 (Shift-JIS): the variable at `0x8101d05c` is set at init and
  never changed. Tables for single-byte codepages, GBK, UHC and Big5 are
  present but unused.
- Paths are UTF-8 at the VFS boundary and UTF-16 inside. `path_parse` turns
  `\` into `/`, resolves `.`/`..` (above the root is ENOENT), and limits paths
  to 0x103 characters. Names reject control characters and `\"./:<>|*?`, and
  lose trailing dots.
- `vfs_decode_path_elem` returns each path element upcased, through the drive's
  exFAT up-case table or ASCII for FAT. That makes iofilemgr's name cache
  case-insensitive, matching the filesystem.
- Times are stored UTC: exFAT timestamps are converted with their UtcOffset
  bytes, FAT ones carry no zone, and the current time comes from
  `ksceRtcGetCurrentClock(&dt, 0)` (through a relocated pointer at
  `0x8101d008`).

## Deleting open files: SceIoTrash

iofilemgr never removes a file that has open fds. It calls `vop_whiteout`
with a hidden `SCEDEL~…` name it generated. exfatfs renames the file to that
name inside the pinned `SceIoTrash` directory on the volume root and marks the
fnode deleted. If no trash directory is pinned, the rename happens in place
and the fnode is not marked. The
open handles keep working on the moved file. When the last fd closes
(`fd_release`, open count `+0x272` reaching 0 on an orphaned fnode), its
clusters are freed. `dread` hides `SCEDEL~` entries while they are still open.

The trash directory is created and pinned by devctl `0x3802`. devctl `0x3803`
deletes one entry from it per call, the first one that is no longer open. Who
issues these devctls was not traced.

## devctl

| cmd | in / out | effect |
|---|---|---|
| `0x3001` | out 0x18 or 0x28 | `SceIoDevInfo`: total, free (minus quota reservation), cluster size; the 0x28 form adds volume serial and label. On mount ID `0x800` (`ux0:`) 32 MiB are subtracted from the free size (0 if less). Allowed when `SceSblACMgrForDriver_D7AD8471(pid, assign)` returns 1 |
| `0x3004` | in 0x18, u64 bytes at +8 | quota: reserve all free clusters except `bytes` (ENOSPC if more than is free) |
| `0x3005` | none | clear the quota |
| `0x3802` | none | create (if needed) and pin `SceIoTrash` |
| `0x3803` | none | purge one trash entry |
| `0x80000001` | out 8 | maximum file size: `0xFFFFFFFF` FAT, unlimited exFAT |

The privileged commands (`0x3004/0x3005/0x3802/0x3803`) need
`SceThreadmgrForDriver_332E127C()`, a word at `TPIDRPRW+0x2C` of the calling
thread, to be `0x80` or `0x40`. ACMgr compares
`ksceKernelSysrootGetThreadAccessLevel()` against the same two values to
separate privileged from user threads, so this is the thread access level.

## Permissions (SceSblACMgr)

exfatfs keeps no Unix permissions on disk. The FAT/exFAT attribute byte
carries them, and `SceSblACMgr` (reverse engineered in
[../acmgr-3.65](../acmgr-3.65/README.md)) translates between the two. Every call
passes a context `{type, &attr, 1}`: type 2 on FAT16 mounts, 3 otherwise, and
the two are handled identically.

- Attribute bits 1 READONLY, 2 HIDDEN and 4 SYSTEM encode the rights.
  The mapping depends on the calling thread's access level
  (`ksceKernelSysrootGetThreadAccessLevel`): 0x40/0x80 privileged, 0x10/0x20
  user.
- Create, mkdir and chstat set the attribute from `st_mode`
  (`acl_attr_from_mode`, `kscePfsACSetFSAttrByMode`). For privileged threads
  `0x186` gives 0, `0x106` SYSTEM, `0x104` SYSTEM|RO, `0x6` SYSTEM|HIDDEN and
  `0x4` all three. For user threads `0x180` gives 0 and `0x100` READONLY.
  Setting or clearing SYSTEM needs a privileged thread (`0x8001000D`).
- stat turns the attribute back into `st_mode` (`acl_attr_to_mode`), plus
  `SCE_S_IFDIR`/`SCE_S_IFREG`.
- Lookups, getstat and dread check read access (`acl_check_read`). User
  threads cannot see SYSTEM|HIDDEN entries; privileged threads see
  everything. exfatfs never asks ACMgr about write access. ACMgr's table would
  deny it to user threads on READONLY and SYSTEM entries, and to privileged
  threads on SYSTEM|READONLY.
- The attribute value `0x0F` is special. It is the root attribute of `lma0:`
  (its root mode is `0x1000000`), and only on that mount (mount ID `0x10000`)
  is it treated as a plain entry; elsewhere such entries fail with
  `0x800F0916`.
- The root directory gets its mode from a fixed table in ACMgr: `sd0:` `0x186`,
  `lma0:` `0x1000000`, every other known assign `0x106`. So for privileged
  callers the root is plain on `sd0:` and SYSTEM elsewhere.
- devctl `0x3001` (free space) is allowed for system programs, privileged
  threads, non-game programs on `ux0:`, and two specific authority IDs.

## Consistency reporting

When the engine finds damage it posts an event through iofilemgr export
`SceIofilemgrForDriver_DD46CD63(mnt_id, 1, event)`, which delivers to
registered I/O event listeners: `0x808D0007` broken cluster chain, `0x808D0008`
corrupt directory entry, `0x808D000B` impossible entry values. It then carries
on or fails the operation.

## Unmounting

`vfs_umount` without the force flag fails with EBUSY while fds are open. A
forced unmount marks the mount invalidated (`0x4000`), syncs, drops the trash
pin, force-closes open fds and detaches every fnode from the drive. From then
on, the last `vop_inactive`/`close`/`unlink` that leaves the drive idle frees
the drive and the mount context.

## Error codes

`uvfat_errno` (`0x810021e8`) maps the engine's negative codes to SCE errors.
The table is in the function index. Some notable mappings:

- Not found is `-4`/`-5` → `0x80010002`.
- Exists is `-9`/`-0x1F` → `0x80010011`. rmdir of a non-empty directory
  translates `-0x1F` itself and returns ENOTEMPTY (`0x8001005A`).
- No space is `-10` → `0x8001001C`.
- I/O errors are `-0xC/-0xD/-0x10/-0x14/-0x22` → `0x80010005`.
- Media errors `0x8032001A` pass through unchanged.

## Not covered

- The I/O event listener side of iofilemgr is outside this module; only how
  exfatfs posts events is documented. `SceSblACMgr` is covered in
  [../acmgr-3.65](../acmgr-3.65/README.md).
- `exfat_mnt.ops` and `exfat_mnt.slots` are never written and only used by
  `mnt_reactivate`, which cannot run for normal mounts. They look like
  leftovers from a shared Sony filesystem template.
- Function arguments marked `?` in the index are passed through without
  affecting behaviour in any path that was traced.
- None of this has been checked on hardware.
