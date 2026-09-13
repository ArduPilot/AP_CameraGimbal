#!/usr/bin/env bash

set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
destination=${1:-"$repo_root/build/ardupilot"}
repository=${ARDUPILOT_REPOSITORY:-https://github.com/ArduPilot/ardupilot.git}
ref=${ARDUPILOT_REF:-master}

if [[ -e "$destination" ]]; then
    if [[ ! -d "$destination/.git" ]]; then
        echo "$destination exists but is not an ArduPilot Git checkout" >&2
        exit 1
    fi
    echo "Reusing ArduPilot checkout at $destination"
else
    mkdir -p "$(dirname "$destination")"
    git clone --depth 1 --branch "$ref" --recurse-submodules --shallow-submodules \
        "$repository" "$destination"
fi

git -C "$destination" submodule update --init --recursive --depth 1
printf 'ArduPilot revision: '
git -C "$destination" show -s --format='%H %D'
