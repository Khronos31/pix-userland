#!/usr/bin/env bash
# Cross-build the three release commands with pinned llvm-mingw UCRT and source libusb.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cache=${LLVM_MINGW_CACHE:-$root/build/deps}
mkdir -p "$root/build" "$cache"
cache=$(cd "$cache" && pwd)
build=$(mktemp -d "$root/build/windows-work.XXXXXX")
trap 'find "$build" -depth -delete' EXIT
export SOURCE_DATE_EPOCH=1700000000

llvm_version=20250910
llvm_name=llvm-mingw-$llvm_version-ucrt-ubuntu-22.04-x86_64
llvm_sha=f83556c9ffa4d4291fadea1a0776c1383332dacdf4d7fbdf974c2928cb32c6f7
usb_sha=fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf
llvm_archive=$cache/$llvm_name.tar.xz
usb_archive=${LIBUSB_SOURCE_ARCHIVE:-$root/third_party/libusb-1.0.30.tar.bz2}
if [[ ! -f "$usb_archive" && -z "${LIBUSB_SOURCE_ARCHIVE:-}" ]]; then
    usb_archive=$cache/libusb-1.0.30.tar.bz2
fi

fetch() {
    local path=$1 url=$2
    if [[ ! -f "$path" ]]; then
        if [[ "${PIX_WINDOWS_OFFLINE:-0}" == 1 ]]; then
            echo "Offline input missing: $path" >&2
            exit 1
        fi
        curl -fL --retry 3 "$url" -o "$path"
    fi
}
fetch "$llvm_archive" "https://github.com/mstorsjo/llvm-mingw/releases/download/$llvm_version/$llvm_name.tar.xz"
fetch "$usb_archive" https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2
printf '%s  %s\n' "$llvm_sha" "$llvm_archive" "$usb_sha" "$usb_archive" | sha256sum -c -
tar -xf "$llvm_archive" -C "$build"
chain=$build/$llvm_name
export PATH="$chain/bin:$PATH"
tar -xjf "$usb_archive" -C "$build"
(
    cd "$build/libusb-1.0.30"
    ./configure --host=x86_64-w64-mingw32 --prefix="$build/usb" \
        --enable-static --disable-shared --disable-udev \
        --disable-examples-build --disable-tests-build \
        CC="$chain/bin/x86_64-w64-mingw32-clang" \
        AR="$chain/bin/llvm-ar" RANLIB="$chain/bin/llvm-ranlib" \
        CFLAGS="-O2 -g0 -ffile-prefix-map=$build=." \
        LDFLAGS='-Wl,--no-insert-timestamp'
    make -j"${JOBS:-4}"
    make install
)
test -f "$build/usb/lib/libusb-1.0.a"
test ! -f "$build/usb/lib/libusb-1.0.dll.a"
test ! -f "$build/usb/bin/libusb-1.0.dll"

version=$(sed -n 's/^#define VERSION "\([0-9][0-9.]*\)"/\1/p' "$root/include/version.h")
test -n "$version"
core_pin=$(head -n 1 "$root/core-integration/dtv-userland-core.revision")
core_root=${DTV_USERLAND_CORE_SOURCE_DIR:-$root/third_party/dtv-userland-core}
source_args=()
if [[ -d "$core_root" ]]; then
    if [[ -e "$core_root/.git" ]]; then
        core_head=$(git -C "$core_root" rev-parse HEAD)
        if [[ "$core_head" != "$core_pin" && "${PIX_CORE_ALLOW_UNPINNED:-0}" != 1 ]]; then
            echo "Core source $core_head does not match pin $core_pin" >&2
            exit 1
        fi
    fi
    source_args=(-DPIX_CORE_SOURCE_DIR="$core_root")
elif [[ -n "${DTV_USERLAND_CORE_SOURCE_DIR:-}" || "${PIX_WINDOWS_OFFLINE:-0}" == 1 ]]; then
    echo "Core source missing: $core_root" >&2
    exit 2
else
    core_root=$build/cmake/_deps/dtv_userland_core-src
fi
out=$root/build/windows-x64
mkdir -p "$out/licenses"
export PKG_CONFIG_LIBDIR="$build/usb/lib/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
cmake -S "$root/core-integration" -B "$build/cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_C_COMPILER="$chain/bin/x86_64-w64-mingw32-clang" \
    -DCMAKE_CXX_COMPILER="$chain/bin/x86_64-w64-mingw32-clang++" \
    -DCMAKE_RC_COMPILER="$chain/bin/llvm-rc" \
    -DCMAKE_EXE_LINKER_FLAGS='-static -Wl,--no-insert-timestamp' \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$build/bin" -DPIX_RUNTIME_VERSION="$version" \
    "${source_args[@]}"
cmake --build "$build/cmake" --target pixd pixctl pix-ts --parallel "${JOBS:-4}"
cp "$build/bin/pixd.exe" "$build/bin/pixctl.exe" "$build/bin/pix-ts.exe" "$out/"
{
    printf 'PIX version %s\nlibusb 1.0.30\nSHA256 %s\n' "$version" "$usb_sha"
    printf 'dtv-userland-core %s\nllvm-mingw %s UCRT\nSHA256 %s\n' "$core_pin" "$llvm_version" "$llvm_sha"
    "$chain/bin/x86_64-w64-mingw32-clang" --version | head -n 1
    for program in pixd.exe pixctl.exe pix-ts.exe; do
        printf '%s\n' "$program"
        "$chain/bin/llvm-objdump" -p "$out/$program" | grep 'DLL Name'
    done
} > "$out/build-info.txt"
cp "$chain/LICENSE.TXT" "$out/licenses/llvm-mingw-LICENSE.TXT"
for name in COPYING COPYING.MinGW-w64.txt COPYING.MinGW-w64-runtime.txt COPYING.winpthreads.txt COPYING.winstorecompat.txt; do
    cp "$chain/x86_64-w64-mingw32/share/mingw32/$name" "$out/licenses/llvm-mingw-$name"
done
python3 "$root/tools/core-notices.py" "$core_root" "$out/licenses"
