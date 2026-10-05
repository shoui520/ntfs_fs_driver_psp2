# acmgr.skprx (retail 3.65), reverse engineered

`os0:kd/acmgr.skprx` (module `SceSblACMgr`) is the kernel's access-control
manager. Other modules ask it what the calling program is allowed to do. It
answers from three sources:

- the program's self authentication info (authority ID, capability and
  attribute bits), through `ksceSysrootGetSelfAuthInfo`;
- the calling thread's access level, through
  `ksceKernelSysrootGetThreadAccessLevel`;
- a few fixed tables of device names and paths built into the module.

It also translates between `SCE_S_*` modes and the attribute bytes that
exfatfs (and a PFS-style variant) store on disk. That is the part
[exfatfs](../exfatfs-3.65/README.md) depends on.

Firmware: retail 3.65 `PSP2UPDAT.PUP`. Text is at `0x81000000` (0x2E94 bytes);
the module has no writable data. Everything here comes from static analysis;
nothing was checked on hardware.

All 89 functions were read in full: 49 exported bodies, 27 internal helpers
and 13 import stubs. The remaining bytes of `.text` are `tbb` jump tables,
padding, literal pools (mostly 64-bit authority IDs) and stub tails.
[reference.md](reference.md) has every function and structure.

## Files here

| file | contents |
|---|---|
| [reference.md](reference.md) | structures and the full function index (generated) |
| [acmgr-names.tsv](acmgr-names.tsv) | address, name, prototype, notes for every function |
| [acmgr-types.spec](acmgr-types.spec) | structure layouts |
| [acmgr-exports.tsv](acmgr-exports.tsv) | every exported NID, its address and its name in the vita-headers database, if any |
| [tools/acmdata.py](tools/acmdata.py) | decodes the module's tables (see below) |

The annotated Ghidra database is rebuilt the same way as exfatfs's: run
`../exfatfs-3.65/tools/genspec.py acmgr-types.spec acmgr-names.tsv` and apply
the result with `ApplyTypes.java`. `gendoc.py` from the same directory renders
reference.md.

## Exports

The 49 bodies are exported from `SceSblACMgrForKernel` (NID `0x11F9B314`)
and `SceSblACMgrForDriver` (NID `0x9AD8E213`). 43 appear in both libraries
under different NIDs. Four are ForKernel only: the two empty functions
`0xFBA1A256` and `0x1948E9DB`, `acm_require_cap_1_85` and
`acm_check_remap_code`. Two are ForDriver only: `acm_attr_from_mode_level`
(`0x0606B87E`) and `acm_is_authid_2801` (`0x2E992B02`). One syscall,
`_sceSblACMgrIsGameProgram` (library `SceSblACMgr`), copies
`ksceSblACMgrIsGameProgram(0)` for the caller to user memory.
[acmgr-exports.tsv](acmgr-exports.tsv) lists every NID.

23 of the bodies have a name in the vita-headers database, which is kept. The
rest are named after what they do (`acm_*`). A name built from a bit number or
an ID, such as `acm_has_cap_1f`, means that only the mechanism is known, not
what callers use it for.

## Obfuscated strings

Every string the module compares against (device names, path prefixes) is
stored bit-inverted, and the comparisons are written as `*s == (u8)~*table`.
No plain-text device name appears in the binary. `tools/acmdata.py` decodes
the tables:

```
python3 tools/acmdata.py acmgr.elf 8100253c 30 plplw   # media types
python3 tools/acmdata.py acmgr.elf 81002228 21 plw     # root modes
```

## Program identity

`SceSelfAuthInfo` (0x90 bytes) holds a 64-bit program authority ID, 32 bytes
of capability bits at `+0x10` and 32 bytes of attribute bits at `+0x30`.
Bits are numbered MSB first: bit `n` is `byte[n >> 3] >> (7 - (n & 7))`.

- **Capability bits**:
  - 0 IsRootProgram, 1 IsSystemProgram, 2/3 NonGameOrGame, 3 NonGame.
  - Others gate single checks: `0x1F`, `0x20`, `0x22` (remap code),
    `0x80`, `0x81`, `0x82` (QA settings, extended memory), `0x83` (USB
    serial), `0x84` (virtual machine), `0x85`, `0x86`, `0x87`.
  - The type-6 attribute code also reads capability word 0 as a program
    class: exactly `0x80`, `0x40`, `0x20` or `0x10`.
- **Attribute bits**: `0x81` marks a restricted program (vs0: whitelist,
  removable-media paths). `0x10` set means not debuggable.
- **Authority ID classes**, used by many `Is*` functions:
  - `0x21…`: games. IsGameProgram also accepts exactly `0x2F00000000000001`.
  - `0x22…`: PSM-style titles.
  - `0x28…`: system applications. SceShell is `0x2800000000000001`.
  - `0x2F0…`: fself.
  - The specific IDs are listed per function in the reference. Examples:
    PspEmu `0x2800000000007009`/`…13`, WebCore `0x2800000000008003`/`…8005`,
    the updater/package installer `0x28008000000000xx`.
- Several checks also accept fself programs when a sysroot hook allows it.
  `SceSysrootForDriver_26AA237C`, `E2515A08` and `56D85EB0` are not in the NID
  database. They call function pointers at `sysroot+0x36C`, `+0x370` and
  `+0x38C`, which other modules register through
  `SceSysrootForDriver_E25D2FD5`, `E2E88E3E` and `A12C9950`, and return 0
  while unset. Who registers them was not traced.

`module_start` registers `acm_check_remap_code` (capability `0x22`, or fself
plus hook `0x36C`) with sysroot. `ksceKernelSysrootCheckRemapCodeForUser`
calls it through the pointer stored at `sysroot+0x324`.

## Paths and devices

- `ksceSblACMgrGetMediaType(path, &type)` classifies a path by device prefix,
  with digits after the device name skipped. Examples: `sd` 1, `os` 2,
  `vs` 3, `ux` 0xC, `ux:app` 0x17, `ux:patch` 0x18, `ux:data` 0x19, `ux:user` 0,
  `gro:app` 0xD, `grw:patch` 0xE, `uma` 0x1B. Matching is by prefix only.
- `acm_check_vs0_path` gives programs with attribute `0x81` a vs0:
  whitelist. Nine authority IDs may reach `vs0:data/external…`,
  `vs0:sys/external` or `vs0:data/external/cert/` and
  `…/webcore/`; all other restricted programs get nothing. Unrestricted
  programs always pass.
- `acm_check_dev_path` returns 0 for `ux0:`, `sd0:`, `pd0:` and `host0:` on
  retail units. With dipsw `0x9F` (development mode) it allows `ux0:`, `sd0:`
  and `host0:`. Non-game programs without attribute `0x81` always pass.
- `acm_get_root_mode` gives the root directory mode of an assign: `sd0:`
  `0x186`, `lma0:` `0x1000000`, the 19 other known assigns `0x106`.
- `acm_check_devinfo(pid, assign)` decides who may read free-space
  information (exfatfs devctl `0x3001`). It allows system programs,
  privileged threads, non-game programs on `ux0:`, `0x220000101CC73883`, and
  `0x220000101CC60019` on `lma0:`.

## Filesystem attributes

The attribute functions take a context `acm_ctx {type, data, count}`:

| type | meaning | handled by |
|---|---|---|
| 1..5 | FAT attribute byte (1..3) or word (4..5); exfatfs uses 2 (FAT16) and 3 | `acm_fat_*` |
| 6 | 16-bit PFS-style attribute: low 3 bits as FAT, `0x2000`/`0x4000` extra flags, `0x8000` directory | `acm_pfs_*` |
| 7 | nothing supported (`0x800F0925`) | |
| 8 | 6 bytes of 4-bit rights per program class, access checks only | `acm_t8_check_access` |

For types 1..5 the low three bits are FAT's READONLY (1), HIDDEN (2) and
SYSTEM (4). Privileged threads are those at access level 0x40 or 0x80, user
threads those at 0x10 or 0x20.

| operation | export | effect |
|---|---|---|
| set from mode | `kscePfsACSetFSAttrByMode` | privileged: `0x186`→0, `0x106`→4 (`0x10106`→1), `0x104`→5, `0x6`→6, `0x4`→7; user: `0x180`→0, `0x100`→1; `0x1000000`→`0x0F`; changing SYSTEM needs a privileged thread |
| set from mode, explicit level | `ForDriver_0606B87E` | same tables, level passed in, no SYSTEM restriction |
| inherit | `ForDriver_B12CEAA8` | new entry takes the parent's low 3 bits (type 6: the whole value) |
| to mode | `ForDriver_3B356B98` | `{0x186, 0x106, 0x186, 0x106, 0x106, 0x104, 0x6, 0x4}[attr & 7]` |
| access check | `ForDriver_BE5667C5` | rights 1 read, 2 write, 8 (third right); user threads: RO → read+8, SYSTEM → read, SYSTEM\|HIDDEN → none; privileged: SYSTEM\|RO → read+8, else all |
| special `0x0F` | `ForDriver_48CFCEA2` | an entry whose attribute is `0x0F` becomes 0 only when the key (exfatfs passes the mount ID) is `0x10000`, i.e. on `lma0:` |

Modes use `SCE_S_IRUSR` 0x100, `IWUSR` 0x80, `IRSYS` 4, `IWSYS` 2. What the
`0x10000` flag means in a mode was not determined; it only picks READONLY
instead of SYSTEM for `0x106`.

## Error codes

`0x800F0916` invalid argument, `0x800F0903` no auth info for the process or
no media-type match, `0x800F0902` not found or not permitted,
`0x800F0925` unsupported context type, `0x800F0928` unknown program class,
`0x800F090D` and `0x8001000D` access denied, `0x800F090E` NULL path,
`0x80010016` bad mode, `0x80010002` mode not in the table.

## Not covered

- Which modules register the three sysroot hooks, and the meaning of
  `SceQafMgrForDriver_694D1096`, were not traced; they are outside this
  module.
- Who uses context types 6 and 8 was not traced; exfatfs only uses types 2
  and 3.
- None of this has been checked on hardware.
