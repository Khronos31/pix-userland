#!/usr/bin/env python3
"""Validate the release set, embedded build identities and optional private assets."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tarfile

_spec = importlib.util.spec_from_file_location("pix_package", Path(__file__).with_name("package-release.py"))
pack = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pack)
_final_spec = importlib.util.spec_from_file_location("pix_finalize", Path(__file__).with_name("finalize-private-release.py"))
finalize = importlib.util.module_from_spec(_final_spec)
_final_spec.loader.exec_module(finalize)

SOURCE_LIBUSB_SHA256 = "fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf"
CORE_PIN = pack.CORE_LOCK
SOURCE_ONLY_KEYS = {"repository_commit", "repository_tree", "dtv_userland_core_tree", "libusb_sha256"}
DT181_ASSET_SIZES = {"fpga.bin": 699673, "bootstrap.bin": 16, "validation.bin": 64}


def validate_flags(with_assets, with_dt295_assets):
    if with_dt295_assets and not with_assets:
        raise ValueError("--with-dt295-assets requires --with-assets")


def _read_archive(path):
    with tarfile.open(path, "r:gz") as archive:
        members = archive.getmembers()
        if any(not member.isfile() for member in members):
            raise ValueError("Only regular archive members permitted")
        if len(archive.getnames()) != len(set(archive.getnames())):
            raise ValueError("Duplicate archive member")
        return {member.name: archive.extractfile(member).read() for member in members}


def _expected_metadata(target, commit, with_assets, with_dt295_assets):
    expected = {
        "project": "pix-userland", "version": pack.VERSION, "target": target,
        "commit": commit, "libusb": "1.0.30", "private": False,
        "libusb_linkage": "static", "device_assets": bool(with_assets and target != "source"),
        "dtv_userland_core_commit": CORE_PIN,
    }
    return expected


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
    if not any(name.startswith("third_party/dtv-userland-core/adapters/") and name.endswith("/COPYING") for name in listed):
        raise ValueError("Source closure lacks the Core adapter COPYING")


def verify_release(root=None, with_assets=False, with_dt295_assets=False, commit=None,
                   source_libusb_sha256=SOURCE_LIBUSB_SHA256):
    validate_flags(with_assets, with_dt295_assets)
    root = Path(root) if root is not None else pack.ROOT
    out = root / "dist"
    if commit is None:
        commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    expected = {f"pix-userland-{pack.VERSION}-{target}.tar.gz": target
                for target in (*pack.TARGETS, "source")}
    actual_files = {p.name for p in out.iterdir() if p.name != "SHA256SUMS"}
    if actual_files != set(expected):
        raise ValueError("Unexpected or missing release files")
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
        expected_metadata = _expected_metadata(target, commit, with_assets, with_dt295_assets)
        if with_assets and target != "source":
            expected_metadata["dt295_assets"] = bool(with_dt295_assets and target == "darwin-arm64")
        elif target != "source" and metadata.get("dt295_assets", False) is not False:
            raise ValueError(f"Unexpected DT295 asset metadata: {target}")
        compared = metadata
        if target == "source":
            if metadata.get("repository_commit") != commit:
                raise ValueError("Source repository commit mismatch")
            compared = {k: v for k, v in metadata.items() if k not in SOURCE_ONLY_KEYS}
        if compared != expected_metadata:
            raise ValueError(f"Release metadata mismatch: {target}")
        assets_prefix = prefix + "assets/"
        asset_names = {member_name for member_name in contents if member_name.startswith(assets_prefix)}
        expected_assets = {}
        if with_assets and target != "source":
            expected_assets.update({assets_prefix + asset: size for asset, size in DT181_ASSET_SIZES.items()})
        if with_dt295_assets and target == "darwin-arm64":
            expected_assets[assets_prefix + "dt295.assets"] = None
        if asset_names != set(expected_assets):
            raise ValueError(f"Incorrect device asset set: {target}")
        for asset_name, size in expected_assets.items():
            if size is not None and len(contents[asset_name]) != size:
                raise ValueError(f"Invalid device asset size: {asset_name}")
        if with_dt295_assets and target == "darwin-arm64":
            finalize.validate_dt295_bundle(contents[assets_prefix + "dt295.assets"])
        if any(member_name.lower().endswith((".dll", ".dylib", ".so")) for member_name in contents):
            raise ValueError("Unexpected shared library")
        for required in ("LICENSE", "COPYING.libusb", "THIRD_PARTY_NOTICES.md", "licenses/musl-COPYRIGHT.txt"):
            if prefix + required not in contents:
                raise ValueError(f"Missing license notice: {target}/{required}")
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
    parser.add_argument("--with-assets", action="store_true")
    parser.add_argument("--with-dt295-assets", action="store_true",
                        help="require and verify assets/dt295.assets in darwin-arm64")
    args = parser.parse_args(argv)
    validate_flags(args.with_assets, args.with_dt295_assets)
    verify_release(with_assets=args.with_assets, with_dt295_assets=args.with_dt295_assets)
    print("Verified seven binary packages (pixd, pixctl, pix-ts) and matching source closure; SHA256SUMS created")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, tarfile.TarError) as exc:
        raise SystemExit(f"verify-release: {exc}")
