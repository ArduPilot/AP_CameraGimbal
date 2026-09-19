#!/bin/bash
# Run from the checkout root in a Cygwin build environment.
set -euo pipefail
python3 windows/prepare_xop.py build/deps/ss928-mpp/src/rtspserver/src build/windows-xop
mkdir -p build/windows/native
python3 tools/export_targets.py
for backend in $(python3 -c 'import json; print(" ".join(json.load(open("build/targets/targets.json"))))'); do
    make -C camera_app -j"${NUMBER_OF_PROCESSORS:-4}" sitl CAMERA_BACKEND="$backend" \
        SITL_TARGET="../build/windows/native/camera-app-$backend.exe" \
        SITL_OBJDIR="build/windows-$backend" \
        HOST_CXX='g++ -D_GNU_SOURCE' RTSP_ROOT=../build/windows-xop
    defines=-DWEB_PORTABLE_SITL
    case "$backend" in
        a8) ;;
        zr10)
            python3 tools/zr10_firmware.py header web/build/zr10_upgrade.h
            ;;
    esac
    make -C web sitl CAMERA_BACKEND="$backend" SITL_TARGET="../build/windows/native/web-$backend.exe" \
        SITL_ROOT=/portable/runtime SITL_BIN_ROOT=/portable/native \
        SITL_EXTRA_DEFINES="$defines"
done
cp /usr/bin/kill.exe build/windows/native/
# Include every Cygwin runtime dependency alongside the services. No installed
# Cygwin, compiler or shell is used by the finished package.
python3 windows/collect_cygwin.py build/windows/native
