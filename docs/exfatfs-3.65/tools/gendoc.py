#!/usr/bin/env python3
"""Render the structure and function reference of exfatfs-3.65.md.

usage: gendoc.py exfat-types.spec exfat-names.tsv > reference.md
"""
import re
import sys


def structs(spec):
    out = []
    cur = None
    for raw in open(spec):
        line = raw.rstrip('\n')
        code, _, comment = line.partition('#')
        p = code.split()
        if not p:
            continue
        if p[0] == 'struct':
            cur = p[1]
            out.append('### `%s` (0x%x bytes)\n' % (cur, int(p[2], 16)))
            out.append('| offset | type | field | notes |')
            out.append('|---|---|---|---|')
        elif p[0] == 'end':
            out.append('')
            cur = None
        elif p[0] == 'global':
            out.append('Global: `%s %s` at `%s`.\n' % (p[2], p[3], p[1]))
        elif cur:
            out.append('| `%s` | `%s` | `%s` | %s |' % (p[0], p[1], p[2], comment.strip()))
    return out


def functions(tsv):
    rows = [l.rstrip('\n').split('\t') for l in open(tsv)][1:]
    rows.sort(key=lambda r: int(r[0], 16) if re.match(r'^[0-9a-f]{8}$', r[0]) else 0)
    out = ['| address | name | prototype | notes |', '|---|---|---|---|']
    for r in rows:
        r += [''] * (4 - len(r))
        esc = lambda s: s.replace('|', '\\|')
        out.append('| `%s` | `%s` | `%s` | %s |' % (r[0], r[1], esc(r[2]), esc(r[3])))
    return out


def main():
    spec, tsv = sys.argv[1], sys.argv[2]
    print('## Structures\n')
    print('Offsets are verified where a function in the index reads or writes them;')
    print('fields not listed are unknown or unused.\n')
    print('\n'.join(structs(spec)))
    print('## Function index\n')
    fl = functions(tsv)
    print('Every function in the module (%d rows, including Ghidra artefacts); `?`' % (len(fl) - 2))
    print('marks an argument whose role was not needed to understand the function.\n')
    print('\n'.join(fl))


if __name__ == '__main__':
    main()
