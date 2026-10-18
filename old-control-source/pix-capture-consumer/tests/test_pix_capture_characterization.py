#!/usr/bin/env python3
import hashlib
import json
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SHIM = ROOT / "tests/pix_capture_characterization_shim.c"


def build(main, include, output):
    crypto = output.with_suffix(".crypto.o")
    subprocess.run([
        "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", f"-I{include}",
        "-Dsecure_clear=pix_crypto_secure_clear", "-c",
        str(ROOT / "src/crypto.c"), "-o", str(crypto),
    ], check=True, capture_output=True, text=True)
    result = subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Wpedantic",
        "-D_POSIX_C_SOURCE=200809L", "-D_FILE_OFFSET_BITS=64",
        f"-I{include}", str(main), str(include.parent / "src/dt181_session.c"),
        str(SHIM), str(ROOT / "src/transport.c"), str(crypto),
        "-Wl,--wrap=pthread_create", "-Wl,--wrap=pthread_join",
        "-Wl,--wrap=pthread_cond_destroy", "-Wl,--wrap=pthread_mutex_destroy",
        "-Wl,--wrap=write",
        "-o", str(output),
    ], check=False, capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(result.stderr)


def main():
    parser = argparse.ArgumentParser(description="Report-only old/new PIX characterization")
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    baseline = args.baseline.resolve()
    output_path = args.output.resolve()
    with tempfile.TemporaryDirectory(prefix="pix-c-capture-") as temp:
        root = Path(temp)
        assets = root / "assets"
        assets.mkdir()
        for name, size in (("fpga.bin", 699673), ("bootstrap.bin", 16), ("validation.bin", 64)):
            (assets / name).write_bytes(bytes(i % 251 for i in range(size)))
        old_bin, new_bin = root / "old", root / "new"
        build(baseline / "src/main.c", baseline / "include", old_bin)
        build(ROOT / "src/main.c", ROOT / "include", new_bin)
        results = {}
        for label, binary in (("old", old_bin), ("new", new_bin)):
            output = root / f"{label}.ts"
            result = subprocess.run([
                str(binary), "--assets", str(assets), "--channel", "27",
                "--time", "1", "--output", str(output),
            ], capture_output=True, timeout=10, check=False)
            packets = output.read_bytes()
            if len(packets) != 3 * 188:
                raise SystemExit(f"{label}: decoder output length was {len(packets)}, expected 564")
            results[label] = {
                "exit": result.returncode,
                "stderr_sha256": hashlib.sha256(result.stderr).hexdigest(),
                "output_sha256": hashlib.sha256(packets).hexdigest(),
                "output_hex": packets.hex(),
                "fd_write_batches": [
                    int(line.rsplit("=", 1)[1])
                    for line in result.stderr.decode().splitlines()
                    if line.startswith("TRACE fd_write length=")
                ],
            }
            if result.stdout:
                raise SystemExit(f"{label}: unexpected stdout")
        if results["old"] != results["new"]:
            raise SystemExit("old/new actual decoder capture differed")
        evidence = {
            "scenario": "3 synthetic clear TS packets split at byte 187",
            "driver_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "shim_sha256": hashlib.sha256(SHIM.read_bytes()).hexdigest(),
            "old_main_sha256": hashlib.sha256((baseline / "src/main.c").read_bytes()).hexdigest(),
            "new_main_sha256": hashlib.sha256((ROOT / "src/main.c").read_bytes()).hexdigest(),
            "result": results["old"],
        }
        output_path.write_text(json.dumps(evidence, indent=2) + "\n")
        print(f"PASS actual ts_decoder_feed old/new bytes and stderr matched; evidence: {output_path}")


if __name__ == "__main__":
    main()
