"""Checks the release workflow's candidate and publish mode boundary."""
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]


class TestReleaseWorkflowGate(unittest.TestCase):
    def mode(self, candidate, ref):
        return subprocess.run(
            ["sh", str(ROOT/"tools/release-publish-mode.sh"), candidate, ref],
            cwd=ROOT, capture_output=True, text=True)

    def test_candidate_never_publishes(self):
        for ref in ("refs/heads/pix-release-embedding", "refs/heads/main"):
            with self.subTest(ref=ref):
                result = self.mode("true", ref)
                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stdout.strip(), "false")

    def test_non_candidate_publish_is_main_only(self):
        result = self.mode("false", "refs/heads/main")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.strip(), "true")
        self.assertNotEqual(self.mode("false", "refs/heads/pix-release-embedding").returncode, 0)

    def test_workflow_guards_publish_job_with_output_and_main_ref(self):
        workflow = (ROOT/".github/workflows/release.yml").read_text()
        self.assertIn("type: boolean", workflow)
        self.assertIn("sh tools/release-publish-mode.sh", workflow)
        self.assertIn("publish: ${{ steps.version.outputs.publish }}", workflow)
        self.assertIn("if: needs.preflight.outputs.publish == 'true' && github.ref == 'refs/heads/main'", workflow)
        self.assertNotIn("secrets.PIX_ASSETS_KEY", workflow)
        self.assertNotIn("finalize-private-release.py", workflow)


if __name__ == "__main__":
    unittest.main()
