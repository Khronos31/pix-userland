# Private project

Keep this repository and all Releases PRIVATE. Never change visibility to public or upload contents to public services. Verify privacy before upload and before publishing a release. The owner explicitly authorized non-draft private Releases and bundling their device assets in binary tar.gz packages for their own use (2026-10-03). This supersedes the earlier draft-only/no-assets packaging rule.

Device assets may enter final private Release binary archives. Keep plaintext assets, recordings and session data out of Git and intermediate CI artifacts. The encrypted `.private/assets.enc` is authorized; its key belongs only in a GitHub Actions secret, never Git or logs. Source packages exclude the asset bundle and plaintext assets. Tests use synthetic data. Project code remains MIT; third-party materials retain their licenses.
