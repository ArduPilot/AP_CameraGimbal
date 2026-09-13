#!/bin/sh
set -eu

destination=${1:-build/deps}
mpp_root=$destination/ss928-mpp
minimp4_root=$destination/minimp4
mpp_url=https://gitee.com/hieulerpi/SS928V100_SDK_V2.0.2.2_MPP_Sample.git
mpp_commit=eef3b001cbf3d55f44392878bb7f4817cdb45e6c
minimp4_url=https://raw.githubusercontent.com/lieff/minimp4/5a212a18dba7dca09543bbc7d65619274fd2931a/minimp4.h
minimp4_sha256=1fc8d29b8c29dbeead9710bd9e95f6aab4d2fec48a55c2cc54a59f0338190bf2

for command in curl git mkdir mv sha256sum; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Missing dependency tool: $command" >&2
        exit 1
    }
done

mkdir -p "$destination"
if [ -d "$mpp_root/.git" ]; then
    actual=$(git -C "$mpp_root" rev-parse HEAD)
    [ "$actual" = "$mpp_commit" ] || {
        echo "Unexpected SS928 MPP revision in $mpp_root" >&2
        echo "  expected $mpp_commit" >&2
        echo "  found    $actual" >&2
        exit 1
    }
elif [ -e "$mpp_root" ]; then
    echo "Refusing non-Git SS928 MPP path: $mpp_root" >&2
    exit 1
else
    git clone --filter=blob:none --no-checkout "$mpp_url" "$mpp_root"
    git -C "$mpp_root" checkout --detach "$mpp_commit"
fi

mkdir -p "$minimp4_root"
if [ -f "$minimp4_root/minimp4.h" ]; then
    printf '%s  %s\n' "$minimp4_sha256" "$minimp4_root/minimp4.h" |
        sha256sum -c - >/dev/null
else
    temporary=$minimp4_root/minimp4.h.tmp.$$
    trap 'rm -f "$temporary"' EXIT HUP INT TERM
    curl -L --fail --silent --show-error "$minimp4_url" -o "$temporary"
    printf '%s  %s\n' "$minimp4_sha256" "$temporary" |
        sha256sum -c - >/dev/null
    mv "$temporary" "$minimp4_root/minimp4.h"
    trap - EXIT HUP INT TERM
fi

echo "Dependencies ready in $destination"
