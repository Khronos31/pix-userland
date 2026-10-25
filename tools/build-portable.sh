#!/bin/sh
# Build libusb without libudev, then build and link pixd, pixctl and pix-ts against its static archive.
# The legacy C CLI is built and tested separately and is not copied into the release output.
# Run under musl for a fully static Linux binary, or with an Android NDK for Bionic.
set -eu
mode=${1:-native}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
case "$mode" in native|musl|android) ;; *) echo 'Usage: build-portable.sh native|musl|android' >&2; exit 2;; esac
embedded_assets=${PIX_EMBEDDED_ASSETS:-OFF}
case "$embedded_assets" in
    ON|on|1|TRUE|true)
        dt181_assets=${PIX_DT181_ASSET_DIR:-$root/assets}
        dt295_assets=${PIX_DT295_ASSET_FILE:-$root/assets/dt295.assets}
        [ -d "$dt181_assets" ] && [ -f "$dt295_assets" ] || {
            echo 'Embedded builds require PIX_DT181_ASSET_DIR and PIX_DT295_ASSET_FILE' >&2
            exit 2
        }
        embedded_assets=ON
        ;;
    OFF|off|0|FALSE|false) embedded_assets=OFF;;
    *) echo 'PIX_EMBEDDED_ASSETS must be ON or OFF' >&2; exit 2;;
esac
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
if [ "$actual" != "$usb_sha" ] && { [ -z "${LIBUSB_SOURCE_TARBALL:-}" ] || [ "${LIBUSB_ALLOW_MODIFIED:-0}" != 1 ]; }; then
    echo 'libusb SHA-256 mismatch; set LIBUSB_ALLOW_MODIFIED=1 only for a reviewed modified source archive' >&2
    exit 1
fi
tar -xjf "$build/libusb.tar.bz2" -C "$build"
cc=${CC:-cc}
cxx=${CXX:-c++}
extra_link=
configure_host=
abi=${ANDROID_ABI:-aarch64}
toolchain_file=
android_abi=
api=${ANDROID_API:-24}
build_target=
if [ "$mode" = android ]; then
    ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
    [ -d "$ndk" ] || { echo 'Set ANDROID_NDK_HOME' >&2; exit 2; }
    case $(uname -s) in Linux) host=linux-x86_64;; Darwin) host=darwin-x86_64;; *) exit 2;; esac
    toolchain=$ndk/toolchains/llvm/prebuilt/$host
    case "$abi" in
        aarch64) target=aarch64-linux-android;triplet=$target;android_abi=arm64-v8a;;
        x86_64) target=x86_64-linux-android;triplet=$target;android_abi=x86_64;;
        armv7a) target=armv7a-linux-androideabi;triplet=arm-linux-androideabi;android_abi=armeabi-v7a;;
        *) echo "Unsupported Android ABI: $abi" >&2; exit 2;;
    esac
    build_target=android-$abi
    cc=$toolchain/bin/${target}${api}-clang
    cxx=$toolchain/bin/${target}${api}-clang++
    [ -x "$cc" ] && [ -x "$cxx" ] || { echo "Android compiler missing: $cc or $cxx" >&2; exit 2; }
    export AR="$toolchain/bin/llvm-ar" RANLIB="$toolchain/bin/llvm-ranlib"
    configure_host=--host=$triplet
    extra_link=-llog
    toolchain_file=$ndk/build/cmake/android.toolchain.cmake
elif [ "$mode" = musl ]; then
    "$cc" -E -dM -include features.h - < /dev/null > "$build/compiler-macros"
    if grep -Eq '__GLIBC__|__BIONIC__' "$build/compiler-macros"; then
        echo 'musl mode requires a musl compiler; use an Alpine build environment' >&2
        exit 2
    fi
    extra_link=-static
    build_target=linux-$(uname -m)
else
    case "$(uname -s)-$(uname -m)" in
        Darwin-arm64) build_target=darwin-arm64;;
        Linux-*) build_target=linux-$(uname -m);;
        *) build_target=$(uname -s)-$(uname -m);;
    esac
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
sh "$root/tools/copy-legacy-test-source.sh" "$root" "$build/app"
# Remove copied object/dependency files so no host objects can leak into cross builds.
(cd "$build/app" && make clean && make -j "${JOBS:-4}" CC="$cc" PKG_CONFIG=false USB_CFLAGS="-I$build/prefix/include/libusb-1.0" USB_LIBS="$build/prefix/lib/libusb-1.0.a $extra_link")
if [ "$mode" != android ]; then
    (cd "$build/app" && make test CC="$cc" PKG_CONFIG=false USB_CFLAGS="-I$build/prefix/include/libusb-1.0" USB_LIBS="$build/prefix/lib/libusb-1.0.a $extra_link" LDFLAGS="$extra_link")
fi

# Release programs: pixd, pixctl and pix-ts from core-integration, with the pinned Core source.
version=$(sed -n 's/^#define VERSION "\([0-9][0-9.]*\)"/\1/p' "$root/include/version.h")
[ -n "$version" ] || { echo 'Unable to read product version from include/version.h' >&2; exit 1; }
core_pin=$(head -n 1 "$root/core-integration/dtv-userland-core.revision")
core_root=${DTV_USERLAND_CORE_SOURCE_DIR:-$root/third_party/dtv-userland-core}
if [ -d "$core_root" ]; then
    if [ -d "$core_root/.git" ] || [ -f "$core_root/.git" ]; then
        core_head=$(git -C "$core_root" rev-parse HEAD)
        if [ "$core_head" != "$core_pin" ] && [ "${PIX_CORE_ALLOW_UNPINNED:-0}" != 1 ]; then
            echo "Core source $core_head does not match pin $core_pin; set PIX_CORE_ALLOW_UNPINNED=1 for a local build" >&2
            exit 1
        fi
    fi
elif [ -n "${DTV_USERLAND_CORE_SOURCE_DIR:-}" ]; then
    echo "DTV_USERLAND_CORE_SOURCE_DIR does not exist: $DTV_USERLAND_CORE_SOURCE_DIR" >&2
    exit 2
else
    core_root=$build/cmake/_deps/dtv_userland_core-src
fi
pkgdir=$build/prefix/lib/pkgconfig
out=$root/build/$mode
[ "$mode" != android ] || out=$out-$abi
mkdir -p "$out"
set -- cmake -S "$root/core-integration" -B "$build/cmake" -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS="$extra_link" -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$build/bin" -DPIX_RUNTIME_VERSION="$version" -DPKG_CONFIG_USE_CMAKE_PREFIX_PATH=ON
[ -z "$toolchain_file" ] || set -- "$@" -DCMAKE_TOOLCHAIN_FILE="$toolchain_file" -DANDROID_ABI="$android_abi" -DANDROID_PLATFORM="android-$api" -DANDROID_STL=c++_static -DCMAKE_FIND_ROOT_PATH="$build/prefix"
[ ! -d "$core_root" ] || set -- "$@" "-DPIX_CORE_SOURCE_DIR=$core_root"
if [ "$embedded_assets" = ON ]; then
    set -- "$@" -DPIX_EMBEDDED_ASSETS=ON \
        "-DPIX_DT181_ASSET_DIR=$dt181_assets" "-DPIX_DT295_ASSET_FILE=$dt295_assets"
fi
CC=$cc CXX=$cxx PKG_CONFIG_PATH=$pkgdir PKG_CONFIG_LIBDIR=$pkgdir "$@"
PKG_CONFIG_PATH=$pkgdir PKG_CONFIG_LIBDIR=$pkgdir cmake --build "$build/cmake" --target pixd pixctl pix-ts --parallel "${JOBS:-4}"
cp "$build/bin/pixd" "$build/bin/pixctl" "$build/bin/pix-ts" "$out/"
cp "$build/libusb-1.0.30/COPYING" "$out/COPYING.libusb"
{
    printf 'PIX version %s\nlibusb 1.0.30\nSHA256 %s\nmode=%s\ntarget=%s\nembedded_assets=%s\ncompiler=%s\ncxx=%s\n' "$version" "$actual" "$mode" "$build_target" "$embedded_assets" "$cc" "$cxx"
    printf 'dtv-userland-core %s\n' "$core_pin"
    "$cc" --version | head -n 1
    "$cxx" --version | head -n 1
} > "$out/build-info.txt"
if [ "$embedded_assets" = ON ]; then
    python3 - "$build/cmake/embedded-assets/pix_embedded_assets.json" >> "$out/build-info.txt" <<'PY'
import hashlib
import json
import sys

path = sys.argv[1]
manifest_bytes = open(path, "rb").read()
manifest = json.loads(manifest_bytes)
print("embedded_source_revision=" + manifest["source_revision"])
print("embedded_core_revision=" + manifest["dtv_userland_core_revision"])
print("embedded_toolchain=" + manifest["toolchain"])
print("embedded_manifest_sha256=" + hashlib.sha256(manifest_bytes).hexdigest())
for model, entries in manifest["assets"].items():
    files = entries["files"] if "files" in entries else {entries["file"]["name"]: entries["file"]}
    for name, item in sorted(files.items()):
        print(f"embedded_asset={model}/{name} size={item['size']} sha256={item['sha256']}")
PY
fi
mkdir -p "$out/licenses"
python3 "$root/tools/core-notices.py" "$core_root" "$out/licenses"
echo "Built $out/pixd, $out/pixctl, and $out/pix-ts" >&2
if [ "$mode" = musl ] && command -v apk >/dev/null 2>&1; then
    apk list --installed musl gcc g++ > "$build/runtime-packages.txt"
    cat "$build/runtime-packages.txt" >> "$out/build-info.txt"
    grep '^musl-1\.2\.5-r' "$build/runtime-packages.txt" >/dev/null || { echo 'Update musl notices for the installed version' >&2; exit 1; }
fi
if [ "$mode" = android ]; then
    cp "$toolchain/NOTICE" "$out/licenses/Android-NDK-build-NOTICE.txt"
fi
