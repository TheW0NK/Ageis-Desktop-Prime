# Aegis

A desktop operating system for x86-64 PCs, built on Aegis Core: a UEFI boot
manager and bootloader, a multiprocessor kernel with ext4 and FAT support, and
a user endpoint. See [docs/ROADMAP.md](docs/ROADMAP.md) for where it is going.

## Building

Needs `gcc`, `nasm`, `gnu-efi`, `mtools`, `gdisk`, `dosfstools`, `e2fsprogs`,
`fakeroot` and `python3`; QEMU and OVMF to run it. On Debian or Ubuntu:

    sudo apt install build-essential nasm gnu-efi mtools gdisk dosfstools e2fsprogs \
        fakeroot python3 qemu-system-x86 ovmf

Then:

    make image          # build/aegis.img, a bootable GPT disk image
    make run            # boot it in QEMU (see the Makefile for device options)
    make test           # boot it headless and run a smoke test

The default account is `user` with password `aegis`
(`make image AEGIS_USER=name AEGIS_PASSWORD=secret` to change them).

## Testing

`tools/qemu-test.py` boots the image headless and drives it with typed
commands, key presses, mouse and tablet input, device hot-plug and screenshots,
then prints the serial log. For example:

    tools/qemu-test.py @login:user:aegis 'ls /dev' @expect:input
    tools/qemu-test.py --usb --tablet @login:user:aegis 'sudo evtest 10' aegis @click:640,400

Run it with `--help` for every step type.
