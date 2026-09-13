#!/bin/sh
# Small /customer/sycamera wrapper. Only the boot-time UUID query is supported.
APP=/mnt/AP_CameraGimbal/zr10
if [ "$#" -gt 0 ]; then
    if [ "$#" = 1 ] && [ "$1" = Uuid ]; then exec /customer/zr10-uuid; fi
    echo 'Unsupported camera utility argument' >&2
    exit 2
fi
# Locate the AP service on flash or on the card.
for _ in 1 2 3 4 5; do
    grep -qs '^/dev/mmcblk0p1 /mnt ' /proc/mounts && break
    [ ! -b /dev/mmcblk0p1 ] || mount -t vfat -o rw,noatime,fmask=0077,dmask=0077 /dev/mmcblk0p1 /mnt 2>/dev/null
    sleep 1
done
if [ -x /customer/camera-app/service.sh ]; then
    exec /customer/camera-app/service.sh
fi
if grep -qs '^/dev/mmcblk0p1 /mnt ' /proc/mounts &&
   [ -x "$APP/service.sh" ]; then
    exec "$APP/service.sh"
fi
echo 'AP CameraGimbal service is missing; camera application not started' >&2
exit 1
