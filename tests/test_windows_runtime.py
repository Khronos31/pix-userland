"""Windows package CLI and synthetic public-runtime proof; never opens real USB."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time


def run(command, expected=0):
    result = subprocess.run([str(value) for value in command], capture_output=True, timeout=20)
    assert result.returncode == expected, (command, result.returncode, result.stdout, result.stderr)
    return result


def wait_ready(process, log):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline and process.poll() is None:
        if "pixd ready:" in log.read_text(encoding="utf-8", errors="replace"):
            return
        time.sleep(0.02)
    raise AssertionError(log.read_text(encoding="utf-8", errors="replace"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("proofs", type=Path)
    args = parser.parse_args()
    assert os.name == "nt", "This proof requires a Windows runner"
    proofs = args.proofs.resolve()
    root_source = Path(__file__).resolve().parents[1]
    vectors = (root_source / "core-integration/tests/pix-runtime-vectors.h").read_text()
    plain = re.search(r"static const uint8_t plain\[\] = \{(.*?)\};", vectors, re.S).group(1)
    expected = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", plain))
    with tempfile.TemporaryDirectory(prefix="pix-win-") as temporary:
        root = Path(temporary)
        run(["tar", "-xzf", args.archive.resolve(), "-C", root])
        package = next(root.glob("pix-userland-*-windows-x64"))
        metadata = json.loads((package / "release-info.json").read_text())
        assert metadata["private"] is False and metadata["device_assets"] is False
        for name in ("pixd", "pixctl", "pix-ts"):
            program = package / (name + ".exe")
            assert metadata["version"].encode() in run([program, "--version"]).stdout
            run([program, "--help"])
            run([program, "--unknown-option"], expected=2)
        run([proofs / "pix_windows_os_thread_proof.exe"])
        assets = root / "素材 日本語"
        assets.mkdir()
        for name, size in (("fpga.bin", 699673), ("bootstrap.bin", 16), ("validation.bin", 64)):
            (assets / name).write_bytes((bytes(range(256)) * (size // 256 + 1))[:size])
        runtime = root / "r"
        fixture = proofs / "pixd_fixture.exe"
        command = [str(fixture), "--device", "serial:PIXDT18101", "--assets", str(assets),
                   "--runtime-dir", str(runtime), "--instance", "proof", "--exit-on-stdin-eof"]
        for attempt in range(2):
            log = root / f"daemon-{attempt}.log"
            with log.open("wb") as output:
                process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=output, stderr=output)
                try:
                    wait_ready(process, log)
                    status = run([package / "pixctl.exe", "--runtime-dir", runtime,
                                  "--instance", "proof", "status"])
                    assert json.loads(status.stdout)["product"] == "pix-userland"
                    if attempt == 0:
                        destination = root / "受信 日本語.ts"
                        run([package / "pix-ts.exe", "--instance", "proof",
                             "--freq", "473142857", "--time", "1", "--output", destination,
                             "--runtime-dir", runtime])
                        assert destination.read_bytes() == expected, "TS binary output differs"
                    process.stdin.close()
                    assert process.wait(timeout=10) == 0, log.read_text(errors="replace")
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=5)
            text = log.read_text(encoding="utf-8", errors="replace")
            assert "PIX_CLEANUP release-interface=" in text and "PIX_CLEANUP close-handle=" in text
            assert not list(runtime.iterdir()), "Runtime namespace remains after EOF cleanup"
        print("Windows package CLI/arguments/UTF-8 assets/binary TS/runtime/EOF/cleanup/restart: PASS")


if __name__ == "__main__":
    main()
