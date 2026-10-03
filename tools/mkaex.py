#!/usr/bin/env python3
"""Wraps an ELF program in an Aegis executable (.aex) header.

    mkaex.py PROGRAM [NAME]     rewrites PROGRAM in place
    mkaex.py IN OUT NAME        writes OUT

Layout: "AEGISAEX", u32 version (1), u32 header size, u64 program offset,
u64 program size, char name[32], then the ELF program.
"""

import struct
import sys

HEADER = struct.Struct('<8sIIQQ32s')


def wrap(elf, name):
    if elf[:4] != b'\x7fELF':
        sys.exit('mkaex: not an ELF program')
    head = HEADER.pack(b'AEGISAEX', 1, HEADER.size, HEADER.size, len(elf), name.encode()[:31])
    return head + elf


def main():
    args = sys.argv[1:]
    if len(args) not in (1, 2, 3):
        sys.exit(__doc__)
    src = args[0]
    dst = args[1] if len(args) == 3 else src
    name = args[-1] if len(args) >= 2 else src.rsplit('/', 1)[-1]
    with open(src, 'rb') as f:
        data = f.read()
    if data[:8] == b'AEGISAEX':
        return
    out = wrap(data, name)
    with open(dst, 'wb') as f:
        f.write(out)


if __name__ == '__main__':
    main()
