# DTV userland common core

Portable C++17, exceptions/RTTI disabled. No OS/USB dependencies in portable targets. Preserve existing behavior while extracting audited independent implementations. This repository starts with reviewed MIT components only; any future GPL adapter must have an explicitly separate target and notice. Do not import card protocol, hardware sequences, vendor assets, or the WinSCard proxy into MIT targets.

Task specification and file ownership for the first increment: `/config/.work/userland-common-20261017/shared/implementation/foundation-spec.md`. Do not edit another worker's files. Existing product tests must remain unchanged. No hardware/HA/firmware/card operations, deployment, public metadata, tag/release, secret reads, or child delegation. Children do not commit/push/beep. Parent reviews and integrates changes.
