#!/usr/bin/env python3
import hashlib
import json
import os
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "tests/test_siano_sms_characterization.c"


def build(source, output):
    include = source.parent
    sanitizer = os.environ.get("SANITIZE")
    sanitizer_flags = ["-g", f"-fsanitize={sanitizer}"] if sanitizer else []
    usb_cflags = subprocess.run(["pkg-config", "--cflags", "libusb-1.0"],
                                check=True, capture_output=True, text=True).stdout.split()
    result = subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Wpedantic",
        *sanitizer_flags,
        "-D_POSIX_C_SOURCE=200809L", "-D_FILE_OFFSET_BITS=64",
        "-ffunction-sections", "-fdata-sections", *usb_cflags, f"-I{include}",
        str(DRIVER), str(source), str(include / "protocol.c"),
        str(include / "stream-state.c"), str(include / "queue-policy.c"),
        str(include / "output-writer.c"), "-Wl,--gc-sections",
        "-pthread", "-o", str(output),
    ], check=False, capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(result.stderr)


def main():
    parser = argparse.ArgumentParser(description="Report-only old/new Siano characterization")
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    baseline = args.baseline.resolve()
    output_path = args.output.resolve()
    with tempfile.TemporaryDirectory(prefix="siano-c-capture-") as temp:
        root = Path(temp)
        outputs = {}
        for label, source in (("old", baseline / "siano-device.c"),
                              ("new", ROOT / "siano-device.c")):
            binary = root / label
            build(source, binary)
            result = subprocess.run([str(binary)], capture_output=True,
                                     timeout=10, check=False)
            outputs[label] = {
                "exit": result.returncode,
                "stdout": result.stdout,
                "stderr": result.stderr.decode(errors="replace"),
            }
            if result.returncode != 0:
                raise SystemExit(f"{label} characterization driver failed: {outputs[label]}")
        if outputs["old"] != outputs["new"]:
            raise SystemExit(f"actual SMS old/new characterization differs: {outputs}")
        stdout = outputs["old"]["stdout"].decode().strip()
        trace, packet_hex = stdout.split(";bytes=", 1)
        evidence = {
            "scenario": "real start_streaming callback; 3 synthetic BDA packets split 187/377; actual queue and output scanner",
            "driver_sha256": hashlib.sha256(DRIVER.read_bytes()).hexdigest(),
            "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "old_device_sha256": hashlib.sha256((baseline / "siano-device.c").read_bytes()).hexdigest(),
            "new_device_sha256": hashlib.sha256((ROOT / "siano-device.c").read_bytes()).hexdigest(),
            "result": {
                "exit": outputs["old"]["exit"],
                "trace": trace,
                "packet_bytes": len(bytes.fromhex(packet_hex)),
                "packet_sha256": hashlib.sha256(bytes.fromhex(packet_hex)).hexdigest(),
                "stderr": outputs["old"]["stderr"],
            },
        }
        output_path.write_text(json.dumps(evidence, indent=2) + "\n")
        print(f"PASS actual SMS old/new callback and packet traces matched; evidence: {output_path}")


if __name__ == "__main__":
    main()
