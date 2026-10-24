#!/bin/sh
# Extract the corresponding-source archive into an empty directory and rebuild pixd, pixctl and
# pix-ts from it without network access, then run the CMake proofs and basic CLI checks.
set -eu
archive=${1:?Usage: check-source-rebuild.sh SOURCE_ARCHIVE EMPTY_WORK_DIR}
work=${2:?Usage: check-source-rebuild.sh SOURCE_ARCHIVE EMPTY_WORK_DIR}
mkdir -p "$work"
[ -z "$(ls -A "$work")" ] || { echo "Work directory must be empty: $work" >&2; exit 2; }
tar -xzf "$archive" -C "$work"
src=$(echo "$work"/pix-userland-*-source)
[ -d "$src/third_party/dtv-userland-core" ] || { echo 'Archive lacks bundled Core source' >&2; exit 1; }
version=$(sed -n 's/^#define VERSION "\([0-9][0-9.]*\)"/\1/p' "$src/include/version.h")
cmake -S "$src/core-integration" -B "$work/build" -DCMAKE_BUILD_TYPE=Release \
    -DPIX_RUNTIME_VERSION="$version" -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build "$work/build" --parallel "${JOBS:-4}"
ctest --test-dir "$work/build" --output-on-failure
for program in pixd pixctl pix-ts; do
    "$work/build/$program" --version | grep -q "$version"
    "$work/build/$program" --help > /dev/null
done
"$work/build/pixd" --models | grep -q '06b8:102b'
echo "Source archive rebuilt offline: pixd, pixctl, pix-ts $version"
