# PIX beta候補artifact

`PIX beta candidate build` workflowは、`pix-userland-0.1.5-beta.1-candidate` Actions artifactを作成します。artifactにはcandidate archive、JSON manifest、`SHA256SUMS`が含まれます。

candidate archiveにはLinux x86_64/aarch64、macOS arm64、Android aarch64/armv7a/x86_64、Windows x64、sourceの各archiveを含みます。内部package versionは`0.1.5`です。DT181およびDT295 device assetは含みません。

これはbeta build専用artifactで、GitHub Releaseやtagを作成しません。

Linux Alpine buildではCLI互換test用にPython 3を入れます。Windows/MSYS2ではPOSIX signal smoke testを実行しません。
