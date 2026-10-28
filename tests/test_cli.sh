#!/bin/sh
set -eu
./pix-ts-legacy --version
./pix-ts-legacy --help >/dev/null
for args in '--model unknown' '--model dt295 --control' '--model dt295 -c T27 --freq 557142857' '--model dt295 -c T27 --lna on' '--channel -1' '--channel 27junk' '--channel 0' '--system bs --channel 2' '--system cs --channel 23' '--channel 27 --time 0' '--channel 27 --tsid 5' '--fd 1 --list' '--fd 1 --device 2' '--decode missing' '--replay-diagnostic missing --channel 27' '--replay-diagnostic missing --diagnostic dir' '--list --diagnostic dir' '--replay-chunk-bytes 1' '--replay-diagnostic missing --replay-chunk-bytes 0' '--replay-diagnostic missing --replay-chunk-bytes 32769' '--device serial:' '--device usb:' '--device usb:1' '--device usb:1:2'; do
    set +e
    ./pix-ts-legacy $args >/dev/null 2>&1
    status=$?
    set -e
    [ "$status" -eq 2 ] || { echo "Bad exit code $status for $args" >&2; exit 1; }
done
echo 'CLI invalid-input tests passed'
for args in '--channel T63' '--channel BS01_8' '--channel T25 --freq 545142857' '--channel BS01_0 --system terrestrial' '--channel BS01_0 --slot 1' '--channel T25 --pid 8193' '--device 1-0 --list' '--list --list-json' '--control --time 10'; do
    set +e
    ./pix-ts-legacy $args >/dev/null 2>&1
    status=$?
    set -e
    [ "$status" -eq 2 ] || { echo "Bad exit code $status for $args" >&2; exit 1; }
done
for args in '--channel T25' '--channel BS01_0' '--channel CS02' '--channel BS01_0 --tsid 0x4010' '--freq 545142857' '--channel T25 --time - --pid 8192' '--channel T25 --device serial:XYZ' '--channel T25 --device usb:1-4.1'; do
    set +e
    ./pix-ts-legacy $args --assets /nonexistent-pix-assets >/dev/null 2>&1
    status=$?
    set -e
    [ "$status" -eq 10 ] || { echo "Valid options rejected ($status): $args" >&2; exit 1; }
done
printf 'quit\n' | ./pix-ts-legacy --control >/dev/null
printf 'quit' | ./pix-ts-legacy --control >/dev/null
echo 'CLI compatibility parsing passed'

# Detect the actual binary mode instead of inferring it from the host OS:
# native Linux builds may either use the macOS-only capture stub or explicitly
# enable DT295_CAPTURE_TESTING for the C capture path.
set +e
./pix-ts-legacy --model dt295 -c T27 --assets /nonexistent-pix-assets -o /dev/null >/dev/null 2>&1
dt295_status=$?
set -e
case "$dt295_status" in
    10) expected_output_status=70 ;;
    2) expected_output_status=2 ;;
    *) echo "Unexpected missing-assets exit code: $dt295_status" >&2; exit 1 ;;
esac

set +e
./pix-ts-legacy -c T25 -o /nonexistent-pix-output/file >/dev/null 2>&1
status=$?
set -e
[ "$status" -eq "$expected_output_status" ] || {
    echo "Bad output-path exit code $status (expected $expected_output_status)" >&2
    exit 1
}
