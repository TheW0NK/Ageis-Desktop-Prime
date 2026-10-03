# Aegis

A desktop operating system for x86-64 PCs, built on Aegis Core: a UEFI boot
manager and bootloader, a multiprocessor kernel with ext4 and FAT support, and
a user endpoint. See [docs/ROADMAP.md](docs/ROADMAP.md) for where it is going.

## Building

Needs `gcc`, `nasm`, `gnu-efi`, `mtools`, `gdisk`, `dosfstools`, `e2fsprogs`,
`fakeroot` and `python3`, plus `xorriso` for the install media; QEMU and OVMF
to run it. On Debian or Ubuntu:

    sudo apt install build-essential nasm gnu-efi mtools gdisk dosfstools e2fsprogs \
        fakeroot python3 xorriso qemu-system-x86 ovmf

Then:

    make image          # build/aegis.img, a bootable GPT disk image
    make run            # boot it in QEMU (see the Makefile for device options)
    make test           # boot it headless and run a smoke test

The default account is `user` with password `aegis`
(`make image AEGIS_USER=name AEGIS_PASSWORD=secret` to change them).

## Installing

    make iso            # build/aegis-install.iso, the install media

The ISO starts on UEFI computers and virtual machines from a CD/DVD drive, or
written to a USB stick (`dd if=build/aegis-install.iso of=/dev/sdX bs=4M`).
It loads a live copy of the system into memory and opens the installer,
which asks for a disk, your name, a password and a computer name, then
**erases that disk** and installs Aegis on it: a 256 MiB EFI system partition
and an ext4 system partition on the rest. Your account is made on the first
start. In VMware, use UEFI firmware and a SATA or NVMe disk of at least 2 GB.

From a running system, `sudo disk install DISK USER PASSWORD` does the same
from the terminal (`disk list` shows the disks).

## Testing

`tools/qemu-test.py` boots the image headless and drives it with typed
commands, key presses, mouse and tablet input, device hot-plug and screenshots,
then prints the serial log. For example:

    tools/qemu-test.py @login:user:aegis 'ls /dev' @expect:input
    tools/qemu-test.py --usb --tablet @login:user:aegis 'sudo evtest 10' aegis @click:640,400

Run it with `--help` for every step type. `make test` runs the smoke tests and
`ktest`, the kernel self-test (threads, signals, memory mapping, shared memory,
pipes, poll and sockets).
