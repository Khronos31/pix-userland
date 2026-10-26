#!/usr/bin/env python3
"""Verify that pixd contains the four validated asset inputs described by its manifest."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess

_spec = importlib.util.spec_from_file_location(
    "pix_package", Path(__file__).with_name("package-release.py"))
pack = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pack)


def verify(binary_path, dt181_directory, dt295_path, manifest_path, build_info_path=None):
    records = pack.asset_input_records(dt181_directory, dt295_path)
    manifest_bytes = Path(manifest_path).read_bytes()
    manifest = json.loads(manifest_bytes)
    if build_info_path is not None:
        build_info = Path(build_info_path).read_bytes()
        embedded = pack.embedded_build_metadata(build_info, manifest, manifest_bytes)
    else:
        source_root = Path(__file__).resolve().parent.parent
        embedded = manifest
        release_info = source_root/"release-info.json"
        if release_info.is_file():
            source_revision = json.loads(release_info.read_bytes()).get("repository_commit")
        else:
            source_revision = subprocess.check_output(
                ["git", "-C", str(source_root), "rev-parse", "HEAD"], text=True).strip()
        if manifest.get("source_revision") not in (source_revision, source_revision + "-dirty"):
            raise ValueError("Embedded source revision differs from the source archive")
        if manifest.get("dtv_userland_core_revision") != pack.CORE_LOCK:
            raise ValueError("Embedded Core revision differs from the source archive")
        if manifest.get("schema") != 1 or manifest.get("generator") != "tools/embed-assets.py":
            raise ValueError("Invalid embedded asset manifest identity")
    if embedded.get("assets") != records:
        raise ValueError("Embedded asset manifest differs from the input files")
    binary = Path(binary_path).read_bytes()
    for model, entries in records.items():
        files = entries["files"] if "files" in entries else {entries["file"]["name"]: entries["file"]}
        for name in files:
            data = (Path(dt181_directory)/name).read_bytes() if model == "dt181" else Path(dt295_path).read_bytes()
            if binary.find(data) < 0:
                raise ValueError(f"Embedded asset bytes missing from pixd: {model}/{name}")
    return embedded


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pixd", type=Path)
    parser.add_argument("dt181_assets", type=Path)
    parser.add_argument("dt295_assets", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--build-info", type=Path)
    args = parser.parse_args(argv)
    verify(args.pixd, args.dt181_assets, args.dt295_assets, args.manifest, args.build_info)
    print("Embedded DT181 and DT295 asset bytes match inputs and manifest")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        raise SystemExit(f"verify-embedded-assets: {exc}")
