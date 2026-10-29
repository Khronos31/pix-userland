import hashlib
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from firmware_bundle import generate_header, pack_dt181, read_manifest, unpack_dt181, verify_bundle

ROOT = Path(__file__).resolve().parents[1]


class FirmwareBundleTests(unittest.TestCase):
    def test_dt181_bundle_restores_original_payload_digests(self):
        manifest = read_manifest()
        bundle = (ROOT / "firmware" / "dt181.bin").read_bytes()
        restored = unpack_dt181(bundle, manifest)
        self.assertEqual(pack_dt181(restored, manifest), bundle)
        for section in manifest["models"]["dt181"]["sections"]:
            self.assertEqual(hashlib.sha256(restored[section["name"]]).hexdigest(),
                             section["sha256"])
        self.assertEqual(len(bundle), manifest["models"]["dt181"]["size"])

    def test_dt181_rejects_same_length_modified_bundle_and_other_model(self):
        manifest = read_manifest()
        bundle = (ROOT / "firmware" / "dt181.bin").read_bytes()
        modified = bytearray(bundle)
        modified[-1] ^= 1
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            verify_bundle("dt181", modified, manifest)
        with self.assertRaisesRegex(ValueError, "size"):
            verify_bundle("dt295", bundle, manifest)
        with self.assertRaisesRegex(ValueError, "size"):
            verify_bundle("dt181", bundle[:-1], manifest)

    def test_dt295_hash_allowlist_and_format_parser(self):
        manifest = read_manifest()
        bundle = (ROOT / "firmware" / "dt295.bin").read_bytes()
        verify_bundle("dt295", bundle, manifest)
        finalizer_path = ROOT / "tools" / "finalize-private-release.py"
        import importlib.util
        spec = importlib.util.spec_from_file_location("pix_finalize", finalizer_path)
        finalizer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(finalizer)
        finalizer.validate_dt295_bundle(bundle)
        modified = bytearray(bundle)
        modified[len(modified) // 2] ^= 1
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            verify_bundle("dt295", modified, manifest)
        with self.assertRaisesRegex(ValueError, "size"):
            verify_bundle("dt181", bundle, manifest)

    def test_generated_runtime_header_uses_canonical_hashes(self):
        manifest = read_manifest()
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "pix_firmware_manifest_generated.h"
            generate_header(output, manifest)
            header = output.read_text(encoding="ascii")
        self.assertIn("PIX_DT181_BUNDLE_SIZE 699773U", header)
        self.assertIn("PIX_DT295_BUNDLE_SIZE 1839100U", header)
        self.assertIn("PIX_DT181_FPGA_SIZE 699673U", header)
        self.assertIn("PIX_DT181_VALIDATION_SHA256", header)

    def test_embed_generator_uses_the_same_whole_bundle_allowlist(self):
        import importlib.util
        generator_path = ROOT / "tools" / "embed-assets.py"
        spec = importlib.util.spec_from_file_location("pix_embed_assets_test", generator_path)
        generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(generator)
        dt181 = ROOT / "firmware" / "dt181.bin"
        dt295 = ROOT / "firmware" / "dt295.bin"
        generator._load_assets(dt181, dt295)
        with tempfile.TemporaryDirectory() as temporary:
            changed = Path(temporary) / "dt181.bin"
            data = bytearray(dt181.read_bytes())
            data[-1] ^= 1
            changed.write_bytes(data)
            self.assertEqual(changed.stat().st_size, dt181.stat().st_size)
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                generator._load_assets(changed, dt295)
            with self.assertRaisesRegex(ValueError, "size"):
                generator._load_assets(dt181, dt181)


if __name__ == "__main__":
    unittest.main()
