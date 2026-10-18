#!/bin/sh
# Run as root in an isolated Linux test container; never operate on host /dev.
set -eu
[ "$(id -u)" -eq 0 ] || { echo 'mdev fixture test requires container root' >&2; exit 77; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/sys/bus/usb/devices/match" "$tmp/sys/bus/usb/devices/other" "$tmp/dev/bus/usb/001"
printf '06b8\n' > "$tmp/sys/bus/usb/devices/match/idVendor"
printf '102b\n' > "$tmp/sys/bus/usb/devices/match/idProduct"
printf '1\n' > "$tmp/sys/bus/usb/devices/match/busnum"
printf '7\n' > "$tmp/sys/bus/usb/devices/match/devnum"
printf '1234\n' > "$tmp/sys/bus/usb/devices/other/idVendor"
printf '102b\n' > "$tmp/sys/bus/usb/devices/other/idProduct"
printf '1\n' > "$tmp/sys/bus/usb/devices/other/busnum"
printf '8\n' > "$tmp/sys/bus/usb/devices/other/devnum"
mknod "$tmp/dev/bus/usb/001/007" c 1 3
mknod "$tmp/dev/bus/usb/001/008" c 1 3
chmod 0600 "$tmp/dev/bus/usb/001/007" "$tmp/dev/bus/usb/001/008"
PIX_SYSFS_ROOT="$tmp/sys" PIX_DEV_ROOT="$tmp/dev" sh packaging/pix-usb-permissions --scan
[ "$(stat -c %a "$tmp/dev/bus/usb/001/007")" = 660 ]
[ "$(stat -c %a "$tmp/dev/bus/usb/001/008")" = 600 ]
echo 'mdev coldplug matching/nonmatching fixture passed'
