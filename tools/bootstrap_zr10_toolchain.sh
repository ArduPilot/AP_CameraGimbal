#!/bin/sh
# Install local build inputs; no system installation and no camera access.
set -eu
cd "$(dirname "$0")/.."
deps="$PWD/build/zr10-deps"
name=armv7-eabihf--uclibc--stable-2018.11-1
url="https://toolchains.bootlin.com/downloads/releases/toolchains/armv7-eabihf/tarballs/$name.tar.bz2"
mkdir -p "$deps"
if [ ! -f "$deps/toolchain.tar.bz2" ]; then
    curl -fL --retry 2 "$url" -o "$deps/toolchain.tar.bz2.part"
    mv "$deps/toolchain.tar.bz2.part" "$deps/toolchain.tar.bz2"
fi
python3 - "$deps" "$PWD" <<'PY'
import hashlib
from pathlib import Path
import subprocess
import sys
root = Path(sys.argv[1])
repo = Path(sys.argv[2])
archive = root / "toolchain.tar.bz2"
expected = "a0300cf5765436607e50d010abbe88a71b2447c40cd9ccd1a733a6e43608f081"
if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
    raise SystemExit("toolchain SHA-256 mismatch")
target = root / "armv7-eabihf--uclibc--stable-2018.11-1"
if not target.exists():
    subprocess.run([sys.executable, str(repo / "tools/safe_tar.py"), str(archive), str(root)], check=True)
print(target)
PY
