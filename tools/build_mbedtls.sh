#!/bin/sh
# Builds the Mbed TLS submodule as static libraries for one target.
# Usage: build_mbedtls.sh <output dir> [cross-compiler prefix, e.g. arm-linux-]
# Result: <output>/lib/{libmbedtls,libmbedx509,libtfpsacrypto}.a and <output>/include
# (the output is also where the build's "stamp" and "cmake.log" go).
# The configuration is camera_app/mbedtls/{mbedtls_config,crypto_config}.h.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
output=$(mkdir -p "$1" && cd "$1" && pwd)
prefix=${2:-}
source=$root/modules/mbedtls
config=$root/camera_app/mbedtls

for command in cmake make perl; do
    command -v "$command" >/dev/null 2>&1 || { echo "Missing tool: $command" >&2; exit 1; }
done
# Some camera toolchains bring their own python3 (without these modules) to the front of PATH, so look for a
# Python that has what the generated sources need: MBEDTLS_PYTHON, the repository's build environment, then PATH.
python=
for candidate in "${MBEDTLS_PYTHON:-}" "$root/build/environment/bin/python3" python3 /usr/bin/python3; do
    [ -n "$candidate" ] && command -v "$candidate" >/dev/null 2>&1 || continue
    if "$candidate" -c 'import jinja2, jsonschema' 2>/dev/null; then python=$(command -v "$candidate"); break; fi
done
[ -n "$python" ] || { echo "Missing Python modules: pip install jinja2 jsonschema" >&2; exit 1; }
# Several builds can ask for the same output at once (make -j on all the simulators): one at a time, and the
# others find it finished. The lock is a directory holding the owner's process id, so one left by a killed
# build is noticed and taken over.
lock=$output/lock.d
waited=0
until mkdir "$lock" 2>/dev/null; do
    owner=$(cat "$lock/pid" 2>/dev/null || true)
    if [ -n "$owner" ] && ! kill -0 "$owner" 2>/dev/null; then rm -rf "$lock"; continue; fi
    waited=$((waited + 1))
    [ "$waited" -ne 60 ] || echo "Waiting for another Mbed TLS build (if none is running, delete $lock)" >&2
    sleep 1
done
echo $$ >"$lock/pid"
trap 'rm -rf "$lock"' EXIT
trap 'exit 1' INT TERM HUP
# The 4.x release tags do not include the generated sources: they are made
# with scripts from the nested "framework" submodules.
if [ ! -f "$source/framework/scripts/mbedtls_framework/__init__.py" ] ||
   [ ! -f "$source/tf-psa-crypto/CMakeLists.txt" ] ||
   [ ! -f "$source/tf-psa-crypto/framework/scripts/mbedtls_framework/__init__.py" ]; then
    (cd "$source" && git submodule update --init framework tf-psa-crypto &&
     cd tf-psa-crypto && git submodule update --init framework)
fi

# The revision is only part of the stamp; a copy of the tree without git metadata
# (rsynced to a device) uses the library's own version header instead.
revision=$(git -C "$source" rev-parse HEAD 2>/dev/null || cat "$source/tf-psa-crypto/include/tf-psa-crypto/version.h" 2>/dev/null | sha256sum | cut -d' ' -f1)
stamp="$prefix $(cat "$config/mbedtls_config.h" "$config/crypto_config.h" | sha256sum | cut -d' ' -f1) $revision"
if [ -f "$output/lib/libmbedtls.a" ] && [ "$(cat "$output/stamp" 2>/dev/null)" = "$stamp" ]; then
    exit 0
fi

compiler=cc
[ -n "$prefix" ] && compiler=${prefix}gcc
build=$output/build
rm -rf "$build"
cmake -S "$source" -B "$build" -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_C_COMPILER="$compiler" -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DCMAKE_C_FLAGS="-ffunction-sections -fdata-sections" \
    -DCMAKE_MAKE_PROGRAM="$(command -v make)" -DPython3_EXECUTABLE="$python" -DINSTALL_MBEDTLS_HEADERS=ON -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
    -DMBEDTLS_CONFIG_FILE="$config/mbedtls_config.h" \
    -DTF_PSA_CRYPTO_CONFIG_FILE="$config/crypto_config.h" >"$output/cmake.log" 2>&1 ||
    { tail -20 "$output/cmake.log" >&2; exit 1; }
cmake --build "$build" -j"$(nproc)" >>"$output/cmake.log" 2>&1 || { tail -30 "$output/cmake.log" >&2; exit 1; }

rm -rf "$output/lib" "$output/include"
cmake --install "$build" --prefix "$output" >>"$output/cmake.log" 2>&1 || { tail -20 "$output/cmake.log" >&2; exit 1; }
rm -rf "$build"
echo "$stamp" >"$output/stamp"
