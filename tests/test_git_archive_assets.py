import hashlib
import io
import json
import subprocess
import tarfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class GitArchiveAssetsTest(unittest.TestCase):
    def test_source_archive_contains_runtime_bundles_and_provenance(self):
        archive_bytes = subprocess.check_output(
            ["git", "-C", str(ROOT), "archive", "--format=tar", "HEAD"])
        with tarfile.open(fileobj=io.BytesIO(archive_bytes), mode="r:") as archive:
            members = {member.name: archive.extractfile(member).read()
                       for member in archive.getmembers() if member.isfile()}
        required = (
            "firmware/dt181.bin", "firmware/dt295.bin", "firmware/manifest.json",
            "firmware/metadata/dt181-source.json",
            "firmware/metadata/dt295-preservation.json",
            "firmware/metadata/dt295-satellite-generation.json",
            "firmware/preserved/verified-ts-iv.bin",
        )
        for path in required:
            self.assertIn(path, members)
        self.assertNotIn(".private/assets.enc", members)

        manifest = json.loads(members["firmware/manifest.json"])
        dt295 = members["firmware/dt295.bin"]
        record = manifest["models"]["dt295"]
        self.assertEqual(len(dt295), record["size"])
        self.assertEqual(hashlib.sha256(dt295).hexdigest(), record["sha256"])
        metadata = json.loads(members["firmware/metadata/dt295-satellite-generation.json"])
        self.assertEqual(metadata["standard_bundle"]["sha256"], record["sha256"])


if __name__ == "__main__":
    unittest.main()
