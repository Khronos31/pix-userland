#!/usr/bin/env bash
# Run in MSYS2 MINGW64 (GCC, make, curl, 7zip, CMake and pkgconf available).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
build=$(mktemp -d)
usb_url=https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.7z
usb_sha=7fb1dfec805b97983763d7d0ae244320da12add1003d4249c96cc4d586398c79
if [ -n "${LIBUSB_MINGW_ARCHIVE:-}" ]; then
    cp "$LIBUSB_MINGW_ARCHIVE" "$build/libusb.7z"
else
    curl -fL --retry 3 "$usb_url" -o "$build/libusb.7z"
fi
actual=$(sha256sum "$build/libusb.7z" | awk '{print $1}')
if [ "$actual" != "$usb_sha" ] && { [ -z "${LIBUSB_MINGW_ARCHIVE:-}" ] || [ "${LIBUSB_ALLOW_MODIFIED:-0}" != 1 ]; }; then
    echo 'libusb SHA-256 mismatch; set LIBUSB_ALLOW_MODIFIED=1 only for a reviewed modified archive' >&2
    exit 1
fi
# Extract only this target: older p7zip cannot decode unrelated ARM64 BCJ members.
7z x "$build/libusb.7z" -o"$build/usb" 'include/*' 'MinGW64/*' >/dev/null
mkdir "$build/app"
cp -R "$root/src" "$root/include" "$root/tests" "$root/tools" "$root/Makefile" "$build/app/"
cd "$build/app"
make clean
make -j4 test CC=gcc PKG_CONFIG=false USB_CFLAGS="-I$build/usb/include" USB_LIBS="$build/usb/MinGW64/static/libusb-1.0.a" OS_LIBS="-lbcrypt -lsetupapi -luser32 -ladvapi32 -lole32" LDFLAGS=-static-libgcc CFLAGS='-O2 -g -Werror'

pkgdir=$build/pkgconfig
mkdir -p "$pkgdir"
cat > "$pkgdir/libusb-1.0.pc" <<EOF
prefix=$build/usb
includedir=\${prefix}/include
libdir=\${prefix}/MinGW64/static
Name: libusb-1.0
Description: static libusb for PIX Windows release
Version: 1.0.30
Cflags: -I\${includedir}
Libs: -L\${libdir} -lusb-1.0 -lbcrypt -lsetupapi -luser32 -ladvapi32 -lole32
EOF
version=$(sed -n 's/^#define VERSION "\([0-9][0-9.]*\)"/\1/p' "$root/include/version.h")
test -n "$version"
core_root=${DTV_USERLAND_CORE_SOURCE_DIR:-$root/third_party/dtv-userland-core}
px4_root=${PX4_USERLAND_SOURCE_DIR:-$root/third_party/px4-userland}
source_args=()
if [[ -d "$core_root" && -d "$px4_root" ]]; then
    source_args=(-DDTV_USERLAND_CORE_SOURCE_DIR="$core_root" -DPX4_USERLAND_SOURCE_DIR="$px4_root" -DFETCHCONTENT_FULLY_DISCONNECTED=ON)
elif [[ -n "${DTV_USERLAND_CORE_SOURCE_DIR:-}" || -n "${PX4_USERLAND_SOURCE_DIR:-}" ]]; then
    echo 'Both DTV_USERLAND_CORE_SOURCE_DIR and PX4_USERLAND_SOURCE_DIR must exist' >&2
    exit 2
fi
out=$root/build/windows-x64
mkdir -p "$out"
PKG_CONFIG_PATH=$pkgdir PKG_CONFIG_LIBDIR=$pkgdir cmake -S "$root" -B "$build/cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_EXE_LINKER_FLAGS='-static -static-libgcc -static-libstdc++' \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$build/bin" -DPIX_RUNTIME_VERSION="$version" \
    "${source_args[@]}"
PKG_CONFIG_PATH=$pkgdir PKG_CONFIG_LIBDIR=$pkgdir cmake --build "$build/cmake" --target pixd pixctl pix-ts --parallel 4
cp "$build/bin/pixd.exe" "$build/bin/pixctl.exe" "$build/bin/pix-ts.exe" "$out/"
{
    printf 'PIX version %s\nlibusb 1.0.30\nSHA256 %s\n' "$version" "$actual"
    gcc --version | head -n 1
    g++ --version | head -n 1
    for program in pixd.exe pixctl.exe pix-ts.exe; do
        printf '%s\n' "$program"
        objdump -p "$out/$program" | grep 'DLL Name'
    done
} > "$out/build-info.txt"
mkdir -p "$out/licenses"
for name in COPYING.MinGW-w64.txt COPYING.MinGW-w64-runtime.txt; do
    notice=$(find /mingw64/share/licenses -name "$name" -print -quit)
    test -n "$notice"
    cp "$notice" "$out/licenses/$name"
done
pacman -Q mingw-w64-x86_64-crt mingw-w64-x86_64-gcc >> "$out/build-info.txt"
