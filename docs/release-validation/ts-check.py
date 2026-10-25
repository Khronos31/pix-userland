#!/usr/bin/env python3
"""Check MPEG-TS packet framing, transport errors, and payload continuity."""

import argparse
import json
from pathlib import Path


def inspect(path):
    data = Path(path).read_bytes()
    sync_errors = sum(data[offset] != 0x47 for offset in range(0, len(data) - 187, 188))
    remainder = len(data) % 188
    tei = 0
    continuity_errors = 0
    previous = {}
    packets = len(data) // 188

    for offset in range(0, len(data) - 187, 188):
        packet = data[offset:offset + 188]
        if packet[0] != 0x47:
            continue
        pid = ((packet[1] & 0x1F) << 8) | packet[2]
        tei += bool(packet[1] & 0x80)
        adaptation = (packet[3] >> 4) & 0x03
        has_payload = adaptation in (1, 3)
        discontinuity = adaptation in (2, 3) and packet[4] > 0 and bool(packet[5] & 0x80)
        if discontinuity:
            previous.pop(pid, None)
        if has_payload:
            counter = packet[3] & 0x0F
            if pid in previous and counter != ((previous[pid] + 1) & 0x0F):
                continuity_errors += 1
            previous[pid] = counter

    return {
        "file": str(path),
        "bytes": len(data),
        "packets": packets,
        "remainder": remainder,
        "sync_errors": sync_errors,
        "tei": tei,
        "continuity_errors": continuity_errors,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ts_file")
    args = parser.parse_args()
    result = inspect(args.ts_file)
    print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    if result["packets"] == 0:
        return 2
    return int(any(result[key] for key in ("remainder", "sync_errors", "tei", "continuity_errors")))


if __name__ == "__main__":
    raise SystemExit(main())
