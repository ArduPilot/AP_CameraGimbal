#!/bin/sh
# AP-only startup, adapted from MT11's app_init/app_selection layout.
# Platform module setup is performed by /customer/demo.sh before this script.
set -eu
app_dir=/customer/camera-app
mkdir -p /tmp/apcg-zt30
chmod 0700 /tmp/apcg-zt30
export LD_LIBRARY_PATH="/config/lib:/customer/ssh/lib:/lib:/usr/lib"
export CAMERA_APP_CONFIG="$app_dir/camera.ini"
export CAMERA_APP_ZT30_SDK="$app_dir/encoder-sdk.so"
export CAMERA_APP_READY_PATH=/tmp/apcg-zt30/camera-app.ready
export CAMERA_APP_RECORD_ROOT=/mnt/DCIM
export CAMERA_APP_CAPTURE_ROOT=/mnt/DCIM/capture
export CAMERA_APP_LOG_ROOT=/tmp/apcg-zt30
export TMP=/tmp/apcg-zt30
# The vendor boot scripts leave loopback unconfigured; web video uses it.
ifconfig lo 127.0.0.1 up || echo "Loopback setup failed; web video unavailable" >&2

# Existing settings and credentials survive application updates.
if [ ! -e "$CAMERA_APP_CONFIG" ]; then
    cp "$app_dir/camera.ini.default" "$CAMERA_APP_CONFIG"
fi
if [ ! -e "$app_dir/web.pass" ]; then
    umask 077
    cp "$app_dir/web.pass.default" "$app_dir/web.pass"
fi
# Recovery services start independently of camera hardware initialization.
"$app_dir/zt30-web" -p 80 >/tmp/apcg-zt30/zt30-web.log 2>&1 &
"$app_dir/start-dropbear.sh" >/tmp/apcg-zt30/dropbear.log 2>&1 &

mkdir -p /mnt
sd_mounted=0
# The platform may have mounted the SD card before AP startup.
while read -r device mountpoint filesystem options rest; do
    case "$device:$mountpoint:$filesystem:$options" in
        /dev/mmcblk0*:/mnt:exfat:rw*|/dev/mmcblk0*:/mnt:vfat:rw*) sd_mounted=1;;
    esac
done < /proc/mounts
for sd_device in /dev/mmcblk0p1 /dev/mmcblk0; do
    [ "$sd_mounted" = 0 ] || break
    [ -b "$sd_device" ] || continue
    if mount -t exfat -o rw,noatime,umask=077 "$sd_device" /mnt 2>/dev/null ||
       mount -t vfat -o rw,noatime,umask=077 "$sd_device" /mnt; then
        sd_mounted=1
        break
    fi
done
if [ "$sd_mounted" = 1 ]; then
    # Prevent this package being reapplied on the next boot.
    rm -f /mnt/SIYI_ZT30_UpgradeSD.bin /mnt/upgrade_script.txt
fi

if pidof siyi_app_359 >/dev/null 2>&1; then
    echo 'Vendor app owns media; refusing concurrent AP startup' >&2
    exit 1
fi
if pidof camera-app >/dev/null 2>&1; then
    echo 'camera-app is already running' >&2
    exit 0
fi
# Reuse the common web logger, which bounds and rotates app logs on tmpfs.
"$app_dir/app-supervisor.sh" &
