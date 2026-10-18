#!/bin/sh
set -eu

# --channel accepts decimal 13..62 (unchanged base-0 numeric) and mirakc
# uppercase T13..T62. An accepted value proceeds to firmware resolution and
# fails with exit 10 on the missing file, so the parse result and the derived
# frequency are observable without opening USB.

MISSING=/definitely/missing/isdbt_rio.inp

accept() {
    if ./siano-ts "$@" --firmware "$MISSING" >/dev/null 2>tests/.channel-err; then
        echo "accepted channel should fail on missing firmware: $*" >&2
        exit 1
    else
        status=$?
    fi
    test "${status:-0}" -eq 10 || {
        echo "accepted channel should reach firmware load (exit 10), got ${status:-0}: $*" >&2
        cat tests/.channel-err >&2
        exit 1
    }
}

reject() {
    if ./siano-ts "$@" --firmware "$MISSING" >/dev/null 2>tests/.channel-err; then
        echo "malformed/out-of-range channel should be rejected: $*" >&2
        exit 1
    else
        status=$?
    fi
    test "${status:-0}" -eq 2 || {
        echo "malformed/out-of-range channel should return usage exit 2, got ${status:-0}: $*" >&2
        cat tests/.channel-err >&2
        exit 1
    }
}

# Prints the derived "channel N -> F Hz" line for an accepted value.
mapping_line() {
    if out=$(./siano-ts --channel "$1" --firmware "$MISSING" 2>&1 >/dev/null); then
        echo "accepted channel should fail on missing firmware: $1" >&2
        exit 1
    else
        status=$?
    fi
    test "${status:-0}" -eq 10 || {
        echo "accepted channel should reach firmware load (exit 10), got ${status:-0}: $1" >&2
        exit 1
    }
    printf '%s\n' "$out" | grep '^channel ' | head -n 1
}

for value in 13 27 62 0x1b 033; do
    accept --channel "$value"
done
accept -c 27
accept --channel=T27
accept -cT27
for value in T13 T27 T62; do
    accept --channel "$value"
done

# T27 must derive exactly the same frequency as numeric 27.
t27_line=$(mapping_line T27)
num_line=$(mapping_line 27)
test -n "$t27_line" || {
    echo "T27 did not print a channel mapping" >&2
    exit 1
}
test "$t27_line" = "$num_line" || {
    echo "T27 mapping '$t27_line' != numeric 27 mapping '$num_line'" >&2
    exit 1
}
printf '%s\n' "$t27_line" | grep -q '^channel 27 -> ' || {
    echo "unexpected T27 mapping line: $t27_line" >&2
    exit 1
}

for value in 12 63 T12 T63 T027 T0x1b T+27 t27; do
    reject --channel "$value"
done
reject --channel ' T27'
reject --channel 'T27 '
reject --channel 'x27'
reject --channel ''
reject --channel T
reject --channel T7
reject --channel T277
reject --channel T2a

rm -f tests/.channel-err
echo "channel tests: PASS"
