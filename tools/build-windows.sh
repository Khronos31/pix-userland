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
cp "$build/usb/MinGW64/dll/libusb-1.0.dll" .
make -j4 test CC=gcc PKG_CONFIG=false USB_CFLAGS="-I$build/usb/include" USB_LIBS="$build/usb/MinGW64/static/libusb-1.0.dll.a" OS_LIBS=-lbcrypt LDFLAGS=-static-libgcc CFLAGS='-O2 -g -Werror'
# Test executables also resolve the DLL from the application directory via PATH.
mkdir -p "$root/build/windows-x64"
cp pix-ts.exe libusb-1.0.dll "$root/build/windows-x64/"
gcc --version > "$root/build/windows-x64/build-info.txt"
objdump -p pix-ts.exe libusb-1.0.dll | grep 'DLL Name' | tee -a "$root/build/windows-x64/build-info.txt"
