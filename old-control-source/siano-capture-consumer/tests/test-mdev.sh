#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
helper=$root/packaging/mdev/siano-ts-mdev.sh
rules=$root/packaging/mdev/siano-ts-mdev.conf
start=$root/packaging/mdev/siano-ts-mdev.start
test -x "$helper" && test -x "$start" && test -f "$rules"
grep -F 'DEVTYPE=usb_device;PRODUCT=3275/80/.*;bus/usb/[0-9]+/[0-9]+ root:video 0660' "$rules" >/dev/null
grep -F 'DEVTYPE=usb_device;PRODUCT=187f/600/.*;bus/usb/[0-9]+/[0-9]+ root:video 0660' "$rules" >/dev/null
grep -F 'DEVTYPE=usb_device;PRODUCT=187f/302/.*;bus/usb/[0-9]+/[0-9]+ root:video 0660' "$rules" >/dev/null
if grep -E '(^|[[:space:]])SUBSYSTEM=usb;|root:root|0600|@[[:space:]]' "$rules" >/dev/null; then
    printf '%s\n' 'siano mdev rules contain a broad or command rule' >&2
    exit 1
fi
grep -F 'exec /usr/local/libexec/siano-ts-mdev --scan' "$start" >/dev/null

test_root=$(mktemp -d)
trap 'rm -rf "$test_root"' EXIT HUP INT TERM
fake_bin=$test_root/bin
sysfs=$test_root/sys
dev=$test_root/dev
log=$test_root/calls.log
mkdir -p "$fake_bin" "$sysfs/bus/usb/devices" "$dev/bus/usb"
: >"$log"

# shellcheck disable=SC2016
printf '%s\n' \
    '#!/bin/sh' \
    'if [ "${MDEV_TEST_MODE-}" = chown-race ] && [ "${2-}" = "${MDEV_RACE_NODE-}" ]; then' \
    '    rm -f "$2"' \
    '    exit 1' \
    'fi' \
    'if [ "${MDEV_TEST_MODE-}" = chown-error ] && [ "${2-}" = "${MDEV_RACE_NODE-}" ]; then' \
    '    exit 1' \
    'fi' \
    'echo "chown $*" >> "$MDEV_TEST_LOG"' >"$fake_bin/chown"
# shellcheck disable=SC2016
printf '%s\n' \
    '#!/bin/sh' \
    'if [ "${MDEV_TEST_MODE-}" = chmod-race ] && [ "${2-}" = "${MDEV_RACE_NODE-}" ]; then' \
    '    rm -f "$2"' \
    '    exit 1' \
    'fi' \
    'if [ "${MDEV_TEST_MODE-}" = chmod-error ] && [ "${2-}" = "${MDEV_RACE_NODE-}" ]; then' \
    '    exit 1' \
    'fi' \
    'echo "chmod $*" >> "$MDEV_TEST_LOG"' >"$fake_bin/chmod"
chmod 0755 "$fake_bin/chown" "$fake_bin/chmod"

make_usb()
{
    entry=$1
    bus=$2
    device=$3
    vendor=$4
    product=$5
    mkdir -p "$sysfs/bus/usb/devices/$entry"
    printf '%s\n' "$bus" >"$sysfs/bus/usb/devices/$entry/busnum"
    printf '%s\n' "$device" >"$sysfs/bus/usb/devices/$entry/devnum"
    printf '%s\n' "$vendor" >"$sysfs/bus/usb/devices/$entry/idVendor"
    printf '%s\n' "$product" >"$sysfs/bus/usb/devices/$entry/idProduct"
}

make_node()
{
    mkdir -p "$dev/bus/usb/$1"
    : >"$dev/bus/usb/$1/$2"
}

make_usb siano-008 1 8 3275 0080
make_usb siano-009 002 009 187f 0600
make_usb siano-010 3 10 187f 0302
make_node 001 008
make_node 002 009
make_node 003 010

make_usb unsupported 4 11 0511 084a
make_node 004 011
make_usb unsupported-product 5 12 3275 9999
make_node 005 012
make_usb missing-product 6 13 3275 ''
rm -f "$sysfs/bus/usb/devices/missing-product/idProduct"
make_usb invalid-bus not-a-number 14 3275 0080
make_usb missing-node 7 15 3275 0080

if "$helper" >/dev/null 2>&1; then
    printf '%s\n' 'siano helper accepted invocation without --scan' >&2
    exit 1
fi
MDEV=not/a/device MDEV_SYSFS_ROOT=$sysfs MDEV_DEV_ROOT=$dev MDEV_TEST_LOG=$log \
    PATH=$fake_bin:$PATH "$helper" --scan
test "$(grep -c '^chown ' "$log")" -eq 3
test "$(grep -c '^chmod ' "$log")" -eq 3
grep -F "chown root:video $dev/bus/usb/001/008" "$log" >/dev/null
grep -F "chown root:video $dev/bus/usb/002/009" "$log" >/dev/null
grep -F "chown root:video $dev/bus/usb/003/010" "$log" >/dev/null
if grep -E '004/011|005/012|006/013|007/015' "$log" >/dev/null; then
    printf '%s\n' 'siano helper changed an unsupported, invalid, or missing node' >&2
    exit 1
fi

race_node=$dev/bus/usb/001/008
make_node 001 008
MDEV_TEST_MODE=chown-race MDEV_RACE_NODE=$race_node MDEV_SYSFS_ROOT=$sysfs MDEV_DEV_ROOT=$dev MDEV_TEST_LOG=$log \
    PATH=$fake_bin:$PATH "$helper" --scan
test ! -e "$race_node"
make_node 001 008
MDEV_TEST_MODE=chmod-race MDEV_RACE_NODE=$race_node MDEV_SYSFS_ROOT=$sysfs MDEV_DEV_ROOT=$dev MDEV_TEST_LOG=$log \
    PATH=$fake_bin:$PATH "$helper" --scan
test ! -e "$race_node"

make_node 001 008
error_log=$test_root/chown-error.log
if MDEV_TEST_MODE=chown-error MDEV_RACE_NODE=$race_node MDEV_SYSFS_ROOT=$sysfs MDEV_DEV_ROOT=$dev MDEV_TEST_LOG=$log \
    PATH=$fake_bin:$PATH "$helper" --scan 2>"$error_log"; then
    printf '%s\n' 'siano helper ignored a live chown error' >&2
    exit 1
fi
grep -F "mdev: chown failed for $race_node" "$error_log" >/dev/null
error_log=$test_root/chmod-error.log
if MDEV_TEST_MODE=chmod-error MDEV_RACE_NODE=$race_node MDEV_SYSFS_ROOT=$sysfs MDEV_DEV_ROOT=$dev MDEV_TEST_LOG=$log \
    PATH=$fake_bin:$PATH "$helper" --scan 2>"$error_log"; then
    printf '%s\n' 'siano helper ignored a live chmod error' >&2
    exit 1
fi
grep -F "mdev: chmod failed for $race_node" "$error_log" >/dev/null

printf '%s\n' 'siano mdev scan tests: PASS'
