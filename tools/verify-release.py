#!/usr/bin/env python3
"""Validate the release set, embedded build identities and optional private assets."""
import argparse
import hashlib
import importlib.util
import io
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
        "commit": commit, "libusb": "1.0.30", "private": True,
        "libusb_linkage": "static", "device_assets": bool(with_assets and target != "source"),
    }
    return expected


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
        if target == "source":
            source_fields = {
                "repository_commit": commit,
                "dtv_userland_core_commit": pack.CORE_LOCK,
                "px4_userland_commit": pack.PX4_LOCK,
                "libusb_sha256": source_libusb_sha256,
            }
            if any(metadata.get(key) != value for key, value in expected_metadata.items()) or \
                    any(metadata.get(key) != value for key, value in source_fields.items()):
                raise ValueError(f"Release metadata mismatch: {target}")
        elif metadata != expected_metadata:
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
            executable = "pix-ts.exe" if target == "windows-x64" else "pix-ts"
            if prefix + executable not in contents:
                raise ValueError(f"Missing executable: {target}")
        else:
            libusb = contents.get(prefix + "third_party/libusb-1.0.30.tar.bz2")
            if libusb is None or hashlib.sha256(libusb).hexdigest() != source_libusb_sha256:
                raise ValueError("libusb source checksum mismatch")
    verify_source_archive(root=root,
                          archive_path=out / f"pix-userland-{pack.VERSION}-source.tar.gz",
                          source_libusb_sha256=source_libusb_sha256)
    sums = "".join(f"{hashlib.sha256((out / name).read_bytes()).hexdigest()}  {name}\n"
                   for name in sorted(expected))
    (out / "SHA256SUMS").write_text(sums, encoding="ascii")
    return expected


def _git_snapshot_contents(root, revision):
    commit = subprocess.check_output(
        ["git", "-C", str(root), "rev-parse", f"{revision}^{{commit}}"], text=True).strip()
    tree = subprocess.check_output(
        ["git", "-C", str(root), "rev-parse", f"{revision}^{{tree}}"], text=True).strip()
    archive_bytes = subprocess.check_output(
        ["git", "-C", str(root), "archive", "--format=tar", revision])
    with tarfile.open(fileobj=io.BytesIO(archive_bytes), mode="r:") as archive:
        contents = {}
        for member in archive.getmembers():
            if member.isdir():
                continue
            if not member.isfile() or member.name.startswith("/") or ".." in Path(member.name).parts:
                raise ValueError(f"Invalid dependency source member: {member.name}")
            contents[member.name] = archive.extractfile(member).read()
    return commit, tree, contents


def verify_source_archive(root=None, archive_path=None, core_source_root=None,
                          px4_source_root=None, source_libusb_sha256=SOURCE_LIBUSB_SHA256,
                          require_pinned_roots=False):
    root = Path(root) if root is not None else pack.ROOT
    path = Path(archive_path) if archive_path is not None else root / "dist" / f"pix-userland-{pack.VERSION}-source.tar.gz"
    if (core_source_root is None) != (px4_source_root is None):
        raise ValueError("Core and PX4 Git roots must be supplied together")
    if require_pinned_roots and core_source_root is None:
        raise ValueError("source-only verification requires pinned Core and PX4 Git roots")
    prefix = f"pix-userland-{pack.VERSION}-source/"
    contents = _read_archive(path)
    if not contents:
        raise ValueError("Source archive is empty")
    for name in contents:
        parts = Path(name).parts
        if not name.startswith(prefix) or ".." in parts or "\\" in name:
            raise ValueError(f"Invalid source archive member: {name}")
        if any(part in {"captures", ".git", ".private", "assets"} for part in parts):
            raise ValueError(f"Private data directory in source archive: {name}")
    relative = {name[len(prefix):]: data for name, data in contents.items()}
    manifest = json.loads(relative["source-manifest.json"])
    metadata = json.loads(relative["release-info.json"])
    if manifest.get("schema") != 1 or manifest.get("kind") != "corresponding-source":
        raise ValueError("Invalid source manifest identity")
    if manifest.get("project") != "pix-userland" or manifest.get("version") != pack.VERSION or manifest.get("private") is not True:
        raise ValueError("Source manifest product identity mismatch")
    expected_pins = {
        "dtv_userland_core_commit": pack.CORE_LOCK,
        "px4_userland_commit": pack.PX4_LOCK,
    }
    for key, value in expected_pins.items():
        if manifest.get(key) != value or metadata.get(key) != value:
            raise ValueError(f"Source dependency pin mismatch: {key}")
    for lock_path, value in (("cmake/dtv-userland-core.revision", pack.CORE_LOCK),
                             ("cmake/px4-userland.revision", pack.PX4_LOCK)):
        if relative.get(lock_path, b"").decode("ascii").strip() != value:
            raise ValueError(f"Archived dependency lock mismatch: {lock_path}")
    for source_path, revision, tree_key in (
            (core_source_root, pack.CORE_LOCK, "dtv_userland_core_tree"),
            (px4_source_root, pack.PX4_LOCK, "px4_userland_tree")):
        if source_path is None:
            continue
        commit, tree, source = _git_snapshot_contents(source_path, revision)
        metadata_key = "dtv_userland_core_commit" if revision == pack.CORE_LOCK else "px4_userland_commit"
        prefix_path = "third_party/dtv-userland-core/" if revision == pack.CORE_LOCK else "third_party/px4-userland/"
        if commit != manifest[metadata_key] or tree != manifest.get(tree_key):
            raise ValueError(f"Pinned dependency identity mismatch: {metadata_key}")
        archived = {name[len(prefix_path):]: data for name, data in relative.items() if name.startswith(prefix_path)}
        if archived != source:
            raise ValueError(f"Pinned dependency source differs from Git archive: {metadata_key}")
    if metadata.get("commit") != manifest.get("repository_commit"):
        raise ValueError("Product source commit mismatch")
    if (metadata.get("project") != "pix-userland" or metadata.get("version") != pack.VERSION or
            metadata.get("target") != "source" or metadata.get("private") is not True or
            metadata.get("libusb") != "1.0.30" or metadata.get("libusb_linkage") != "static" or
            metadata.get("device_assets") is not False or
            metadata.get("repository_tree") != manifest.get("repository_tree") or
            metadata.get("libusb_sha256") != source_libusb_sha256):
        raise ValueError("Source release metadata mismatch")
    product_commit = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
    product_tree = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD^{tree}"], text=True).strip()
    if (product_commit != manifest.get("repository_commit") or
            product_tree != manifest.get("repository_tree")):
        raise ValueError("Source archive was generated for a different product HEAD")
    expected_product = {}
    for name, _file in pack.source_files():
        try:
            expected_product[name] = subprocess.check_output(
                ["git", "-C", str(root), "show", f"HEAD:{name}"])
        except subprocess.CalledProcessError as exc:
            raise ValueError(f"Product source is not present in fixed HEAD: {name}") from exc
    archived_product = {name: data for name, data in relative.items()
                        if name in expected_product}
    if archived_product != expected_product:
        raise ValueError("Product source archive differs from the current source inventory")
    usb = relative.get("third_party/libusb-1.0.30.tar.bz2")
    if usb is None or hashlib.sha256(usb).hexdigest() != source_libusb_sha256:
        raise ValueError("Source archive libusb checksum mismatch")
    inventory = manifest.get("files")
    expected_inventory = set(relative) - {"source-manifest.json", "SHA256SUMS"}
    if not isinstance(inventory, dict) or set(inventory) != expected_inventory:
        raise ValueError("Source manifest file inventory mismatch")
    for name, data in relative.items():
        if name in {"source-manifest.json", "SHA256SUMS"}:
            continue
        record = inventory[name]
        if record != {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}:
            raise ValueError(f"Source inventory hash mismatch: {name}")
    license_files = manifest.get("licenses")
    if not isinstance(license_files, dict) or not license_files:
        raise ValueError("Source manifest license hashes are missing")
    for name, digest in license_files.items():
        data = relative.get(name)
        if data is None or hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"Source license hash mismatch: {name}")
    required = {
        "CMakeLists.txt", "cmake/dtv-userland-core.revision", "cmake/px4-userland.revision",
        "pixd.cpp", "tools/pixctl.cpp", "userland/tools/pix_ts.cpp", "PROVENANCE.md",
        "COPYING.libusb", "LICENSE", "THIRD_PARTY_NOTICES.md",
        "third_party/dtv-userland-core/LICENSE", "third_party/dtv-userland-core/PROVENANCE.json",
        "third_party/dtv-userland-core/adapters/host_daemon/COPYING",
        "third_party/dtv-userland-core/adapters/host_facade/COPYING",
        "third_party/dtv-userland-core/adapters/host_runtime/COPYING",
        "third_party/dtv-userland-core/adapters/host_ts/COPYING",
        "third_party/dtv-userland-core/adapters/windows_winscard/COPYING",
        "third_party/px4-userland/LICENSE", "third_party/px4-userland/PROVENANCE.md",
        "third_party/px4-userland/THIRD_PARTY_NOTICES.md",
    }
    if not required <= set(relative):
        raise ValueError(f"Source archive is missing required closure files: {sorted(required-set(relative))}")
    product_provenance = relative["PROVENANCE.md"].decode("utf-8")
    notices = relative["THIRD_PARTY_NOTICES.md"].decode("utf-8")
    for text, label in ((product_provenance, "product provenance"), (notices, "product notices")):
        if (pack.CORE_LOCK not in text or pack.PX4_LOCK not in text or
                "MIT" not in text or "GPL-2.0-only" not in text):
            raise ValueError(f"{label} does not preserve fixed pins and license boundaries")
    sums = {}
    for line in relative["SHA256SUMS"].decode("ascii").splitlines():
        digest, name = line.split("  ", 1)
        sums[name] = digest
    expected_sums = {name: hashlib.sha256(data).hexdigest()
                     for name, data in relative.items() if name != "SHA256SUMS"}
    if sums != expected_sums:
        raise ValueError("Source SHA256SUMS mismatch")
    return {"archive": str(path.resolve()), "files": len(relative), "manifest": manifest}


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--with-assets", action="store_true")
    parser.add_argument("--with-dt295-assets", action="store_true",
                        help="require and verify assets/dt295.assets in darwin-arm64")
    parser.add_argument("--source-only", action="store_true",
                        help="verify only the corresponding-source archive")
    parser.add_argument("--source-archive", type=Path)
    parser.add_argument("--core-source-root", type=Path)
    parser.add_argument("--px4-source-root", type=Path)
    args = parser.parse_args(argv)
    if args.source_only:
        result = verify_source_archive(archive_path=args.source_archive,
                                       core_source_root=args.core_source_root,
                                       px4_source_root=args.px4_source_root,
                                       require_pinned_roots=True)
        print(f"Verified source archive with {result['files']} files and fixed Core/PX4 pins")
        return
    validate_flags(args.with_assets, args.with_dt295_assets)
    verify_release(with_assets=args.with_assets, with_dt295_assets=args.with_dt295_assets)
    print("Verified seven binary packages and matching source; SHA256SUMS created")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, tarfile.TarError) as exc:
        raise SystemExit(f"verify-release: {exc}")
