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

# The live system: its files plus room to write while it runs (it is kept
# in memory, so the room is kept small).
root_mb=$(( $(du -sm "$root_dir" | cut -f1) + 48 ))
mkfs.ext4 -q -F -L aegis-live -b 4096 -O ^has_journal -d "$root_dir" "$work/live.img" "${root_mb}M"

cp -r "$esp_dir" "$work/esp"
cp "$work/live.img" "$work/esp/EFI/Aegis/live.img"
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
cmdline=root=ram0

[install-verbose]
title=Install Aegis (verbose boot)
type=kernel
path=\EFI\Aegis\kernel.elf
ramdisk=\EFI\Aegis\live.img
cmdline=root=ram0 verbose
BCD
if [ -f "$esp_dir/EFI/Aegis/recovery.efi" ]; then
    printf '\n[recovery]\ntitle=Aegis Recovery\ntype=efi\npath=\\EFI\\Aegis\\recovery.efi\n' >> "$work/esp/EFI/Aegis/bcd"
fi

esp_kb=$(( $(du -sk --apparent-size "$work/esp" | cut -f1) * 105 / 100 + 8192 ))
[ "$esp_kb" -lt 36864 ] && esp_kb=36864
mkfs.fat -F 32 -n AEGIS-BOOT -C "$work/efi.img" "$esp_kb" >/dev/null
mcopy -s -i "$work/efi.img" "$work/esp/"* ::/

mkdir -p "$work/iso"
cat > "$work/iso/README.TXT" <<'README'
Aegis install media. Start the computer from this disc or USB stick in
UEFI mode to install Aegis. Installing erases the disk you choose.
README

rm -f "$out"
xorriso -as mkisofs -quiet -iso-level 3 -V AEGIS_INSTALL -o "$out" \
    -partition_offset 16 -append_partition 2 0xef "$work/efi.img" -appended_part_as_gpt \
    -e --interval:appended_partition_2:all:: -no-emul-boot \
    "$work/iso"
echo "Built $out ($(( $(stat -c %s "$out") / 1048576 )) MiB)"
