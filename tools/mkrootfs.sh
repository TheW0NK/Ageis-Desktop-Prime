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
# The Aegis layout:
#   osystem/   the operating system: core services, devices, boot files (the
#              EFI partition), resources (fonts, pictures, app data), logs,
#              data, backups, temporary files, other disks (volumes)
#   sysapps/   system apps and commands, and the app registry
#   userApps/  apps installed from .aip packages (commands in userApps/commands)
#   userfiles/ everyone's files: userfiles/<name>/home and /system
#   serve/     content served to other computers
#   msc/       settings and account files
mkdir -p "$root"/osystem/core "$root"/osystem/devices "$root"/osystem/boot "$root"/osystem/temp \
         "$root"/osystem/resources "$root"/osystem/logs "$root"/osystem/data "$root"/osystem/backups \
         "$root"/osystem/volumes "$root"/sysapps/registry "$root"/userApps/commands "$root"/userfiles/superuser \
         "$root"/serve "$root"/msc
# Each user has /userfiles/<name>/home (their files) and /userfiles/<name>/system
# (settings, encrypted credentials, app data).
ud="$root"/userfiles/"$user"
[ "$live" = 1 ] && ud="$root"/osystem/temp/no-user
mkdir -p "$ud"/home/Desktop "$ud"/home/Documents "$ud"/home/Downloads "$ud"/home/Images "$ud"/home/Music \
         "$ud"/system/settings "$ud"/system/credentials "$ud"/system/appdata
echo "$user" > "$ud"/system/settings/name
echo light > "$ud"/system/settings/theme
echo en > "$ud"/system/settings/language
echo default > "$ud"/system/settings/background
# Sample pictures, also offered as backgrounds.
"$here"/mksamples.py "$root"/osystem/resources/backgrounds
cp "$root"/osystem/resources/backgrounds/*.png "$ud"/home/Images/
# Sample music: one tune made here, and Ogg and MP3 versions of others.
"$here"/mkmusic.py "$ud/home/Music/Aegis Theme.wav" theme
cp "$here"/samples/*.ogg "$here"/samples/*.mp3 "$ud"/home/Music/
# A sample app package to try the package installer with.
mkdir -p "$root"/osystem/resources/samples "$root"/osystem/data/aip
"$here"/mkaip.py "$here"/samples/aip/dice "$root"/osystem/resources/samples/Dice.aip >/dev/null
cp "$root"/osystem/resources/samples/Dice.aip "$ud"/home/Downloads/
cp "$endpoint"/sbin/* "$root"/osystem/core/
cp "$endpoint"/bin/* "$root"/sysapps/
cp -r "$here"/../endpoint/rootfs/. "$root"/
# Graphical programs: registry entries and their data files.
for dir in "$here"/../endpoint/apps/*/ "$here"/../endpoint/system/*/; do
    name=$(basename "$dir")
    for app in "$dir"*.app; do
        [ -e "$app" ] && cp "$app" "$root"/sysapps/registry/
    done
    if [ -d "$dir"res ]; then
        mkdir -p "$root"/osystem/resources/"$name"
        cp -r "$dir"res/. "$root"/osystem/resources/"$name"/
    fi
done
mkdir -p "$root"/osystem/resources/certificates "$root"/osystem/resources/fonts
cp "$here"/../third_party/fonts/*.ttf "$root"/osystem/resources/fonts/
cp "$here"/../third_party/ca-certificates.pem "$root"/osystem/resources/certificates/ca-bundle.pem

cat > "$root"/msc/crontab <<CRONTAB
# System jobs: schedule, account, command. Edit with the Cron Jobs app.
#   minute hour day month weekday  user  command
CRONTAB
if [ "$live" = 1 ]; then
    rm -rf "$root"/osystem/temp/no-user
    ud="$root"/userfiles
    echo "This is the live system on the Aegis install media." > "$root"/msc/live
    mkdir -p "$root"/osystem/installer
    cp -r "$esp" "$root"/osystem/installer/esp
    printf 'root:x:0:0:root:/userfiles/superuser:/sysapps/terminal\n' > "$root"/msc/passwd
    printf 'root:x:0:root\nadm:x:4:\nsudo:x:27:\nvideo:x:44:\naudio:x:63:\ninput:x:50:\n' > "$root"/msc/group
    printf 'root:!:\n' > "$root"/msc/shadow
else
cat > "$root"/msc/passwd <<PASSWD
root:x:0:0:root:/userfiles/superuser:/sysapps/terminal
$user:x:1000:1000:$user:/userfiles/$user/home:/sysapps/terminal
PASSWD
cat > "$root"/msc/group <<GROUP
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
} > "$root"/msc/shadow
fi

fakeroot sh -c "
    chown -R 0:0 '$root'
    [ '$live' = 1 ] || chown -R 1000:1000 '$ud'
    chmod 0755 '$root' '$root'/osystem '$root'/osystem/core '$root'/osystem/resources '$root'/sysapps \
          '$root'/msc '$root'/userfiles '$root'/userApps '$root'/userApps/commands '$root'/serve
    if [ '$live' != 1 ]; then
        chmod 0711 '$ud'
        chmod 0700 '$ud'/home '$ud'/system '$ud'/system/settings '$ud'/system/credentials '$ud'/system/appdata
        chmod 0600 '$ud'/system/settings/*
    else
        chmod 0755 '$ud'
    fi
    chmod 0700 '$root'/userfiles/superuser '$root'/osystem/backups
    chmod 1777 '$root'/osystem/temp
    chmod 0755 '$root'/sysapps/* '$root'/osystem/core/*
    chmod 0644 '$root'/sysapps/registry/* '$root'/msc/passwd '$root'/msc/group '$root'/msc/motd '$root'/msc/hostname \\
          '$root'/msc/hosts '$root'/osystem/resources/certificates/ca-bundle.pem '$root'/msc/crontab
    chmod 0600 '$root'/msc/shadow
    chmod 0755 '$root'/osystem/logs '$root'/osystem/data '$root'/osystem/data/aip
    if [ '$live' = 1 ]; then
        '$here'/mkiso.sh '$out' '$esp' '$root'
    else
        '$here'/mkimage.sh '$out' '$esp' '$root' $size
    fi
"
