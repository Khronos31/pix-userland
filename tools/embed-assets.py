#!/usr/bin/env python3
"""Generate a deterministic C translation unit for the validated private PIX assets."""
import argparse
import hashlib
import json
import os
from pathlib import Path
from firmware_bundle import read_manifest, unpack_dt181, verify_bundle

import importlib.util
_spec = importlib.util.spec_from_file_location(
    "pix_finalize", Path(__file__).with_name("finalize-private-release.py"))
_finalize = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_finalize)


def _load_assets(dt181_path, dt295_path):
    manifest = read_manifest()
    dt181 = Path(dt181_path).read_bytes()
    dt295 = Path(dt295_path).read_bytes()
    verify_bundle("dt181", dt181, manifest)
    verify_bundle("dt295", dt295, manifest)
    unpack_dt181(dt181, manifest)
    _finalize.validate_dt295_bundle(dt295)
    return dt181, dt295, manifest


def _emit_array(stream, name, data):
    stream.write(f"static const uint8_t {name}[{len(data)}] = {{\n")
    for offset in range(0, len(data), 12):
        row = data[offset:offset + 12]
        stream.write("    " + ", ".join(f"0x{value:02x}" for value in row) + ",\n")
    stream.write("};\n\n")


def generate(dt181_path, dt295_path, output_directory, source_revision,
             core_revision, toolchain):
    dt181, dt295, manifest_data = _load_assets(dt181_path, dt295_path)
    expected = manifest_data["models"]
    manifest = {
        "schema": 1,
        "source_revision": source_revision,
        "dtv_userland_core_revision": core_revision,
        "generator": "tools/embed-assets.py",
        "toolchain": toolchain,
        "assets": {
            "dt181": {
                "provenance": "StationTV/Pixela device data; not covered by the PIX MIT license",
                "file": {"name": expected["dt181"]["file"], "size": len(dt181),
                         "sha256": hashlib.sha256(dt181).hexdigest()},
            },
            "dt295": {
                "provenance": "Private runtime bundle described by docs/dt295.md; not covered by the PIX MIT license",
                "file": {"name": expected["dt295"]["file"], "size": len(dt295),
                         "sha256": hashlib.sha256(dt295).hexdigest()},
            },
        },
    }
    output = Path(output_directory)
    output.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(output, 0o700)
    header = output / "pix_embedded_assets.h"
    source = output / "pix_embedded_assets.c"
    manifest_path = output / "pix_embedded_assets.json"
    header.write_text(
        "#ifndef PIX_EMBEDDED_ASSETS_H\n"
        "#define PIX_EMBEDDED_ASSETS_H\n"
        "#include <stddef.h>\n#include <stdint.h>\n"
        "#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
        "const uint8_t *pix_embedded_dt181_bundle(size_t *size);\n"
        "const uint8_t *pix_embedded_dt295_bundle(size_t *size);\n"
        "const char *pix_embedded_assets_manifest(void);\n"
        "#ifdef __cplusplus\n}\n#endif\n#endif\n",
        encoding="ascii")
    manifest_text = json.dumps(manifest, sort_keys=True, separators=(",", ":"))
    with source.open("w", encoding="ascii", newline="\n") as stream:
        stream.write('#include "pix_embedded_assets.h"\n\n')
        _emit_array(stream, "dt181_bundle", dt181)
        _emit_array(stream, "dt295_bundle", dt295)
        stream.write(f"static const char asset_manifest[] = {json.dumps(manifest_text)};\n\n")
        stream.write("const uint8_t *pix_embedded_dt181_bundle(size_t *size) {\n")
        stream.write("    if (size != NULL) *size = sizeof(dt181_bundle);\n    return dt181_bundle;\n}\n")
        stream.write("const uint8_t *pix_embedded_dt295_bundle(size_t *size) {\n")
        stream.write("    if (size != NULL) *size = sizeof(dt295_bundle);\n    return dt295_bundle;\n}\n")
        stream.write("const char *pix_embedded_assets_manifest(void) { return asset_manifest; }\n")
    manifest_path.write_text(json.dumps(manifest, sort_keys=True, indent=2) + "\n",
                              encoding="ascii")
    for path in (header, source, manifest_path):
        os.chmod(path, 0o600)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dt181-firmware", required=True)
    parser.add_argument("--dt295-firmware", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--source-revision", default="unknown")
    parser.add_argument("--core-revision", default="unknown")
    parser.add_argument("--toolchain", default="unknown")
    args = parser.parse_args()
    generate(args.dt181_firmware, args.dt295_firmware, args.output_dir,
             args.source_revision, args.core_revision, args.toolchain)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as exc:
        raise SystemExit(f"embed-assets: {exc}")
