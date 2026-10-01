#!/bin/sh
# Stages the root filesystem and builds the disk image under fakeroot, so
# files are owned by root and the user's home by the user.
#
#   mkrootfs.sh OUTPUT_IMAGE ESP_DIR ENDPOINT_BUILD USER PASSWORD [SIZE_MB]
set -eu

out=$1
esp=$2
endpoint=$3
user=$4
password=$5
size=${6:-512}
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$out")/rootfs

rm -rf "$root"
mkdir -p "$root"/bin "$root"/sbin "$root"/etc "$root"/boot "$root"/root \
         "$root"/home/"$user" "$root"/tmp
cp "$endpoint"/sbin/* "$root"/sbin/
cp "$endpoint"/bin/* "$root"/bin/
cp -r "$here"/../endpoint/rootfs/. "$root"/

cat > "$root"/etc/passwd <<PASSWD
root:x:0:0:root:/root:/bin/terminal
$user:x:1000:1000:$user:/home/$user:/bin/terminal
PASSWD
cat > "$root"/etc/group <<GROUP
root:x:0:root
adm:x:4:$user
sudo:x:27:$user
input:x:50:
$user:x:1000:$user
GROUP
{
    echo "root:!:"
    echo "$user:$("$here"/mkpasswd.py "$password"):"
} > "$root"/etc/shadow

fakeroot sh -c "
    chown -R 0:0 '$root'
    chown -R 1000:1000 '$root/home/$user'
    chmod 0755 '$root' '$root'/bin '$root'/sbin '$root'/etc '$root'/home
    chmod 0700 '$root'/root '$root/home/$user'
    chmod 1777 '$root'/tmp
    chmod 0755 '$root'/bin/* '$root'/sbin/*
    chmod 0644 '$root'/etc/passwd '$root'/etc/group '$root'/etc/motd '$root'/etc/hostname
    chmod 0600 '$root'/etc/shadow
    '$here'/mkimage.sh '$out' '$esp' '$root' $size
"
