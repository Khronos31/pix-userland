#!/usr/bin/env python3
import hashlib
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FAKE = ROOT / "tests/pix_session_fake.c"
BASELINE = ROOT / "tests/pix_session_baseline.json"
CASES = {
    "normal": ([], {}),
    "signal": ([], {"PIX_FAULT": "signal"}),
    "control-quit": (["--control"], {"PIX_FAULT": "control-quit"}),
    "control-eof": (["--control"], {"PIX_FAULT": "control-eof"}),
    "retune": (["--control"], {"PIX_FAULT": "control-retune"}),
    "retune-accumulated-error": (["--control"], {"PIX_FAULT": "control-retune,decoder"}),
    "retune-stop-short-circuit": (["--control"], {"PIX_FAULT": "control-retune,fail-device-stop", "PIX_DEVICE_EXIT": "8"}),
    "card-absent": ([], {"PIX_FAULT": "card-absent"}),
    "open-failure": ([], {"PIX_FAULT": "open"}),
    "power-failure": ([], {"PIX_FAULT": "fail-power", "PIX_DEVICE_EXIT": "10"}),
    "session-failure": ([], {"PIX_FAULT": "fail-session", "PIX_DEVICE_EXIT": "10"}),
    "tuner-failure": ([], {"PIX_FAULT": "fail-tuner", "PIX_DEVICE_EXIT": "7"}),
    "tune-failure": ([], {"PIX_FAULT": "fail-tune", "PIX_DEVICE_EXIT": "9"}),
    "device-usb-arbitration": ([], {"PIX_FAULT": "fail-tune,also-usb", "PIX_DEVICE_EXIT": "8"}),
    "status-failure": ([], {"PIX_FAULT": "fail-status", "PIX_DEVICE_EXIT": "8"}),
    "fifo-failure": ([], {"PIX_FAULT": "fail-fifo", "PIX_DEVICE_EXIT": "8"}),
    "stream-start-failure": ([], {"PIX_FAULT": "stream-start"}),
    "start-failure": ([], {"PIX_FAULT": "fail-start", "PIX_DEVICE_EXIT": "7"}),
    "worker-create-failure": ([], {"PIX_FAULT": "worker-create"}),
    "poll-failure": ([], {"PIX_FAULT": "poll"}),
    "certify-failure": ([], {"PIX_FAULT": "fail-certify", "PIX_DEVICE_EXIT": "7"}),
    "device-stop-failure": ([], {"PIX_FAULT": "fail-device-stop", "PIX_DEVICE_EXIT": "8"}),
    "usb-stop-failure": ([], {"PIX_FAULT": "usb-stop"}),
    "shutdown-failure": ([], {"PIX_FAULT": "fail-shutdown", "PIX_DEVICE_EXIT": "7"}),
    "poll-and-shutdown": ([], {"PIX_FAULT": "poll,fail-shutdown", "PIX_DEVICE_EXIT": "8"}),
    "usb-priority": ([], {"PIX_FAULT": "poll,fail-device-stop,usb-stop", "PIX_DEVICE_EXIT": "8"}),
    "queue-rejection": ([], {"PIX_FAULT": "queue-error"}),
    "decoder-error": ([], {"PIX_FAULT": "decoder"}),
}


def build(output, work, main_source=None):
    main_source = main_source or ROOT / "src/main.c"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Wpedantic",
               "-D_POSIX_C_SOURCE=200809L", "-D_FILE_OFFSET_BITS=64", f"-I{ROOT / 'include'}",
               str(main_source)]
    if Path(main_source).resolve() == (ROOT / "src/main.c").resolve():
        command.append(str(ROOT / "src/dt181_session.c"))
    command += [str(FAKE), "-Wl,--wrap=pthread_create", "-Wl,--wrap=pthread_join",
                "-Wl,--wrap=pthread_cond_destroy", "-Wl,--wrap=pthread_mutex_destroy",
               "-o", str(output)]
    subprocess.run(command, cwd=work, check=True, capture_output=True, text=True)


def run(binary, fixture, scenario, timeout_seconds=10):
    args, extra = CASES[scenario]
    env = os.environ.copy()
    env.update(extra)
    timed_args = [] if "--control" in args else ["--time", "1"]
    try:
        result = subprocess.run([str(binary), "--assets", str(fixture), "--channel", "27",
                                 *timed_args, "--output", "/dev/null", *args],
                                cwd=fixture.parent, env=env, capture_output=True, text=True,
                                timeout=timeout_seconds)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"{scenario}: fake actual-main case exceeded {timeout_seconds}s; child was terminated") from exc
    return {"exit": result.returncode, "stdout": result.stdout, "stderr": result.stderr}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline-main", type=Path)
    options = parser.parse_args()
    build_root = ROOT.parent / "build-pix-luna-harness"
    build_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="run-", dir=build_root) as temp:
        work = Path(temp)
        fixture = work / "synthetic-fixtures"
        fixture.mkdir()
        for name, size in (("fpga.bin", 699673), ("bootstrap.bin", 16), ("validation.bin", 64)):
            (fixture / name).write_bytes(bytes((i % 251 for i in range(size))))
        binary = work / "pix-main-current"
        build(binary, work)
        after = {name: run(binary, fixture, name) for name in CASES}
        if options.baseline_main:
            old_binary = work / "pix-main-fixed-source"
            build(old_binary, work, options.baseline_main)
            before = {name: run(old_binary, fixture, name) for name in CASES}
            for name in CASES:
                if before[name] != after[name]:
                    raise SystemExit(f"{name}: actual old/new main traces differ\nold={before[name]}\nnew={after[name]}")
            (build_root / "baseline-traces-followup.json").write_text(
                json.dumps(before, indent=2, sort_keys=True) + "\n")
            (build_root / "candidate-traces-followup.json").write_text(
                json.dumps(after, indent=2, sort_keys=True) + "\n")
        expected = json.loads(BASELINE.read_text())
        actual = {name: {"exit": result["exit"],
                         "stderr_sha256": hashlib.sha256(result["stderr"].encode()).hexdigest()}
                  for name, result in after.items()}
        if expected != actual:
            raise SystemExit(f"actual-main output differs from fixed-source baseline\nexpected={expected}\nactual={actual}")
        if any(result["stdout"] for result in after.values()):
            raise SystemExit("synthetic main unexpectedly wrote to stdout")
        saved_candidate = build_root / "candidate-traces-followup.json"
        saved_candidate.write_text(json.dumps(after, indent=2, sort_keys=True) + "\n")
        retune_trace = after["retune"]["stderr"]
        if retune_trace.count("TRACE pixy_status") != 1:
            raise SystemExit("retune unexpectedly changed initial status-query count")
        if retune_trace.count("TRACE pixy_tune_slot") != 2:
            raise SystemExit("successful retune did not tune exactly once after initial tune")
        if after["status-failure"]["exit"] != 8 or "fake status failure" not in after["status-failure"]["stderr"]:
            raise SystemExit("status failure was not exercised with the expected error classification")
        for name, expected_context in (("power-failure", "pixy_power context=10"),
                                       ("session-failure", "pixy_session context=10"),
                                       ("tuner-failure", "pixy_tuner_init context=7")):
            if f"TRACE {expected_context}" not in after[name]["stderr"]:
                raise SystemExit(f"{name}: failure context changed")
        if after["retune-accumulated-error"]["stderr"].count("TRACE pixy_tune_slot") != 1:
            raise SystemExit("retune cleared a queued decoder failure before checking it")
        short = after["retune-stop-short-circuit"]["stderr"]
        if short.count("TRACE pixy_stop") != 1 or short.count("TRACE usb_stream_stop") != 1:
            raise SystemExit("retune stop did not short-circuit USB stop before final cleanup")
        arbitration = after["device-usb-arbitration"]
        if arbitration["exit"] != 7 or "TRACE tune" not in arbitration["stderr"]:
            raise SystemExit("simultaneous USB/device failure did not retain USB-first classification")
        events = [line for line in after["normal"]["stderr"].splitlines() if line.startswith("TRACE ")]
        ordered = [next(i for i, line in enumerate(events) if line.startswith(prefix))
                   for prefix in ("TRACE pixy_tune_slot", "TRACE pixy_status", "TRACE pixy_fifo_clear",
                                  "TRACE usb_stream_start", "TRACE pixy_start")]
        if ordered != sorted(ordered):
            raise SystemExit("initial tune/status or capture prepare operation order changed")
        if after["card-absent"]["stderr"].find("TRACE pixy_status") < 0 or "TRACE usb_stream_start" in after["card-absent"]["stderr"]:
            raise SystemExit("initial card absence did not exit before stream preparation")
        if "TRACE pixy_start" in after["fifo-failure"]["stderr"] or "TRACE pixy_start" in after["stream-start-failure"]["stderr"]:
            raise SystemExit("prepare did not preserve short-circuit behavior")
        cleanup_error = after["device-stop-failure"]["stderr"]
        if "TRACE pixy_stop" not in cleanup_error or "TRACE usb_stream_stop" not in cleanup_error:
            raise SystemExit("final cleanup did not try USB stop after hardware stop failure")
        required = ["TRACE final_enqueue", "TRACE worker_join", "TRACE pixy_shutdown", "TRACE usb_close",
                    "TRACE queue_cond_destroy", "TRACE queue_mutex_destroy"]
        positions = [events.index(event) for event in required]
        if positions != sorted(positions):
            raise SystemExit("cleanup callback/worker/device/USB order changed")
        report = after["normal"]["stderr"].find('{"packets"')
        if report < after["normal"]["stderr"].index("TRACE worker_join") or report > after["normal"]["stderr"].index("TRACE pixy_shutdown"):
            raise SystemExit("cleanup report moved outside worker join and device shutdown")
        print(f"PASS {len(CASES)} actual-main traces matched fixed-source baseline; candidate: {saved_candidate}")


if __name__ == "__main__":
    main()
