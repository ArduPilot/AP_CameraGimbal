#!/bin/sh
# Historical filename retained for upgrades; AP CameraGimbal is the only app.
export LD_LIBRARY_PATH="/app/libs:${LD_LIBRARY_PATH:-}"
export TMP=/run
if pidof camera-app >/dev/null 2>&1; then
    echo 'camera-app is already running' >&2
    exit 0
fi
if pidof siyi_camera_app >/dev/null 2>&1; then
    echo 'Vendor camera is still running; refusing concurrent hardware ownership' >&2
    exit 1
fi
if [ ! -x /app/bin/camera-app ]; then
    echo 'camera-app is missing; web UI remains available for recovery' >&2
    exit 1
fi
if [ -x /app/bin/mt11-web ]; then
    /app/bin/camera-app 2>&1 | /app/bin/mt11-web --capture-app-log replacement &
else
    /app/bin/camera-app >>/run/camera_app.log 2>&1 &
fi
