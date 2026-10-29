#!/usr/bin/env python3
"""Read and validate the PIX machine-specific firmware bundles."""
import hashlib
import importlib.util
import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "firmware" / "manifest.json"
DT181_MAGIC = b"P181A1\0\0"
DT181_SECTION_NAMES = ("fpga", "bootstrap", "validation")


def read_manifest(path=MANIFEST):
    manifest = json.loads(Path(path).read_text(encoding="ascii"))
    if manifest.get("schema") != 1 or set(manifest.get("models", {})) != {"dt181", "dt295"}:
        raise ValueError("invalid PIX firmware manifest")
    for model in ("dt181", "dt295"):
        record = manifest["models"][model]
        if not isinstance(record.get("size"), int) or record["size"] <= 0:
            raise ValueError(f"invalid {model} bundle size")
        if not re.fullmatch(r"[0-9a-f]{64}", record.get("sha256", "")):
            raise ValueError(f"invalid {model} bundle SHA-256")
        allowlist = record.get("sha256_allowlist", [record["sha256"]])
        if not isinstance(allowlist, list) or not allowlist or allowlist[0] != record["sha256"] or \
                any(not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest)
                    for digest in allowlist) or len(set(allowlist)) != len(allowlist):
            raise ValueError(f"invalid {model} bundle SHA-256 allowlist")
        if not isinstance(record.get("file"), str) or not record["file"]:
            raise ValueError(f"invalid {model} bundle name")
    sections = manifest["models"]["dt181"].get("sections", [])
    if tuple(section.get("name") for section in sections) != DT181_SECTION_NAMES or any(
            not isinstance(section.get("size"), int) or section["size"] <= 0 or
            not re.fullmatch(r"[0-9a-f]{64}", section.get("sha256", ""))
            for section in sections):
        raise ValueError("invalid DT181 section sizes")
    expected_size = 20 + sum(section["size"] for section in sections)
    if manifest["models"]["dt181"]["size"] != expected_size:
        raise ValueError("DT181 bundle size differs from its section sizes")
    if manifest["models"]["dt181"].get("format") != "P181A1" or \
            manifest["models"]["dt295"].get("format") != "D295A1":
        raise ValueError("unknown PIX firmware bundle format")
    return manifest


def pack_dt181(sections, manifest=None):
    manifest = read_manifest() if manifest is None else manifest
    sections_by_name = {section["name"]: section for section in
                        manifest["models"]["dt181"]["sections"]}
    if set(sections) != set(DT181_SECTION_NAMES):
        raise ValueError("DT181 payload must contain exactly three sections")
    payloads = []
    for name in DT181_SECTION_NAMES:
        data = bytes(sections[name])
        section = sections_by_name[name]
        if len(data) != section["size"] or hashlib.sha256(data).hexdigest() != section["sha256"]:
            raise ValueError(f"invalid DT181 section size: {name}")
        payloads.append(data)
    bundle = DT181_MAGIC + struct.pack("<III", *(len(data) for data in payloads)) + b"".join(payloads)
    verify_bundle("dt181", bundle, manifest)
    return bundle


def unpack_dt181(data, manifest=None):
    manifest = read_manifest() if manifest is None else manifest
    data = bytes(data)
    record = manifest["models"]["dt181"]
    if len(data) != record["size"] or data[:8] != DT181_MAGIC:
        raise ValueError("invalid DT181 firmware bundle header or length")
    lengths = struct.unpack_from("<III", data, 8)
    expected = tuple(section["size"] for section in record["sections"])
    if lengths != expected or 20 + sum(lengths) != len(data):
        raise ValueError("invalid DT181 firmware section lengths")
    offset = 20
    result = {}
    for section, length in zip(record["sections"], lengths):
        name = section["name"]
        result[name] = data[offset:offset + length]
        if hashlib.sha256(result[name]).hexdigest() != section["sha256"]:
            raise ValueError(f"invalid DT181 section SHA-256: {name}")
        offset += length
    verify_bundle("dt181", data, manifest)
    return result


def verify_bundle(model, data, manifest=None):
    manifest = read_manifest() if manifest is None else manifest
    if model not in ("dt181", "dt295"):
        raise ValueError("unsupported PIX firmware model")
    record = manifest["models"][model]
    if len(data) != record["size"]:
        raise ValueError(f"invalid {model.upper()} firmware bundle size")
    if hashlib.sha256(data).hexdigest() not in record.get("sha256_allowlist", [record["sha256"]]):
        raise ValueError(f"unrecognized {model.upper()} firmware bundle SHA-256")
    if model == "dt181":
        if data[:8] != DT181_MAGIC:
            raise ValueError("invalid DT181 firmware bundle header")
        lengths = struct.unpack_from("<III", data, 8)
        expected_lengths = tuple(section["size"] for section in record["sections"])
        if lengths != expected_lengths:
            raise ValueError("invalid DT181 firmware section lengths")
        offset = 20
        for section, length in zip(record["sections"], lengths):
            digest = hashlib.sha256(data[offset:offset + length]).hexdigest()
            if digest != section["sha256"]:
                raise ValueError(f"invalid DT181 firmware section SHA-256: {section['name']}")
            offset += length
    return record


def generate_header(output, manifest=None):
    manifest = read_manifest() if manifest is None else manifest
    lines = ["#ifndef PIX_FIRMWARE_MANIFEST_GENERATED_H", "#define PIX_FIRMWARE_MANIFEST_GENERATED_H",
             "#include <stddef.h>", "#include <stdint.h>"]
    for model in ("dt181", "dt295"):
        record = manifest["models"][model]
        prefix = model.upper()
        lines.append(f"#define PIX_{prefix}_BUNDLE_SIZE {record['size']}U")
        hashes = record.get("sha256_allowlist", [record["sha256"]])
        if model == "dt295":
            lines.append(f"#define PIX_{prefix}_BUNDLE_SHA256_COUNT {len(hashes)}U")
            lines.append(f"static const uint8_t PIX_{prefix}_BUNDLE_SHA256[{len(hashes)}][32] = {{")
            for digest in hashes:
                values = ", ".join(f"0x{value:02x}U" for value in bytes.fromhex(digest))
                lines.append(f"    {{{values}}},")
            lines.append("};")
        else:
            digest = bytes.fromhex(record["sha256"])
            values = ", ".join(f"0x{value:02x}U" for value in digest)
            lines.append(f"static const uint8_t PIX_{prefix}_BUNDLE_SHA256[32] = {{{values}}};")
        if model == "dt181":
            for section in record["sections"]:
                section_prefix = f"PIX_DT181_{section['name'].upper()}"
                section_digest = bytes.fromhex(section["sha256"])
                section_values = ", ".join(f"0x{value:02x}U" for value in section_digest)
                lines.append(f"#define {section_prefix}_SIZE {section['size']}U")
                lines.append(f"static const uint8_t {section_prefix}_SHA256[32] = {{{section_values}}};")
    lines.extend(["#endif", ""])
    Path(output).write_text("\n".join(lines), encoding="ascii", newline="\n")


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("header", "verify"))
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--model", choices=("dt181", "dt295"))
    parser.add_argument("--bundle", type=Path)
    args = parser.parse_args()
    manifest = read_manifest(args.manifest)
    if args.command == "header":
        if args.output is None:
            parser.error("header requires --output")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        generate_header(args.output, manifest)
    else:
        if args.model is None or args.bundle is None:
            parser.error("verify requires --model and --bundle")
        data = args.bundle.read_bytes()
        verify_bundle(args.model, data, manifest)
        if args.model == "dt295":
            module_spec = importlib.util.spec_from_file_location(
                "pix_finalize", Path(__file__).with_name("finalize-private-release.py"))
            module = importlib.util.module_from_spec(module_spec)
            module_spec.loader.exec_module(module)
            module.validate_dt295_bundle(data)
        print(f"{args.model} firmware bundle verified")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, json.JSONDecodeError, struct.error) as exc:
        raise SystemExit(f"firmware-bundle: {exc}")
