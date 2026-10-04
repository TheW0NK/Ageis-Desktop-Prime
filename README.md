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
written to a USB stick (`dd if=build/aegis-install.iso of=/dev/sdX bs=4M` on Linux).
It loads a live copy of the system into memory and opens the installer,
which asks for a disk, your name, a password and a computer name, then
**erases that disk** and installs Aegis on it: a 256 MiB EFI system partition
and an ext4 system partition on the rest. Your account is made on the first
start. In VMware, use UEFI firmware and a SATA or NVMe disk of at least 2 GB.

From a running system, `elevate disk install DISK USER PASSWORD` does the same
from the terminal (`disk list` shows the disks: dA, dB, ...).

## Using the desktop

- **Drag and drop.** Drag files between Files windows, the desktop, the
  Trash and app shortcuts, or onto an app to open them. Files move within
  a disk and are copied between disks; hold Ctrl to copy, Shift to move,
  Alt to make a shortcut, and press Esc to cancel. Selected text drags
  between text fields.
- **Workspaces.** Four of them: Ctrl+Super+Left/Right switches, and with
  Shift the focused window comes along. The taskbar shows the windows on
  the current one; right-click a window's button to move it.
- **Command palette.** Super+P lists the focused app's menu commands,
  open windows, workspaces, apps and session actions; type to filter and
  press Enter.
- **Guest account.** With the "Guest account" feature on (Feature
  Manager), the sign-in screen has **Sign in as Guest**: no password, no
  administrator rights, and everything the guest saved is erased when they
  sign out.
- **Hiding things.** Settings > Appearance can hide the desktop items (the
  files stay in the Desktop folder) and the taskbar, which then leaves no
  trace and comes back while the pointer is at the bottom edge of the
  screen.
- Super+L locks the screen, Super+arrows snap windows, Print Screen saves
  a screenshot to Images/Screenshots.

## Recovery

Installed systems have **Aegis Recovery** and **Aegis (safe mode)** in the boot
menu (press an arrow key while it counts down); the install media has
Recovery too. Recovery finds the installed system and offers:

- **Start Aegis**: restart, restart once in safe mode (no routines or
  sound), shut down, or restart into the UEFI firmware settings.
- **Startup settings**: turn sentries on or off, show startup messages, the
  boot menu wait, screen resolution and extra kernel options.
- **Boot configuration**: edit the BCD file directly.
- **Reset a password** (the account's saved credentials are set aside).
- **System image**: save the whole system to `/osystem/backups`, or restore one.
- **Reinstall**: put back fresh system files keeping accounts, settings, apps
  and files, or erase the disk and install again.
- **Terminal**: a superuser terminal with the system at `/osystem/volumes/system`.

`elevate crash` stops the computer with the crash screen, to see it; the stop
codes and their lines are in `kernel/head/stopcodes.h`.

## Folders and names

| Folder | Holds |
|---|---|
| `/osystem` | The system: `core` (init, sentries, the display), `boot` (the EFI partition), `devices`, `resources` (fonts, app files), `logs`, `data`, `backups`, `temp`, `volumes` (mounted disks) |
| `/sysapps` | System apps and commands |
| `/userApps` | Apps people install (`.aip` packages); `commands` for their commands |
| `/userfiles` | One folder per account; the superuser's is `/userfiles/superuser` |
| `/serve` | Content served to the network |
| `/msc` | Settings for the whole computer (accounts, routines, host name) |

Aegis has its own words for things: a running program is a **thread** and the
threads inside it are **strands**; background programs started by init are
**sentries** (account-sentry, routine-sentry, audio-sentry); scheduled
commands are **routines**; a signed-in desktop is a **shift**; the
all-powerful account is the **superuser**, and administrators `elevate` to it.
Disks are `dA`, `dB`, ... and their partitions `dA1`, `dA2`, ...; the kernel
log is `klog`.

Aegis's own file formats:

| Format | What | Looks like |
|---|---|---|
| `.aacc` | Accounts and groups (`/msc/accounts.aacc`, readable by everyone) and password hashes (`/msc/secrets.aacc`, superuser only) | `aegis accounts 1`, then blocks such as `account alex` with indented `id: 1000`, `display: Alex`, ... |
| `.aset` | Settings: the computer's (`/msc/computer.aset`), features (`/msc/features.aset`) and each person's (`/userfiles/<name>/system/settings.aset`) | `aegis settings 1`, then `key: value` lines |
| `.tscr` | Terminal scripts | `terminal script 1`, then one command per line (`$@` is the arguments) |
| `.aex` | Aegis executables: an x86-64 program inside an Aegis header (`tools/mkaex.py` makes one) | `AEGISAEX`, version, program offset and size, name |
| `.aip` | App packages (see `docs/PACKAGES.md`) | |
| `.aui` / `.as` | Window layouts and AegisScript (see `docs/AUI.md`, `docs/SCRIPT.md`) | |

The terminal prompt is `user@computer@folder - :`. Its commands:

| Command | Does |
|---|---|
| `list`, `show`, `find` | List a folder, print files, find lines containing text |
| `go`, `where` | Change folder, show the current folder |
| `copy`, `move`, `delete`, `newfolder`, `newfile`, `link` | Work with files |
| `details`, `access`, `owner`, `space` | File details, permissions, owner, free space |
| `tasks`, `stop` | Running threads; signal or end them |
| `elevate` | Run a command as the superuser |
| `me`, `ids`, `system`, `date`, `uptime` | About you and the computer |
| `vars`, `set`, `history`, `clear`, `wait`, `echo`, `run`, `help` | The terminal itself |
| `restart`, `shutdown`, `flush` | Power and disks |

Typing a command from another system (`ls`, `cd`, `sudo`, ...) says what it is
called here. Programs include `klog`, `network`, `connect`, `checksum`,
`disk`, `aip`, `fetch`, `ping` and `host`.

## Testing

`tools/qemu-test.py` boots the image headless and drives it with typed
commands, key presses, mouse and tablet input, device hot-plug and screenshots,
then prints the serial log. For example:

    tools/qemu-test.py @login:user:aegis 'list /osystem/devices' @expect:input
    tools/qemu-test.py --usb --tablet @login:user:aegis 'elevate evtest 10' aegis @click:640,400

Run it with `--help` for every step type. `make test` runs the smoke tests and
`ktest`, the kernel self-test (threads, signals, memory mapping, shared memory,
pipes, poll and sockets).
