#!/usr/bin/env python3
"""Builds an Aegis installer package (.aip) from a folder.

    mkaip.py FOLDER OUTPUT.aip

FOLDER/manifest holds key=value lines (id, name, version, publisher,
description, exec, icon, suite, opens, permissions, command, scope); every
other file in FOLDER is packed, keeping its relative path and mode. The
format is described in endpoint/lib/aip.c.
"""
import hashlib
import os
import struct
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    with open(os.path.join(src, 'manifest'), 'rb') as f:
        manifest = f.read()
    files = []
    for root, dirs, names in os.walk(src):
        dirs.sort()
        for name in sorted(names):
            path = os.path.join(root, name)
            rel = os.path.relpath(path, src).replace(os.sep, '/')
            if rel == 'manifest':
                continue
            with open(path, 'rb') as f:
                data = f.read()
            files.append((rel.encode(), os.stat(path).st_mode & 0o777, data))
    body = b'AEGISAIP' + struct.pack('<IIII', 1, 0, len(manifest), len(files)) + manifest
    for rel, mode, data in files:
        body += struct.pack('<HHIQ', len(rel), 0, mode, len(data)) + rel + data
    body += hashlib.sha256(body).digest() + b'AIPEND\0\0'
    with open(out, 'wb') as f:
        f.write(body)
    print('Built %s (%d files, %d bytes)' % (out, len(files), len(body)))


if __name__ == '__main__':
    main()
