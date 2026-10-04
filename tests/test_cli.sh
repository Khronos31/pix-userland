#!/bin/sh
set -eu
./pix-ts --version
./pix-ts --help >/dev/null
for args in '--channel -1' '--channel 27junk' '--channel 0' '--system bs --channel 2' '--system cs --channel 23' '--channel 27 --time 0' '--channel 27 --tsid 5' '--fd 1 --list' '--fd 1 --device 2' '--decode missing' '--replay-diagnostic missing --channel 27' '--replay-diagnostic missing --diagnostic dir' '--list --diagnostic dir' '--replay-chunk-bytes 1' '--replay-diagnostic missing --replay-chunk-bytes 0' '--replay-diagnostic missing --replay-chunk-bytes 32769'; do
    set +e
    ./pix-ts $args >/dev/null 2>&1
    status=$?
    set -e
    [ "$status" -eq 2 ] || { echo "Bad exit code $status for $args" >&2; exit 1; }
done
echo 'CLI invalid-input tests passed'
