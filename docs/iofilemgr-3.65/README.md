# iofilemgr.skprx (retail 3.65), reverse engineered

`os0:kd/iofilemgr.skprx` (module `SceIofilemgr`) is the Vita's VFS layer.
Every `sceIo*` / `ksceIo*` call goes through it:

- it parses paths and finds mounts;
- it keeps vnodes, open files and a name cache;
- it checks permissions with [SceSblACMgr](../acmgr-3.65/README.md);
- it caches file data;
- it schedules I/O by device class and priority;
- it implements the async API, mount and error events, and the mount table.

Filesystems such as [exfatfs](../exfatfs-3.65/README.md), the
[sdstor](../sdstor-3.65/README.md) block devices and ntfsfs sit under it and
see only vfs-op and vop calls.

Firmware: retail 3.65 `PSP2UPDAT.PUP`. Text is at `0x81000000` (0x20980
bytes), data at `0x81021000` (0x44 bytes in the file, 0x2238 in memory).
Everything here is from static analysis; nothing was checked on hardware.

The module has 782 functions: 637 bodies, all read in full, and 145 import
stubs. Many bodies are reached only through function pointers, op tables or
register calls, so Ghidra missed them. They were added by a pointer scan and
by hand from the gaps between functions. The remaining gaps are `tbb`/`tbh`
jump tables, literal pools, padding and the module info.
[reference.md](reference.md) lists every function.

## Files here

| file | contents |
|---|---|
| [reference.md](reference.md) | structures and the full function index (generated) |
| [iofilemgr-names.tsv](iofilemgr-names.tsv) | address, name, prototype, notes for every function |
| [iofilemgr-types.spec](iofilemgr-types.spec) | structure layouts (`SceVfsMount`, `SceVfsVnode`, `SceVfsFile`, mount parameters, assign entries, scheduler requests and queues) |
| [iofilemgr-exports.tsv](iofilemgr-exports.tsv) | every exported NID, its address and its name in the vita-headers database, if any |
| [tools/iofassign.py](tools/iofassign.py) | decodes the built-in assign (mount point) templates |

The Ghidra database is rebuilt as for exfatfs:
`../exfatfs-3.65/tools/genspec.py iofilemgr-types.spec iofilemgr-names.tsv`,
applied with `ApplyTypes.java`. `gendoc.py` from the same directory renders
reference.md.

## Exports

- `SceIofilemgrForDriver`: 148 entries, mostly the `ksceIo*` API, the
  `vfs*` / `ksceVop*` / `ksceVfsOp*` interface for filesystems, and the
  mount, error-event and async helpers.
- `SceIofilemgr`: 59 user syscalls.
- Module entries: `module_start` and the module info.

20 exports have no name in the vita-headers database. Where the role is now
known the function is named here (e.g. `SceIofilemgrForDriver_8F0DE34D` is
`ksceIoComplete`), and the NID is kept in the notes.

## Layers

A request passes through these layers:

1. **API** (`ksceIo*`, `0x81000410`..): runs with the user-mode bit of
   TPIDRURO cleared, looks the fd up, and checks the path (`iof_path_queue`:
   NULL → `0x8001000E`, ≥ 0x400 characters → `0x8001005B`, raw `/…` paths only
   for some calls). Once the scheduler is up (`0x81022a8c == 1`), kernel calls
   become scheduler requests (`iof_sched_*`). Before that, or for files that
   bypass it, they call the `vfs*` function on a fresh 0x2000 stack.
2. **I/O scheduler** (below): queues per device class, priorities,
   chunking, async completion.
3. **vfs\*** (`vfsOpen`, `vfsRead`, …, `0x81004a0c`..`0x81008e20`): path
   walking, vnode and file objects, permission checks, delete-while-open,
   rename rules, buffer cache.
4. **Dispatchers** (`ksceVop*`, `ksceVfsOp*`, `0x8100d6f8`..): call the
   filesystem's op table slot. A NULL slot gives `0x80010030`.

User syscalls (`0x81015e1c`..`0x8101774c`) wrap the API:

- `Sysmem_4E6D8BC3() > 0` → `0x80010058`.
- PUIDs are translated: a bad PUID becomes `0xC0010009`, other errors get
  bit `0x40000000`.
- User buffers are mapped into the kernel (`SceIoUserMapRead`, `…Write`,
  `…PRead`, `…Pwrite` and the async forms). Games map with flag `0x11`.
- With Fios overlays on, paths go through `ksceFiosKernelOverlayResolveSync`
  first. Fios errors are mapped (`iof_fios_errno`).
- ioctl/devctl command bit `0x800` needs a system program and bits `0x600`
  are kernel only (`0x80010030`).

## Mount table (assigns)

`iof_assign_init` (`0x8101844c`) fills a 32-entry table at `0x81022b30`
(0x38 bytes each). Each entry holds:

- a mount ID;
- a `SceVfsMountParam`: root path, blockdev, fs_type and opt, mnt_flags, vfs
  name, data, misc, vops;
- the misc info: assign name, unit name, blockdev, fallback blockdev and
  mount ID;
- the list of mount events for that assign.

`tools/iofassign.py` prints the table:

| id | assign | blockdev (fallback) | opt (I/O class) | mnt_flags |
|---|---|---|---|---|
| 0x1 | `sdstor0:` | `sdstor_dev_fs` devfs | | |
| 0x2 | `md0:` | `md_dev_fs` devfs | | |
| 0x100 | `sd0:` (only with external storage) | `ext-pp-act-a` (`ext-lp-act-entire`) | 0x203 | 0x30002 |
| 0x200 / 0x300 / 0xB00 / 0xC00 | `os0:` `vs0:` `sa0:` `pd0:` | `int-lp-act-os`, `int-lp-ign-vsh` / `-sysdata` / `-pidata` | 0x100 | 0x1002 (RDONLY) |
| 0x400 / 0x500 / 0x700 | `vd0:` `tm0:` `ud0:` | `int-lp-ign-vshdata` / `-vtrm` / `-updater` | 0x100 | 0x10002 |
| 0x600 | `ur0:` | `int-lp-ign-user` | 0x100 | 0x50002 (WRITE_CACHE) |
| 0x800 | `ux0:` | `xmc-lp-ign-userext` (`int-lp-ign-userext` with internal storage only) | 0x202 | 0x30002 |
| 0x900 / 0xA00 | `gro0:` / `grw0:` | `gcd-lp-ign-gamero` / `-gamerw` | 0x201 | 0x21002 / 0x20002 |
| 0xD00 / 0xE00 | `imc0:` / `xmc0:` | `int-lp-ign-userext` / `xmc-lp-ign-userext` | 0x202 | 0x30002 |
| 0x10000 / 0x20000 / 0x50000 / 0x60000 | `lma0:` `lmb0:` `mfa0:` `mfb0:` | loop mounts (type 5) | from the backing file | 0x5 |
| 0xF00 | `uma0:` | `uma-pp-act-a` (`uma-lp-act-entire`) | 0x203 | 0x2 |

All `sdstor0:` blockdevs are prefixed `sdstor0:`, and every block mount uses
the vfs `"exfat"` with fs_type 1. The low byte of mnt_flags is the mount type
(1 PFS, 2 FSROOT, 3 DEVFS, 5 STACKFS loop, 6 HOSTFS). Higher bits:

| bit | meaning |
|---|---|
| 0x1000 | RDONLY |
| 0x2000 | NOBUF |
| 0x10000 | INTERNAL |
| 0x20000 | EXTERNAL |
| 0x40000 | WRITE_CACHE |

### Mounting and unmounting

`ksceIoMount(id, path, permission, data, 0, 0)` queues a request for the
"SceIofilemgrMount" thread (`iof_mntq_thread`), so mounts are serialised.
Before that thread exists, the request runs inline. The worker
(`iof_mntq_do_mount`):

1. copies the entry's parameter;
2. uses `path` as the blockdev if given (not for IDs below 0x100); otherwise
   `vfsMount` uses the info blockdev, then the fallback;
3. applies the permission bits: 1 sets RDONLY, 2 clears it, others are ORed
   into mnt_flags;
4. calls `vfsMount`.

After a successful mount:

- the assign is linked under the assign that owns its blockdev
  (`vfsGetMountId`);
- `ux0:`, `grw0:`, `lma0:` and `lmb0:` are added to the idle daemon;
- mount events with bit 1 are signalled. A failed mount signals 0x10.

`ksceIoUmount(id, force, 0, 0)` first unmounts child assigns, then calls
`vfsUmount`. Events with bit 2 are signalled (0x20 on failure).
`vfsMountForPFS` and `vfsUmountForPFS` queue requests with explicit
parameters and the caller's TLS pid.

`vfsMount` itself is described in the reference (`0x81004a0c`). By mount
type:

- FSROOT needs a DEV vnode as blockdev.
- PFS mounts over an existing directory that is not itself PFS.
- DEVFS has no backing vnode.
- STACKFS needs an unopened regular or device file.
- There can be only one HOSTFS.

## Vnodes, files and paths

- **Paths** (`iof_parse_path`, `iof_split_path`, `iof_walk`):
  - `dev:` plus up to 16 elements;
  - `.` and empty elements are dropped, and `..` pops but never above the
    root;
  - each element is canonicalised by the filesystem's `decode_path_elem`
    (`0x80010030` means use it raw);
  - lookups go through the name cache (positive, negative-LRU, mount-point
    and stale entries).
- **Vnodes** come from a pool (`vfsGetNewVnode`). When the pool is full,
  unreferenced vnodes are reclaimed, unless their mount has NO_RECLAIM. A
  vnode lock is recursive.
- **Files** (`SceVfsFile`) carry the fd, position and flags.
  - Each process has quotas: `iof_quota_take`, max open files from local
    storage, otherwise `0x80010018`.
  - The forced-unmount and suspend gate (`iof_file_gate`) blocks or fails
    file operations while media is being removed or the system suspends.
- **Permissions**: iofilemgr itself checks access before calling the
  filesystem: `acm_check_access(pid, ctx, rights)`. Rights are read 1, write
  2, attributes 8. The context is built from the vnode `acl_data` by mount
  fs_type (`iof_acl_ctx`):
  - fs_type 1 → `{3, acl, 1}`, 2 → `{4, acl, 4}`, 3 (PFS) → `{6, acl, 2}`,
    0x10 (block devices) → `{8, acl, 6}`;
  - attribute `0x0F` on fs types 1/2 is cleared first, so it only counts as
    plain on `lma0:`.
- **open** (`vfsOpen`):
  - creates through `vop_create` (mode limited to `0xF101FF`);
  - `O_TRUNC` is `vop_chstat(SCE_CST_SIZE, size 0)`;
  - `O_EXCL` on an existing file → `0x80010011`;
  - flag `0x4000` (exclusive) is privileged and conflicts with other openers
    and with a buffer cache;
  - writes on a RDONLY mount → `0x8001001E`.
- **rename** (`vfsRename`): the target must not exist (`0x80010011`, no
  replace); across mounts → `0x80010001`. Open files, buffer cache and locks
  move to the new vnode that `vop_rename` returns.
- **remove while open** (`vfsRemove`): the open fds are synced, then the file
  gets a hidden name: `SCEDEL~` written over its first 7 characters, plus
  more `~` while the filesystem answers EEXIST.
  - `vop_whiteout` is tried first; if not implemented,
    `vop_rename(dvp, vp, old, NULL, &nvp, new)` with **ndvp NULL = same
    directory**.
  - The vnodes become `0x500` (INACTIVE|DELETED).
  - On the last close `vfsClose` calls `ksceVopRemove(parent, vp, hidden
    name)`. The parent is **NULL** when the name-cache entry has no parent
    (`0x81006248`).
- **flock** (`vfsFlock`, `iof_flock_*`): BSD-style locks per (mount, fid)
  with up to 16 holders, shared across renames.

## Buffer cache

Unless the mount has NOBUF (0x2000), `ksceVopOpen` gives each vnode a cache
(`iof_bc_create(0x2000, 2, 0x200, mount io cache size)`). It holds 8 sets ×
2 ways of 512-byte lines, plus one window the size of the mount's default
I/O cache. It is filled and written back through the filesystem's own
read/write vops, at aligned offsets with the file position set temporarily.

- Writes are written back at once unless the mount has WRITE_CACHE
  (`ur0:`).
- Reads at or past `vp->size` return 0 without calling the filesystem.
- Writes past EOF fill the gap and extend `vp->size`.
- `chstat` size and mode changes flush or trim the cache first.
- ioctl `0x1001`/`0x1002` set and get its geometry.

## I/O scheduler

`iof_sched_init` (`0x810130d4`) creates five queues. Each has a worker
thread `SceIoSchedWorker<n>` and 16 priority lists:

| queue | id | used by | chunk |
|---|---|---|---|
| Internal | 0x100 | `os0:`, `vs0:`, `ur0:`, … | 256 KiB |
| Game Card | 0x201, 0x203 | `gro0:`/`grw0:` (0x201), `sd0:`/`uma0:` (0x203) | 256 KiB |
| Removable | 0x202 | `ux0:`, `xmc0:`, `imc0:` | 256 KiB |
| Host File System | 0x300 | `host0:` | none |
| Default | other | | 256 KiB |

The queue of a request is the mount opt of its path (`vfsGetMntOpt`), or
for an fd the value recorded at open (ioctl 0x200/0x201: priority, queue,
pid). Bit 16 of the queue sends chstat and read/write to `ScePfsFacade`
instead of the `vfs*` functions.

- **Priorities**:
  - 0..15. Game and non-game programs are offset by 6 and limited (see
    `iof_set_proc_prio`).
  - Defaults: thread, then process, then global.
  - Set with `sceIoSet*Priority*`.
- **Requests** (`iof_req`, 0xB8 bytes, embedded in a 0xF8-byte
  `SceIoAsyncEvent` object):
  - built by `iof_req_init` and an op-specific builder (ops 1 open … 0x1D);
  - synchronous requests run in the calling thread (`iof_queue_run_sync`);
  - async ones go to the worker (`iof_queue_submit`).
- **Chunking**: reads and writes are cut into chunks of the queue size, at
  most 8 at a time. The first chunk is shortened so the rest is aligned to
  the file position (`iof_req_make_chunks`). On the internal queues, reads
  are done positionally and the position is then advanced.
- **Fairness**: an idle worker helps other queues (event bits `0xff00`), at
  most 2 helpers per queue. Helpers take the requester's priority and
  affinity, and never take writes or a request on the same path/fd as a
  running one (`iof_prio_pick`). On the removable queue, lower-priority work
  waits up to 0.4 s after high-priority bulk transfers.
- **Execution**: the worker impersonates the requester (TLS pid,
  permission) and runs the op on a 0x2000 stack. The PFS error `0x80142318`
  leaves the request queued for a retry.

## Async API and events

- `ksceIo*Async` return an event UID.
  - Read/write buffers of user callers are mapped for the duration.
  - Completion (`iof_req_complete`) stores the result, fills the caller's
    `SceIoAsyncParam`, and sets the event.
  - `ksceIoCancel` removes a queued request (running → `0x80010010`).
  - `ksceIoComplete` (`8F0DE34D`) and `ksceIoCompleteMultiple` (`B499287E`)
    delete finished events.
  - `ksceIoIoctlAsync` and `ksceIoDevctlAsync` are not implemented
    (`0x80020004`).
- **Error events** (`ksceIoCreateErrorEvent`, `SceIoErrorEvent<Assign>`):
  - a process registers a 0x1C-byte user record for a mount ID;
  - `SceIofilemgrForDriver_DD46CD63(mount id, mask, info)` writes
    `{mount id, info+4, result, info+0x10}` to each matching unfired event
    and signals it;
  - `ksceIoClearErrorEvent` re-arms it.
- **Mount events** (`ksceIoCreateMountEvent`, `SceIoMountEvent<Assign>`):
  - mask bits: 1 mounted, 2 unmounted, 0x10/0x20 failures;
  - 0x100/0x200 media inserted/removed, posted by sdstor through
    `SceIofilemgrForDriver_39ABDB9E`.
- Process exit deletes a process's async, error and mount events and closes
  its files.

## Idle daemon

The "SceIofilemgrDaemon" thread (`iof_daemon`) keeps class slots. It starts
with internal 0x100, removable 0x202 and game card 0x201. Mounting `ux0:`,
`grw0:`, `lma0:` or `lmb0:` adds that mount and its class. The mount list
also starts with `sd0:`, `vd0:`, `ur0:`, `ud0:` and `ux0:`, but `sd0:` (class
0x203) never gets a slot.

10 s after the last I/O on a class, the daemon calls `vfsDevctl(assign,
0x3803, NULL, 0, NULL, 0)` on the first listed mount of that class, and
keeps calling it while no new I/O arrives:

| result | daemon action |
|---|---|
| 0 | call again at once |
| > 0, or any other error | call again after 100 µs |
| `0x80010002` (ENOENT) | drop the mount from the list and go on with the next one |
| facility 0x32 / 0x3D error | stop until the next idle period |
| `0x80010005` / `0x80010013` | stop and free the class slot |

exfatfs uses 0x3803 to purge one trash entry per call, and answers ENOENT
when nothing is left (see [exfatfs](../exfatfs-3.65/README.md#devctl)).

## Built-in devices

- `sdstor0:` and `md0:` are devfs assigns for the block-device modules.
- `tty0:`… (`dummy_ttyp_dev_fs`, `ttyp_*`): writes go to the debug console,
  reads return 0.

## Relevance to ntfsfs

- ntfsfs mounts through the normal `uma0:`/`sd0:`/… entries, which name the
  vfs `"exfat"`; retargeting the entry's vfs name keeps the mount thread,
  events and daemon. `uma0:` tries `sdstor0:uma-pp-act-a`, then
  `sdstor0:uma-lp-act-entire`. A disk without a usable MBR partition therefore
  reaches the filesystem as the whole device.
- ntfsfs mounts get fs_type 1, so iofilemgr checks access with ACMgr type 3 on
  `acl_data[0]` (FAT-style attribute). ntfsfs stores the NTFS
  READONLY/HIDDEN/SYSTEM bits there, which use the same values. Like
  exfatfs, it also calls ACMgr itself:
  - for create/chstat modes;
  - to hide unreadable entries in dread and getstat;
  - for devctl 0x3001.
- Path lookups go through `decode_path_elem`. exfatfs returns the element
  upcased, so the name cache, and with it the vnode, is shared between
  differently cased names. A filesystem that matches names case-insensitively
  must do the same, or one file gets several vnodes, each with its own size
  and buffer cache. vop_lookup receives the decoded name; create, rename and
  remove receive the raw one.
- A rename must refuse an existing target with `0x80010011`. Delete-while-open
  relies on it to pick a free `SCEDEL~` name; replacing would delete another
  open file.
- Keep `vp->size` current after every write, truncate and create: reads past
  it never reach the filesystem, and the buffer cache uses it.
- `O_TRUNC` arrives as `vop_chstat` with `SCE_CST_SIZE` and size 0.
- `vop_rename` must accept ndvp NULL (same directory) and return a new vnode.
  `vop_remove` must accept a NULL dvp after a delete-while-open.
- Unless the mount has NOBUF, data passes through iofilemgr's buffer cache.
  The filesystem sees aligned 512-byte and window-sized reads and writes.
- `vfsMount` holds the block device vnode's lock (taken by
  `iof_path_dir_vnode`) through the VFS mount op, set_root, devctl 0x3802 and,
  on failure, the umount op. The lock is recursive per thread, so the device
  can be read from the mounting thread, but another thread that locks it
  waits until the mount is over. ntfsfs reads on its worker thread, which
  therefore skips the lock while the thread it works for holds it.
- A writable FSROOT mount receives devctl 0x3802 (exfatfs: create
  `SceIoTrash`) right after mounting, and is unmounted again unless it returns
  0 or `0x8001001C`. ntfsfs returns 0.
- Suspend. `power.skprx` dispatches the suspend events 0x100, 0x102, 0x20F
  down to 0x200, then 0x400, 0x401, 0x402. At 0x200 sdstor powers the game
  card / SD slot off and marks the device removed (I/O then fails with
  0x80010013). At 0x401 iofilemgr writes back every mount written since the
  last suspend (`iof_flush_dirty_mounts` 0x8100afa4): each vnode's
  `ksceVopSync(vp, fd_list, 1)`, which flushes the buffer cache, then the VFS
  op sync. While that sync fails with anything but 0x80010030 or a facility
  0x32/0x3D error, the mount stays marked and the loop never ends, so the
  power thread spins and the system does not suspend. exfatfs has nothing
  left to write by then. ntfsfs writes its mounts back with `ksceIoSync` on
  event 0x100, and answers 0x80010030 to a sync that finds the card gone
  while suspending.
- An ntfs mount on `ux0:` or `grw0:` receives the idle devctl 0x3803. It must
  answer ENOENT (`0x80010002`). `0x80010030` would make the daemon repeat the
  call every 100 µs for as long as the device is idle. ntfsfs does this
  (`vfs_devctl`). `uma0:` and `sd0:` never receive it.

## Error codes

Common results:

| code | meaning |
|---|---|
| `0x80010002` | ENOENT |
| `0x80010009` | EBADF |
| `0x8001000C` | ENOMEM |
| `0x8001000D` | EACCES |
| `0x8001000E` | EFAULT |
| `0x80010010` | EBUSY |
| `0x80010011` | EEXIST |
| `0x80010013` | ENODEV, unmounted |
| `0x80010014` | ENOTDIR |
| `0x80010015` | EISDIR |
| `0x80010016` | EINVAL |
| `0x80010018` | EMFILE |
| `0x8001001E` | EROFS |
| `0x80010030` | not supported |
| `0x80010058` | process not allowed to do I/O now |
| `0x8001005B` | ENAMETOOLONG |
| `0x80010001` | EPERM, e.g. a cross-mount rename |

## Not covered

- Imports missing from the NID database (Threadmgr, Sysmem, Processmgr,
  Sysroot, PfsFacade, Fios2) are described only by how iofilemgr uses them.
  `SceSysrootForDriver_8B8F1D01` (called on entry to `_sceIoLseek`) was not
  identified.
- The Fios overlay and PFS facade modules themselves are outside this module.
- None of this has been checked on hardware.
