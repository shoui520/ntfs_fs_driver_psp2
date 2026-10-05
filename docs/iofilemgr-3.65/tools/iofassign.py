#!/usr/bin/env python3
"""iofassign.py <iofilemgr.elf>: decode the assign templates used by iof_assign_init (0x8101844c)."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
BASE, OFF = 0x81000000, 0xa0
def w(a): return struct.unpack_from('<I', d, a - BASE + OFF)[0]
def s(a):
    if not (BASE <= a < BASE + 0x20980): return hex(a)
    o = a - BASE + OFF; e = d.index(b'\0', o); return repr(d[o:e].decode('latin1'))
def info(p):
    # SceVfsMountData {assign, unit name, blockdev, blockdev_no_part, mount id}
    v = [w(p + 4 * i) for i in range(5)]
    return '%s unit=%s blockdev=%s fallback=%s id=%#x' % tuple(
        [s(x) if x else '-' for x in v[:4]] + [v[4]])
# (mount id, template address or None, info pointer)
T = [(1, None, 0x8101d51c), (2, None, 0x8101d8f0), (0x100, 0x8101dabc, 0x8101d924), (0x200, 0x8101db3c, 0x8101d938),
     (0x300, 0x8101dc30, 0x8101d65c), (0xb00, 0x8101d490, 0x8101d904), (0x400, 0x8101da5c, 0x8101d584),
     (0x500, 0x8101d6a0, 0x8101d5c8), (0x600, 0x8101dbc0, 0x8101da34), (0xc00, 0x8101dba0, 0x8101dc1c),
     (0x700, 0x8101d4b0, 0x8101d978), (0x800, 0x8101d470, 0x8101d6ec), (0x800, 0x8101d470, 0x8101d898),
     (0x900, 0x8101da90, 0x8101d9dc), (0xa00, 0x8101db5c, 0x8101da48), (0xd00, 0x8101dadc, 0x8101d98c),
     (0xe00, 0x8101d4fc, 0x8101dafc), (0x50000, 0x8101d94c, 0x8101d6d8), (0x60000, 0x8101d5dc, 0x8101d5fc),
     (0x10000, 0x8101d7a4, 0x8101d9b4), (0x20000, 0x8101d784, 0x8101d9c8), (0xf00, 0x8101d7d0, 0x8101da7c)]
for mid, t, inf in T:
    if t:
        f = [w(t + 4 * i) for i in range(8)]
        print('%#7x root=%s fs_type=%d opt=%#x mnt_flags=%#x vfs=%s  %s' % (
            mid, s(f[0]), f[2] & 0xff, f[2] >> 16, f[3], s(f[4]), info(inf)))
    else:
        print('%#7x (built in code)  %s' % (mid, info(inf)))
