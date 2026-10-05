#!/usr/bin/env python3
"""acmdata.py <acmgr.elf> <hexaddr> <count> <fmt> : decode acmgr rodata.
fmt letters per field: p = pointer to bit-inverted string (length from the next 'l' field),
l = length, w = word, s = pointer to plain C string. Example: pl pl w."""
import struct, sys
D = open(sys.argv.pop(1), 'rb').read()
def rd(a, n): return D[0xa0 + a - 0x81000000:0xa0 + a - 0x81000000 + n]
def w(a): return struct.unpack('<I', rd(a, 4))[0]
def inv(a, n): return bytes((~b) & 0xff for b in rd(a, n))
def cstr(a):
    s = rd(a, 256); return s[:s.index(0)].decode('latin1')
a, cnt, fmt = int(sys.argv[1], 16), int(sys.argv[2]), sys.argv[3]
for i in range(cnt):
    vals = [w(a + 4 * j) for j in range(len(fmt))]
    out = []
    for j, f in enumerate(fmt):
        v = vals[j]
        if f == 'p':
            n = vals[j + 1] if j + 1 < len(fmt) and fmt[j + 1] == 'l' else 16
            out.append(repr(inv(v, n).decode('latin1')) if 0x81000000 <= v < 0x81003000 else hex(v))
        elif f == 's':
            out.append(repr(cstr(v)) if 0x81000000 <= v < 0x81003000 else hex(v))
        elif f == 'w':
            out.append(hex(v))
        elif f == 'l':
            out.append(str(v))
    print('%08x  %s' % (a, '  '.join(out)))
    a += 4 * len(fmt)
