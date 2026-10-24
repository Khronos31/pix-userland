#!/bin/sh
# Keep the isolated legacy test copy complete for packaging/source-closure tests.
set -eu
root=${1:?Usage: copy-legacy-test-source.sh SOURCE_ROOT DESTINATION}
destination=${2:?Usage: copy-legacy-test-source.sh SOURCE_ROOT DESTINATION}
cp -R "$root/src" "$root/include" "$root/tests" "$root/tools" "$root/Makefile" \
    "$root/core-integration" "$root/pix-dt181-consumer.cpp" "$root/pix-dt181-consumer.h" \
    "$root/pix-dt181-receiver-adapter.cpp" "$root/pix-dt181-receiver-adapter.h" \
    "$root/pix-dt295-receiver-adapter.cpp" "$root/pix-dt295-receiver-adapter.h" "$destination/"
