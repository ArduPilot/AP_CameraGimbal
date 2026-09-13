#!/bin/sh
# Load the installed platform drivers, then launch AP CameraGimbal.
insmod /config/modules/4.9.84/usb-common.ko
insmod /config/modules/4.9.84/usbcore.ko
insmod /config/modules/4.9.84/ehci-hcd.ko
insmod /config/modules/4.9.84/scsi_mod.ko
insmod /config/modules/4.9.84/usb-storage.ko
insmod /config/modules/4.9.84/cifs.ko
insmod /config/modules/4.9.84/nls_utf8.ko
insmod /config/modules/4.9.84/grace.ko
insmod /config/modules/4.9.84/sunrpc.ko
insmod /config/modules/4.9.84/lockd.ko
insmod /config/modules/4.9.84/nfs.ko
insmod /config/modules/4.9.84/nfsv2.ko
insmod /config/modules/4.9.84/mmc_core.ko
insmod /config/modules/4.9.84/mmc_block.ko
insmod /config/modules/4.9.84/kdrv_sdmmc.ko
insmod /config/modules/4.9.84/fat.ko
insmod /config/modules/4.9.84/msdos.ko
insmod /config/modules/4.9.84/vfat.ko
insmod /config/modules/4.9.84/exfat.ko
insmod /config/modules/4.9.84/ntfs.ko
insmod /config/modules/4.9.84/sd_mod.ko
insmod /config/modules/4.9.84/ms_notify.ko
insmod /config/modules/4.9.84/media.ko
insmod /config/modules/4.9.84/videodev.ko
insmod /config/modules/4.9.84/v4l2-common.ko
insmod /config/modules/4.9.84/usb-common.ko
insmod /config/modules/4.9.84/videobuf2-core.ko
insmod /config/modules/4.9.84/videobuf2-v4l2.ko
insmod /config/modules/4.9.84/videobuf2-memops.ko
insmod /config/modules/4.9.84/videobuf2-vmalloc.ko
insmod /config/modules/4.9.84/udc-core.ko
insmod /config/modules/4.9.84/libcomposite.ko
insmod /config/modules/4.9.84/usb_f_uvc.ko
insmod /config/modules/4.9.84/udc-msb250x.ko
insmod /config/modules/4.9.84/g_webcam.ko


#kernel_mod_list
insmod /config/modules/4.9.84/mhal.ko
#misc_mod_list
insmod /config/modules/4.9.84/mi_common.ko
insmod /config/modules/4.9.84/mi_sys.ko cmdQBufSize=256 logBufSize=256
insmod /config/modules/4.9.84/mi_sensor.ko
insmod /config/modules/4.9.84/mi_ao.ko
insmod /config/modules/4.9.84/mi_rgn.ko
insmod /config/modules/4.9.84/mi_ai.ko
insmod /config/modules/4.9.84/mi_vpe.ko
insmod /config/modules/4.9.84/mi_shadow.ko
insmod /config/modules/4.9.84/mi_gyro.ko
insmod /config/modules/4.9.84/mi_vif.ko
insmod /config/modules/4.9.84/mi_venc.ko
insmod /config/modules/4.9.84/mi_divp.ko
insmod /config/modules/4.9.84/mi_panel.ko
insmod /config/modules/4.9.84/mi_disp.ko
#mi module
major=`cat /proc/devices | busybox awk "\\$2==\""mi_poll"\" {print \\$1}"`
busybox mknod /dev/mi_poll c $major 0

#misc_mod_list_late
insmod /customer/gc4663_MIPI.ko chmap=1
mkdir -p /customer/sd
mount -t vfat /dev/mmcblk0p1 /customer/sd
rm /customer/sd/ZR10_UpgradeSD.bin -f
rm /customer/sd/upgrade_script.txt -f
umount /customer/sd
mount -t exfat /dev/mmcblk0p1 /customer/sd
rm /customer/sd/ZR10_UpgradeSD.bin -f
rm /customer/sd/upgrade_script.txt -f
umount /customer/sd
mdev -s
rm /tmp/mac_addr/*
/home/mac.sh
chmod a+x /customer/sycamera
/customer/sycamera &
