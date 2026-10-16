#!/bin/sh
set -eu

# Usage: tests/test_cli_compat.sh [HEAD-pix-ts] [working-pix-ts]
baseline=${1:-}
candidate=${2:-./pix-ts}
python=${PYTHON:-python3}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/pix-cli-compat.XXXXXX")
trap 'for name in output.ts synthetic.ts candidate.out candidate.err baseline.out baseline.err; do unlink "$tmp/$name" 2>/dev/null || true; done; rmdir "$tmp" 2>/dev/null || true' EXIT HUP INT TERM

status_of() {
    binary=$1
    shift
    set +e
    "$binary" "$@" >/dev/null 2>&1
    status=$?
    set -e
    printf '%s' "$status"
}

help_line=$("$candidate" --help | sed -n '1p' | tr -d '\r')
case "$help_line" in
    "pix-ts "*" — PIX-DT181-PU0 libusb TS receiver") ;;
    *) echo "Legacy help first line changed: $help_line" >&2; exit 1 ;;
esac
candidate_version=$("$candidate" --version | tr -d '\r')
case "$candidate_version" in "pix-ts "*) ;; *) echo "Unexpected version output: $candidate_version" >&2; exit 1 ;; esac
if [ -n "$baseline" ]; then
    baseline_help_line=$("$baseline" --help | sed -n '1p' | tr -d '\r')
    [ "$help_line" = "$baseline_help_line" ] || { echo "Help first line differs from HEAD" >&2; exit 1; }
    baseline_version=$("$baseline" --version | tr -d '\r')
    [ "$candidate_version" = "$baseline_version" ] || { echo "Version differs from HEAD" >&2; exit 1; }
fi

# These invalid combinations are parser-only and must not enumerate USB.
for args in \
    '--channel -1' \
    '--channel 27junk' \
    '--channel 0' \
    '--system bs --channel 2' \
    '--system cs --channel 23' \
    '--channel 27 --time 0' \
    '--channel 27 --tsid 5' \
    '--model unknown' \
    '--model dt295 --control' \
    '--model dt295 -c BS01_0' \
    '--model dt295 -c T27 --freq 557142857' \
    '--fd 1 --list' \
    '--fd 1 --device 2' \
    '--decode missing' \
    '--replay-chunk-bytes 1' \
    '--list --diagnostic ignored'; do
    # Word splitting is deliberate: each fixture is a fixed argv list.
    # shellcheck disable=SC2086
    actual=$(status_of "$candidate" $args)
    [ "$actual" -eq 2 ] || { echo "Expected exit 2, got $actual: $args" >&2; exit 1; }
    if [ -n "$baseline" ]; then
        # shellcheck disable=SC2086
        old=$(status_of "$baseline" $args)
        [ "$actual" = "$old" ] || { echo "Exit mismatch for $args: HEAD=$old candidate=$actual" >&2; exit 1; }
    fi
done

# A valid DT181 capture request with missing private assets stops at the legacy
# asset failure (10), before USB discovery or device access.
actual=$(status_of "$candidate" --channel T27 --assets "$tmp/missing-assets")
[ "$actual" -eq 10 ] || { echo "Expected DT181 asset exit 10, got $actual" >&2; exit 1; }
if [ -n "$baseline" ]; then
    old=$(status_of "$baseline" --channel T27 --assets "$tmp/missing-assets")
    [ "$old" = "$actual" ] || { echo "DT181 default-model exit changed: HEAD=$old candidate=$actual" >&2; exit 1; }
fi
actual=$(status_of "$candidate" --model dt181 --channel T27 --assets "$tmp/missing-assets")
[ "$actual" -eq 10 ] || { echo "Explicit dt181 did not preserve legacy path: $actual" >&2; exit 1; }

# The documented output policy truncates an existing file before asset loading.
test_output_policy() {
    binary=$1
    path="$tmp/output.ts"
    printf 'must be truncated\n' > "$path"
    actual=$(status_of "$binary" --channel T27 --assets "$tmp/missing-assets" --output "$path")
    [ "$actual" -eq 10 ] || { echo "Expected asset failure after output open, got $actual" >&2; exit 1; }
    [ ! -s "$path" ] || { echo "Existing output was not truncated" >&2; exit 1; }
}
test_output_policy "$candidate"
if [ -n "$baseline" ]; then test_output_policy "$baseline"; fi

# Control-mode quit exits without opening a receiver.
test_control_quit() {
    binary=$1
    set +e
    printf 'quit\n' | "$binary" --control >/dev/null 2>&1
    actual=$?
    set -e
    [ "$actual" -eq 0 ] || { echo "Control quit returned $actual" >&2; exit 1; }
}
test_control_quit "$candidate"
if [ -n "$baseline" ]; then test_control_quit "$baseline"; fi

# While control mode is idle it polls stdin without USB; both stop signals
# should take the documented clean-interrupt path and return zero.
test_control_signal() {
    "$python" - "$1" <<'PY'
import os, signal, subprocess, sys, time
if os.name == 'nt':
    print('Skipping idle-control POSIX signal smoke test on Windows')
    raise SystemExit(0)
p = subprocess.Popen([sys.argv[1], '--control'], stdin=subprocess.PIPE,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.05)
p.send_signal(signal.SIGTERM)
status = p.wait(timeout=3)
if status != 0:
    raise SystemExit('SIGTERM returned %d, expected 0' % status)
p = subprocess.Popen([sys.argv[1], '--control'], stdin=subprocess.PIPE,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.05)
p.send_signal(signal.SIGINT)
status = p.wait(timeout=3)
if status != 0:
    raise SystemExit('SIGINT returned %d, expected 0' % status)
PY
}
test_control_signal "$candidate"
if [ -n "$baseline" ]; then test_control_signal "$baseline"; fi

# A synthetic null-packet fixture exercises the offline DT181 inspector without
# opening USB. Its return code and statistics must remain byte-for-byte stable.
"$python" - "$tmp/synthetic.ts" <<'PY'
import sys
p = bytearray([0xff] * 188)
p[0:4] = bytes((0x47, 0x1f, 0xff, 0x10))
open(sys.argv[1], 'wb').write(p)
PY
set +e
"$candidate" --inspect "$tmp/synthetic.ts" >"$tmp/candidate.out" 2>"$tmp/candidate.err"
candidate_status=$?
set -e
[ "$candidate_status" -eq 8 ] || { echo "Synthetic offline inspector returned $candidate_status, expected legacy 8" >&2; exit 1; }
if [ -n "$baseline" ]; then
    set +e
    "$baseline" --inspect "$tmp/synthetic.ts" >"$tmp/baseline.out" 2>"$tmp/baseline.err"
    baseline_status=$?
    set -e
    [ "$candidate_status" = "$baseline_status" ] || { echo "Offline inspector exit changed: HEAD=$baseline_status candidate=$candidate_status" >&2; exit 1; }
    cmp -s "$tmp/candidate.out" "$tmp/baseline.out" || { echo "Offline stdout changed" >&2; exit 1; }
    cmp -s "$tmp/candidate.err" "$tmp/baseline.err" || { echo "Offline DT181 statistics changed" >&2; exit 1; }
fi

echo "CLI compatibility tests passed${baseline:+ against HEAD baseline}"
