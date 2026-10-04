#!/bin/sh
# Builds the Aegis install media: a UEFI-bootable ISO (CD/DVD, or written to
# a USB stick) whose EFI partition holds the boot files and live.img, the
# live system that the bootloader loads into memory and that starts the
# installer. Run under fakeroot so file owners are root.
#
#   mkiso.sh OUTPUT_ISO ESP_DIR LIVE_ROOT_DIR
set -eu

out=$1
esp_dir=$2
root_dir=$3
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

command -v xorriso >/dev/null || { echo "mkiso.sh: xorriso is needed (apt install xorriso)" >&2; exit 1; }

# The live system: its files plus a little room to write while it runs.
# It is kept small: the whole EFI image must stay under 32 MiB, the most an
# El Torito boot entry can describe (some firmware, VirtualBox's among it,
# will not boot an entry without its size).
root_mb=$(( $(du -sm "$root_dir" | cut -f1) + 6 ))
mkfs.ext4 -q -F -L aegis-live -b 4096 -O ^has_journal -d "$root_dir" "$work/live.img" "${root_mb}M"

cp -r "$esp_dir" "$work/esp"
cp "$work/live.img" "$work/esp/EFI/Aegis/live.img"
# Kept for update files, as the new recovery system.
cp "$work/live.img" "$(dirname "$out")/live.img"
cat > "$work/esp/EFI/Aegis/bcd" <<'BCD'
# Aegis Boot Configuration Data for the install media.
timeout=5
default=install
loader=\EFI\Aegis\btloader.efi
diagnostics=false
resolution=auto

[install]
title=Install Aegis
type=kernel
path=\EFI\Aegis\kernel.elf
ramdisk=\EFI\Aegis\live.img
cmdline=root=ramdisk

[install-verbose]
title=Install Aegis (verbose boot)
type=kernel
path=\EFI\Aegis\kernel.elf
ramdisk=\EFI\Aegis\live.img
cmdline=root=ramdisk verbose
BCD
# Recovery from the install media: the same live system, started with
# "recovery" so it opens the recovery tools for an installed system.
cat >> "$work/esp/EFI/Aegis/bcd" <<'BCD'

[recovery]
title=Aegis Recovery
type=kernel
path=\EFI\Aegis\kernel.elf
ramdisk=\EFI\Aegis\live.img
cmdline=root=ramdisk recovery
BCD

esp_kb=$(( $(du -sk --apparent-size "$work/esp" | cut -f1) * 105 / 100 + 1024 ))
if [ "$esp_kb" -ge 32768 ]; then
    echo "mkiso.sh: the EFI image would be $esp_kb KiB; it must be under 32 MiB" >&2
    exit 1
fi
mkfs.fat -n AEGIS-BOOT -C "$work/efi.img" "$esp_kb" >/dev/null
mcopy -s -i "$work/efi.img" "$work/esp/"* ::/

mkdir -p "$work/iso/boot"
cat > "$work/iso/README.TXT" <<'README'
Aegis install media. Start the computer from this disc or USB stick in
UEFI mode to install Aegis. Installing erases the disk you choose.
README
# The EFI image as a file inside the ISO (what CD/DVD firmware looks for,
# VirtualBox's included), and again as a GPT partition for USB sticks.
cp "$work/efi.img" "$work/iso/boot/efi.img"

rm -f "$out"
xorriso -as mkisofs -quiet -iso-level 3 -V AEGIS_INSTALL -o "$out" \
    -e boot/efi.img -no-emul-boot \
    -append_partition 2 0xef "$work/efi.img" -appended_part_as_gpt \
    "$work/iso"
echo "Built $out ($(( $(stat -c %s "$out") / 1048576 )) MiB)"
