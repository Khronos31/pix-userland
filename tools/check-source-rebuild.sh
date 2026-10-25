#!/bin/sh
# Extract the corresponding-source archive into an empty directory and rebuild pixd, pixctl and
# pix-ts from it without network access, then run the CMake proofs and basic CLI checks.
set -eu
archive=${1:?Usage: check-source-rebuild.sh SOURCE_ARCHIVE EMPTY_WORK_DIR}
work=${2:?Usage: check-source-rebuild.sh SOURCE_ARCHIVE EMPTY_WORK_DIR}
mode=${3:-native}
case "$mode" in native|windows-x64) ;; *) echo 'Expected native or windows-x64' >&2; exit 2;; esac
mkdir -p "$work"
[ -z "$(ls -A "$work")" ] || { echo "Work directory must be empty: $work" >&2; exit 2; }
tar -xzf "$archive" -C "$work"
src=$(echo "$work"/pix-userland-*-source)
[ -d "$src/third_party/dtv-userland-core" ] || { echo 'Archive lacks bundled Core source' >&2; exit 1; }
source_revision=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["repository_commit"])' "$src/release-info.json")
if [ "$mode" = windows-x64 ]; then
    PIX_WINDOWS_OFFLINE=1 PIX_EMBEDDED_ASSETS=ON PIX_SOURCE_REVISION_OVERRIDE="$source_revision" bash "$src/tools/build-windows.sh"
    for program in pixd pixctl pix-ts; do
        python3 "$src/tools/audit-binary.py" windows-x64 "$src/build/windows-x64/$program.exe"
    done
    python3 "$src/tools/verify-embedded-assets.py" "$src/build/windows-x64/pixd.exe" \
        "$src/assets" "$src/assets/dt295.assets" "$src/build/windows-x64/embedded-assets.json" \
        --build-info "$src/build/windows-x64/build-info.txt"
    echo 'Source archive rebuilt offline: pixd.exe, pixctl.exe, pix-ts.exe (Windows execution is separate)'
    exit 0
fi
version=$(sed -n 's/^#define VERSION "\([0-9][0-9.]*\)"/\1/p' "$src/include/version.h")
mkdir -p "$work/libusb-source" "$work/libusb-prefix"
tar -xjf "$src/third_party/libusb-1.0.30.tar.bz2" -C "$work/libusb-source"
(
    cd "$work/libusb-source/libusb-1.0.30"
    ./configure --prefix="$work/libusb-prefix" --disable-udev --disable-shared --enable-static
    make -j"${JOBS:-4}"
    make install
)
export PKG_CONFIG_PATH="$work/libusb-prefix/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
cmake -S "$src/core-integration" -B "$work/build-static" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXE_LINKER_FLAGS=-static -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$work/build-static/bin" \
    -DPIX_RUNTIME_VERSION="$version" -DPIX_CORE_SOURCE_DIR="$src/third_party/dtv-userland-core" \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DPIX_EMBEDDED_ASSETS=ON \
    -DPIX_DT181_ASSET_DIR="$src/assets" -DPIX_DT295_ASSET_FILE="$src/assets/dt295.assets" \
    -DPIX_SOURCE_REVISION_OVERRIDE="$source_revision"
cmake --build "$work/build-static" --target pixd pixctl pix-ts --parallel "${JOBS:-4}"
for program in pixd pixctl pix-ts; do
    python3 "$src/tools/audit-binary.py" "linux-$(uname -m)" "$work/build-static/bin/$program"
done
python3 "$src/tools/verify-embedded-assets.py" "$work/build-static/bin/pixd" \
    "$src/assets" "$src/assets/dt295.assets" "$work/build-static/embedded-assets/pix_embedded_assets.json"
unset PKG_CONFIG_PATH PKG_CONFIG_LIBDIR
cmake -S "$src/core-integration" -B "$work/build-proof" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$work/build-proof/bin" \
    -DPIX_RUNTIME_VERSION="$version" -DPIX_CORE_SOURCE_DIR="$src/third_party/dtv-userland-core" \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DPIX_EMBEDDED_ASSETS=ON \
    -DPIX_DT181_ASSET_DIR="$src/assets" -DPIX_DT295_ASSET_FILE="$src/assets/dt295.assets" \
    -DPIX_SOURCE_REVISION_OVERRIDE="$source_revision"
cmake --build "$work/build-proof" --parallel "${JOBS:-4}"
ctest --test-dir "$work/build-proof" --output-on-failure
for program in pixd pixctl pix-ts; do
    "$work/build-static/bin/$program" --version | grep -q "$version"
    "$work/build-static/bin/$program" --help > /dev/null
done
"$work/build-static/bin/pixd" --models | grep -q '06b8:102b'
echo "Source archive rebuilt offline with embedded assets: pixd, pixctl, pix-ts $version"
