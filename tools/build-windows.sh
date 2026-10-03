#!/usr/bin/env bash
# Run in MSYS2 MINGW64 (GCC, make, curl, 7zip available).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
build=$(mktemp -d)
curl -fL --retry 3 https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.7z -o "$build/libusb.7z"
echo "7fb1dfec805b97983763d7d0ae244320da12add1003d4249c96cc4d586398c79  $build/libusb.7z" | sha256sum -c -
# Extract only this target: older p7zip cannot decode unrelated ARM64 BCJ members.
7z x "$build/libusb.7z" -o"$build/usb" 'include/*' 'MinGW64/*' >/dev/null
mkdir "$build/app"
cp -R "$root/src" "$root/include" "$root/tests" "$root/Makefile" "$build/app/"
cd "$build/app"
make clean
make -j4 test CC=gcc PKG_CONFIG=false USB_CFLAGS="-I$build/usb/include" USB_LIBS="$build/usb/MinGW64/static/libusb-1.0.a" OS_LIBS="-lbcrypt -lsetupapi -luser32 -ladvapi32 -lole32" LDFLAGS=-static-libgcc CFLAGS='-O2 -g -Werror'
mkdir -p "$root/build/windows-x64"
cp pix-ts.exe "$root/build/windows-x64/"
gcc --version > "$root/build/windows-x64/build-info.txt"
objdump -p pix-ts.exe | grep 'DLL Name' | tee -a "$root/build/windows-x64/build-info.txt"
mkdir -p "$root/build/windows-x64/licenses"
for name in COPYING.MinGW-w64.txt COPYING.MinGW-w64-runtime.txt; do
    notice=$(find /mingw64/share/licenses -name "$name" -print -quit)
    test -n "$notice"
    cp "$notice" "$root/build/windows-x64/licenses/$name"
done
pacman -Q mingw-w64-x86_64-crt mingw-w64-x86_64-gcc >> "$root/build/windows-x64/build-info.txt"
