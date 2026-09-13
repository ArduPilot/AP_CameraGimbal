#!/bin/sh
# Restore the original launch path. Keep SD configuration and recordings.
set -eu
if [ ! -f /customer/sycamera.vendor ]; then
    echo 'No saved vendor app: restore the original SIYI firmware from SD.' >&2
    exit 1
fi
if [ -r /tmp/zr10-service.pid ]; then
    pid=$(cat /tmp/zr10-service.pid)
    case "$pid" in ''|*[!0-9]*) exit 1;; esac
    # Verify the pid still belongs to this supervisor before signaling it.
    tr '\000' ' ' </proc/"$pid"/cmdline | grep -Eq '/(zr10|camera-app)/service.sh' || exit 1
    kill -TERM "$pid"
    for _ in $(seq 1 20); do
        [ -e /tmp/zr10-service.pid ] || break
        sleep 1
    done
    [ ! -e /tmp/zr10-service.pid ] || { echo 'Service has not stopped' >&2;exit 1; }
fi
if [ -f /customer/sycamera.vendor ]; then
    grep -q 'Small /customer/sycamera wrapper' /customer/sycamera || {
        echo 'Unknown active sycamera; refusing overwrite' >&2;exit 1;
    }
    mv /customer/sycamera.vendor /customer/sycamera
fi
sync
if ! pidof sycamera sycamera.vendor >/dev/null; then
    (cd / && /customer/sycamera </dev/null >/dev/console 2>&1 &)
fi
echo 'Vendor launch path restored; SD settings and media retained.'
