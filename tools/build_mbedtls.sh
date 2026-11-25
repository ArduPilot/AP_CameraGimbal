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

for command in cmake make python3 perl; do
    command -v "$command" >/dev/null 2>&1 || { echo "Missing tool: $command" >&2; exit 1; }
done
python3 -c 'import jinja2, jsonschema' 2>/dev/null || {
    echo "Missing Python modules: pip install jinja2 jsonschema" >&2; exit 1; }
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
    -DINSTALL_MBEDTLS_HEADERS=ON -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
    -DMBEDTLS_CONFIG_FILE="$config/mbedtls_config.h" \
    -DTF_PSA_CRYPTO_CONFIG_FILE="$config/crypto_config.h" >"$output/cmake.log" 2>&1 ||
    { tail -20 "$output/cmake.log" >&2; exit 1; }
cmake --build "$build" -j"$(nproc)" >>"$output/cmake.log" 2>&1 || { tail -30 "$output/cmake.log" >&2; exit 1; }

rm -rf "$output/lib" "$output/include"
cmake --install "$build" --prefix "$output" >>"$output/cmake.log" 2>&1 || { tail -20 "$output/cmake.log" >&2; exit 1; }
rm -rf "$build"
echo "$stamp" >"$output/stamp"
