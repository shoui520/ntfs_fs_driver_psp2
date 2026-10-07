# ntfsfs

NTFS read/write support for PS Vita / PS TV storage, as a taiHEN kernel plugin
built on [ntfs-3g](https://github.com/tuxera/ntfs-3g).

Target firmware: **3.65** (the reverse engineering it is based on is 3.65; other
versions are untested).

## How it works

- Registers an `"ntfs"` VFS with iofilemgr (`ksceVfsAddVfs`), implementing the
  same vfs-op / vop contracts as Sony's `exfatfs.skprx`
  ([docs/RE-3.65.md](RE-3.65.md); the full exfatfs reverse engineering is
  in [docs/exfatfs-3.65/](exfatfs-3.65/README.md), the SceSblACMgr
  permission module it calls in [docs/acmgr-3.65/](acmgr-3.65/README.md),
  iofilemgr in [docs/iofilemgr-3.65/](iofilemgr-3.65/README.md) and the
  sdstor block devices in [docs/sdstor-3.65/](sdstor-3.65/README.md)).
- Hooks `ksceIoMount`. When the stock exfat mount of a removable device fails,
  it points that device's iofilemgr mount-table entry at `"ntfs"` and retries
  with the same arguments, including a block device named by the caller (as
  `_vshIoMount` passes it), then restores the entry. So exFAT media keep
  working and the normal mount path (mount daemon, notifications) is used.
  When the device holds NTFS, a failure reports the NTFS error; exfat's own
  error for an NTFS volume is always `0x80010005`.
- Devices: `uma0:` (USB mass storage), `sd0:`, `grw0:` (game card slot /
  SD2Vita). Both a bare NTFS volume and an MBR disk with an NTFS (type 0x07)
  partition are accepted.
- All libntfs-3g work runs on one kernel thread with a 128 KiB stack.
- Block I/O goes through `ksceVopPread`/`ksceVopPwrite` on the device vnode in
  whole device sectors (512 bytes, or 4096 on disks that need it), with
  read-modify-write for partial sectors and exfatfs's single retry after a
  failed transfer.
- Lookups are case-insensitive, like exFAT. Path elements are upcased with the
  volume's `$UpCase` table for iofilemgr's name cache, as exfatfs does, so
  differently cased names share one vnode. NTFS metadata files (`$MFT`, ...)
  are hidden.
- Files deleted while open get the `SCEDEL~` names iofilemgr assigns. These are
  hidden too, and the file is deleted when its last handle closes, as exfatfs
  does.
- Volumes that Windows left hibernated or unclean mount read-only.
- Before the system suspends, ntfsfs writes its mounts back while the card
  still has power (sdstor powers the slot off before iofilemgr's own flush;
  see [iofilemgr-3.65](iofilemgr-3.65/README.md)). Device errors reach
  iofilemgr with their own codes, as with exfatfs.
- Permissions work as on exFAT. The NTFS READONLY, HIDDEN and SYSTEM
  attributes act like the FAT ones, and iofilemgr checks them with
  `SceSblACMgr`. Like exfatfs, ntfsfs calls `SceSblACMgr`:
  - to turn create and chstat modes into attributes;
  - to hide entries the caller may not read;
  - to decide who may query free space (devctl 0x3001).

## Build

```sh
export VITASDK=/usr/local/vitasdk   # your VitaSDK
cmake -S . -B build && cmake --build build
```

Produces `build/ntfsfs.skprx`. Add it under `*KERNEL` in `ur0:tai/config.txt`.

The module must be built with `-fno-short-enums` (CMakeLists.txt does this):
libntfs-3g declares on-disk structures with enum-typed fields, and the Vita
toolchain's default 1-byte enums would shrink e.g. `INDEX_ROOT` from 32 to 29
bytes. `src/vfs.c` has static asserts for this.

## Tests

`tests/host` runs the real `src/vfs.c`, `src/devio.c`, `src/acl.c` and libntfs-3g against an
NTFS image through a mock of the iofilemgr VFS API, then checks the result with
ntfsprogs:

```sh
tests/host/run.sh                        # host, with ASan/UBSan
# Cortex-A9 (the Vita's CPU) under qemu-user:
CC=arm-linux-gnueabihf-gcc SAN= EXTRA_CFLAGS="-mcpu=cortex-a9 -mthumb" \
RUN="qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf" tests/host/run.sh
```

Needs `mkntfs`, `ntfscp`, `ntfscat`, `ntfsls`, `ntfsfix`. The runs cover
512-byte and 4096-byte sector devices. The host build has no `SceSblACMgr`;
mode `SCE_S_IWUSR` alone decides READONLY there.

## Status

Not yet run on a Vita. Verified so far: host and Cortex-A9 tests of the VFS
layer and libntfs-3g, and that every firmware NID the module imports exists in
3.65. Known gaps:

- `ux0:` / `xmc0:` / `imc0:` are not handled.
- Unloading is refused while the module is loaded (mounts would dangle).

## License

GPL-2.0-or-later, as libntfs-3g (`third_party/ntfs-3g`, upstream commit in
`third_party/ntfs-3g/UPSTREAM`, unmodified).
