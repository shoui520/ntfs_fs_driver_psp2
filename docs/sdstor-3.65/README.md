# sdstor.skprx (retail 3.65), reverse engineered

`os0:kd/sdstor.skprx` (module `SceSdstor`) provides the `sdstor0:` block
device. It finds the storage devices (internal eMMC, the game-card slot, the
memory card, USB mass storage and a fifth device on some models), reads their
partition tables and exposes every partition as a file such as
`sdstor0:int-lp-act-os` or `sdstor0:uma-pp-act-a`. Filesystems
([exfatfs](../exfatfs-3.65/README.md), and ntfsfs) open those files and
read and write them with vops. It also reacts to cards being inserted, removed
and swapped across suspend, and mounts or unmounts the matching drives.

Firmware: retail 3.65 `PSP2UPDAT.PUP`. Text is at `0x81000000` (0x6D38 bytes),
data at `0x81007000`. Everything here is from static analysis; nothing was
checked on hardware.

The module has 168 functions: 85 bodies, all read in full, and 83 import
stubs. Twelve bodies are reached only through function pointers or registers,
so Ghidra missed them. They were added by a pointer scan and by hand from the
gaps between functions, and the remaining gaps are jump tables, literals and
stub tails. [reference.md](reference.md) lists every function.

## Files here

| file | contents |
|---|---|
| [reference.md](reference.md) | structures and the full function index (generated) |
| [sdstor-names.tsv](sdstor-names.tsv) | address, name, prototype, notes for every function |
| [sdstor-types.spec](sdstor-types.spec) | structure layouts |
| [sdstor-exports.tsv](sdstor-exports.tsv) | the module's exports (only `module_start` and the module info) |

Tools are shared with [exfatfs-3.65/tools](../exfatfs-3.65/tools).

## Devices

Five device slots (`sds_dev`, 0x54 bytes each at `0x81008558`):

| no | name prefix | hardware | driver calls |
|---|---|---|---|
| 0 | `int-` | internal eMMC | `ksceSdifInitializeMmcDevice(0)`, `ksceSdif*SectorMmc` |
| 1 | `ext-`, `gcd-` | game-card slot: an MMC game card, or an SD card when `SceSysrootForDriver_F804F761` says so | MMC + `ksceSblGcAuthMgrGcAuthCartAuthentication`, or `ksceSdifInitializeSdDevice` |
| 2 | `mcd-` | memory card | `SceMsif` |
| 3 | `uma-` | USB mass storage | `SceUsbMassForDriver_*` (optional, through relocated pointers) |
| 4 | `usd-` | SD on models with `ksceKernelSysrootCheckModelCapability(0xB)` | `ksceSdifInitializeSdDevice(3)` |

`xmc-` is not a device. It means "the current memory device": a fixed choice
when one has been claimed, otherwise the first ready device in the order
{memory card, device 4, …}.

A device is brought up on first use (`sds_dev_check`). Sector size and count
come from the controller. A USB device with 2^32 sectors or more (2 TiB with
512-byte sectors) is refused with `0x80024902`. A write-protected medium is
flagged, and opening it for writing fails with `0x8001000D`.

## Partitions and names

Each device has up to 16 partitions plus slot 16, the whole device.
`sds_dev_scan` reads sector 0:

- **Sony MBR** (internal eMMC, memory card, game card): the sector starts with
  `Sony Computer Entertainment Inc.` and ends with `0xAA55`. It has 16
  17-byte entries at `+0x50`: start, size, partition code, type, active flag
  and 6 bytes of access rights.
- **PC MBR** (USB, SD cards): only partition 1 is used.
  - On USB and on an SD in the game-card slot, any MBR with a `0xAA55`
    signature is accepted unless sector 0 looks like a boot sector (`EB xx 90`
    / `E9`).
  - On SD cards (`sds_sd_mbr_valid`) the MBR must follow the SD Association
    rules exactly: partitions 2-4 empty, and a system ID matching the card
    size (FAT12/FAT16/FAT32/exFAT = 1/4/6/0x0B/0x0C/7). The CHS values must
    equal the SD-spec geometry, and the partition must be aligned to the
    boundary unit. Otherwise only the whole device is available.

Names are `<dev>-<lp|pp>-<ina|act|ign>-<code>` (`sds_parse_name`):

- `lp` selects a logical partition by its code: `unused`, `idstor`, `sloader`,
  `os`, `vsh`, `vshdata`, `vtrm`, `user`, `userext`, `gamero`, `gamerw`,
  `updater`, `sysdata`, `mediaid`, `pidata`, `entire`.
- `pp` selects a physical partition by letter (`a` = first).
- `act`/`ina` must match the entry's active flag; `ign` accepts either.
- `userext` on the game-card slot or device 4 means physical partition `a`.
- The 3-digit form `NNN` (device × 100 + slot, e.g. `016` for the whole eMMC)
  is the canonical name. `decode_path_elem` rewrites every resolvable name to
  it, so iofilemgr caches one vnode per partition.

So `sdstor0:uma-pp-act-a` is partition 1 of a USB drive's PC MBR, whatever its
type byte, and `uma-lp-act-entire` is the whole drive.

## The VFS

`sdstor_dev_fs` (type 0x10) is registered with `ksceVfsAddVfs` and mounted as
mount ID 1 (`sdstor0:`). It implements:

- VFS ops: mount, umount (always `0x80010010`, so it cannot be unmounted),
  set_root, init, devctl and decode_path_elem.
- Vops: open, close, lookup, read, write, lseek, remove, pread, pwrite,
  inactive and sync.

There are no directories.

- **lookup** makes a regular-file vnode whose size is the partition size and
  whose ACL data is the partition's 6 rights bytes.
- **open** checks those rights with `SceSblACMgr` context type 8:
  `acm_check_access(0, {8, rights, 6}, 1 or 3)`. Each program class (system
  apps, the updater, …) gets 4 bits, see
  [acmgr-3.65](../acmgr-3.65/README.md#filesystem-attributes). It then opens
  a handle.
- **read/write/pread/pwrite** need sector-aligned offsets and lengths
  (`0x80010022`) inside the partition (`0x80010021`). They transfer in pieces
  of at most 32 MiB minus one sector.
- **lseek** also needs alignment and accepts SEEK_SET/CUR/END.

## devctl

All commands except 1 require type-8 rights `ff 0f 00 00 00 00`, which in
practice means a `0x28…` system program.

| cmd | effect |
|---|---|
| 1 | partition info for a name: start, sector count, controller context, a controller query byte, the default memory device |
| 2 | memory card operation (`SceMsifForDriver_6EDE7DBA`) |
| 3 | sets a per-slot flag for the memory card, device 4 and an SD slot |
| 4 / 5 | ask a slot thread to rescan as if a card was inserted / removed |
| 6 | SD card command (`SceSdifForDriver_35BA9DF8`) when nothing on the device is open; the input PUID is checked |
| 7 | write a fresh SD-spec MBR to an SD card (`sds_sd_write_mbr`), then drop the device's vnodes |
| 8 | drop the device's vnodes (`vfsRmdev` of every `sdstor0:NNN`) and forget its partition table |

## Card insertion, removal and suspend

Each removable slot has a thread (`SceSdstorIntr`) woken by card
insert/remove sub-interrupts and by the suspend handler.

- **Insert**:
  - memory card and device 4: mount the memory drive (`ux0:`, mount ID
    0x800, or `xmc0:`, 0xE00, in internal-only mode) through `ksceIoMount`.
  - game card: mount `gro0:` (0x900) and `grw0:` (0xA00), read-only when
    write-protected; `sd0:` (0x100) is mounted only if neither of those
    mounted.
  - USB: only the device is brought up. **sdstor does not mount `uma0:`**;
    another module does.
- **Remove**: force-unmount the matching mounts, `vfsRmdev` every partition
  vnode, and post `SceIofilemgrForDriver_39ABDB9E(mount id, 0x200)` (media
  gone; insertion posts `0x100`) to the assign's mount events (see
  [iofilemgr-3.65](../iofilemgr-3.65/README.md#async-api-and-events)).
- **Suspend**: memory and game cards get 16 random bytes written to their
  `mediaid` partition; SD cards have their CID saved. On resume the card is
  compared and, if it was swapped, unmounted and remounted.

## Relevance to ntfsfs

- On USB the PC MBR's partition 1 is `sdstor0:uma-pp-act-a`, whatever its
  type byte and boot flag (`pp` names ignore act/ina). No other partition
  gets a name. A disk whose sector 0 looks like a boot sector (a bare NTFS
  volume) has no partition table, only the whole device.
- iofilemgr falls back to the whole device (`uma-lp-act-entire`) only when
  partition 1 does not exist. ntfsfs therefore retries a failed mount on the
  whole device, and finds NTFS in a later MBR partition there.
- On an SD card the strict SD MBR check means a card partitioned for NTFS
  exposes only the whole device (`…-lp-act-entire`, slot 16). ntfsfs's
  whole-disk MBR scan covers that.
- Transfers must be multiples of the device sector and aligned to it
  (`0x80010022`). On USB the sector size comes from the drive and can be 4096.
  ntfsfs probes it on sector 0 and aligns all I/O to it.
- `uma0:` mounting is not done here; the ntfsfs `ksceIoMount` hook therefore
  has to stay in the mount path used by whatever module mounts it.

## Not covered

The imports from SceSdif, SceMsif, SceUsbMass, ScePervasive and SceSyscon that
are not in the NID database are described only by how sdstor uses them (see
the index). `SceSysrootForDriver_F804F761` (game-card slot as an SD slot) is
inferred from its uses. Nothing here was checked on hardware.
