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
python3 - "$deps" <<'PY'
import hashlib
from pathlib import Path
import sys
import tarfile
root = Path(sys.argv[1])
archive = root / "toolchain.tar.bz2"
expected = "a0300cf5765436607e50d010abbe88a71b2447c40cd9ccd1a733a6e43608f081"
if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
    raise SystemExit("toolchain SHA-256 mismatch")
target = root / "armv7-eabihf--uclibc--stable-2018.11-1"
if not target.exists():
    with tarfile.open(archive) as tar:
        if hasattr(tarfile, "data_filter"):
            tar.extractall(root, filter="data")
        else:
            members = []
            destination = root.resolve()
            for original in tar.getmembers():
                member = original.copy()
                member_path = Path(member.name)
                if member_path.is_absolute() or ".." in member_path.parts:
                    raise SystemExit(f"unsafe archive path: {member.name}")
                target_path = destination / member_path
                if not target_path.resolve().is_relative_to(destination):
                    raise SystemExit(f"unsafe archive path: {member.name}")
                if member.issym():
                    link_target = (target_path.parent / member.linkname).resolve()
                    if not link_target.is_relative_to(destination):
                        raise SystemExit(f"unsafe archive symlink: {member.name}")
                elif member.islnk():
                    link_path = Path(member.linkname)
                    if link_path.is_absolute() or ".." in link_path.parts:
                        raise SystemExit(f"unsafe archive hardlink: {member.name}")
                elif not (member.isdir() or member.isreg()):
                    raise SystemExit(f"unsupported archive member: {member.name}")
                member.mode &= 0o777
                members.append(member)
            tar.extractall(root, members=members)
print(target)
PY
