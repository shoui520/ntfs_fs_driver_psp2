#!/usr/bin/env python3
"""Check every import NID of a Vita module against firmware export tables.

usage: nidcheck.py <module.velf|.elf> <fw module dir>...
Parses SceModuleInfo (e_entry -> segment/offset) of every ELF under the given
dirs, collects exported (library NID, function NID) pairs, and reports any
import of the target module that no firmware module exports.
"""
import os
import struct
import sys


def load(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF':
        return None
    phoff, = struct.unpack_from('<I', d, 0x1c)
    phnum, = struct.unpack_from('<H', d, 0x2c)
    segs = [struct.unpack_from('<8I', d, phoff + i * 32) for i in range(phnum)]
    return d, segs


def va2off(segs, va):
    for s in segs:
        if s[0] == 1 and s[2] <= va < s[2] + s[4]:
            return va - s[2] + s[1]
    return None


def modinfo(d, segs):
    e_entry, = struct.unpack_from('<I', d, 0x18)
    seg = segs[e_entry >> 30]
    off = seg[1] + (e_entry & 0x3fffffff)
    name = d[off + 4:off + 31].split(b'\0')[0].decode('latin1')
    exp_top, exp_end, imp_top, imp_end = struct.unpack_from('<4I', d, off + 0x24)
    base = seg[1]
    return name, base, exp_top, exp_end, imp_top, imp_end, seg


def exports(path):
    r = load(path)
    if not r:
        return set()
    d, segs = r
    try:
        name, base, et, ee, it, ie, seg = modinfo(d, segs)
    except Exception:
        return set()
    out = set()
    o = base + et
    while o < base + ee:
        size, = struct.unpack_from('<H', d, o)
        if size not in (0x20,):
            break
        nfunc, nvar = struct.unpack_from('<HH', d, o + 6)
        libnid, = struct.unpack_from('<I', d, o + 0x10)
        nids_va, = struct.unpack_from('<I', d, o + 0x18)
        no = va2off(segs, nids_va)
        if no is not None:
            for i in range(nfunc + nvar):
                out.add((libnid, struct.unpack_from('<I', d, no + 4 * i)[0]))
        o += size
    return out


def imports(path):
    d, segs = load(path)
    name, base, et, ee, it, ie, seg = modinfo(d, segs)
    out = []
    o = base + it
    while o < base + ie:
        size, = struct.unpack_from('<H', d, o)
        if size == 0x34:
            nfunc, nvar = struct.unpack_from('<HH', d, o + 6)
            libnid, libname_va = struct.unpack_from('<II', d, o + 0x10)
            fnids_va, = struct.unpack_from('<I', d, o + 0x1c)
            vnids_va, = struct.unpack_from('<I', d, o + 0x24)
        elif size == 0x24:
            nfunc, nvar = struct.unpack_from('<HH', d, o + 6)
            libnid, libname_va, fnids_va, _, vnids_va = struct.unpack_from('<5I', d, o + 0xc)
        else:
            raise SystemExit('unknown import entry size 0x%x' % size)
        lo = va2off(segs, libname_va)
        lib = d[lo:lo + 64].split(b'\0')[0].decode()
        fo = va2off(segs, fnids_va)
        for i in range(nfunc):
            out.append((lib, libnid, struct.unpack_from('<I', d, fo + 4 * i)[0]))
        if nvar:
            vo = va2off(segs, vnids_va)
            for i in range(nvar):
                out.append((lib, libnid, struct.unpack_from('<I', d, vo + 4 * i)[0]))
        o += size
    return out


def main():
    target, dirs = sys.argv[1], sys.argv[2:]
    exp = set()
    for d in dirs:
        for root, _, files in os.walk(d):
            for f in files:
                if f.endswith('.elf'):
                    exp |= exports(os.path.join(root, f))
    bad = 0
    imps = imports(target)
    for lib, libnid, nid in imps:
        ok = (libnid, nid) in exp
        if not ok:
            bad += 1
        print('%-4s %-28s %08X %08X' % ('OK' if ok else 'MISS', lib, libnid, nid))
    print('%d imports, %d missing from firmware exports' % (len(imps), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
