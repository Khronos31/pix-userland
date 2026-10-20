# Source provenance

The source package identifies the PIX product snapshot by Git commit and tree
in `source-manifest.json`. Its dependency locks are
`cmake/dtv-userland-core.revision` (`35249f85347093ca3b431cbe4a730b2366de7ada`)
and `cmake/px4-userland.revision`
(`90b08dfe79b0b40e393f015975c761863830866a`). The archive carries Git
archives of those exact dependency commits, not their current working heads.

The root `LICENSE` applies to the PIX-owned original source covered by that
license. Individual SPDX notices remain controlling for files that carry
their own terms: the public daemon and receiver facade sources use
GPL-2.0-or-later, while `tools/pixctl.cpp` and `userland/tools/pix_ts.cpp` use
GPL-2.0-only. The full PX4 license and source provenance are bundled with its
pinned source.

The `dtv-userland-core` root MIT license applies only to the source scope
listed in its pinned `PROVENANCE.json`. Its optional product adapters retain
their separate GPL-2.0-only `COPYING` files and provenance. Product code and
those adapters are not relicensed by the core MIT license. libusb 1.0.30
remains LGPL-2.1-or-later; its archive checksum and license are included.
