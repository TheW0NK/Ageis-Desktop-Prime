#!/bin/sh
# Stages the root filesystem and builds the disk image under fakeroot, so
# files are owned by root and the user's home by the user.
#
#   mkrootfs.sh OUTPUT_IMAGE ESP_DIR ENDPOINT_BUILD USER PASSWORD [SIZE_MB]
#
# With LIVE=1 it builds the install media instead: OUTPUT_IMAGE is an ISO
# whose system has no accounts and starts the installer (USER and PASSWORD
# are ignored).
set -eu
live=${LIVE:-0}

out=$1
esp=$2
endpoint=$3
user=$4
password=$5
size=${6:-512}
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$out")/rootfs
[ "$live" = 1 ] && root=$(dirname "$out")/liveroot

rm -rf "$root"
mkdir -p "$root"/bin "$root"/sbin "$root"/etc "$root"/boot "$root"/root "$root"/tmp \
         "$root"/usr/share/applications
# Each user has /users/<name>/home (their files) and /users/<name>/system
# (settings, encrypted credentials, app data).
ud="$root"/users/"$user"
[ "$live" = 1 ] && ud="$root"/tmp/no-user
mkdir -p "$ud"/home/Desktop "$ud"/home/Documents "$ud"/home/Downloads "$ud"/home/Images "$ud"/home/Music \
         "$ud"/system/settings "$ud"/system/credentials "$ud"/system/appdata
echo "$user" > "$ud"/system/settings/name
echo light > "$ud"/system/settings/theme
echo en > "$ud"/system/settings/language
echo default > "$ud"/system/settings/background
# Sample pictures, also offered as backgrounds.
"$here"/mksamples.py "$root"/usr/share/backgrounds
cp "$root"/usr/share/backgrounds/*.png "$ud"/home/Images/
# Sample music: one tune made here, and Ogg and MP3 versions of others.
"$here"/mkmusic.py "$ud/home/Music/Aegis Theme.wav" theme
cp "$here"/samples/*.ogg "$here"/samples/*.mp3 "$ud"/home/Music/
# A sample app package to try the package installer with.
mkdir -p "$root"/usr/share/samples "$root"/apps/bin "$root"/var/lib/aip
"$here"/mkaip.py "$here"/samples/aip/dice "$root"/usr/share/samples/Dice.aip >/dev/null
cp "$root"/usr/share/samples/Dice.aip "$ud"/home/Downloads/
cp "$endpoint"/sbin/* "$root"/sbin/
cp "$endpoint"/bin/* "$root"/bin/
cp -r "$here"/../endpoint/rootfs/. "$root"/
# Graphical programs: registry entries and their data files.
for dir in "$here"/../endpoint/apps/*/ "$here"/../endpoint/system/*/; do
    name=$(basename "$dir")
    for app in "$dir"*.app; do
        [ -e "$app" ] && cp "$app" "$root"/usr/share/applications/
    done
    if [ -d "$dir"res ]; then
        mkdir -p "$root"/usr/share/"$name"
        cp -r "$dir"res/. "$root"/usr/share/"$name"/
    fi
done
mkdir -p "$root"/etc/ssl/certs "$root"/usr/share/fonts
cp "$here"/../third_party/fonts/*.ttf "$root"/usr/share/fonts/
cp "$here"/../third_party/ca-certificates.pem "$root"/etc/ssl/certs/ca-bundle.pem

mkdir -p "$root"/var/log
cat > "$root"/etc/crontab <<CRONTAB
# System jobs: schedule, account, command. Edit with the Cron Jobs app.
#   minute hour day month weekday  user  command
CRONTAB
if [ "$live" = 1 ]; then
    rm -rf "$root"/tmp/no-user "$root"/users
    mkdir -p "$root"/users
    echo "This is the live system on the Aegis install media." > "$root"/etc/live
    mkdir -p "$root"/usr/share/installer
    cp -r "$esp" "$root"/usr/share/installer/esp
    printf 'root:x:0:0:root:/root:/bin/terminal\n' > "$root"/etc/passwd
    printf 'root:x:0:root\nadm:x:4:\nsudo:x:27:\nvideo:x:44:\naudio:x:63:\ninput:x:50:\n' > "$root"/etc/group
    printf 'root:!:\n' > "$root"/etc/shadow
    ud="$root"/users
else
cat > "$root"/etc/passwd <<PASSWD
root:x:0:0:root:/root:/bin/terminal
$user:x:1000:1000:$user:/users/$user/home:/bin/terminal
PASSWD
cat > "$root"/etc/group <<GROUP
root:x:0:root
adm:x:4:$user
sudo:x:27:$user
video:x:44:$user
audio:x:63:$user
input:x:50:
$user:x:1000:$user
GROUP
{
    echo "root:!:"
    echo "$user:$("$here"/mkpasswd.py "$password"):"
} > "$root"/etc/shadow
fi

fakeroot sh -c "
    chown -R 0:0 '$root'
    [ '$live' = 1 ] || chown -R 1000:1000 '$ud'
    chmod 0755 '$root' '$root'/bin '$root'/sbin '$root'/etc '$root'/users '$root'/usr '$root'/usr/share
    if [ '$live' != 1 ]; then
        chmod 0711 '$ud'
        chmod 0700 '$ud'/home '$ud'/system '$ud'/system/settings '$ud'/system/credentials '$ud'/system/appdata
        chmod 0600 '$ud'/system/settings/*
    else
        chmod 0755 '$ud'
    fi
    chmod 0700 '$root'/root
    chmod 1777 '$root'/tmp
    chmod 0755 '$root'/bin/* '$root'/sbin/*
    chmod 0644 '$root'/etc/passwd '$root'/etc/group '$root'/etc/motd '$root'/etc/hostname '$root'/etc/hosts \\
          '$root'/etc/ssl/certs/ca-bundle.pem
    chmod 0600 '$root'/etc/shadow
    chmod 0644 '$root'/etc/crontab
    chmod 0755 '$root'/var '$root'/var/log '$root'/var/lib '$root'/var/lib/aip '$root'/apps '$root'/apps/bin
    if [ '$live' = 1 ]; then
        '$here'/mkiso.sh '$out' '$esp' '$root'
    else
        '$here'/mkimage.sh '$out' '$esp' '$root' $size
    fi
"
