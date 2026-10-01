set -eu

out=$1
esp_dir=$2
root_dir=$3
size_mb=${4:-256}
esp_mb=64
root_uuid=${AEGIS_ROOT_PARTUUID:-6a1c0de5-ae61-4c0e-9e70-000000000001}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

rm -f "$out"
truncate -s "${size_mb}M" "$out"
sgdisk -o \
    -n 1:2048:+${esp_mb}M -t 1:ef00 -c 1:"EFI System" \
    -n 2:0:0 -t 2:8300 -c 2:"Aegis" -u 2:"$root_uuid" "$out" >/dev/null

esp_start=2048
root_start=$(sgdisk -i 2 "$out" | awk '/First sector/ {print $3}')
root_end=$(sgdisk -i 2 "$out" | awk '/Last sector/ {print $3}')
root_kib=$(( (root_end - root_start + 1) / 2 ))

cp -r "$esp_dir" "$work/esp"
bcd="$work/esp/EFI/Aegis/bcd"
if [ -f "$bcd" ]; then
    sed -i "s|^cmdline=\(.*\)$|cmdline=root=PARTUUID=$root_uuid \1|; s|  *$||" "$bcd"
fi
mkfs.fat -F 32 -n AEGIS-ESP -C "$work/esp.img" $((esp_mb * 1024)) >/dev/null
mcopy -s -i "$work/esp.img" "$work/esp/"* ::/

mkfs.ext4 -q -F -L aegis-root -b 4096 -d "$root_dir" "$work/root.img" "${root_kib}k"

dd if="$work/esp.img" of="$out" bs=512 seek=$esp_start conv=notrunc status=none
dd if="$work/root.img" of="$out" bs=512 seek="$root_start" conv=notrunc status=none
echo "Built $out (root PARTUUID $root_uuid)"
