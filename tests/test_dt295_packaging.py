"""Synthetic tests for optional private DT295 release packaging."""
import gzip
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tarfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))


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


SYNTHETIC_LIBUSB = b"synthetic libusb source"


def source_members(commit):
    """Synthetic corresponding-source closure whose manifest matches its archive."""
    files = {
        "LICENSE": (ROOT / "LICENSE").read_bytes(), "COPYING.libusb": b"copying",
        "THIRD_PARTY_NOTICES.md": b"notices", "licenses/musl-COPYRIGHT.txt": b"musl",
        "licenses/GPL-2.0.txt": (ROOT / "licenses/GPL-2.0.txt").read_bytes(),
        "core-integration/CMakeLists.txt": b"project()",
        "core-integration/dtv-userland-core.revision": verify.CORE_PIN.encode() + b"\n",
        "core-integration/pixd.cpp": b"pixd", "core-integration/tools/pixctl.cpp": b"pixctl",
        "core-integration/tools/pix_ts.cpp": b"pix-ts",
        "tools/build-windows.sh": b"build", "include/os.h": b"os",
        "src/dt181_capture_host.c": b"capture", "pix-dt181-consumer.cpp": b"consumer",
        "core-integration/tests/windows_os_thread_proof.c": b"thread-proof",
        "tests/test_windows_runtime.py": b"runtime-proof",
        "third_party/dtv-userland-core/LICENSE": b"mit",
        "third_party/dtv-userland-core/PROVENANCE.json": b"{}",
        "third_party/dtv-userland-core/CMakeLists.txt": b"project()",
        "third_party/dtv-userland-core/adapters/host_ts/COPYING": b"gpl",
        "third_party/libusb-1.0.30.tar.bz2": SYNTHETIC_LIBUSB,
    }
    for name in ("dt181.bin", "dt295.bin", "manifest.json"):
        files["firmware/" + name] = (ROOT / "firmware" / name).read_bytes()
    files["firmware/metadata/dt181-source.json"] = (ROOT / "firmware/metadata/dt181-source.json").read_bytes()
    files["firmware/metadata/dt295-preservation.json"] = (ROOT / "firmware/metadata/dt295-preservation.json").read_bytes()
    files["firmware/metadata/dt295-satellite-generation.json"] = (ROOT / "firmware/metadata/dt295-satellite-generation.json").read_bytes()
    files["firmware/preserved/verified-ts-iv.bin"] = (ROOT / "firmware/preserved/verified-ts-iv.bin").read_bytes()
    asset_inputs = verify._source_asset_records(files, "")
    identity = {
        "repository_commit": commit, "repository_tree": "b" * 40,
        "dtv_userland_core_commit": verify.CORE_PIN, "dtv_userland_core_tree": "c" * 40,
        "libusb_sha256": hashlib.sha256(SYNTHETIC_LIBUSB).hexdigest(),
    }
    metadata = {
        "project": "pix-userland", "version": finalize.pack.VERSION, "target": "source",
        "commit": commit, "libusb": "1.0.30", "private": False, "libusb_linkage": "static",
        "device_assets": False, "asset_inputs": asset_inputs, **identity,
    }
    files["release-info.json"] = json.dumps(metadata).encode() + b"\n"
    manifest = {"schema": 1, "kind": "corresponding-source", **identity,
                "files": {name: {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}
                          for name, data in files.items()}}
    files["source-manifest.json"] = json.dumps(manifest, sort_keys=True).encode() + b"\n"
    names = sorted(files)
    files["SHA256SUMS"] = "".join(f"{hashlib.sha256(files[name]).hexdigest()}  {name}\n"
                                  for name in names).encode()
    return files


def binary_members(target, commit):
    suffix = ".exe" if target == "windows-x64" else ""
    source = source_members(commit)
    asset_inputs = verify._source_asset_records(source, "")
    members = {
        "release-info.json": json.dumps({
            "project": "pix-userland", "version": finalize.pack.VERSION,
            "target": target, "commit": commit, "libusb": "1.0.30",
            "private": False, "libusb_linkage": "static", "device_assets": True,
            "dtv_userland_core_commit": verify.CORE_PIN, "asset_inputs": asset_inputs,
        }).encode() + b"\n",
        "LICENSE": (ROOT / "LICENSE").read_bytes(), "COPYING.libusb": b"copying",
        "THIRD_PARTY_NOTICES.md": b"notices", "licenses/musl-COPYRIGHT.txt": b"musl",
        "licenses/GPL-2.0.txt": (ROOT / "licenses/GPL-2.0.txt").read_bytes(),
        "build-info.txt": f"dtv-userland-core {verify.CORE_PIN}\n".encode(),
        "licenses/dtv-userland-core-LICENSE": b"mit",
        "licenses/dtv-userland-core-host_ts-COPYING": b"gpl",
        "firmware/dt181.bin": source["firmware/dt181.bin"],
        "firmware/dt295.bin": source["firmware/dt295.bin"],
    }
    members.update({program + suffix: b"binary" for program in verify.pack.PROGRAMS})
    return members


def make_release(tmp_path, monkeypatch):
    root = tmp_path / "release"
    dist = root / "dist"
    dist.mkdir(parents=True)
    targets = tuple(finalize.pack.TARGETS)
    finalize.pack.ROOT = root
    verify.pack.ROOT = root
    finalize.pack.TARGETS = targets
    verify.pack.TARGETS = targets
    commit = "a" * 40
    for target in (*targets, "source"):
        members = source_members(commit) if target == "source" else binary_members(target, commit)
        write_archive(dist / f"pix-userland-{finalize.pack.VERSION}-{target}.tar.gz", target, members)
    return root, commit


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


  def test_invalid_bundle_is_rejected_before_any_archive_changes(self):
    with __import__("tempfile").TemporaryDirectory() as temp:
      tmp_path = Path(temp)
      root, _ = make_release(tmp_path, self)
      before = {path.name: path.read_bytes() for path in (root / "dist").glob("*.tar.gz")}
      invalid = tmp_path / "invalid.bin"
      invalid.write_bytes((ROOT / "firmware/dt295.bin").read_bytes() + b"extra")
      with self.assertRaisesRegex(ValueError, "size"):
          finalize.finalize_private_release(ROOT / "firmware/dt181.bin", invalid, root=root)
      after = {path.name: path.read_bytes() for path in (root / "dist").glob("*.tar.gz")}
      self.assertEqual(after, before)


  def test_every_target_requires_both_external_bundles(self):
    with __import__("tempfile").TemporaryDirectory() as temp:
      tmp_path = Path(temp)
      root, commit = make_release(tmp_path, self)
      for target in finalize.pack.TARGETS:
        archive_path = root / "dist" / f"pix-userland-{finalize.pack.VERSION}-{target}.tar.gz"
        members = read_archive(archive_path)
        prefix = f"pix-userland-{finalize.pack.VERSION}-{target}/"
        for name in ("dt181.bin", "dt295.bin"):
            self.assertIn(prefix + "firmware/" + name, members)
        self.assertEqual(json.loads(members[prefix + "release-info.json"])["device_assets"], True)
      verify.verify_release(root=root, commit=commit,
                            source_libusb_sha256=hashlib.sha256(SYNTHETIC_LIBUSB).hexdigest())

  def test_packager_adds_both_external_bundles_to_every_target(self):
    pack = load_script("package-release.py", "test_package_release_bundles")
    pack.git_value = lambda *args: "a" * 40
    with __import__("tempfile").TemporaryDirectory() as temp:
      root = Path(temp)
      source = root / "source"
      required = ("README.md", "LICENSE", "COPYING.libusb", "THIRD_PARTY_NOTICES.md",
                  "docs/releases.md", "packaging/pix-usb-permissions", "packaging/pix-termux",
                  "licenses/musl-COPYRIGHT.txt", "licenses/GPL-2.0.txt")
      for name in required:
          path = source / name
          path.parent.mkdir(parents=True, exist_ok=True)
          path.write_text("fixture")
      for name in ("dt181.bin", "dt295.bin"):
          path = source / "firmware" / name
          path.parent.mkdir(parents=True, exist_ok=True)
          path.write_bytes((ROOT / "firmware" / name).read_bytes())
      pack.ROOT = source
      pack.source_files = lambda: [(name, source / name) for name in required[4:]]
      binary = root / "binary"
      (binary / "licenses").mkdir(parents=True)
      (binary / "build-info.txt").write_text(f"dtv-userland-core {pack.CORE_LOCK}\n")
      (binary / "licenses/musl-COPYRIGHT.txt").write_text("musl")
      (binary / "licenses/GPL-2.0.txt").write_bytes((ROOT / "licenses/GPL-2.0.txt").read_bytes())
      for target in pack.TARGETS:
          suffix = ".exe" if target == "windows-x64" else ""
          for program in pack.PROGRAMS:
              (binary / (program + suffix)).write_bytes(b"binary")
          pack.package(target, binary_dir=binary, output_dir=root / "dist")
          archive_path = root / "dist" / f"pix-userland-{pack.VERSION}-{target}.tar.gz"
          prefix = f"pix-userland-{pack.VERSION}-{target}/"
          contents = read_archive(archive_path)
          self.assertEqual(contents[prefix + "firmware/dt181.bin"],
                           (ROOT / "firmware/dt181.bin").read_bytes())
          self.assertEqual(contents[prefix + "firmware/dt295.bin"],
                           (ROOT / "firmware/dt295.bin").read_bytes())
          info = json.loads(contents[prefix + "release-info.json"])
          self.assertTrue(info["device_assets"])

  def test_verifier_rejects_missing_or_tampered_external_bundle(self):
    for name, change, message in (
        ("missing", lambda members, prefix: members.pop("firmware/dt295.bin"),
         "Binary asset differs from source input"),
        ("tampered", lambda members, prefix: members.__setitem__(
            "firmware/dt295.bin", members["firmware/dt295.bin"][:-1] + b"x"),
         "Binary asset differs from source input"),
    ):
      with self.subTest(case=name), __import__("tempfile").TemporaryDirectory() as temp:
        root, commit = make_release(Path(temp), self)
        self._rewrite(root, "linux-x86_64",
                      lambda members: change(members, f"pix-userland-{finalize.pack.VERSION}-linux-x86_64/"))
        with self.assertRaisesRegex(ValueError, message):
          self._verify(root, commit)

  def test_verifier_rejects_embedded_metadata_and_manifest(self):
    with __import__("tempfile").TemporaryDirectory() as temp:
      root, commit = make_release(Path(temp), self)
      def add_old_metadata(members):
          info = json.loads(members["release-info.json"])
          info["embedded_assets"] = {"legacy": True}
          members["release-info.json"] = json.dumps(info).encode()
          members["embedded-assets.json"] = b"{}"
      self._rewrite(root, "linux-x86_64", add_old_metadata)
      with self.assertRaisesRegex(ValueError, "Embedded asset output is no longer supported"):
          self._verify(root, commit)


  def test_verifier_rejects_grant_only_change(self):
    with __import__("tempfile").TemporaryDirectory() as temp:
      root, commit = make_release(Path(temp), self)
      original = (ROOT / "LICENSE").read_text()
      wrong = original.replace(
          "either version 2 of the\nLicense, or (at your option) any later version.",
          "version 2 of the\nLicense only; no later version is permitted.")
      self.assertNotEqual(wrong, original)
      self._rewrite(root, "linux-x86_64",
                    lambda members: members.update({"LICENSE": wrong.encode()}))
      with self.assertRaisesRegex(ValueError, "PIX GPL-2.0-or-later grant"):
        self._verify(root, commit)

  def _rewrite(self, root, target, edit):
    path = root / "dist" / f"pix-userland-{finalize.pack.VERSION}-{target}.tar.gz"
    prefix = f"pix-userland-{finalize.pack.VERSION}-{target}/"
    members = {name.removeprefix(prefix): data for name, data in read_archive(path).items()}
    edit(members)
    write_archive(path, target, members)

  def _verify(self, root, commit):
    verify.verify_release(root=root, commit=commit,
                          source_libusb_sha256=hashlib.sha256(SYNTHETIC_LIBUSB).hexdigest())

  def test_three_program_release_and_source_closure_verify(self):
    with __import__("tempfile").TemporaryDirectory() as temp:
      root, commit = make_release(Path(temp), self)
      self._verify(root, commit)

  def test_binary_package_requires_all_three_programs_and_rejects_legacy_cli(self):
    for removed in ("pixd", "pixctl", "pix-ts"):
      with self.subTest(removed=removed), __import__("tempfile").TemporaryDirectory() as temp:
        root, commit = make_release(Path(temp), self)
        self._rewrite(root, "linux-x86_64", lambda members: members.pop(removed))
        with self.assertRaisesRegex(ValueError, "Missing executable"):
          self._verify(root, commit)
    with __import__("tempfile").TemporaryDirectory() as temp:
      root, commit = make_release(Path(temp), self)
      self._rewrite(root, "linux-x86_64", lambda members: members.update({"pix-ts-legacy": b"old"}))
      with self.assertRaisesRegex(ValueError, "Legacy standalone CLI"):
        self._verify(root, commit)

  def test_binary_package_requires_core_pin_and_notices(self):
    cases = {
      "build-info": (lambda members: members.update({"build-info.txt": b"no pin\n"}), "Core pin"),
      "license": (lambda members: members.pop("licenses/dtv-userland-core-LICENSE"), "Core license"),
      "copying": (lambda members: members.pop("licenses/dtv-userland-core-host_ts-COPYING"), "Core license"),
      "gpl2": (lambda members: members.pop("licenses/GPL-2.0.txt"), "GPL-2.0"),
      "gpl2-changed": (lambda members: members.update({"licenses/GPL-2.0.txt": b"changed"}), "GPLv2 text"),
      "pix-license": (lambda members: members.update({"LICENSE": b"not a license"}), "PIX GPL"),
    }
    for name, (edit, message) in cases.items():
      with self.subTest(case=name), __import__("tempfile").TemporaryDirectory() as temp:
        root, commit = make_release(Path(temp), self)
        self._rewrite(root, "windows-x64", edit)
        with self.assertRaisesRegex(ValueError, message):
          self._verify(root, commit)

  def test_source_closure_detects_tampering_and_missing_dependency(self):
    cases = {
      "changed-core-file": (lambda members: members.update(
          {"third_party/dtv-userland-core/LICENSE": b"changed"}), "hash mismatch"),
      "dropped-core": (lambda members: [members.pop(name) for name in list(members)
                                        if name.startswith("third_party/dtv-userland-core/")],
                       "file list differs"),
      "changed-pin": (lambda members: members.update(
          {"core-integration/dtv-userland-core.revision": b"0" * 40 + b"\n"}), "Core pin"),
    }
    for name, (edit, message) in cases.items():
      with self.subTest(case=name), __import__("tempfile").TemporaryDirectory() as temp:
        root, commit = make_release(Path(temp), self)
        self._rewrite(root, "source", edit)
        with self.assertRaisesRegex(ValueError, message):
          self._verify(root, commit)

  def test_source_allowlist_covers_core_integration_and_adapter_sources(self):
    names = {name for name, _ in load_script("package-release.py", "test_package_release_real").source_files()}
    for required in ("core-integration/CMakeLists.txt", "core-integration/dtv-userland-core.revision",
                     "core-integration/pixd.cpp", "core-integration/tools/pixctl.cpp",
                     "core-integration/tools/pix_ts.cpp", "pix-dt181-receiver-adapter.cpp",
                     "pix-dt295-receiver-adapter.cpp", "tests/pix_dt295_adapter_lifecycle_proof.cpp",
                     "tools/build-windows.sh", "core-integration/tests/windows_os_thread_proof.c",
                     "tests/test_windows_runtime.py"):
      self.assertIn(required, names)
    for required in ("firmware/dt181.bin", "firmware/dt295.bin", "firmware/manifest.json",
                     "firmware/metadata/dt181-source.json", "firmware/metadata/dt295-preservation.json",
                     "firmware/metadata/dt295-satellite-generation.json",
                     "firmware/preserved/verified-ts-iv.bin"):
      self.assertIn(required, names)
    self.assertNotIn(".private/assets.enc", names)


if __name__ == "__main__":
    unittest.main()
