# License and third-party notices

The original pix-userland source is licensed under MIT; see LICENSE. Linking the libraries below does not relicense their code under MIT. The license files in `licenses/` apply to their respective components, not indiscriminately to the whole application.

| Component | Use | Terms / notice |
|---|---|---|
| libusb 1.0.30 | Static library in every release executable | LGPL-2.1-or-later; COPYING.libusb; copyright holders and individual notices in corresponding source |
| dtv-userland-core (fixed commit in core-integration/dtv-userland-core.revision) | Statically linked into pixd, pixctl and pix-ts | Root MIT (licenses/dtv-userland-core-LICENSE) for the files listed in its PROVENANCE.json; host adapters carry per-file SPDX identifiers, some GPL-2.0-only, with their COPYING files in licenses/dtv-userland-core-*-COPYING |
| musl 1.2.5 | Static Linux C runtime | MIT and compatible notices in licenses/musl-COPYRIGHT.txt |
| GCC runtime | Compiler support, where linked by GCC | GPLv3 with GCC Runtime Library Exception 3.1; licenses/GPL-3.0.txt and GCC-RUNTIME-EXCEPTION.txt |
| MinGW-w64 | Windows CRT support and headers | ZPL-2.1 and separately identified terms; licenses/mingw-w64-*.txt and build-specific notices |
| LLVM compiler-rt / Android NDK r27d | Compiler support/startup code where incorporated | Apache-2.0 with LLVM exceptions and component-specific terms; licenses/compiler-rt-LICENSE.txt and Android-NDK-r27d-NOTICE.txt |
| OS system libraries / WinUSB | Provided by the operating system | Not copied into these archives; OS terms apply |

The source release contains the complete original application source (including core-integration/), build scripts, the exact dtv-userland-core snapshot under third_party/dtv-userland-core/ with its license and provenance files, and the unmodified libusb 1.0.30 source tarball used by the portable builds. source-manifest.json and SHA256SUMS record the file hashes, the product and Core commit/tree, and the libusb archive checksum. It is provided at the same private Release as the binaries. You may modify libusb and rebuild/relink the application; no signature or device assets are required for compilation. See docs/releases.md. On Windows, the official libusb static archive is used; its version and distribution checksum are recorded by the build script, with the corresponding libusb source in the source package.

Linking the Core host adapters does not relicense them under MIT: the SPDX notice in each source file and the COPYING files in licenses/ apply to those adapters and to the combined pixd, pixctl and pix-ts binaries. The root PIX license does not replace them. The legacy standalone C CLI (pix-ts-legacy) is built only for diagnostics and tests and is not distributed.

The bundled `assets/` (FPGA and original-application authentication data) are separate manufacturer-derived materials for this owner's private use. The project's MIT license does not license those materials or grant third-party rights in them. The encrypted input bundle is excluded from the source release.

Notice sources: musl v1.2.5 COPYRIGHT; GCC 15.1 COPYING3/COPYING.RUNTIME; MinGW-w64 v12 notices (the Windows build also includes notices from the installed CRT package); LLVM compiler-rt 18.1.8 license; Android NDK r27d bundled toolchain NOTICE. Compiler/package versions used for each binary are in build-info.txt. Optional build-time Python cryptography is not linked into pix-ts.
