#!/usr/bin/env python3
"""Add authorized private device assets to prepared binary packages."""
import argparse
import gzip
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import tarfile
import tempfile
from firmware_bundle import read_manifest, verify_bundle

_spec = importlib.util.spec_from_file_location("pix_package", Path(__file__).with_name("package-release.py"))
pack = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pack)

DT295_MAGIC = b"D295A1\0\0"
DT295_SECTION_COUNT = 6
DT295_HEADER_SIZE = 8 + DT295_SECTION_COUNT * 36
DT295_MATERIAL_SIZE = 0x1280
DT295_MAX_SECTION = 16 * 1024 * 1024
DT295_MAX_BUNDLE = 32 * 1024 * 1024
DT295_MAX_FIELDS = 4096


def validate_dt295_bundle(data):
    """Validate the D295A1 container, all section hashes and runtime constraints."""
    if not isinstance(data, bytes) or len(data) < DT295_HEADER_SIZE or data[:8] != DT295_MAGIC:
        raise ValueError("invalid DT295 asset bundle header")
    rows = []
    total = 0
    for i in range(DT295_SECTION_COUNT):
        off = 8 + i * 36
        length = struct.unpack_from("<I", data, off)[0]
        digest = data[off + 4:off + 36]
        total += length
        if total > DT295_MAX_BUNDLE or length > DT295_MAX_BUNDLE:
            raise ValueError("DT295 asset bundle size limit")
        rows.append((length, digest))
    sections = []
    cursor = DT295_HEADER_SIZE
    for length, digest in rows:
        end = cursor + length
        if end > len(data):
            raise ValueError("truncated DT295 asset section")
        section = data[cursor:end]
        if hashlib.sha256(section).digest() != digest:
            raise ValueError("DT295 asset section SHA-256 mismatch")
        sections.append(section)
        cursor = end
    if cursor != len(data):
        raise ValueError("trailing bytes in DT295 asset bundle")
    idle, trc, material, fields, key_request, iv = sections
    if not idle or len(idle) > DT295_MAX_SECTION or not trc or len(trc) > DT295_MAX_SECTION:
        raise ValueError("DT295 idle and TRC sections must be 1..16 MiB")
    if len(material) != DT295_MATERIAL_SIZE:
        raise ValueError("DT295 material section has an invalid size")
    if not fields or len(fields) % 8 or len(fields) > DT295_MAX_FIELDS * 8:
        raise ValueError("DT295 TRC field section has an invalid size")
    if len(key_request) != 12 or len(iv) != 16:
        raise ValueError("DT295 key request or IV section has an invalid size")
    for off in range(0, len(fields), 8):
        address, shift, width, value, reserved = struct.unpack_from("<HBBHH", fields, off)
        if not 0x1000 <= address <= 0x1AFE or address & 1:
            raise ValueError("DT295 TRC field address is outside configuration registers")
        if shift > 15 or not width or width > 16 - shift or value >= (1 << width) or reserved:
            raise ValueError("DT295 TRC field bit range or reserved word is invalid")
    return tuple(sections)


def _load_dt181_firmware(path):
    data = Path(path).read_bytes()
    verify_bundle("dt181", data, read_manifest())
    return data


def _archive_update(path, target, dt181_blob, dt295_blob):
    prefix = f"pix-userland-{pack.VERSION}-{target}/"
    with tarfile.open(path, "r:gz") as archive:
        members = archive.getmembers()
        if any(not member.isfile() for member in members):
            raise ValueError(f"non-regular package member in {path.name}")
        if len({member.name for member in members}) != len(members):
            raise ValueError(f"duplicate package member in {path.name}")
        entries = [(member.name, archive.extractfile(member).read(), member.mode) for member in members]
    if any(not name.startswith(prefix) or ".." in Path(name).parts for name, _, _ in entries):
        raise ValueError(f"invalid package member path in {path.name}")
    if any(name.startswith(prefix + "firmware/") for name, _, _ in entries):
        raise ValueError(f"package already has device assets: {path.name}")
    info_name = prefix + "release-info.json"
    matching = [index for index, item in enumerate(entries) if item[0] == info_name]
    if len(matching) != 1:
        raise ValueError(f"missing or duplicate release metadata in {path.name}")
    index = matching[0]
    name, encoded, mode = entries[index]
    try:
        metadata = json.loads(encoded)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"invalid release metadata in {path.name}") from exc
    if metadata.get("target") != target or metadata.get("device_assets"):
        raise ValueError(f"unexpected pre-finalized release metadata in {path.name}")
    include_dt295 = dt295_blob is not None and target == "darwin-arm64"
    metadata["device_assets"] = True
    metadata["dt295_assets"] = include_dt295
    entries[index] = (name, json.dumps(metadata, indent=2).encode() + b"\n", mode)
    entries.append((prefix + "firmware/dt181.bin", dt181_blob, 0o600))
    if include_dt295:
        entries.append((prefix + "firmware/dt295.bin", dt295_blob, 0o600))
    output = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=output, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w") as archive:
            for member_name, data, member_mode in entries:
                member = tarfile.TarInfo(member_name)
                member.size = len(data)
                member.mode = member_mode
                member.mtime = 0
                archive.addfile(member, io.BytesIO(data))
    return output.getvalue()


def finalize_private_release(dt181_firmware_path, dt295_firmware_path=None, root=None):
    """Preflight all inputs/packages, then stage all seven updated binary archives."""
    root = Path(root) if root is not None else pack.ROOT
    dt181_blob = _load_dt181_firmware(dt181_firmware_path)
    dt295_blob = None
    if dt295_firmware_path is not None:
        dt295_blob = Path(dt295_firmware_path).read_bytes()
        verify_bundle("dt295", dt295_blob, read_manifest())
        validate_dt295_bundle(dt295_blob)

    plans = []
    for target in pack.TARGETS:
        path = root / "dist" / f"pix-userland-{pack.VERSION}-{target}.tar.gz"
        plans.append((path, _archive_update(path, target, dt181_blob, dt295_blob)))

    staged = []
    try:
        for path, data in plans:
            fd, name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
            staged_path = Path(name)
            staged.append((staged_path, path))
            with os.fdopen(fd, "wb") as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
        for temporary, path in staged:
            os.replace(temporary, path)
    except Exception:
        for temporary, _ in staged:
            try:
                temporary.unlink()
            except OSError:
                pass
        raise
    return [path for path, _ in plans]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dt181_firmware", help="validated DT181 firmware bundle")
    parser.add_argument("--dt295-firmware", help="validated DT295 firmware bundle for darwin-arm64 only")
    args = parser.parse_args(argv)
    finalize_private_release(args.dt181_firmware, args.dt295_firmware)
    suffix = " and the macOS DT295 bundle" if args.dt295_firmware else ""
    print(f"Seven binary packages finalized with private DT181 assets{suffix}; source unchanged")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, tarfile.TarError) as exc:
        raise SystemExit(f"finalize-private-release: {exc}")
