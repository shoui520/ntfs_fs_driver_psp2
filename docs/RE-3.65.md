# exfatfs / iofilemgr reverse engineering notes (retail 3.65)

These notes record what ntfsfs relies on, and the evidence for it. Everything
here is from the retail 3.65 `PSP2UPDAT.PUP`
(133754368 bytes, `version.txt` = 3.65), decrypted and analysed in Ghidra with
VitaLoaderRedux. Addresses are the modules' link addresses (text at
`0x81000000`); Thumb function entries are listed without the low bit.

Other firmware versions need their own bytes compared before these conclusions
are reused.

Status legend: **verified** = read in the 3.65 binary; **unverified** = inferred,
needs confirmation on hardware or further RE.

## Modules

| file | module | text / data |
|---|---|---|
| `os0:kd/exfatfs.skprx` | SceExfatfs | `0x81000000` + `0x1ce28`, data `0x8101d000` (memsz `0x295e28`) |
| `os0:kd/iofilemgr.skprx` | SceIofilemgr | `0x81000000` + `0x20980`, data `0x81021000` (memsz `0x2238`) |

## 1. How exfatfs plugs into the VFS

`module_start` (`0x81001908`):

1. `FUN_8100b0cc(&0x8101d00c)`: fills a `SceVfsInfo` at `0x8101d00c` and calls
   `ksceVfsAddVfs` (`SceIofilemgrForDriver_673D2FCD`). `0x80010011` (already
   registered) is treated as success. **verified**
2. Mounts `os0:` (`ksceIoMount(0x200, ...)`), and on manufacturing/special boot
   modes `sd0:`/`ux0:`/`gro0:`/`grw0:`. **verified**
3. Queues `FUN_810017b4` on a work queue (`ksceKernelEnqueueWorkQueue`,
   "SceExfatInit"), which mounts `vs0 sa0 vd0 tm0 ud0 pd0 ur0` and sends three
   `sdstor0:` devctls. **verified**

`SceVfsInfo` (`0x8101d00c`), matching `psp2kern/vfs.h` field for field:

| field | value |
|---|---|
| vfs_ops | `0x8101bbc8` (set at runtime) |
| vfs_name | `"exfat"`, len 6 |
| type | **2** (vfs.h only names 0 and 0x10) |
| default_vops | `0x8101bc08` (set at runtime) |

VFS op table `0x8101bbc8`: mount `0x8100abe4`, umount `0x81009704`,
set_root `0x81008e30`, get_root `0x81008d50`, sync `0x81009034`,
init `0x81008c88`, fini `0x81008c8c`, devctl `0x8100978c`,
decode_path_elem `0x8100963c`; reserved slots NULL. **verified**

Vop table `0x8101bc08` (29 slots): every vop is implemented except
`reserved` and `zerofill`. Notable entries: open `0x810091dc`,
create `0x8100a69c`, close `0x8100ab2c`, lookup `0x81009ca0`,
read `0x81008f68`, write `0x8100a1f4`, lseek `0x81009268`, dopen `0x8100916c`,
dread `0x810093dc`, getstat `0x81009eac`, rename `0x8100a0dc`,
pread `0x8100a31c`, pwrite `0x8100a3e4`, inactive `0x8100a878`,
whiteout `0x8100a97c`, cleanup `0x8100908c`. **verified**

All `SceVfsMount`/`SceVfsVnode`/`SceVfsFile` offsets used by exfatfs agree with
`psp2kern/vfs.h` (e.g. `mnt+0x40` mnt_vnode, `+0x7c` mnt_data, `+0xc4` data;
`vnode+0x48` node_data, `+0x78` type, `+0x80` size; `file+0x8` position,
`+0x20` fd). **verified**

## 2. Vop contracts ntfsfs copies

- **vfs_mount** (`0x8100abe4`): the block device is `mnt->mnt_vnode`. exfatfs
  locks it, `vfsAllocateFile(vp, &file, dev_file_path->name)`, sets
  `file->flags` to 1 (RDONLY mount flag `0x1000`) or 3, `ksceVopOpen`, then
  `vp->ref_count++`. For device vnodes it first calls `ksceVfsOpDevctl(cmd 1)`
  and refuses RW if the write-protect bit is set (`0x8001001E`). On success it
  sets `mnt->available_entry_num = 0x40` and `default_io_cache_size =
  min(cluster, 0x8000)`. **verified**
- **sector I/O** (`0x81012ea8`): lengths must be multiples of 512. Lock device
  vnode; `ksceVopPread` if the device vop table has pread, else
  `ksceVopLseek` + `ksceVopRead`; on error `ksceVopSync(…, 2)` /
  `ksceVopCleanup` then retry once. **verified**
- **get_root / lookup / create** (`0x81008d50`, `0x81009ca0`, `0x8100a69c`):
  `vfsGetNewVnode(mnt, mnt->mnt_vfs_inf->default_vops, 0, &vp)`, lock it, set
  node_data, mnt, dd (parent), state = 1, type (`0x1002` root, 2 dir, 1 file),
  size, fid, `acl_data[0]` = attribute byte, `ref_count = 1`, and return it
  **still locked**. **verified**
- **set_root** (`0x81008e30`): fills a vnode supplied by iofilemgr; locks and
  unlocks it itself. **verified**
- **lookup** gets `SceVfsPath{name, name_length}` that is not NUL-terminated.
  **verified**
- **create** returns `0x80010011` if the name exists. **verified**
- **read/write** advance `file->position`; **pread/pwrite** do not;
  **lseek** handles whence 0/1/2 using `vp->size` for SEEK_END. **verified**
- **open** stores the FS handle in `file->fd`; **close** clears it. **verified**
- **dread** returns 1 per entry, 0 at the end (it maps `0x80010002`), and
  skips entries named `SCEDEL~…`. **verified**
- **getstat** zeroes and fills a 0x58-byte `SceIoStat`; dirent is 0x160 bytes
  with `d_name` at 0x58. **verified**
- **rename** (`0x8100a0dc`, not a Ghidra function; read from disassembly):
  `0x80010001` across mounts (the value is EPERM, not EXDEV); on success builds a new locked vnode
  (`dd` = new parent, type/size/acl copied from the old vnode) in `*nvpp`.
  **verified**
- **inactive** frees the FS node and clears node_data. **verified**
- **devctl 0x3001** fills `SceIoDevInfo` (0x18, or 0x28 accepted), gated by an
  `SceSblACMgrForDriver_D7AD8471` check (`acm_check_devinfo`, see
  [acmgr-3.65](acmgr-3.65/README.md#paths-and-devices)); for mount id `0x800` it hides 32 MiB of
  free space. The other commands (`0x3004/0x3005` quota, `0x3802/0x3803`
  trash, `0x80000001` max file size) are described in
  [exfatfs-3.65](exfatfs-3.65/README.md#devctl).

## 3. iofilemgr behaviour

The full module is in [iofilemgr-3.65/](iofilemgr-3.65/README.md); this
section keeps the points ntfsfs depends on.


- Every vop/vfs-op dispatcher returns `0x80010030` when the slot is NULL
  (e.g. `SceIofilemgrForDriver_F7DAC0F5` = decode_path_elem dispatcher at
  `0x8100fb9c`). **verified**
- The path walker (`0x81003b..`, `0x81003c9c`) treats `0x80010030` from
  decode_path_elem as "use the raw element". A VFS may leave it NULL. **verified**
- `vfsMount` (`SceIofilemgrForDriver_B62DE9A6`, `0x81004a0c`), FSROOT case:
  requires fs_type 1 or 2; if `blockdev_name` is NULL it resolves
  `misc->blockdev_name`, then `misc->blockdev_name_no_part`; stores
  `param->misc` in `mnt->mnt_data` and `param->data` in `mnt->data`; allocates
  the root vnode with `param->vops` or the VFS default vops. **verified**
- Static mount params (text, e.g. `0x8101d7d0` for `/uma/exfat`):
  `{root_path, blockdev, fs_type | opt << 16, mnt_flags, vfs_name, data, misc,
  vops}`. **verified** Mount data (`SceVfsMountData`) entries, e.g.
  `0x8101da7c`: `uma0:`, `exfatuma0`, `sdstor0:uma-pp-act-a`,
  `sdstor0:uma-lp-act-entire`, `0xF00`. **verified**

  | id | assign | blockdev | fallback | opt | flags |
  |---|---|---|---|---|---|
  | 0x100 | sd0: | ext-pp-act-a | ext-lp-act-entire | 0x203 | 0x30002 |
  | 0x800 | ux0: | xmc-lp-ign-userext | – | 0x202 | 0x30002 |
  | 0xA00 | grw0: | gcd-lp-ign-gamerw | – | 0x201 | 0x20002 |
  | 0xE00 | xmc0: | xmc-lp-ign-userext | – | 0x202 | 0x30002 |
  | 0xF00 | uma0: | uma-pp-act-a | uma-lp-act-entire | 0x203 | 0x2 |

- `ksceIoMount` (`SceIofilemgrForDriver_D070BC48`, `0x81018dbc`) packs its
  arguments into a request handled by the mount daemon (or directly by
  `iof_mntq_do_mount` (`0x81017ea8`) before the daemon runs). The handler looks the id up in a
  **runtime table at `0x81022b30`: 32 entries of 0x38 bytes**,
  `{id, root_path, blockdev, fs_type|opt, mnt_flags, vfs_name, data, misc,
  vops, …, waiters @ +0x28}`, copies it, applies the permission argument
  (bit0 → `| 0x1000` RDONLY, bit1 → clear RDONLY, other bits OR'd into
  mnt_flags), calls `vfsMount`, then for ids `0x800`, `0xA00`, `0x10000`,
  `0x20000` runs `iof_daemon_add_mount` (`0x81015b60`, idle devctl 0x3803) and wakes the entry's waiters. **verified**

- Removing a file that is still open (`vfsRemove`, `0x81006600`, vnode `fd_num` at
  `+0x94` > 0): iofilemgr syncs it, then moves it to a hidden name
  (`SCEDEL~` padded with `~`) in the same directory with `vop_whiteout`; if that
  returns `0x80010030` it calls `vop_rename(odvp, ovp, old, ndvp = NULL, &nvp,
  new)`, retrying on `0x80010011`, migrates the open files to `nvp` and marks
  the old vnode `0x500`. exfatfs's rename treats `ndvp == NULL` as the same
  directory (`0x8100a1c8`), and its dread hides `SCEDEL~` names. **verified**

ntfsfs therefore retries a failed mount by pointing the runtime entry's
`vfs_name` at `"ntfs"` and calling the original `ksceIoMount` again with the
same arguments, so this path runs unchanged. If the caller named no block
device and that fails too, it retries once more with the entry's whole-device
blockdev (`misc->blockdev_name_no_part`) as the path.

- `_vshIoMount` (`SceVshBridge_3C522C35`, `0x81003df4` in
  `bootimage.skprx:vshbridge`) copies 24 bytes from its user buffer and a path
  of up to 0x400 bytes, checks `SceSblACMgrForDriver_8612B243`, then calls
  `ksceIoMount(id, path, permission, buf[0], buf[1], buf[2])` (`0x81001f48`,
  import `SceIofilemgrForDriver_D070BC48`), so it reaches the hook with the
  caller's path. **verified**
- exfatfs fails an NTFS volume with `0x80010005`: its FAT path finds
  `TotSec16` and `TotSec32` both zero (`uvfat_volume_mount`, -0x22), which
  `uvfat_errno` maps to EIO. **verified**

sdstor answers the remaining device questions
([sdstor-3.65](sdstor-3.65/README.md#partitions-and-names)), **verified**:

- `sdstor0:uma-pp-act-a` is MBR partition 1 whatever its type byte (0x07 for
  NTFS and exFAT alike) and boot flag.
- Later partitions have no name. ntfsfs reaches them through the whole
  device and its own MBR scan.

## 4. NIDs

Every import of ntfsfs resolves against the 3.65 export tables of `os0:kd` and
`bootimage.skprx`, except the taiHEN libraries (`taihenForKernel`,
`taihenModuleUtils`), which taiHEN provides at runtime. The same check passes
69/69 for the stock exfatfs.

## Unverified / not analysed

The whole module is now documented in [exfatfs-3.65/](exfatfs-3.65/README.md).
That covers all 282 functions, each read in full: the engine (FAT12/16/32 and
exFAT), its caches, the SceIoTrash delete-while-open mechanism, the ACL
wrappers, the codepage converters, path element decoding and every devctl.
`SceSblACMgr`, which those ACL wrappers call, is documented the same way in
[acmgr-3.65/](acmgr-3.65/README.md) (all 89 functions), and so are
[iofilemgr-3.65/](iofilemgr-3.65/README.md) (all 782 functions, including the
I/O scheduler, async API, mount thread and the error events behind
`SceIofilemgrForDriver_DD46CD63`) and [sdstor-3.65/](sdstor-3.65/README.md)
(all 168 functions). What remains open:
- Behaviour on hardware of everything above: **unmeasured**.
