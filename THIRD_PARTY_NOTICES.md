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

The source release contains the complete original application source, build scripts, and the unmodified libusb 1.0.30 source tarball used by the portable builds. It is provided at the same private Release as the binaries. You may modify libusb and rebuild/relink the application; no signature or device assets are required for compilation. See docs/releases.md. On Windows, the official libusb static archive is used; its version and distribution checksum are recorded by the build script, with the corresponding libusb source in the source package.

The bundled `assets/` (FPGA and original-application authentication data) are separate manufacturer-derived materials for this owner's private use. The project's MIT license does not license those materials or grant third-party rights in them. The encrypted input bundle is excluded from the source release.

Notice sources: musl v1.2.5 COPYRIGHT; GCC 15.1 COPYING3/COPYING.RUNTIME; MinGW-w64 v12 notices (the Windows build also includes notices from the installed CRT package); LLVM compiler-rt 18.1.8 license; Android NDK r27d bundled toolchain NOTICE. Compiler/package versions used for each binary are in build-info.txt. Optional build-time Python cryptography is not linked into pix-ts.
