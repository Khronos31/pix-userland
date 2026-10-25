#!/usr/bin/env python3
"""Generate a deterministic C translation unit for the validated private PIX assets."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path

DT181_SIZES = {"fpga.bin": 699673, "bootstrap.bin": 16, "validation.bin": 64}

_spec = importlib.util.spec_from_file_location(
    "pix_finalize", Path(__file__).with_name("finalize-private-release.py"))
_finalize = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_finalize)


def _load_assets(dt181_directory, dt295_path):
    dt181 = {}
    for name, size in DT181_SIZES.items():
        data = (Path(dt181_directory) / name).read_bytes()
        if len(data) != size:
            raise ValueError(f"invalid DT181 asset size: {name}")
        dt181[name] = data
    dt295 = Path(dt295_path).read_bytes()
    _finalize.validate_dt295_bundle(dt295)
    return dt181, dt295


def _emit_array(stream, name, data):
    stream.write(f"static const uint8_t {name}[{len(data)}] = {{\n")
    for offset in range(0, len(data), 12):
        row = data[offset:offset + 12]
        stream.write("    " + ", ".join(f"0x{value:02x}" for value in row) + ",\n")
    stream.write("};\n\n")


def generate(dt181_directory, dt295_path, output_directory, source_revision,
             core_revision, toolchain):
    dt181, dt295 = _load_assets(dt181_directory, dt295_path)
    manifest = {
        "schema": 1,
        "source_revision": source_revision,
        "dtv_userland_core_revision": core_revision,
        "generator": "tools/embed-assets.py",
        "toolchain": toolchain,
        "assets": {
            "dt181": {
                "provenance": "StationTV/Pixela device data; not covered by the PIX MIT license",
                "files": {name: {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                          for name, data in sorted(dt181.items())},
            },
            "dt295": {
                "provenance": "Private runtime bundle described by docs/dt295.md; not covered by the PIX MIT license",
                "file": {"name": "dt295.assets", "size": len(dt295),
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
        "const uint8_t *pix_embedded_dt181_firmware(size_t *size);\n"
        "const uint8_t *pix_embedded_dt181_bootstrap(size_t *size);\n"
        "const uint8_t *pix_embedded_dt181_validation(size_t *size);\n"
        "const uint8_t *pix_embedded_dt295_bundle(size_t *size);\n"
        "const char *pix_embedded_assets_manifest(void);\n"
        "#ifdef __cplusplus\n}\n#endif\n#endif\n",
        encoding="ascii")
    manifest_text = json.dumps(manifest, sort_keys=True, separators=(",", ":"))
    with source.open("w", encoding="ascii", newline="\n") as stream:
        stream.write('#include "pix_embedded_assets.h"\n\n')
        _emit_array(stream, "dt181_firmware", dt181["fpga.bin"])
        _emit_array(stream, "dt181_bootstrap", dt181["bootstrap.bin"])
        _emit_array(stream, "dt181_validation", dt181["validation.bin"])
        _emit_array(stream, "dt295_bundle", dt295)
        stream.write(f"static const char asset_manifest[] = {json.dumps(manifest_text)};\n\n")
        stream.write("const uint8_t *pix_embedded_dt181_firmware(size_t *size) {\n")
        stream.write("    if (size != NULL) *size = sizeof(dt181_firmware);\n    return dt181_firmware;\n}\n")
        stream.write("const uint8_t *pix_embedded_dt181_bootstrap(size_t *size) {\n")
        stream.write("    if (size != NULL) *size = sizeof(dt181_bootstrap);\n    return dt181_bootstrap;\n}\n")
        stream.write("const uint8_t *pix_embedded_dt181_validation(size_t *size) {\n")
        stream.write("    if (size != NULL) *size = sizeof(dt181_validation);\n    return dt181_validation;\n}\n")
        stream.write("const uint8_t *pix_embedded_dt295_bundle(size_t *size) {\n")
        stream.write("    if (size != NULL) *size = sizeof(dt295_bundle);\n    return dt295_bundle;\n}\n")
        stream.write("const char *pix_embedded_assets_manifest(void) { return asset_manifest; }\n")
    manifest_path.write_text(json.dumps(manifest, sort_keys=True, indent=2) + "\n",
                              encoding="ascii")
    for path in (header, source, manifest_path):
        os.chmod(path, 0o600)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dt181-assets", required=True)
    parser.add_argument("--dt295-assets", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--source-revision", default="unknown")
    parser.add_argument("--core-revision", default="unknown")
    parser.add_argument("--toolchain", default="unknown")
    args = parser.parse_args()
    generate(args.dt181_assets, args.dt295_assets, args.output_dir,
             args.source_revision, args.core_revision, args.toolchain)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as exc:
        raise SystemExit(f"embed-assets: {exc}")
