#!/bin/sh
# Minimal, pinned FFmpeg libraries for radiometric codecs (no ffmpeg process).
set -eu
mode=${1:-host}
case "$mode" in host|aarch64) ;; *) echo 'usage: build_thermal_codecs.sh [host|aarch64]' >&2; exit 2;; esac
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
root=$repo/build/deps/thermal-codecs
version=8.0.1
sha=05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41
mkdir -p "$root/downloads"
# Make host and cross builds safe to invoke together.
exec 9>"$root/.lock"
python3 -c 'import fcntl; fcntl.flock(9, fcntl.LOCK_EX)'
archive=$root/downloads/ffmpeg-$version.tar.xz
if [ ! -f "$archive" ]; then
    curl --fail --location --silent --show-error "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz" -o "$archive.tmp"
    printf '%s  %s\n' "$sha" "$archive.tmp" | sha256sum -c -
    mv "$archive.tmp" "$archive"
fi
printf '%s  %s\n' "$sha" "$archive" | sha256sum -c -
if [ ! -d "$root/ffmpeg-$version" ]; then
    tar -xf "$archive" -C "$root"
fi
prefix=$root/$mode
build=$root/build-$mode
cross=${THERMAL_CROSS_COMPILE:-aarch64-linux-gnu-}
recipe=$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$repo/tools/build_thermal_codecs.sh")
configuration="$version $mode $cross $recipe"
if [ -f "$prefix/configuration" ] && [ "$(cat "$prefix/configuration")" = "$configuration" ] && [ -f "$prefix/lib/libavcodec.a" ] && [ -f "$prefix/lib/libavutil.a" ]; then
    exit 0
fi
mkdir -p "$build"
cd "$build"
set -- --prefix="$prefix" --disable-everything --disable-autodetect \
    --disable-programs --disable-doc --disable-debug --disable-network \
    --disable-avdevice --disable-avformat --disable-avfilter --disable-swresample \
    --disable-swscale --disable-shared --enable-static --enable-pthreads \
    --enable-encoder=ffv1,jpegls --enable-decoder=ffv1,jpegls \
    --extra-cflags=-O2
if [ "$mode" = aarch64 ]; then
    set -- "$@" --enable-cross-compile --arch=aarch64 --target-os=linux --cross-prefix="$cross"
else
    set -- "$@" --disable-x86asm
fi
"$root/ffmpeg-$version/configure" "$@"
make -j"${THERMAL_BUILD_JOBS:-6}"
make install
printf '%s\n' "$configuration" > "$prefix/configuration"
cp "$root/ffmpeg-$version/COPYING.LGPLv2.1" "$prefix/"
