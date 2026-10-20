"""Synthetic tests for optional private DT295 release packaging."""
import gzip
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import tarfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load_script(name, module_name):
    spec = importlib.util.spec_from_file_location(module_name, ROOT / "tools" / name)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


finalize = load_script("finalize-private-release.py", "test_finalize_private_release")
verify = load_script("verify-release.py", "test_verify_release")


def synthetic_bundle():
    # Synthetic control-session and transport IVs are independent placeholders.
    material = bytearray(finalize.DT295_MATERIAL_SIZE)
    iv = bytes(range(16, 32))
    assert iv != bytes(material[0x85218 - 0x841D0:0x85218 - 0x841D0 + 16])
    fields = struct.pack("<HBBHH", 0x1000, 0, 1, 1, 0)
    sections = (b"idle-fixture", b"trc-fixture", bytes(material), fields,
                b"key-request!", iv)
    header = finalize.DT295_MAGIC + b"".join(
        struct.pack("<I", len(section)) + hashlib.sha256(section).digest()
        for section in sections
    )
    return header + b"".join(sections)


def write_archive(path, target, members):
    prefix = f"pix-userland-{finalize.pack.VERSION}-{target}/"
    output = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=output, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w") as archive:
            for name, data in members.items():
                info = tarfile.TarInfo(prefix + name)
                info.size = len(data)
                info.mode = 0o644
                archive.addfile(info, io.BytesIO(data))
    path.write_bytes(output.getvalue())


def read_archive(path):
    with tarfile.open(path, "r:gz") as archive:
        return {member.name: archive.extractfile(member).read()
                for member in archive.getmembers()}


def make_release(tmp_path, monkeypatch):
    root = tmp_path / "release"
    finalize.pack.ROOT = ROOT
    verify.pack.ROOT = ROOT
    source_files = finalize.pack.source_files()
    for name, path in source_files:
        destination = root / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(path.read_bytes())
    fixture_sources = {
        "CMakeLists.txt": b"# synthetic release fixture\n",
        "LICENSE": b"MIT License\n",
        "COPYING.libusb": b"libusb license fixture\n",
        "THIRD_PARTY_NOTICES.md": (
            f"Core {finalize.pack.CORE_LOCK}; PX4 {finalize.pack.PX4_LOCK}; MIT; GPL-2.0-only\n"
        ).encode(),
        "PROVENANCE.md": (
            f"Core {finalize.pack.CORE_LOCK}; PX4 {finalize.pack.PX4_LOCK}; MIT; GPL-2.0-only\n"
        ).encode(),
        "cmake/dtv-userland-core.revision": (finalize.pack.CORE_LOCK + "\n").encode(),
        "cmake/px4-userland.revision": (finalize.pack.PX4_LOCK + "\n").encode(),
        "pixd.cpp": b"// synthetic release fixture\n",
        "userland/tools/pix_ts.cpp": b"// synthetic release fixture\n",
        "docs/releases.md": b"Synthetic release fixture\n",
        "packaging/pix-usb-permissions": b"# synthetic release fixture\n",
        "licenses/musl-COPYRIGHT.txt": b"musl license fixture\n",
    }
    for name, data in fixture_sources.items():
        destination = root / name
        if not destination.exists():
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.name", "PIX fixture"], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.email", "pix-fixture@example.invalid"], check=True)
    subprocess.run(["git", "-C", str(root), "add", "-A", "-f"], check=True)
    subprocess.run(["git", "-C", str(root), "commit", "-q", "-m", "synthetic release fixture"], check=True)
    targets = tuple(finalize.pack.TARGETS)
    finalize.pack.ROOT = root
    verify.pack.ROOT = root
    finalize.pack.TARGETS = targets
    verify.pack.TARGETS = targets
    commit = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
    tree = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD^{tree}"], text=True).strip()
    dist = root / "dist"
    dist.mkdir()
    libusb_source = b"synthetic libusb source"
    source_members = {name: path.read_bytes() for name, path in finalize.pack.source_files()}
    source_members.update({
        "third_party/dtv-userland-core/LICENSE": b"MIT License\n",
        "third_party/dtv-userland-core/PROVENANCE.json": b"{}\n",
        "third_party/dtv-userland-core/adapters/host_runtime/COPYING": b"GPL-2.0-only\n",
        "third_party/dtv-userland-core/adapters/host_daemon/COPYING": b"GPL-2.0-only\n",
        "third_party/dtv-userland-core/adapters/host_facade/COPYING": b"GPL-2.0-only\n",
        "third_party/dtv-userland-core/adapters/host_ts/COPYING": b"GPL-2.0-only\n",
        "third_party/dtv-userland-core/adapters/windows_winscard/COPYING": b"GPL-2.0-only\n",
        "third_party/px4-userland/LICENSE": b"GPL-2.0-only\n",
        "third_party/px4-userland/PROVENANCE.md": b"PX4 fixture\n",
        "third_party/px4-userland/THIRD_PARTY_NOTICES.md": b"PX4 fixture notices\n",
        "third_party/libusb-1.0.30.tar.bz2": libusb_source,
    })
    core_tree = "31ba29eab93f8166e53ff5b37458c9d554899152"
    px4_tree = "3f69476186f07be34e79e7a40cf1fb3f8b0c55e8"
    source_id = {
        "repository_commit": commit, "repository_tree": tree,
        "dtv_userland_core_commit": finalize.pack.CORE_LOCK,
        "dtv_userland_core_tree": core_tree,
        "px4_userland_commit": finalize.pack.PX4_LOCK,
        "px4_userland_tree": px4_tree,
    }
    source_members["release-info.json"] = json.dumps({
        "project": "pix-userland", "version": finalize.pack.VERSION,
        "target": "source", "commit": commit, "libusb": "1.0.30",
        "private": True, "libusb_linkage": "static", "device_assets": False,
        **source_id, "libusb_sha256": hashlib.sha256(libusb_source).hexdigest(),
    }).encode() + b"\n"
    source_files_manifest = {
        name: {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}
        for name, data in source_members.items()
    }
    licenses = {
        name: hashlib.sha256(data).hexdigest()
        for name, data in source_members.items()
        if name in {"LICENSE", "COPYING.libusb", "third_party/dtv-userland-core/LICENSE",
                    "third_party/px4-userland/LICENSE"} or name.endswith("/COPYING")
    }
    source_members["source-manifest.json"] = json.dumps({
        "schema": 1, "kind": "corresponding-source", "project": "pix-userland",
        "version": finalize.pack.VERSION, "private": True, **source_id,
        "libusb_version": "1.0.30",
        "libusb_archive_sha256": hashlib.sha256(libusb_source).hexdigest(),
        "licenses": licenses, "files": source_files_manifest,
    }, sort_keys=True, indent=2).encode() + b"\n"
    source_members["SHA256SUMS"] = "".join(
        f"{hashlib.sha256(data).hexdigest()}  {name}\n"
        for name, data in sorted(source_members.items())
    ).encode("ascii")
    for target in (*targets, "source"):
        if target == "source":
            write_archive(dist / f"pix-userland-{finalize.pack.VERSION}-source.tar.gz",
                          target, source_members)
            continue
        members = {
            "release-info.json": json.dumps({
                "project": "pix-userland", "version": finalize.pack.VERSION,
                "target": target, "commit": commit, "libusb": "1.0.30",
                "private": True, "libusb_linkage": "static", "device_assets": False,
            }).encode() + b"\n",
            "LICENSE": b"license", "COPYING.libusb": b"copying",
            "THIRD_PARTY_NOTICES.md": b"notices", "licenses/musl-COPYRIGHT.txt": b"musl",
        }
        suffix = ".exe" if target == "windows-x64" else ""
        for program in ("pixd", "pixctl", "pix-ts"):
            members[program + suffix] = b"binary"
        write_archive(dist / f"pix-userland-{finalize.pack.VERSION}-{target}.tar.gz", target, members)
    return root, commit


def make_dt181_assets(tmp_path):
    assets = tmp_path / "dt181"
    assets.mkdir()
    for name, size in finalize.DT181_SIZES.items():
        (assets / name).write_bytes(bytes([len(name)]) * size)
    return assets


class TestDT295Packaging(unittest.TestCase):
  def test_bundle_validator_checks_all_hashes_and_structure(self):
    bundle = synthetic_bundle()
    self.assertEqual(len(finalize.validate_dt295_bundle(bundle)), 6)
    offset = finalize.DT295_HEADER_SIZE
    for index in range(finalize.DT295_SECTION_COUNT):
        corrupt = bytearray(bundle)
        corrupt[offset] ^= 0x80
        with self.subTest(section=index), self.assertRaisesRegex(ValueError, "SHA-256"):
            finalize.validate_dt295_bundle(bytes(corrupt))
        offset += struct.unpack_from("<I", bundle, 8 + index * 36)[0]
    with self.assertRaisesRegex(ValueError, "trailing"):
        finalize.validate_dt295_bundle(bundle + b"x")


  def test_optional_dt295_is_only_added_to_matching_binary_and_source_stays_unchanged(self):
    with __import__("tempfile").TemporaryDirectory() as temp:
      tmp_path = Path(temp)
      root, commit = make_release(tmp_path, self)
      source_path = root / "dist" / f"pix-userland-{finalize.pack.VERSION}-source.tar.gz"
      source_before = source_path.read_bytes()
      dt181 = make_dt181_assets(tmp_path)
      bundle_path = tmp_path / "dt295.assets"
      bundle_path.write_bytes(synthetic_bundle())

      finalize.finalize_private_release(dt181, bundle_path, root=root)

      for target in finalize.pack.TARGETS:
        contents = read_archive(root / "dist" / f"pix-userland-{finalize.pack.VERSION}-{target}.tar.gz")
        prefix = f"pix-userland-{finalize.pack.VERSION}-{target}/"
        metadata = json.loads(contents[prefix + "release-info.json"])
        expected = target == "darwin-arm64"
        self.assertIs(metadata["dt295_assets"], expected)
        self.assertEqual((prefix + "assets/dt295.assets" in contents), expected)
        if expected:
            self.assertEqual(contents[prefix + "assets/dt295.assets"], bundle_path.read_bytes())
      self.assertEqual(source_path.read_bytes(), source_before)

      verify.verify_release(root=root, with_assets=True, with_dt295_assets=True,
                          commit=commit, source_libusb_sha256=hashlib.sha256(b"synthetic libusb source").hexdigest())


  def test_legacy_finalization_sets_false_and_verifies_without_dt295(self):
    with __import__("tempfile").TemporaryDirectory() as temp:
      tmp_path = Path(temp)
      root, commit = make_release(tmp_path, self)
      finalize.finalize_private_release(make_dt181_assets(tmp_path), root=root)
      for target in finalize.pack.TARGETS:
        contents = read_archive(root / "dist" / f"pix-userland-{finalize.pack.VERSION}-{target}.tar.gz")
        prefix = f"pix-userland-{finalize.pack.VERSION}-{target}/"
        self.assertIs(json.loads(contents[prefix + "release-info.json"])["dt295_assets"], False)
        self.assertFalse(any(name.startswith(prefix + "assets/dt295") for name in contents))
      verify.verify_release(root=root, with_assets=True, commit=commit,
                          source_libusb_sha256=hashlib.sha256(b"synthetic libusb source").hexdigest())


  def test_invalid_bundle_is_rejected_before_any_archive_changes(self):
    with __import__("tempfile").TemporaryDirectory() as temp:
      tmp_path = Path(temp)
      root, _ = make_release(tmp_path, self)
      before = {path.name: path.read_bytes() for path in (root / "dist").glob("*.tar.gz")}
      invalid = tmp_path / "invalid.assets"
      invalid.write_bytes(synthetic_bundle() + b"extra")
      with self.assertRaisesRegex(ValueError, "trailing"):
          finalize.finalize_private_release(make_dt181_assets(tmp_path), invalid, root=root)
      after = {path.name: path.read_bytes() for path in (root / "dist").glob("*.tar.gz")}
      self.assertEqual(after, before)


  def test_verifier_requires_both_asset_flags_and_detects_bad_dt295_hash(self):
    with self.assertRaisesRegex(ValueError, "requires --with-assets"):
        verify.validate_flags(False, True)
    with __import__("tempfile").TemporaryDirectory() as temp:
      tmp_path = Path(temp)
      root, commit = make_release(tmp_path, self)
      bundle_path = tmp_path / "dt295.assets"
      bundle_path.write_bytes(synthetic_bundle())
      finalize.finalize_private_release(make_dt181_assets(tmp_path), bundle_path, root=root)
      archive_path = root / "dist" / f"pix-userland-{finalize.pack.VERSION}-darwin-arm64.tar.gz"
      members = read_archive(archive_path)
      asset_name = f"pix-userland-{finalize.pack.VERSION}-darwin-arm64/assets/dt295.assets"
      bad_bundle = bytearray(members[asset_name])
      bad_bundle[-1] ^= 1
      members[asset_name] = bytes(bad_bundle)
      write_archive(archive_path, "darwin-arm64", {
        name.removeprefix(f"pix-userland-{finalize.pack.VERSION}-darwin-arm64/"): data
        for name, data in members.items()
      })
      with self.assertRaisesRegex(ValueError, "SHA-256"):
        verify.verify_release(root=root, with_assets=True, with_dt295_assets=True,
                              commit=commit, source_libusb_sha256=hashlib.sha256(b"synthetic libusb source").hexdigest())


if __name__ == "__main__":
    unittest.main()
