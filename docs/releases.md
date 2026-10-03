# Private Releases

このリポジトリとReleaseは非公開の個人用です。Releaseが非Draftでも、リポジトリの閲覧権限が必要です。
リポジトリの可視性は変更しません。CIは開始時、成果物保存前、機器データ復元前、Releaseへの保存・発行前にPrivateを確認します。
操作対象も `Khronos31/pix-userland` に限定します。

## v0.1.1

- 全7環境のlibusb 1.0.30を静的リンク。Windowsもlibusb DLL不要。
- 7種類のバイナリをtar.gzに統一し、所有者の実機用assetsを同梱。
- 全ファイルを検査・アップロード後、Privateリポジトリ内の非Draft Releaseとして発行。
- 自作ソースのMITを維持し、ライブラリとコンパイラランタイムの告知を追加。
- READMEを構成から整理し、ビルド確認とUSB実機確認を区別。

| アーカイブ | 内容 |
|---|---|
| linux-x86_64 / linux-aarch64 | musl完全静的、udev非依存 |
| darwin-arm64 | libusb静的、macOS標準ライブラリ・フレームワークに依存 |
| android-aarch64 / android-armv7a / android-x86_64 | NDK r27d / API 24、libusb静的、Bionic等に依存 |
| windows-x64 | MinGW-w64、libusb静的、WinUSBとOS標準DLLに依存 |
| source | 自作Cソース、ビルド/梱包スクリプト、ライセンス、libusb 1.0.30対応ソース |
| SHA256SUMS | 8アーカイブのSHA-256 |

各バイナリには `assets/fpga.bin`、`assets/bootstrap.bin`、`assets/validation.bin` が入ります。
元のアプリ、録画、セッションデータは含めません。ソースアーカイブには実機assetsと暗号化入力を含めません。
GitHubが自動生成するSource codeアーカイブには、リポジトリ内の暗号化入力は入りますが、復元キーは含まれません。
対応libusbソースも必要な場合は、手動同梱の `pix-userland-VERSION-source.tar.gz` を使ってください。

## CIの実行

Actions画面の **Private release** をmainから手動実行します。
`include/version.h` のVERSIONからタグ名を生成します。同じタグ/Releaseの再利用は拒否します。
全ビルド成功後、同じコミットを対象に一時Draftへファイルを揃え、検査後にDraftを解除します。
失敗してDraftだけ残った場合は、その内容を確認してから整理・再実行してください。
Linux/macOS/Windowsは単体テスト、AndroidはクロスビルドとELF検査を行います。
CIに実機は接続していないため、CI成功は各OSでの受信を保証しません。

## 実機データの保管

2026-10-03の所有者の指定により、最終のPrivate Releaseバイナリだけにassetsを同梱します。
`.private/assets.enc` はAES-256-GCMで認証暗号化した入力です。復元キーはGitHub Actions secretの `PIX_ASSETS_KEY` に保存し、Gitには入れません。
キーはランダム32バイトをBase64化したものです。生成や更新時にログへ出力しないでください。
`tools/assets-bundle.py seal ASSET_DIR .private/assets.enc` はこの環境変数を読み取ります。
キーを更新する場合は暗号化ファイルとActions secretをセットで更新します。

最終Releaseジョブだけが復元し、3つの名前・サイズを検証してバイナリアーカイブへ追加します。
中間CI artifactsはassetsなしです。復元データをコンソールへ表示しません。
ソースやテストデータは自作/合成のものを使い、メーカー由来assetsにMITライセンスを付けません。

## 再ビルドと再リンク

`release-info.json` にコミット・バージョン・対象・静的libusb・assets同梱の有無を記録します。
`build-info.txt` はコンパイラなどの情報です。自作ソースはMIT、libusbはLGPL-2.1以降です。
その他は `THIRD_PARTY_NOTICES.md` と `licenses/` を参照してください。

sourceアーカイブの `third_party/libusb-1.0.30.tar.bz2` を `tools/build-portable.sh` が優先利用します。
公式ソースはSHA-256を照合します。通常のソースcheckoutでは、未配置なら公式サイトから取得します。
コンパイル・単体テストに実機assetsは不要です。

改変したlibusbとの再リンク例（POSIX。改変版に固定チェックサムの検査を適用しない手順）:

```sh
# sourceアーカイブを展開したルートで実行
mkdir -p build/relink
cd build/relink
tar -xjf ../../third_party/libusb-1.0.30.tar.bz2
cd libusb-1.0.30
# 必要な変更をソースに加えてから実行
./configure --disable-udev --disable-shared --enable-static
make -j4
cd ../../..
make clean
make USB_CFLAGS='-Ibuild/relink/libusb-1.0.30/libusb' \
  USB_LIBS='build/relink/libusb-1.0.30/libusb/.libs/libusb-1.0.a'
```

macOSではUSB_LIBSへ `-lobjc -framework IOKit -framework CoreFoundation -framework Security` も加えます。
Linuxの完全静的版はmusl環境で `LDFLAGS=-static` を指定します。
AndroidはNDKツールチェーンでlibusbとアプリの両方をビルドし、`-llog` をリンクします。
Windowsも改変libusbのMinGW64静的アーカイブを、MakefileのUSB_LIBSで指定できます。
`tools/build-windows.sh` のリンク引数が基準です。署名や独自の再リンク制限は設けません。
