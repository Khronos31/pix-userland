# Private releases

このプロジェクトは非公開専用です。リポジトリの可視性をPublicに変更しないでください。
CIは開始時・成果物の保存前・Release作成前にGitHub APIでPrivateであることを確認します。
操作対象も `Khronos31/pix-userland` に限定します。すべてのReleaseはDraftとして作成します。
GitHub Pagesや外部レジストリへの公開は行いません。

## 対象

| アーカイブ | 内容 |
|---|---|
| linux-x86_64 | musl、完全静的、udev非依存 |
| linux-aarch64 | musl、完全静的、udev非依存 |
| darwin-arm64 | libusb静的、macOS標準ライブラリのみ |
| android-aarch64 | NDK r27d / API 24、Bionic |
| android-armv7a | NDK r27d / API 24、Bionic |
| android-x86_64 | NDK r27d / API 24、Bionic |
| windows-x64 | MinGW-w64、libusb DLL同梱、WinUSBバックエンド |
| source | Cソース、ビルド・梱包スクリプト、libusb 1.0.30の対応ソース |
| SHA256SUMS | 上記8アーカイブのSHA-256 |

ReleaseはGitHubのActions画面で **Private release** をmainから手動実行します。
タグ名は `include/version.h` のVERSIONから生成します。同じタグの再利用は拒否します。
全ビルド成功後、同じコミットを対象にしたDraft Releaseを作成します。
失敗したDraftを修復する場合は、既存ファイルを確認して手動で整理してから再実行してください。
バージョン更新時は `include/version.h` を変更します。

Linux/macOS/Windowsでは暗号・TS復元・CLIテストを実行します。Androidはクロスビルドと
ELFのアーキテクチャ・依存確認を行います。USB実機をCIに接続していないため、CI成功は
各OSでの受信を保証しません。WindowsとAndroidのUSB実機動作は未検証です。

## 再ビルドとlibusb

各アーカイブの `release-info.json` はコミット・バージョン・対象を記録します。
`build-info.txt` はコンパイラ情報等を記録します。CソースはMIT、libusbはLGPL-2.1以降です。
sourceアーカイブに固定チェックサムのlibusb 1.0.30ソースを同梱します。
`tools/build-portable.sh` はその公式ソースを取得してビルドします。
改変したlibusbと再リンクするには、そのソースをビルド後、MakefileのUSB_CFLAGS / USB_LIBSで
改変版ヘッダと静的ライブラリを指定してください。署名・再リンク制限はありません。
Windows DLLは置き換え可能です。Windows版はMSYS2 MINGW64の `tools/build-windows.sh` で再ビルドできます。

実機用FPGA・認証データ・録画TS・セッションデータはGitとReleaseのどちらにも含めません。
梱包は明示的なファイル許可リストで行います。Privateでも必要なデータだけを保存します。
