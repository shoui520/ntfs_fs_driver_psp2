#!/usr/bin/env python3
"""Append func lines from exfat-names.tsv to the type spec (stdout)."""
import re, sys
types = {'exfat_mnt', 'uvfat_fd', 'uvfat_drive', 'uvfat_global', 'uvfat_fnode'}
spec = open(sys.argv[1]).read()
types |= set(re.findall(r'^struct (\w+)', spec, re.M))
out = [l for l in spec.splitlines() if not l.startswith('func ')]
for line in open(sys.argv[2]).read().splitlines()[1:]:
    f = line.split('\t')
    if len(f) < 3 or not f[0]:
        continue
    addr, name, proto = f[0], f[1], f[2].strip()
    if proto == '-' or not re.match(r'^[0-9a-f]{8}$', addr):
        continue
    proto = proto.replace('SceSSize', 'int').replace('SceOff', 'longlong').replace('SceUID', 'int').replace('SceSize', 'uint').replace('SceMode', 'int')
    m = re.match(r'^(.+?)\b(\w+)\s*\((.*)\)\s*$', proto)
    if not m or '...' in proto or '?' in proto:
        out.append('func 0x%s %s' % (addr, name))
        continue
    ret, args = m.group(1).strip(), m.group(3).strip()
    def fix(t):
        t = t.replace('const ', '')
        for k, v in (('u8', 'uchar'), ('u16', 'ushort'), ('u32', 'uint'), ('u64', 'ulonglong'), ('s32', 'int')):
            t = re.sub(r'\b%s\b' % k, v, t)
        ws = re.findall(r'[A-Za-z_]\w*', t)
        known = {'void', 'int', 'uint', 'uchar', 'ushort', 'ulonglong', 'longlong', 'char', 'bool', 'long'} | types
        for w in ws:
            if w not in known and w not in t.split()[-1:]:
                t = re.sub(r'\b%s\b' % w, 'void', t)
        return t
    al = []
    for i, a in enumerate([a.strip() for a in args.split(',')] if args else []):
        if a == 'void':
            continue
        toks = a.replace('*', ' * ').split()
        if len(toks) == 1:
            t0 = toks[0]
            known = {'int', 'uint', 'uchar', 'ushort', 'ulonglong', 'char', 'u8', 'u16', 'u32', 'u64', 's32'} | types
            al.append((fix(t0) + ' a%d' % i) if t0 in known else 'int ' + t0)
        else:
            al.append(fix(' '.join(toks[:-1])).replace(' * ', '*').replace(' *', '*') + ' ' + toks[-1])
    sig = '%s %s(%s)' % (fix(ret).replace(' *', '*'), name, ', '.join(al) or 'void')
    sig = sig.replace('bool', 'int')
    out.append('func 0x%s %s %s' % (addr, name, sig))
print('\n'.join(out))
