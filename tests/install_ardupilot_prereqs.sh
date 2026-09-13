#!/usr/bin/env bash

set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
ardupilot=${1:-"$repo_root/build/ardupilot"}
installer="$ardupilot/Tools/environment_install/install-prereqs-ubuntu.sh"

if [[ ! -x "$installer" ]]; then
    echo "ArduPilot prerequisite installer not found at $installer" >&2
    exit 1
fi

# This test only builds native SITL. Avoid the GUI and embedded toolchains.
export DISABLE_MAVNATIVE=True
export DO_AP_STM_ENV=0
export SKIP_AP_EXT_ENV=1
export SKIP_AP_GIT_CHECK=1
export SKIP_AP_GRAPHIC_ENV=1
"$installer" -qy

# MT11 SITL generates deterministic H.264/JPEG fixtures during its build.
sudo apt-get install --assume-yes --quiet ffmpeg

# The release wheel can lag the message definitions used by the camera
# (for example GIMBAL_DEVICE_INFORMATION.cap_flags2). Generate the test
# bindings from the same pinned submodule as the camera's C bindings.
if [[ -f "$HOME/venv-ardupilot/bin/activate" ]]; then
    # shellcheck disable=SC1091
    source "$HOME/venv-ardupilot/bin/activate"
fi
MDEF="$repo_root/modules/mavlink/message_definitions" PYMAVLINK_FAST_INDEX=0 \
    python3 -m pip install --no-deps --force-reinstall "$repo_root/modules/mavlink/pymavlink"
