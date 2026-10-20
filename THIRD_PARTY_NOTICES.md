# License and third-party notices

The original pix-userland source is licensed under MIT; see LICENSE. Linking the libraries below does not relicense their code under MIT. The license files in `licenses/` apply to their respective components, not indiscriminately to the whole application.

| Component | Use | Terms / notice |
|---|---|---|
| libusb 1.0.30 | Static library in every release executable | LGPL-2.1-or-later; COPYING.libusb; copyright holders and individual notices in corresponding source |
| musl 1.2.5 | Static Linux C runtime | MIT and compatible notices in licenses/musl-COPYRIGHT.txt |
| GCC runtime | Compiler support, where linked by GCC | GPLv3 with GCC Runtime Library Exception 3.1; licenses/GPL-3.0.txt and GCC-RUNTIME-EXCEPTION.txt |
| MinGW-w64 | Windows CRT support and headers | ZPL-2.1 and separately identified terms; licenses/mingw-w64-*.txt and build-specific notices |
| LLVM compiler-rt / Android NDK r27d | Compiler support/startup code where incorporated | Apache-2.0 with LLVM exceptions and component-specific terms; licenses/compiler-rt-LICENSE.txt and Android-NDK-r27d-NOTICE.txt |
| OS system libraries / WinUSB | Provided by the operating system | Not copied into these archives; OS terms apply |

The corresponding-source archive contains the production daemon, public CLI and TS program sources, build inputs, and exact pinned dependencies. Its manifest records source identity, file hashes, dependency commits, license-file hashes, and the libusb 1.0.30 archive checksum. The archive remains private and excludes device assets. See `PROVENANCE.md` and docs/releases.md. On Windows, the official libusb static archive is used; its version and distribution checksum are recorded by the build script, with the corresponding libusb source in the source package.

The fixed `dtv-userland-core` source is commit `9591ed861291f6d718ac9943328df20b2d3f1463`; the fixed PX4 source is commit `90b08dfe79b0b40e393f015975c761863830866a`. The archive carries each exact Git snapshot and its source provenance and license files. The core root MIT license covers only the files enumerated in its pinned `PROVENANCE.json`. Optional product-facing adapters remain separately licensed under GPL-2.0-only and carry their own `COPYING` files. PX4 source remains GPL-2.0-only under its included `LICENSE`. The root PIX license does not replace the SPDX notices on the daemon, CLI, TS source, or adapters.

The bundled `assets/` (FPGA and original-application authentication data) are separate manufacturer-derived materials for this owner's private use. The project's MIT license does not license those materials or grant third-party rights in them. The encrypted input bundle is excluded from the source release.

Notice sources: musl v1.2.5 COPYRIGHT; GCC 15.1 COPYING3/COPYING.RUNTIME; MinGW-w64 v12 notices (the Windows build also includes notices from the installed CRT package); LLVM compiler-rt 18.1.8 license; Android NDK r27d bundled toolchain NOTICE. Compiler/package versions used for each binary are in build-info.txt. Optional build-time Python cryptography is not linked into pix-ts.
