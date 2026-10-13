#!/bin/sh
# Independent AP web recovery; camera pipeline waits never block this service.
set -eu
app_dir=${APCG_ZT30_APP_DIR:-/customer/camera-app}
run_dir=${APCG_ZT30_RUN_DIR:-/tmp/apcg-zt30}
mkdir -p "$run_dir"
exec 9>"$run_dir/web-supervisor.lock"
flock -n 9 || exit 0
trap 'exit 0' HUP INT TERM
payload_failed=0
web_retry_after=0
web_listener_running()
{
    for pid in $(pidof zt30-web 2>/dev/null || true); do
        args=$(tr '\000' '\n' 2>/dev/null < "/proc/$pid/cmdline") || continue
        launch=$(printf '%s\n' "$args" | sed -n '1p')
        case "$launch" in "$app_dir/zt30-web"|"$app_dir"/slots/*/zt30-web) ;; *) continue;; esac
        [ "$(printf '%s\n' "$args" | sed -n '2p')" = -p ] || continue
        [ "$(printf '%s\n' "$args" | sed -n '3p')" = 80 ] && return 0
    done
    return 1
}
while :; do
    if web_listener_running; then
        sleep 3
        continue
    fi
    read -r uptime rest < /proc/uptime
    now=${uptime%%.*}
    if [ "$now" -lt "$web_retry_after" ]; then
        sleep 1
        continue
    fi
    if ! selection=$("$app_dir/zt30-slot" --root "$app_dir" resolve 2>&1); then
        if [ "$payload_failed" = 0 ]; then
            echo "No valid AP web payload; recovery SSH remains available; retrying: $selection" >&2
        fi
        payload_failed=1
        sleep 5
        continue
    fi
    payload_failed=0
    slot=$selection
    export CAMERA_APP_WEBROOT="$slot/webroot"
    web_retry_after=$((now + 10))
    "$slot/zt30-web" -p 80 9>&- >>"$run_dir/zt30-web.log" 2>&1 &
    sleep 1
done
