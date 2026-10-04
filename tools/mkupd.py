#!/usr/bin/env python3
"""Makes an Aegis system update (.upd): the system files of a staged root
(as tools/mkrootfs.sh leaves it), the boot files and the recovery image.

    mkupd.py OUTPUT.upd ROOT_DIR ESP_DIR RECOVERY_IMG VERSION [BUILD] [DESCRIPTION]

Accounts, settings, logs, apps people installed and everyone's files are
left out: Recovery keeps the computer's own (see endpoint/lib/update.c for
the format and endpoint/lib/install.c for what is kept).
"""

import hashlib
import os
import sys
import tempfile

# Not part of the system: as install.c's skipped() with keeping on.
LEFT_OUT = {
    '/lost+found',
    '/osystem/devices', '/osystem/temp', '/osystem/volumes', '/osystem/boot', '/userfiles', '/msc/live',
    '/msc/firstboot.aset', '/osystem/installer', '/osystem/updates', '/osystem/logs', '/osystem/data',
    '/osystem/backups', '/userApps', '/serve', '/msc/accounts.aacc', '/msc/secrets.aacc',
    '/msc/computer.aset', '/msc/features.aset', '/msc/routines', '/msc/hosts',
}
BOOT_FILES = ['EFI/BOOT/BOOTX64.EFI', 'EFI/Aegis/btloader.efi', 'EFI/Aegis/kernel.elf']


def main():
    if len(sys.argv) < 6:
        sys.exit(__doc__)
    out, root, esp, recovery, version = sys.argv[1:6]
    build = sys.argv[6] if len(sys.argv) > 6 else ''
    description = sys.argv[7] if len(sys.argv) > 7 else ''
    files = 0
    sha = hashlib.sha256()
    size = 0

    with tempfile.TemporaryFile() as body:
        def put(data):
            nonlocal size
            body.write(data)
            sha.update(data)
            size += len(data)

        def put_file(src, name, mode):
            nonlocal files
            n = os.path.getsize(src)
            put(b'F %o %d %s\n' % (mode, n, name.encode()))
            with open(src, 'rb') as f:
                while True:
                    chunk = f.read(1 << 20)
                    if not chunk:
                        break
                    put(chunk)
            files += 1

        for dirpath, dirnames, filenames in os.walk(root):
            rel = '/' + os.path.relpath(dirpath, root) if dirpath != root else ''
            dirnames[:] = sorted(d for d in dirnames if rel + '/' + d not in LEFT_OUT)
            if rel:
                put(b'D %o %s\n' % (0o755, rel.encode()))
            for name in sorted(filenames):
                path = rel + '/' + name
                full = os.path.join(dirpath, name)
                if path in LEFT_OUT or os.path.islink(full) or not os.path.isfile(full):
                    continue
                mode = 0o755 if os.stat(full).st_mode & 0o111 else 0o644
                put_file(full, path, mode)
        for d in ('/EFI', '/EFI/BOOT', '/EFI/Aegis'):
            put(b'D 755 esp:%s\n' % d.encode())
        for name in BOOT_FILES:
            put_file(os.path.join(esp, name), 'esp:/' + name, 0o644)
        put_file(recovery, 'esp:/EFI/Aegis/recovery.img', 0o644)
        put(b'E\n')

        header = ['aegis update 1', 'version: ' + version]
        if build:
            header.append('build: ' + build)
        if description:
            header.append('description: ' + description)
        header += ['files: %d' % files, 'size: %d' % size, 'sha256: ' + sha.hexdigest(), '', '']
        body.seek(0)
        with open(out, 'wb') as f:
            f.write('\n'.join(header).encode())
            while True:
                chunk = body.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
    print('%s: Aegis %s, %d files, %d KiB' % (out, version, files, size // 1024))


if __name__ == '__main__':
    main()
