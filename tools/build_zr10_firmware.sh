#!/bin/sh
# Customer-only flash image built from checked-in platform assets and AP code.
set -eu
[ "$#" = 3 ] || { echo "Usage: $0 PLATFORM_DIR APP_ARCHIVE OUTPUT" >&2; exit 2; }
platform=$1
archive=$2
output=$3
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
(cd "$platform" && sha256sum -c SHA256SUMS)
for command in mkfs.jffs2 python3; do
    command -v "$command" >/dev/null || { echo "Missing build tool: $command" >&2; exit 1; }
done
mkdir -p "$(dirname "$output")"
work=$(mktemp -d "$(dirname "$output")/zr10-firmware.XXXXXX")
trap 'rm -rf "$work"' EXIT
tree=$work/customer
mkdir -p "$tree/sd"
install -m 0644 "$platform/gc4663_MIPI.ko" "$tree/gc4663_MIPI.ko"
install -m 0644 "$platform/day_gc4663-2-aec-AWB.bin" "$tree/day_gc4663-2-aec-AWB.bin"
install -m 0644 "$platform/network_config.ini" "$tree/network_config.ini"
tar -xzf "$archive" -C "$work"
(cd "$work/zr10" && sha256sum -c SHA256SUMS)
mv "$work/zr10" "$tree/camera-app"
install -m 0755 "$tree/camera-app/boot.sh" "$tree/sycamera"
install -m 0755 "$tree/camera-app/zr10-uuid" "$tree/zr10-uuid"
install -m 0755 "$root/packaging/zr10/demo.sh" "$tree/demo.sh"
: >"$tree/.sys_upgrade_finish_flag"
mkfs.jffs2 -U -r "$tree" -o "$work/unpadded.jffs2" -e 0x10000 -l -q
used=$(wc -c <"$work/unpadded.jffs2")
# Leave at least two eraseblocks for JFFS2 GC and vendor state writes.
[ "$used" -le $((0x6b0000 - 2 * 0x10000)) ] || {
    echo "Customer image too full: $used bytes" >&2; exit 1;
}
mkfs.jffs2 -U -r "$tree" -o "$work/customer.jffs2" -e 0x10000 -l --pad=0x6b0000 -q
python3 "$root/tools/zr10_firmware.py" pack "$work/customer.jffs2" "$work/firmware.bin"
python3 "$root/tools/zr10_firmware.py" check "$work/firmware.bin"
mv "$work/firmware.bin" "$output"
sha256sum "$output" >"$output.sha256"
echo "Built $output; customer uses $((used / 1024)) of 6848 KiB (AP CameraGimbal startup)"
