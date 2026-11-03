#!/usr/bin/env python3
"""Validate the release set, external firmware bundles and source closure."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tarfile
from firmware_bundle import read_manifest, verify_bundle

_spec = importlib.util.spec_from_file_location("pix_package", Path(__file__).with_name("package-release.py"))
pack = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pack)
_final_spec = importlib.util.spec_from_file_location("pix_finalize", Path(__file__).with_name("finalize-private-release.py"))
finalize = importlib.util.module_from_spec(_final_spec)
_final_spec.loader.exec_module(finalize)

SOURCE_LIBUSB_SHA256 = "fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf"
GPL2_SHA256 = "edaef632cbb643e4e7a221717a6c441a4c1a7c918e6e4d56debc3d8739b233f6"
PIX_SOURCE_LICENSE = "GPL-2.0-or-later"
PIX_SOURCE_GRANT = b"""The PIX-maintained source files in this repository are free software: you can
redistribute them and/or modify them under the terms of the GNU General Public
License as published by the Free Software Foundation, either version 2 of the
License, or (at your option) any later version."""
CORE_PIN = pack.CORE_LOCK
SOURCE_ONLY_KEYS = {"repository_commit", "repository_tree", "dtv_userland_core_tree", "libusb_sha256"}
FIRMWARE_MANIFEST = read_manifest()


def _read_archive(path):
    with tarfile.open(path, "r:gz") as archive:
        members = archive.getmembers()
        if any(not member.isfile() for member in members):
            raise ValueError("Only regular archive members permitted")
        if len(archive.getnames()) != len(set(archive.getnames())):
            raise ValueError("Duplicate archive member")
        return {member.name: archive.extractfile(member).read() for member in members}


def _expected_metadata(target, commit, asset_inputs):
    expected = {
        "project": "pix-userland", "version": pack.VERSION, "target": target,
        "commit": commit, "libusb": "1.0.30", "private": False,
        "libusb_linkage": "static",
        "device_assets": target != "source",
        "dtv_userland_core_commit": CORE_PIN,
        "asset_inputs": asset_inputs,
    }
    return expected


def _source_asset_records(contents, prefix):
    records = {}
    for model, provenance in (("dt181", pack.DT181_PROVENANCE),
                              ("dt295", pack.DT295_PROVENANCE)):
        file_name = FIRMWARE_MANIFEST["models"][model]["file"]
        path = prefix + "firmware/" + file_name
        data = contents.get(path)
        if data is None:
            raise ValueError(f"Missing source firmware input: {file_name}")
        verify_bundle(model, data, FIRMWARE_MANIFEST)
        if model == "dt295": finalize.validate_dt295_bundle(data)
        records[model] = {"provenance": provenance,
                          "file": {"name": file_name, "size": len(data),
                                   "sha256": hashlib.sha256(data).hexdigest()}}
    return records


def _verify_source_closure(contents, prefix, metadata, libusb_sha256):
    """Check the corresponding-source manifest against the archive and the fixed Core pin."""
    try:
        manifest = json.loads(contents[prefix + "source-manifest.json"])
        sums = contents[prefix + "SHA256SUMS"].decode("ascii")
    except (KeyError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("Missing or invalid source manifest or SHA256SUMS") from exc
    pin = contents.get(prefix + "core-integration/dtv-userland-core.revision", b"").decode("ascii").strip()
    if pin != CORE_PIN:
        raise ValueError("Source archive Core pin differs from the verifier pin")
    for key in ("repository_commit", "repository_tree", "dtv_userland_core_commit",
                "dtv_userland_core_tree", "libusb_sha256"):
        if manifest.get(key) != metadata.get(key) or not manifest.get(key):
            raise ValueError(f"Source manifest identity mismatch: {key}")
    if manifest["dtv_userland_core_commit"] != CORE_PIN or manifest["libusb_sha256"] != libusb_sha256:
        raise ValueError("Source manifest dependency identity mismatch")
    listed = manifest.get("files", {})
    actual = {name[len(prefix):]: data for name, data in contents.items()
              if name[len(prefix):] not in {"source-manifest.json", "SHA256SUMS"}}
    if set(listed) != set(actual):
        raise ValueError("Source manifest file list differs from the archive")
    for name, data in actual.items():
        if listed[name] != {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}:
            raise ValueError(f"Source manifest hash mismatch: {name}")
    expected_sums = {line.split("  ", 1)[1]: line.split("  ", 1)[0] for line in sums.splitlines()}
    if set(expected_sums) != set(actual) | {"source-manifest.json"} or any(
            expected_sums[name] != hashlib.sha256(contents[prefix + name]).hexdigest() for name in expected_sums):
        raise ValueError("Source SHA256SUMS differs from the archive")
    for required in ("third_party/dtv-userland-core/LICENSE", "third_party/dtv-userland-core/PROVENANCE.json",
                     "third_party/dtv-userland-core/CMakeLists.txt", "core-integration/CMakeLists.txt",
                     "core-integration/pixd.cpp", "core-integration/tools/pixctl.cpp",
                     "core-integration/tools/pix_ts.cpp", "tools/build-windows.sh", "include/os.h",
                     "src/dt181_capture_host.c", "pix-dt181-consumer.cpp",
                     "core-integration/tests/windows_os_thread_proof.c", "tests/test_windows_runtime.py"):
        if required not in listed:
            raise ValueError(f"Source closure lacks {required}")
    for required in ("firmware/dt181.bin", "firmware/dt295.bin", "firmware/manifest.json",
                     "firmware/metadata/dt181-source.json",
                     "firmware/metadata/dt295-preservation.json",
                     "firmware/metadata/dt295-satellite-generation.json",
                     "firmware/preserved/verified-ts-iv.bin"):
        if required not in listed:
            raise ValueError(f"Source closure lacks {required}")
    if not any(name.startswith("third_party/dtv-userland-core/adapters/") and name.endswith("/COPYING") for name in listed):
        raise ValueError("Source closure lacks the Core adapter COPYING")


def verify_release(root=None, commit=None, source_libusb_sha256=SOURCE_LIBUSB_SHA256):
    root = Path(root) if root is not None else pack.ROOT
    out = root / "dist"
    if commit is None:
        commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    expected = {f"pix-userland-{pack.VERSION}-{target}.tar.gz": target
                for target in (*pack.TARGETS, "source")}
    actual_files = {p.name for p in out.iterdir() if p.name != "SHA256SUMS"}
    if actual_files != set(expected):
        raise ValueError("Unexpected or missing release files")
    source_name = f"pix-userland-{pack.VERSION}-source.tar.gz"
    source_prefix = f"pix-userland-{pack.VERSION}-source/"
    source_contents = _read_archive(out/source_name)
    source_assets = _source_asset_records(source_contents, source_prefix)
    for name, target in expected.items():
        path = out / name
        prefix = f"pix-userland-{pack.VERSION}-{target}/"
        contents = _read_archive(path)
        for member_name in contents:
            parts = Path(member_name).parts
            if not member_name.startswith(prefix) or ".." in parts:
                raise ValueError("Invalid archive member")
            if any(part in {"captures", ".git", ".private"} for part in parts):
                raise ValueError("Private data directory in archive")
            if member_name.endswith((".ts", ".context", ".mp4", ".usb.bin")):
                raise ValueError("Private capture data in archive")
        metadata_name = prefix + "release-info.json"
        try:
            metadata = json.loads(contents[metadata_name])
        except (KeyError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError(f"Missing or invalid release metadata: {target}") from exc
        expected_metadata = _expected_metadata(target, commit, source_assets)
        compared = metadata
        if target == "source":
            if metadata.get("repository_commit") != commit:
                raise ValueError("Source repository commit mismatch")
            compared = {k: v for k, v in metadata.items() if k not in SOURCE_ONLY_KEYS}
            if metadata.get("asset_inputs") != source_assets:
                raise ValueError("Source asset metadata differs from source input bytes")
            expected_metadata["asset_inputs"] = source_assets
        else:
            for model in ("dt181", "dt295"):
                asset_name = source_assets[model]["file"]["name"]
                asset_path = prefix + "firmware/" + asset_name
                source_path = source_prefix + "firmware/" + asset_name
                if contents.get(asset_path) != source_contents.get(source_path):
                    raise ValueError(f"Binary asset differs from source input: {target}/{asset_name}")
            if prefix + "embedded-assets.json" in contents or "embedded_assets" in metadata:
                raise ValueError(f"Embedded asset output is no longer supported: {target}")
        if compared != expected_metadata:
            raise ValueError(f"Release metadata mismatch: {target}")
        firmware_prefix = prefix + "firmware/"
        asset_names = {member_name for member_name in contents if member_name.startswith(firmware_prefix)}
        expected_assets = {firmware_prefix + FIRMWARE_MANIFEST["models"][model]["file"]
                           for model in ("dt181", "dt295")} if target != "source" else set()
        if target != "source" and asset_names != expected_assets:
            raise ValueError(f"Incorrect device asset set: {target}")
        for asset_name in expected_assets:
            model = "dt181" if asset_name.endswith("dt181.bin") else "dt295"
            verify_bundle(model, contents[asset_name], FIRMWARE_MANIFEST)
            if model == "dt295":
                finalize.validate_dt295_bundle(contents[asset_name])
        if any(member_name.lower().endswith((".dll", ".dylib", ".so")) for member_name in contents):
            raise ValueError("Unexpected shared library")
        for required in ("LICENSE", "COPYING.libusb", "THIRD_PARTY_NOTICES.md", "licenses/musl-COPYRIGHT.txt",
                         "licenses/GPL-2.0.txt"):
            if prefix + required not in contents:
                raise ValueError(f"Missing license notice: {target}/{required}")
        license_text = contents[prefix + "LICENSE"]
        gpl2_text = contents[prefix + "licenses/GPL-2.0.txt"]
        if PIX_SOURCE_LICENSE.encode() not in license_text or PIX_SOURCE_GRANT not in license_text or gpl2_text not in license_text:
            raise ValueError(f"Invalid PIX GPL-2.0-or-later grant or GPLv2 text: {target}")
        if hashlib.sha256(gpl2_text).hexdigest() != GPL2_SHA256:
            raise ValueError(f"GPLv2 text mismatch: {target}")
        if target != "source":
            suffix = ".exe" if target == "windows-x64" else ""
            for program in pack.PROGRAMS:
                if prefix + program + suffix not in contents:
                    raise ValueError(f"Missing executable: {target}/{program}{suffix}")
            if prefix + "pix-ts-legacy" + suffix in contents:
                raise ValueError(f"Legacy standalone CLI must not be packaged: {target}")
            build_info = contents.get(prefix + "build-info.txt", b"").decode("utf-8", "replace")
            if f"dtv-userland-core {CORE_PIN}" not in build_info:
                raise ValueError(f"build-info.txt lacks the Core pin: {target}")
            build_info = contents.get(prefix + "build-info.txt", b"").decode("utf-8", "replace")
            if "embedded_assets" in build_info or "embedded_asset=" in build_info:
                raise ValueError(f"Unexpected embedded asset build metadata: {target}")
            if prefix + "licenses/dtv-userland-core-LICENSE" not in contents or not any(
                    name.startswith(prefix + "licenses/dtv-userland-core-") and name.endswith("-COPYING")
                    for name in contents):
                raise ValueError(f"Missing Core license notices: {target}")
        else:
            _verify_source_closure(contents, prefix, metadata, source_libusb_sha256)
            libusb = contents.get(prefix + "third_party/libusb-1.0.30.tar.bz2")
            if libusb is None or hashlib.sha256(libusb).hexdigest() != source_libusb_sha256:
                raise ValueError("libusb source checksum mismatch")
    sums = "".join(f"{hashlib.sha256((out / name).read_bytes()).hexdigest()}  {name}\n"
                   for name in sorted(expected))
    (out / "SHA256SUMS").write_text(sums, encoding="ascii")
    return expected


def main(argv=None):
    parser = argparse.ArgumentParser()
    args = parser.parse_args(argv)
    verify_release()
    print("Verified seven binary packages (pixd, pixctl, pix-ts) and matching source closure; SHA256SUMS created")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, tarfile.TarError) as exc:
        raise SystemExit(f"verify-release: {exc}")
