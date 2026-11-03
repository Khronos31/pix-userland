#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PIX_PUBLIC_TS=$1
export PIX_PUBLIC_TS
tmp=$(mktemp -d)
trap 'exec 7>&- 2>/dev/null || :; /bin/rm -rf "$tmp"' EXIT INT TERM
mkdir -p "$tmp/stage/packaging" "$tmp/bin"
cp "$root/packaging/pix-termux" "$tmp/stage/packaging/pix-termux"
cat > "$tmp/stage/pixd" <<'EOF'
#!/bin/sh
runtime=
printf 'pixd-arg=%s\n' "$@" >> "$PIX_TERMINUX_LOG"
while [ "$#" -gt 0 ]; do
    if [ "$1" = --runtime-dir ]; then runtime=$2; shift 2; else shift; fi
done
printf '%s\n' "$runtime" > "$PIX_TEST_RUNTIME_FILE"
printf 'daemon-fd=%s\n' "$(test -e "/proc/$$/fd/$PIX_TEST_USB_FD" && echo present || echo absent)" \
    >> "$PIX_TERMINUX_LOG"
echo daemon-diagnostic >&2
trap '
    printf "daemon-fd-at-stop=%s\\n" "$(test -e "/proc/$PPID/fd/$PIX_TEST_USB_FD" && echo present || echo absent)" >> "$PIX_TERMINUX_LOG"
    printf "daemon-stop\\n" >> "$PIX_TERMINUX_LOG"
    exit "${PIX_TEST_DAEMON_STATUS:-0}"
' TERM
while :; do sleep 0.05; done
EOF
cat > "$tmp/stage/pixctl" <<'EOF'
#!/bin/sh
printf 'pixctl-fd=%s\n' "$(test -e "/proc/$$/fd/$PIX_TEST_USB_FD" && echo present || echo absent)" \
    >> "$PIX_TERMINUX_LOG"
printf 'pixctl %s\n' "$*" >> "$PIX_TERMINUX_LOG"
exit 0
EOF
cat > "$tmp/stage/pix-ts" <<'EOF'
#!/usr/bin/env python3
import os
import signal
import subprocess
import sys
import time

log = os.environ["PIX_TERMINUX_LOG"]
descriptor = os.environ["PIX_TEST_USB_FD"]
mode = os.environ.get("PIX_TEST_CLIENT_MODE", "exit")
arguments = sys.argv[1:]
validation = any(argument.startswith("--pix-termux-validation") for argument in arguments)
for index, argument in enumerate(arguments[:-1]):
    if argument == "--runtime-dir" and "pix-termux-validation-" in arguments[index + 1]:
        validation = True
if validation:
    result = subprocess.run(
        [os.environ["PIX_PUBLIC_TS"], *sys.argv[1:]],
        stdout=sys.stdout,
        stderr=sys.stderr,
    )
    raise SystemExit(result.returncode)
with open(log, "a") as calls:
    client_fd = "present" if os.path.exists("/proc/self/fd/" + descriptor) else "absent"
    calls.write("pix-ts-fd=%s\n" % client_fd)
    calls.write("pix-ts %s\n" % " ".join(sys.argv[1:]))
    for argument in sys.argv[1:]:
        calls.write("pix-ts-arg=%s\n" % argument)
writer_check = (
    "import os; print('writer-fd=' + ('present' if os.path.exists('/proc/self/fd/' "
    "+ os.environ['PIX_TEST_USB_FD']) else 'absent'))"
)
subprocess.run([sys.executable, "-c", writer_check], stdout=open(log, "a"))
if mode == "delayed-stop":
    def stop(signum, frame):
        time.sleep(0.2)
        with open(log, "a") as calls:
            calls.write("client-stop\n")
        raise SystemExit(0)

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    with open(log, "a") as calls:
        calls.write("client-ready pid=%s ppid=%s\n" % (os.getpid(), os.getppid()))
    while True:
        time.sleep(0.05)
if mode == "ignore-term-then-exit":
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    with open(log, "a") as calls:
        calls.write("client-ready pid=%s ppid=%s\n" % (os.getpid(), os.getppid()))
    time.sleep(0.3)
    raise SystemExit(0)
raise SystemExit(int(os.environ.get("PIX_TEST_CLIENT_STATUS", "0")))
EOF
cat > "$tmp/bin/rm" <<'EOF'
#!/bin/sh
if [ "${PIX_TEST_FAIL_RM:-0}" = 1 ]; then
    echo 'synthetic rm failure' >&2
    exit 1
fi
exec /bin/rm "$@"
EOF
chmod +x "$tmp/stage/packaging/pix-termux" "$tmp/stage/pixd" \
    "$tmp/stage/pixctl" "$tmp/stage/pix-ts" "$tmp/bin/rm"
: > "$tmp/fd-source"
exec 7< "$tmp/fd-source"
PIX_TEST_FIRMWARE_PATH='./firmware/DT181 バンドル.bin'
export PIX_TEST_FIRMWARE_PATH

run_case()
{
    name=$1 client_status=$2 daemon_status=$3 expected=$4
    log="$tmp/$name.log"
    runtime_file="$tmp/$name.runtime"
    stderr_file="$tmp/$name.stderr"
    if PIX_TERMINUX_LOG="$log" PIX_TEST_RUNTIME_FILE="$runtime_file" \
        PIX_TEST_USB_FD=7 PIX_TEST_CLIENT_STATUS="$client_status" \
        PIX_TEST_DAEMON_STATUS="$daemon_status" TMPDIR="$tmp" \
        "$tmp/stage/packaging/pix-termux" --firmware "$PIX_TEST_FIRMWARE_PATH" \
        --receiver 0 --channel T27 7 \
        2> "$stderr_file"; then
        status=0
    else
        status=$?
    fi
    if [ "$status" -ne "$expected" ]; then
        echo "$name: launcher exit $status, expected $expected" >&2
        return 1
    fi
    runtime=$(cat "$runtime_file")
    if [ -e "$runtime" ]; then
        echo "$name: runtime directory was not removed" >&2
        return 1
    fi
    grep -q '^daemon-fd=present$' "$log"
    grep -q '^pixd-arg=--firmware$' "$log"
    grep -Fqx "pixd-arg=$PIX_TEST_FIRMWARE_PATH" "$log"
    grep -q '^pixctl-fd=absent$' "$log"
    grep -q '^pix-ts-fd=absent$' "$log"
    grep -q '^pix-ts --runtime-dir .*--instance pix-fd --receiver 0 --channel T27$' "$log"
    if grep -Fqx "pix-ts-arg=$PIX_TEST_FIRMWARE_PATH" "$log"; then
        echo "$name: firmware argument leaked to pix-ts" >&2
        return 1
    fi
    grep -q '^writer-fd=absent$' "$log"
    grep -q '^daemon-fd-at-stop=present$' "$log"
    grep -q '^daemon-stop$' "$log"
    if [ "$daemon_status" -ne 0 ]; then
        grep -q '^daemon-diagnostic$' "$stderr_file"
    fi
}

run_case success 0 0 0
run_case client-failure 5 0 5
run_case daemon-failure 0 9 9
run_case both-fail 5 9 5

: > "$tmp/legacy.log"
for option in -F -V; do
    status=0
    PIX_TERMINUX_LOG="$tmp/legacy.log" PIX_TEST_RUNTIME_FILE="$tmp/legacy.runtime" \
        PIX_TEST_USB_FD=7 TMPDIR="$tmp" \
        "$tmp/stage/packaging/pix-termux" --firmware "$PIX_TEST_FIRMWARE_PATH" \
        "$option" "$PIX_TEST_FIRMWARE_PATH" 7 2>"$tmp/legacy.stderr" || status=$?
    [ "$status" -eq 2 ]
    grep -F "unknown argument: $option" "$tmp/legacy.stderr" >/dev/null
    [ ! -s "$tmp/legacy.log" ]
    [ ! -e "$tmp/legacy.runtime" ]
done

for output_option in -o --output; do
    name=$(printf '%s' "$output_option" | tr -d '-')
    log="$tmp/output-$name.log"
    runtime_file="$tmp/output-$name.runtime"
    stderr_file="$tmp/output-$name.stderr"
    if PIX_TERMINUX_LOG="$log" PIX_TEST_RUNTIME_FILE="$runtime_file" \
        PIX_TEST_USB_FD=7 TMPDIR="$tmp" \
        "$tmp/stage/packaging/pix-termux" --firmware "$PIX_TEST_FIRMWARE_PATH" \
        --receiver 0 --channel T27 "$output_option" -V 7 2>"$stderr_file"; then
        status=0
    else
        status=$?
    fi
    [ "$status" -eq 0 ]
    runtime=$(cat "$runtime_file")
    grep -Fqx "pix-ts --runtime-dir $runtime --instance pix-fd --receiver 0 --channel T27 $output_option -V" "$log"
    grep -Fqx "pix-ts-arg=$output_option" "$log"
    grep -Fqx 'pix-ts-arg=-V' "$log"
    [ ! -e "$runtime" ]
done

run_metadata_case()
{
    name=$1
    shift
    log="$tmp/metadata-$name.log"
    runtime_file="$tmp/metadata-$name.runtime"
    PIX_TERMINUX_LOG="$log" PIX_TEST_RUNTIME_FILE="$runtime_file" \
        PIX_TEST_USB_FD=7 TMPDIR="$tmp" \
        "$tmp/stage/packaging/pix-termux" --firmware "$PIX_TEST_FIRMWARE_PATH" \
        "$@" 7 >"$tmp/metadata-$name.stdout" 2>"$tmp/metadata-$name.stderr"
    grep -Fq 'usage:' "$tmp/metadata-$name.stdout"
    test ! -s "$log"
    test ! -e "$runtime_file"
}

run_metadata_case device --device --output -h
run_metadata_case channel --channel --output -h

log="$tmp/metadata-both.log"
PIX_TERMINUX_LOG="$log" PIX_TEST_RUNTIME_FILE="$tmp/metadata-both.runtime" \
    PIX_TEST_USB_FD=7 TMPDIR="$tmp" \
    "$tmp/stage/packaging/pix-termux" --version --help 7 \
    >"$tmp/metadata-both.stdout" 2>"$tmp/metadata-both.stderr"
grep -Fq 'usage:' "$tmp/metadata-both.stdout"
test ! -s "$log"
test ! -e "$tmp/metadata-both.runtime"

log="$tmp/version.log"
PIX_TERMINUX_LOG="$log" PIX_TEST_RUNTIME_FILE="$tmp/version.runtime" \
    PIX_TEST_USB_FD=7 TMPDIR="$tmp" \
    "$tmp/stage/packaging/pix-termux" --version 7 \
    >"$tmp/version.stdout" 2>"$tmp/version.stderr"
grep -Eq '^pix-termux [0-9]+\.[0-9]+\.[0-9]+$' "$tmp/version.stdout"
test ! -s "$log"
test ! -e "$tmp/version.runtime"

printf 'KEEP\n' > "$tmp/output-sentinel.ts"
run_invalid_case()
{
    name=$1
    shift
    log="$tmp/invalid-$name.log"
    runtime_file="$tmp/invalid-$name.runtime"
    status=0
    PIX_TERMINUX_LOG="$log" PIX_TEST_RUNTIME_FILE="$runtime_file" \
        PIX_TEST_USB_FD=7 TMPDIR="$tmp" \
        "$tmp/stage/packaging/pix-termux" --firmware "$PIX_TEST_FIRMWARE_PATH" \
        "$@" 7 >"$tmp/invalid-$name.stdout" 2>"$tmp/invalid-$name.stderr" || status=$?
    [ "$status" -eq 2 ]
    test ! -s "$log"
    test ! -e "$runtime_file"
    [ "$(cat "$tmp/output-sentinel.ts")" = KEEP ]
}

run_invalid_case missing-receiver --channel T27 --packet-count 1
run_invalid_case duplicate-receiver --receiver 0 --channel T27 --packet-count 1 --receiver 1
run_invalid_case duplicate-output --receiver 0 --channel T27 --packet-count 1 \
    --output "$tmp/output-sentinel.ts" --output "$tmp/output-sentinel.ts"

log="$tmp/output-h.log"
PIX_TERMINUX_LOG="$log" PIX_TEST_RUNTIME_FILE="$tmp/output-h.runtime" \
    PIX_TEST_USB_FD=7 TMPDIR="$tmp" \
    "$tmp/stage/packaging/pix-termux" --firmware "$PIX_TEST_FIRMWARE_PATH" \
    --receiver 0 --channel T27 --packet-count 1 --output -h 7 \
    >"$tmp/output-h.stdout" 2>"$tmp/output-h.stderr"
grep -Fqx 'pix-ts-arg=-h' "$log"

if find "$tmp" -maxdepth 1 -type d -name 'pix-termux.*' | grep . >/dev/null; then
    echo 'legacy option created launcher runtime state' >&2
    exit 1
fi
if find "$tmp" -maxdepth 1 -type d -name 'pix-termux-validation-*' | grep . >/dev/null; then
    echo 'argument validation created runtime state' >&2
    exit 1
fi

signal_case()
{
    signal_name=$1 mode=$2
    python3 - "$tmp/stage/packaging/pix-termux" "$signal_name" "$mode" "$tmp" <<'PY'
import os
import signal
import subprocess
import sys
import time

launcher, signal_name, mode, tmp = sys.argv[1:]
log = os.path.join(tmp, mode + ".log")
runtime_file = os.path.join(tmp, mode + ".runtime")
stderr_file = os.path.join(tmp, mode + ".stderr")
open(log, "w").close()
if os.path.exists(runtime_file):
    os.unlink(runtime_file)
environment = os.environ.copy()
environment.update({
    "PIX_TERMINUX_LOG": log,
    "PIX_TEST_RUNTIME_FILE": runtime_file,
    "PIX_TEST_USB_FD": "7",
    "PIX_TEST_FIRMWARE_PATH": "./firmware/DT181 バンドル.bin",
    "PIX_TEST_CLIENT_MODE": mode,
    "TMPDIR": tmp,
})
with open(stderr_file, "w") as stderr:
    process = subprocess.Popen(
        [launcher, "--firmware", environment["PIX_TEST_FIRMWARE_PATH"],
         "--receiver", "0", "--channel", "T27", "7"],
        stderr=stderr,
        env=environment,
        pass_fds=(7,),
        preexec_fn=lambda: signal.signal(signal.SIGINT, signal.SIG_DFL),
        start_new_session=True,
    )
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        try:
            with open(log) as calls:
                if any(line.startswith("client-ready ") for line in calls):
                    break
        except FileNotFoundError:
            pass
        if process.poll() is not None:
            raise SystemExit(mode + ": launcher exited before client became ready")
        time.sleep(0.05)
    else:
        process.send_signal(signal.SIGTERM)
        process.wait(timeout=3)
        raise SystemExit(mode + ": client did not become ready")
    process.send_signal(getattr(signal, "SIG" + signal_name))
    status = process.wait(timeout=5)
if status != 0:
    with open(log) as calls:
        sys.stderr.write(calls.read())
    with open(stderr_file) as errors:
        sys.stderr.write(errors.read())
    raise SystemExit(mode + ": launcher exit %d, expected graceful exit 0" % status)
with open(log) as calls:
    output = calls.read()
if mode == "delayed-stop" and "client-stop\n" not in output:
    raise SystemExit(mode + ": client stop handler was not observed")
if "daemon-stop\n" not in output:
    raise SystemExit(mode + ": daemon stop handler was not observed")
if "daemon-fd-at-stop=present\n" not in output:
    raise SystemExit(mode + ": launcher callback FD was closed before daemon quiescence")
with open(runtime_file) as runtime:
    if os.path.exists(runtime.read().strip()):
        raise SystemExit(mode + ": runtime directory was not removed")
PY
}

signal_case TERM delayed-stop
signal_case TERM ignore-term-then-exit
signal_case INT delayed-stop

PIX_TEST_FIRMWARE_PATH='./firmware/DT295 bundle.bin'
if PIX_TERMINUX_LOG="$tmp/fail-rm.log" PIX_TEST_RUNTIME_FILE="$tmp/fail-rm.runtime" \
    PIX_TEST_USB_FD=7 PIX_TEST_FAIL_RM=1 PATH="$tmp/bin:$PATH" TMPDIR="$tmp" \
    "$tmp/stage/packaging/pix-termux" --firmware "$PIX_TEST_FIRMWARE_PATH" \
    --receiver 0 --channel T27 7 \
    2> "$tmp/fail-rm.stderr"; then
    echo 'runtime cleanup failure unexpectedly returned success' >&2
    exit 1
fi

if PIX_TERMINUX_LOG="$tmp/invalid.log" PIX_TEST_RUNTIME_FILE="$tmp/invalid.runtime" \
    PIX_TEST_USB_FD=7 TMPDIR="$tmp" PIX_TEST_FIRMWARE_PATH="$PIX_TEST_FIRMWARE_PATH" \
    "$tmp/stage/packaging/pix-termux" --firmware "$PIX_TEST_FIRMWARE_PATH" --channel T27 invalid >/dev/null 2>&1; then
    echo 'invalid descriptor unexpectedly accepted' >&2
    exit 1
fi
test ! -s "$tmp/invalid.log"
if PIX_TERMINUX_LOG="$tmp/missing-firmware.log" PIX_TEST_RUNTIME_FILE="$tmp/missing-firmware.runtime" \
    PIX_TEST_USB_FD=7 TMPDIR="$tmp" \
    "$tmp/stage/packaging/pix-termux" --channel T27 7 >/dev/null 2>&1; then
    echo 'missing firmware argument unexpectedly accepted' >&2
    exit 1
fi
test ! -s "$tmp/missing-firmware.log"
echo 'Termux callback daemon/client supervisor: PASS'
