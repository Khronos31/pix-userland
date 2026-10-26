"""Tests embedded asset metadata, byte proof, and corresponding-source closure."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import test_dt295_packaging as fixture


verify = fixture.verify
pack = verify.pack


def embedded_members(target, commit, assets):
    files = assets
    manifest = {
        "schema": 1,
        "source_revision": commit,
        "dtv_userland_core_revision": verify.CORE_PIN,
        "generator": "tools/embed-assets.py",
        "toolchain": "synthetic-test-toolchain",
        "assets": {
            "dt181": {
                "provenance": pack.DT181_PROVENANCE,
                "files": {
                    name.removeprefix("assets/"): {
                        "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                    for name, data in files.items() if name.startswith("assets/") and name != "assets/dt295.assets"
                },
            },
            "dt295": {
                "provenance": pack.DT295_PROVENANCE,
                "file": {"name": "dt295.assets", "size": len(files["assets/dt295.assets"]),
                         "sha256": hashlib.sha256(files["assets/dt295.assets"]).hexdigest()},
            },
        },
    }
    manifest_bytes = (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode()
    build_info = (
        f"dtv-userland-core {verify.CORE_PIN}\n"
        f"embedded_source_revision={commit}\n"
        f"embedded_core_revision={verify.CORE_PIN}\n"
        "embedded_toolchain=synthetic-test-toolchain\n"
        f"embedded_manifest_sha256={hashlib.sha256(manifest_bytes).hexdigest()}\n"
    )
    for model, entries in manifest["assets"].items():
        records = entries["files"] if "files" in entries else {entries["file"]["name"]: entries["file"]}
        for name, record in sorted(records.items()):
            build_info += f"embedded_asset={model}/{name} size={record['size']} sha256={record['sha256']}\n"
    metadata = fixture.binary_members(target, commit)
    release_info = json.loads(metadata["release-info.json"])
    release_info["device_assets"] = True
    release_info["embedded_assets"] = pack.embedded_build_metadata(
        build_info.encode(), manifest, manifest_bytes)
    metadata["release-info.json"] = (json.dumps(release_info, sort_keys=True) + "\n").encode()
    metadata["build-info.txt"] = build_info.encode()
    metadata["embedded-assets.json"] = manifest_bytes
    metadata["pixd" + (".exe" if target == "windows-x64" else "")] += b"".join(
        files[name] for name in sorted(files))
    return metadata


def write_embedded_release(root, commit):
    out = root/"dist"
    source_prefix = f"pix-userland-{pack.VERSION}-source/"
    source = fixture.source_members(commit)
    fixture.write_archive(out/f"pix-userland-{pack.VERSION}-source.tar.gz", "source", source)
    source_contents = {name: data for name, data in source.items() if name.startswith("assets/")}
    for target in pack.TARGETS:
        members = embedded_members(target, commit, source_contents)
        fixture.write_archive(out/f"pix-userland-{pack.VERSION}-{target}.tar.gz", target, members)


class TestEmbeddedRelease(unittest.TestCase):
    def test_embedded_release_verifies_all_targets_and_source_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)/"release"
            (root/"dist").mkdir(parents=True)
            commit = "d"*40
            fixture.finalize.pack.ROOT = root
            verify.pack.ROOT = root
            write_embedded_release(root, commit)
            verify.verify_release(root=root, with_embedded_assets=True, commit=commit,
                                  source_libusb_sha256=hashlib.sha256(fixture.SYNTHETIC_LIBUSB).hexdigest())

    def test_embedded_release_rejects_missing_bytes_and_wrong_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)/"release"
            (root/"dist").mkdir(parents=True)
            commit = "e"*40
            fixture.finalize.pack.ROOT = root
            verify.pack.ROOT = root
            write_embedded_release(root, commit)
            path = root/"dist"/f"pix-userland-{pack.VERSION}-linux-x86_64.tar.gz"
            prefix = f"pix-userland-{pack.VERSION}-linux-x86_64/"
            members = {name.removeprefix(prefix): data for name, data in fixture.read_archive(path).items()}
            members["pixd"] = b"binary without embedded assets"
            fixture.write_archive(path, "linux-x86_64", members)
            with self.assertRaisesRegex(ValueError, "lacks embedded asset bytes"):
                verify.verify_release(root=root, with_embedded_assets=True, commit=commit,
                                      source_libusb_sha256=hashlib.sha256(fixture.SYNTHETIC_LIBUSB).hexdigest())

    def test_embedded_release_rejects_release_metadata_not_matching_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)/"release"
            (root/"dist").mkdir(parents=True)
            commit = "f"*40
            fixture.finalize.pack.ROOT = root
            verify.pack.ROOT = root
            write_embedded_release(root, commit)
            path = root/"dist"/f"pix-userland-{pack.VERSION}-linux-x86_64.tar.gz"
            prefix = f"pix-userland-{pack.VERSION}-linux-x86_64/"
            members = {name.removeprefix(prefix): data for name, data in fixture.read_archive(path).items()}
            release_info = json.loads(members["release-info.json"])
            release_info["embedded_assets"]["manifest_sha256"] = "0"*64
            members["release-info.json"] = (json.dumps(release_info, sort_keys=True) + "\n").encode()
            fixture.write_archive(path, "linux-x86_64", members)
            with self.assertRaisesRegex(ValueError, "Release metadata differs"):
                verify.verify_release(root=root, with_embedded_assets=True, commit=commit,
                                      source_libusb_sha256=hashlib.sha256(fixture.SYNTHETIC_LIBUSB).hexdigest())


if __name__ == "__main__":
    unittest.main()
