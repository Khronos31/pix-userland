#!/usr/bin/env python3
"""Build a private, integrity-checked DT295 runtime bundle from user files."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile

DRIVER_SHA256 = "46889d2f8b046485a0cc25451c1691ca87b13601d0566c807ee6e1c9084fc878"
MAT_BASE = 0x841D0
MAT_END = 0x85450
IMAGE_LIMIT = 16 * 1024 * 1024
FIELD_LIMIT = 4096


def u16(value):
    return struct.pack("<H", value)


def extract_driver_material(path):
    raw = Path(path).read_bytes()
    if hashlib.sha256(raw).hexdigest() != DRIVER_SHA256:
        raise ValueError("unsupported official driver SHA-256")
    if len(raw) < 0x40 or raw[:2] != b"MZ":
        raise ValueError("driver is not a PE image")
    pe = struct.unpack_from("<I", raw, 0x3C)[0]
    if pe + 24 > len(raw) or raw[pe:pe + 4] != b"PE\0\0":
        raise ValueError("invalid PE header")
    sections = struct.unpack_from("<H", raw, pe + 6)[0]
    optional_size = struct.unpack_from("<H", raw, pe + 20)[0]
    optional = pe + 24
    if optional + optional_size + sections * 40 > len(raw) or optional_size < 32:
        raise ValueError("truncated PE section table")
    magic = struct.unpack_from("<H", raw, optional)[0]
    if magic != 0x20B:
        raise ValueError("official driver must be PE32+")
    image_base = struct.unpack_from("<Q", raw, optional + 24)[0]
    section_rows = []
    for i in range(sections):
        off = optional + optional_size + 40 * i
        virtual_size, rva, raw_size, raw_pos = struct.unpack_from("<4I", raw, off + 8)
        section_rows.append((rva, max(virtual_size, raw_size), raw_size, raw_pos))

    def read_va(va, size):
        rva = va - image_base
        for start, virtual_size, raw_size, raw_pos in section_rows:
            delta = rva - start
            if 0 <= delta and delta + size <= min(virtual_size, raw_size):
                pos = raw_pos + delta
                if pos + size <= len(raw):
                    return raw[pos:pos + size]
        raise ValueError(f"required material at VA {va:x} is outside driver sections")

    material = bytearray(MAT_END - MAT_BASE)
    spans = ((0x841D0, 22 * 8), (0x845A0, 18 * 8), (0x84630, 26 * 8), (0x84700, 28 * 8),
             (0x85148, 30), (0x85168, 12), (0x85178, 28),
             (0x85198, 16), (0x851A8, 16), (0x851B8, 16), (0x851C8, 16),
             (0x851D8, 16), (0x851E8, 16), (0x851F8, 16), (0x85208, 16),
             (0x85218, 16), (0x85230, 16 * 32))
    for va, size in spans:
        off = va - MAT_BASE
        material[off:off + size] = read_va(va, size)
    return bytes(material)


def parse_profile(path):
    profile = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(profile, dict):
        raise ValueError("profile must be a JSON object")
    if type(profile.get("schema")) is not int or profile.get("schema") != 1 or type(profile.get("target")) is not int or profile.get("target") != 0:
        raise ValueError("profile must have schema 1 and target 0")
    words = profile.get("ts_key_request_words")
    if not isinstance(words, list) or len(words) != 3 or any(type(x) is not int or not 0 <= x <= 0xFFFFFFFF for x in words):
        raise ValueError("ts_key_request_words must contain three uint32 values")
    rows = profile.get("fields")
    if not isinstance(rows, list) or not rows or len(rows) > FIELD_LIMIT:
        raise ValueError("invalid TRC field list")
    fields = []
    normalized = {
        "INIT_S_AUTO_PID_MODE": 0,
        "INIT_S_PAYLOAD_UNIT_START_DISABLE_MODE": 1,
    }
    seen_normalized = set()
    route_prefixes = ("INIT_S_V_PID_", "INIT_S_A_PID_", "INIT_S_PMT_PID_", "INIT_S_PCR_PID_",
                      "INIT_S_PID_", "INIT_S_ECM_")
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError("TRC fields must be objects")
        name = row.get("name", "")
        if not isinstance(name, str):
            raise ValueError("TRC field name must be a string")
        if name in ("INIT_S_PROGRAM_NUMBER", "INIT_S_V_STREAM_TYPE", "INIT_S_A_STREAM_TYPE",
                    "INIT_S_V_ECM", "INIT_S_A_ECM") or name.startswith(route_prefixes):
            continue
        a, shift, width, value = (row.get(k) for k in ("address", "shift", "width", "value"))
        if name in normalized:
            if name in seen_normalized:
                raise ValueError(f"profile contains duplicate {name}")
            seen_normalized.add(name)
            value = normalized[name]
        if any(type(v) is not int for v in (a, shift, width, value)):
            raise ValueError("TRC field values must be integers")
        if not 0x1000 <= a <= 0x1AFE or a & 1:
            raise ValueError("TRC field outside configuration registers")
        if not 0 <= shift < 16 or not 1 <= width <= 16 - shift or not 0 <= value < (1 << width):
            raise ValueError("TRC field bit range is invalid")
        fields.append(u16(a) + bytes((shift, width)) + u16(value) + b"\0\0")
    if seen_normalized != set(normalized):
        missing = sorted(set(normalized) - seen_normalized)
        raise ValueError("profile missing required normalized field(s): " + ", ".join(missing))
    if not fields:
        raise ValueError("profile has no route-independent TRC fields")
    return b"".join(fields), struct.pack(">3I", *words)


def load_profile_sections(path, manifest=None):
    if manifest is None:
        from firmware_bundle import read_manifest
        manifest = read_manifest()
    data = Path(path).read_bytes()
    from firmware_bundle import verify_bundle
    verify_bundle("dt295", data, manifest)
    spec = importlib.util.spec_from_file_location(
        "pix_finalize", Path(__file__).with_name("finalize-private-release.py"))
    finalize = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(finalize)
    sections = finalize.validate_dt295_bundle(data)
    return sections[3], sections[4], sections[5]


def section(data):
    return struct.pack("<I", len(data)) + hashlib.sha256(data).digest() + data


def build_bundle(idle, trc, material, fields, key_request, ts_iv):
    if not idle or len(idle) > IMAGE_LIMIT or not trc or len(trc) > IMAGE_LIMIT:
        raise ValueError("idle and TRC images must be 1..16 MiB")
    if len(material) != MAT_END - MAT_BASE:
        raise ValueError("driver material has an invalid size")
    if not fields or len(fields) % 8 or len(fields) > FIELD_LIMIT * 8:
        raise ValueError("TRC fields have an invalid size")
    if len(key_request) != 12 or len(ts_iv) != 16:
        raise ValueError("key request must be 12 bytes and TS IV must be 16 bytes")
    sections = [idle, trc, material, fields, key_request, ts_iv]
    bundle = b"D295A1\0\0" + b"".join(
        struct.pack("<I", len(data)) + hashlib.sha256(data).digest() for data in sections)
    return bundle + b"".join(sections)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--driver", required=True, help="official pxmb86m0x_usb.sys")
    ap.add_argument("--idle", required=True, help="official idle firmware image")
    ap.add_argument("--trc", required=True, help="official terrestrial TRC image")
    ap.add_argument("--profile", help="private target-0 profile JSON")
    ap.add_argument("--base-bundle", help="verified standard DT295 bundle to reuse normalized fields, key request and IV")
    ap.add_argument("--iv", help="16-byte IV extracted from the official application")
    ap.add_argument("--output", required=True, help="private firmware bundle output, usually firmware/dt295.bin")
    args = ap.parse_args()
    idle = Path(args.idle).read_bytes()
    trc = Path(args.trc).read_bytes()
    if bool(args.profile) == bool(args.base_bundle):
        raise ValueError("specify exactly one of --profile or --base-bundle")
    if args.base_bundle:
        if args.iv:
            raise ValueError("--iv cannot be used with --base-bundle")
        fields, key_request, iv = load_profile_sections(args.base_bundle)
    else:
        if not args.iv:
            raise ValueError("--iv is required with --profile")
        fields, key_request = parse_profile(args.profile)
        iv = Path(args.iv).read_bytes()
    if not 0 < len(idle) <= IMAGE_LIMIT or not 0 < len(trc) <= IMAGE_LIMIT:
        raise ValueError("idle and TRC images must be 1..16 MiB")
    if len(iv) != 16:
        raise ValueError("IV must contain exactly 16 bytes")
    material = extract_driver_material(args.driver)
    bundle = build_bundle(idle, trc, material, fields, key_request, iv)
    dest = Path(args.output)
    dest.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=".dt295-assets-", dir=dest.parent)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "wb") as f:
            f.write(bundle)
            f.flush()
            os.fsync(f.fileno())
        os.replace(temp_name, dest)
        os.chmod(dest, 0o600)
    except Exception:
        try:
            os.unlink(temp_name)
        except OSError:
            pass
        raise


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, json.JSONDecodeError, struct.error) as exc:
        raise SystemExit(f"prepare-dt295-assets: {exc}")
