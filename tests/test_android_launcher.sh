#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
mkdir -p "$tmp/stage/packaging" "$tmp/bin"
cp "$root/packaging/pix-termux" "$tmp/stage/packaging/pix-termux"
cat > "$tmp/stage/pixd" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$PIX_TERMINUX_LOG"
trap 'exit 0' TERM INT
while :; do sleep 1; done
EOF
cat > "$tmp/stage/pixctl" <<'EOF'
#!/bin/sh
printf 'pixctl %s\n' "$*" >> "$PIX_TERMINUX_LOG"
exit 0
EOF
cat > "$tmp/stage/pix-ts" <<'EOF'
#!/bin/sh
printf 'pix-ts %s\n' "$*" >> "$PIX_TERMINUX_LOG"
EOF
chmod +x "$tmp/stage/packaging/pix-termux" "$tmp/stage/pixd" \
    "$tmp/stage/pixctl" "$tmp/stage/pix-ts"
PIX_TERMINUX_LOG="$tmp/calls.log" TMPDIR="$tmp" \
    "$tmp/stage/packaging/pix-termux" --system isdb-t --channel T27 \
    --time 1 -o "$tmp/capture.ts" 7
grep -q '^--fd 7 --runtime-dir ' "$tmp/calls.log"
grep -q 'pixctl .*--instance pix-fd status$' "$tmp/calls.log"
grep -q 'pix-ts .*--instance pix-fd --system isdb-t --channel T27 --time 1 ' \
    "$tmp/calls.log"
if PIX_TERMINUX_LOG="$tmp/invalid.log" TMPDIR="$tmp" \
    "$tmp/stage/packaging/pix-termux" --channel T27 invalid >/dev/null 2>&1; then
    echo 'invalid descriptor unexpectedly accepted' >&2
    exit 1
fi
test ! -s "$tmp/invalid.log"
echo 'Termux callback daemon/client launcher: PASS'
