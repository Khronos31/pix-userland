#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/build/deps"
curl -fL --retry 3 https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2 -o "$root/build/deps/libusb-1.0.30.tar.bz2"
python3 "$root/tools/package-release.py" source "$@"
