#!/usr/bin/env python3
"""Check MPEG-TS framing, transport errors, and payload continuity."""

import argparse
import json
from pathlib import Path


def equivalent_duplicate(previous, packet, has_pcr):
    if not has_pcr:
        return previous == packet
    return (
        previous[:6] == packet[:6]
        and (previous[10] & 0x7e) == (packet[10] & 0x7e)
        and previous[12:] == packet[12:]
    )


def inspect(path, excluded_cc):
    data = Path(path).read_bytes()
    sync_errors = sum(data[offset] != 0x47 for offset in range(0, len(data) - 187, 188))
    remainder = len(data) % 188
    tei = 0
    invalid_packets = 0
    continuity_error_indices = []
    duplicate_packet_indices = []
    null_packet_indices = []
    invalid_packet_indices = []
    excluded_seen = []
    previous = {}
    packets = len(data) // 188
    untrusted_epoch = 0

    for index, offset in enumerate(range(0, len(data) - 187, 188)):
        packet = data[offset:offset + 188]
        if packet[0] != 0x47:
            untrusted_epoch += 1
            continue
        pid = ((packet[1] & 0x1F) << 8) | packet[2]
        if packet[1] & 0x80:
            tei += 1
            untrusted_epoch += 1
            continue
        if pid == 0x1FFF:
            null_packet_indices.append(index)
            continue
        adaptation = (packet[3] >> 4) & 0x03
        if adaptation == 0:
            invalid_packets += 1
            invalid_packet_indices.append(index)
            untrusted_epoch += 1
            continue
        has_payload = adaptation in (1, 3)
        discontinuity = adaptation in (2, 3) and packet[4] > 0 and bool(packet[5] & 0x80)
        has_pcr = False
        if adaptation in (2, 3):
            length = packet[4]
            if (adaptation == 2 and length != 183) or (adaptation == 3 and length > 182):
                invalid_packets += 1
                invalid_packet_indices.append(index)
                untrusted_epoch += 1
                continue
            if length > 0:
                has_pcr = bool(packet[5] & 0x10)
                if has_pcr:
                    extension = ((packet[10] & 1) << 8) | packet[11]
                    if length < 7 or (packet[10] & 0x7e) != 0x7e or extension >= 300:
                        invalid_packets += 1
                        invalid_packet_indices.append(index)
                        untrusted_epoch += 1
                        continue

        state = previous.get(pid)
        if not discontinuity and state is not None:
            counter = packet[3] & 0x0F
            expected = (state["counter"] + (1 if has_payload else 0)) & 0x0F
            repeated = has_payload and counter == state["counter"]
            if repeated and state["has_payload"] and not state["repeated"] and state["epoch"] == untrusted_epoch and equivalent_duplicate(state["packet"], packet, has_pcr):
                duplicate_packet_indices.append(index)
            elif counter != expected:
                if index in excluded_cc:
                    excluded_seen.append(index)
                else:
                    continuity_error_indices.append(index)
        repeated = state is not None and has_payload and (packet[3] & 0x0F) == state["counter"]
        previous[pid] = {
            "counter": packet[3] & 0x0F,
            "has_payload": has_payload,
            "repeated": repeated,
            "epoch": untrusted_epoch,
            "packet": packet,
        }

    return {
        "file": str(path),
        "bytes": len(data),
        "packets": packets,
        "remainder": remainder,
        "sync_errors": sync_errors,
        "tei": tei,
        "invalid_packets": invalid_packets,
        "invalid_packet_indices": invalid_packet_indices,
        "null_packet_indices": null_packet_indices,
        "duplicate_packet_indices": duplicate_packet_indices,
        "continuity_errors": len(continuity_error_indices),
        "continuity_error_indices": continuity_error_indices,
        "excluded_cc_indices": sorted(excluded_cc),
        "excluded_cc_mismatches": excluded_seen,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ts_file")
    parser.add_argument(
        "--exclude-cc-at", type=int, action="append", default=[], metavar="PACKET_INDEX",
        help="exclude continuity comparison only at this zero-based packet index",
    )
    args = parser.parse_args()
    if len(set(args.exclude_cc_at)) != len(args.exclude_cc_at) or any(i < 0 for i in args.exclude_cc_at):
        parser.error("CC exclusion indices must be unique and nonnegative")
    result = inspect(args.ts_file, set(args.exclude_cc_at))
    if any(i >= result["packets"] for i in args.exclude_cc_at):
        parser.error("CC exclusion index is outside the TS packet range")
    print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    if result["packets"] == 0:
        return 2
    return int(any(result[key] for key in ("remainder", "sync_errors", "tei", "invalid_packets", "continuity_errors")))


if __name__ == "__main__":
    raise SystemExit(main())
