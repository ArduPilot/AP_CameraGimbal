#!/bin/sh
set -eu

usage()
{
    echo "Usage: $0 OUTPUT_ROOT [CROSS_COMPILE]" >&2
    exit 2
}

[ "$#" -ge 1 ] && [ "$#" -le 2 ] || usage

output_root=$1
mkdir -p "$output_root"
output_root=$(cd "$output_root" && pwd)
cross_compile=${2:-aarch64-linux-gnu-}
downloads=$output_root/downloads
sources=$output_root/sources
bin_dir=$output_root/bin

rsync_version=3.5.0
strace_version=7.1
musl_version=1.2.5
libpcap_version=1.10.6
tcpdump_version=4.99.6
libxcrypt_version=4.5.2
dropbear_version=2026.94
ltrace_version=0.8.1
elfutils_version=0.196
zlib_version=1.3.2

rsync_archive=rsync-$rsync_version.tar.gz
strace_archive=strace-$strace_version.tar.xz
musl_archive=musl-$musl_version.tar.gz
libpcap_archive=libpcap-$libpcap_version.tar.xz
tcpdump_archive=tcpdump-$tcpdump_version.tar.xz
libxcrypt_archive=libxcrypt-$libxcrypt_version.tar.xz
dropbear_archive=dropbear-$dropbear_version.tar.bz2
ltrace_archive=ltrace-$ltrace_version.tar.gz
elfutils_archive=elfutils-$elfutils_version.tar.bz2
zlib_archive=zlib-$zlib_version.tar.xz

rsync_url=https://download.samba.org/pub/rsync/src/$rsync_archive
strace_url=https://github.com/strace/strace/releases/download/v$strace_version/$strace_archive
musl_url=https://musl.libc.org/releases/$musl_archive
libpcap_url=https://www.tcpdump.org/release/$libpcap_archive
tcpdump_url=https://www.tcpdump.org/release/$tcpdump_archive
libxcrypt_url=https://github.com/besser82/libxcrypt/releases/download/v$libxcrypt_version/$libxcrypt_archive
dropbear_url=https://matt.ucc.asn.au/dropbear/releases/$dropbear_archive
ltrace_url=https://gitlab.com/cespedes/ltrace/-/archive/$ltrace_version/$ltrace_archive
elfutils_url=https://sourceware.org/elfutils/ftp/$elfutils_version/$elfutils_archive
zlib_url=https://zlib.net/$zlib_archive

rsync_sha256=c7ffd1ef653e99540f661e47cb00b7f9cad1ee6b972399b16f93d672656e0d33
strace_sha256=81743ecf2a5b44186b2f5038afdc8beda7e5c70aed15b4fbfbcc6e9ece24490f
musl_sha256=a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4
libpcap_sha256=ec97d1206bdd19cb6bdd043eaa9f0037aa732262ec68e070fd7c7b5f834d5dfc
tcpdump_sha256=40a8cefd45f0d2a06827e6658efb830d484868c449ad80f7efb33516af44f3da
libxcrypt_sha256=71513a31c01a428bccd5367a32fd95f115d6dac50fb5b60c779d5c7942aec071
dropbear_sha256=e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81
ltrace_sha256=11c85a1353fcf2b5438b19d0ccc2d376c96656ce6f11cf9537e3a92b84392c58
elfutils_sha256=fd5cc6b77ad6773cac93cb3f415f9318ac3b3455eecf801f6b4a742c4f6c7209
zlib_sha256=d7a0654783a4da529d1bb793b7ad9c3318020af77667bcae35f95d0e42a792f3

for command in autoconf automake autoreconf bison chmod cp curl dirname flex \
               gawk grep libtoolize make mkdir mv perl pkg-config rm sed \
               sha256sum tar touch; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Missing MT11 tools build dependency: $command" >&2
        exit 1
    }
done
for command in ar gcc g++ ranlib readelf strip; do
    command -v "${cross_compile}${command}" >/dev/null 2>&1 || {
        echo "Missing cross tool: ${cross_compile}${command}" >&2
        exit 1
    }
done

mkdir -p "$downloads" "$sources" "$bin_dir"

fetch()
{
    url=$1
    archive=$2
    expected=$3
    destination=$downloads/$archive

    if [ ! -f "$destination" ]; then
        temporary=$destination.tmp.$$
        trap 'rm -f "$temporary"' EXIT HUP INT TERM
        curl -L --fail --silent --show-error "$url" -o "$temporary"
        printf '%s  %s\n' "$expected" "$temporary" |
            sha256sum -c - >/dev/null
        mv "$temporary" "$destination"
        trap - EXIT HUP INT TERM
    fi
    printf '%s  %s\n' "$expected" "$destination" |
        sha256sum -c - >/dev/null
}

extract()
{
    archive=$1
    directory=$2

    if [ ! -d "$sources/$directory" ]; then
        tar -xf "$downloads/$archive" -C "$sources"
    fi
    [ -d "$sources/$directory" ] || {
        echo "$archive did not extract $directory" >&2
        exit 1
    }
}

fetch "$rsync_url" "$rsync_archive" "$rsync_sha256"
fetch "$strace_url" "$strace_archive" "$strace_sha256"
fetch "$musl_url" "$musl_archive" "$musl_sha256"
fetch "$libpcap_url" "$libpcap_archive" "$libpcap_sha256"
fetch "$tcpdump_url" "$tcpdump_archive" "$tcpdump_sha256"
fetch "$libxcrypt_url" "$libxcrypt_archive" "$libxcrypt_sha256"
fetch "$dropbear_url" "$dropbear_archive" "$dropbear_sha256"
fetch "$ltrace_url" "$ltrace_archive" "$ltrace_sha256"
fetch "$elfutils_url" "$elfutils_archive" "$elfutils_sha256"
fetch "$zlib_url" "$zlib_archive" "$zlib_sha256"

extract "$rsync_archive" "rsync-$rsync_version"
extract "$strace_archive" "strace-$strace_version"
extract "$musl_archive" "musl-$musl_version"
extract "$libpcap_archive" "libpcap-$libpcap_version"
extract "$tcpdump_archive" "tcpdump-$tcpdump_version"
extract "$libxcrypt_archive" "libxcrypt-$libxcrypt_version"
extract "$dropbear_archive" "dropbear-$dropbear_version"
extract "$ltrace_archive" "ltrace-$ltrace_version"
extract "$elfutils_archive" "elfutils-$elfutils_version"
extract "$zlib_archive" "zlib-$zlib_version"

rsync_source=$sources/rsync-$rsync_version
strace_source=$sources/strace-$strace_version
musl_source=$sources/musl-$musl_version
libpcap_source=$sources/libpcap-$libpcap_version
tcpdump_source=$sources/tcpdump-$tcpdump_version
libxcrypt_source=$sources/libxcrypt-$libxcrypt_version
dropbear_source=$sources/dropbear-$dropbear_version
ltrace_source=$sources/ltrace-$ltrace_version
elfutils_source=$sources/elfutils-$elfutils_version
zlib_source=$sources/zlib-$zlib_version
musl_prefix=$output_root/musl
libxcrypt_prefix=$output_root/libxcrypt
zlib_prefix=$output_root/zlib
elfutils_prefix=$output_root/elfutils
empty_pkgconfig=$output_root/empty-pkgconfig

(
    cd "$musl_source"
    ./configure --prefix="$musl_prefix" --target=aarch64-linux-musl \
        CROSS_COMPILE="$cross_compile" --disable-shared
    make
    make install
)

# musl intentionally provides libc headers only. Add the target compiler's
# Linux UAPI headers for strace and libpcap without importing glibc headers.
cross_gcc=${cross_compile}gcc
cross_libc=$("$cross_gcc" -print-file-name=libc.a)
kernel_headers=$(dirname "$cross_libc")/../include
for directory in asm asm-generic linux; do
    [ -d "$kernel_headers/$directory" ] || {
        echo "Cannot find target kernel headers: $kernel_headers/$directory" >&2
        exit 1
    }
    cp -R "$kernel_headers/$directory" "$musl_prefix/include/"
done
musl_cc=$musl_prefix/bin/musl-gcc

# musl does not provide crypt(3). Link Dropbear with a static libxcrypt so
# password authentication can validate the SHA-512 root verifier in shadow.
(
    cd "$libxcrypt_source"
    AR=${cross_compile}ar CC="$musl_cc" RANLIB=${cross_compile}ranlib \
        CFLAGS=-O2 \
        ./configure --host=aarch64-linux-musl \
        --prefix="$libxcrypt_prefix" --disable-shared --enable-static \
        --disable-obsolete-api --disable-werror
    make
    make install
)

(
    cd "$rsync_source"
    AR=${cross_compile}ar CC="$musl_cc" RANLIB=${cross_compile}ranlib \
        CFLAGS='-O2 -static' LDFLAGS=-static \
        ./configure --host=aarch64-linux-musl --disable-md2man \
        --disable-openssl --disable-xxhash --disable-zstd --disable-lz4 \
        --disable-iconv --disable-acl-support --disable-xattr-support
    make rsync
)

(
    cd "$strace_source"
    AR=${cross_compile}ar CC="$musl_cc" RANLIB=${cross_compile}ranlib \
        STRIP=${cross_compile}strip CFLAGS=-O2 LDFLAGS=-static \
        ./configure --host=aarch64-linux-musl --enable-mpers=no \
        --disable-gcc-Werror
    make
)

(
    cd "$libpcap_source"
    AR=${cross_compile}ar CC="$musl_cc" RANLIB=${cross_compile}ranlib \
        PKG_CONFIG=false CFLAGS=-O2 \
        ./configure --host=aarch64-linux-musl --without-libnl \
        --disable-usb --disable-bluetooth --disable-dbus
    make libpcap.a
)

(
    cd "$tcpdump_source"
    AR=${cross_compile}ar CC="$musl_cc" RANLIB=${cross_compile}ranlib \
        PKG_CONFIG=false CFLAGS=-O2 LDFLAGS=-static \
        ./configure --host=aarch64-linux-musl --without-crypto
    make tcpdump
)

(
    cd "$dropbear_source"
    AR=${cross_compile}ar CC="$musl_cc" RANLIB=${cross_compile}ranlib \
        CPPFLAGS="-I$libxcrypt_prefix/include" \
        LDFLAGS="-L$libxcrypt_prefix/lib" LIBS=-lcrypt CFLAGS=-O2 \
        ./configure --host=aarch64-linux-musl --enable-static \
        --disable-zlib --disable-lastlog --disable-utmp --disable-utmpx \
        --disable-wtmp --disable-wtmpx --enable-bundled-libtom
    make PROGRAMS='dropbear dropbearkey'
)

# ltrace needs libelf, but not elfutils' larger DWARF stack. Build only the
# static libelf/libeu pieces and their zlib dependency with the target's glibc
# toolchain. The resulting ltrace was tested on the MT11's Linux 4.19 rootfs.
(
    cd "$zlib_source"
    CC=${cross_compile}gcc AR=${cross_compile}ar \
        RANLIB=${cross_compile}ranlib \
        ./configure --prefix="$zlib_prefix" --static
    make
    make install
)

mkdir -p "$empty_pkgconfig" "$elfutils_prefix/include" \
    "$elfutils_prefix/lib"
(
    cd "$elfutils_source"
    PKG_CONFIG=pkg-config PKG_CONFIG_LIBDIR="$empty_pkgconfig" \
        PKG_CONFIG_PATH='' CC=${cross_compile}gcc CXX=${cross_compile}g++ \
        AR=${cross_compile}ar RANLIB=${cross_compile}ranlib \
        CFLAGS='-O2 -std=gnu17' CPPFLAGS="-I$zlib_prefix/include" \
        LDFLAGS="-L$zlib_prefix/lib" \
        ./configure --build="$(./config/config.guess)" \
        --host=aarch64-linux-gnu \
        --prefix="$elfutils_prefix" --disable-nls \
        --disable-libdebuginfod --disable-debuginfod --with-zlib \
        --without-bzlib --without-lzma --without-zstd
    make -C lib libeu.a
    make -C libelf libelf.a
)
cp "$elfutils_source/libelf/libelf.h" "$elfutils_prefix/include/"
cp "$elfutils_source/libelf/gelf.h" "$elfutils_prefix/include/"
cp "$elfutils_source/libelf/nlist.h" "$elfutils_prefix/include/"
cp "$elfutils_source/libelf/libelf.a" "$elfutils_prefix/lib/"
cp "$elfutils_source/lib/libeu.a" "$elfutils_prefix/lib/"

(
    cd "$ltrace_source"
    autoreconf -fi
    CC=${cross_compile}gcc AR=${cross_compile}ar \
        RANLIB=${cross_compile}ranlib CFLAGS='-O2 -std=gnu17' \
        CPPFLAGS="-I$zlib_prefix/include" \
        LDFLAGS="-L$elfutils_prefix/lib -L$zlib_prefix/lib" \
        LIBS='-leu -lz' \
        ./configure --build="$(./config.guess)" --host=aarch64-linux-gnu \
        --with-libelf="$elfutils_prefix" --without-libunwind \
        --without-elfutils
    # GNU libtool consumes -all-static and emits the required compiler
    # -static option; a plain -static is dropped from its final link command.
    make LDFLAGS="-L$elfutils_prefix/lib -L$zlib_prefix/lib -all-static"
)

install_tool()
{
    source_file=$1
    name=$2
    temporary=$bin_dir/$name.tmp.$$

    cp "$source_file" "$temporary"
    "${cross_compile}strip" "$temporary"
    chmod 0755 "$temporary"
    "${cross_compile}readelf" -h "$temporary" |
        grep -q 'Machine:[[:space:]]*AArch64' || {
        echo "$name is not an AArch64 executable" >&2
        exit 1
    }
    if "${cross_compile}readelf" -d "$temporary" 2>/dev/null |
       grep -q '(NEEDED)'; then
        echo "$name has unexpected shared-library dependencies" >&2
        exit 1
    fi
    mv "$temporary" "$bin_dir/$name"
}

install_tool "$rsync_source/rsync" rsync
install_tool "$strace_source/src/strace" strace
install_tool "$tcpdump_source/tcpdump" tcpdump
install_tool "$dropbear_source/dropbear" dropbear
install_tool "$dropbear_source/dropbearkey" dropbearkey
install_tool "$ltrace_source/ltrace" ltrace

printf '%s\n' \
    "musl $musl_version" \
    "rsync $rsync_version" \
    "strace $strace_version" \
    "libpcap $libpcap_version" \
    "tcpdump $tcpdump_version" \
    "libxcrypt $libxcrypt_version" \
    "dropbear $dropbear_version" \
    "ltrace $ltrace_version" \
    "elfutils $elfutils_version" \
    "zlib $zlib_version" >"$output_root/versions"
touch "$output_root/.built"

echo "MT11 support tools ready in $bin_dir"
