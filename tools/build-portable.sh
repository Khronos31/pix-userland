#!/bin/sh
# Build libusb without libudev, then link pix-ts against its static archive.
# Run under musl for a fully static Linux binary, or with an Android NDK for Bionic.
set -eu
mode=${1:-native}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
case "$mode" in native|musl|android) ;; *) echo 'Usage: build-portable.sh native|musl|android' >&2; exit 2;; esac
build=$(mktemp -d "${TMPDIR:-/tmp}/pix-build.XXXXXX")
echo "Build directory: $build" >&2
# Keep intermediates for reproducibility/debugging; no unbounded rm -rf.
usb_url=https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2
usb_sha=fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf
if [ -n "${LIBUSB_SOURCE_TARBALL:-}" ]; then
    cp "$LIBUSB_SOURCE_TARBALL" "$build/libusb.tar.bz2"
elif [ -f "$root/third_party/libusb-1.0.30.tar.bz2" ]; then
    cp "$root/third_party/libusb-1.0.30.tar.bz2" "$build/libusb.tar.bz2"
else
    curl -fL --retry 3 "$usb_url" -o "$build/libusb.tar.bz2"
fi
actual=$( (sha256sum "$build/libusb.tar.bz2" 2>/dev/null || shasum -a 256 "$build/libusb.tar.bz2") | awk '{print $1}')
[ "$actual" = "$usb_sha" ] || { echo 'libusb SHA-256 mismatch' >&2; exit 1; }
tar -xjf "$build/libusb.tar.bz2" -C "$build"
cc=${CC:-cc}
extra_link=
configure_host=
abi=${ANDROID_ABI:-aarch64}
if [ "$mode" = android ]; then
    ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
    [ -d "$ndk" ] || { echo 'Set ANDROID_NDK_HOME' >&2; exit 2; }
    case $(uname -s) in Linux) host=linux-x86_64;; Darwin) host=darwin-x86_64;; *) exit 2;; esac
    toolchain=$ndk/toolchains/llvm/prebuilt/$host
    api=${ANDROID_API:-24}
    case "$abi" in aarch64) target=aarch64-linux-android;triplet=$target;; x86_64) target=x86_64-linux-android;triplet=$target;; armv7a) target=armv7a-linux-androideabi;triplet=arm-linux-androideabi;; *) exit 2;; esac
    cc=$toolchain/bin/${target}${api}-clang
    [ -x "$cc" ] || { echo "Compiler missing: $cc" >&2; exit 2; }
    export AR="$toolchain/bin/llvm-ar" RANLIB="$toolchain/bin/llvm-ranlib"
    configure_host=--host=$triplet
    extra_link=-llog
elif [ "$mode" = musl ]; then
    "$cc" -E -dM -include features.h - < /dev/null > "$build/compiler-macros"
    if grep -Eq '__GLIBC__|__BIONIC__' "$build/compiler-macros"; then
        echo 'musl mode requires a musl compiler; use an Alpine build environment' >&2
        exit 2
    fi
    extra_link=-static
fi
(
    cd "$build/libusb-1.0.30"
    # No pkg-config dependency on the build host's libudev or libusb.
    CC="$cc" ./configure $configure_host --prefix="$build/prefix" --disable-udev --disable-shared --enable-static
    make -j "${JOBS:-4}"
    make install
)
if [ "$mode" = native ] && [ "$(uname -s)" = Darwin ]; then
    extra_link='-lobjc -Wl,-framework,IOKit -Wl,-framework,CoreFoundation -Wl,-framework,Security'
fi
mkdir "$build/app"
cp -R "$root/src" "$root/include" "$root/tests" "$root/Makefile" "$build/app/"
# Remove copied object/dependency files so no host objects can leak into cross builds.
(cd "$build/app" && make clean && make -j "${JOBS:-4}" CC="$cc" PKG_CONFIG=false USB_CFLAGS="-I$build/prefix/include/libusb-1.0" USB_LIBS="$build/prefix/lib/libusb-1.0.a $extra_link")
if [ "$mode" != android ]; then
    (cd "$build/app" && make test CC="$cc" PKG_CONFIG=false USB_CFLAGS="-I$build/prefix/include/libusb-1.0" USB_LIBS="$build/prefix/lib/libusb-1.0.a $extra_link" LDFLAGS="$extra_link")
fi
out=$root/build/$mode
[ "$mode" != android ] || out=$out-$abi
mkdir -p "$out"
cp "$build/app/pix-ts" "$out/"
cp "$build/libusb-1.0.30/COPYING" "$out/COPYING.libusb"
printf 'libusb 1.0.30\nSHA256 %s\nmode=%s\ncompiler=%s\n' "$usb_sha" "$mode" "$cc" > "$out/build-info.txt"
"$cc" --version >> "$out/build-info.txt"
echo "Built $out/pix-ts" >&2
if [ "$mode" = musl ] && command -v apk >/dev/null 2>&1; then
    apk info -v musl gcc >> "$out/build-info.txt"
    apk info -v musl | grep '^musl-1\.2\.5-r' >/dev/null || { echo 'Update musl notices for the installed version' >&2; exit 1; }
fi
if [ "$mode" = android ]; then
    mkdir -p "$out/licenses"
    cp "$toolchain/NOTICE" "$out/licenses/Android-NDK-build-NOTICE.txt"
fi
