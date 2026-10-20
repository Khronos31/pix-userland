# Private Releases

このリポジトリとReleaseは非公開の個人用です。Releaseが非Draftでも、リポジトリの閲覧権限が必要です。
リポジトリの可視性は変更しません。CIは開始時、成果物保存前、機器データ復元前、Releaseへの保存・発行前にPrivateを確認します。
操作対象も `Khronos31/pix-userland` に限定します。

## v0.1.4

- mirakc向けに `T27` / `BS01_0` / `CS22` の文字列選局、`--time -`、USB位置指定、`--list-json`、PIDフィルター、標準入力による再選局を追加。受信stdoutは188バイトTSのみ、ログはstderr。
- BSスロットを受信機のTSID配列から解決。再選局時にUSB転送を停止・回収して再開し、旧局の障害を統計初期化で隠さない。
- 地上波の番号指定を13〜62chへ拡張。以前の52ch上限はソフト入力検証であり、ハード制約の根拠ではなかった。53〜62chの有効信号による実機受信は未検証。
- 終了コードをsiano/px4の共通分類へ変更: 3機器なし、4使用中、5選局タイムアウト、7USB障害、8TS/backpressure、9カード未挿入、10機器データ、70内部・ローカルI/O。最初の障害を終了処理の後続障害で上書きしない。
- 既存出力ファイルを上書きする動作へ統一。ファームウェアの内容、ログ・統計文言、機器固有機能には差が残る。
- Linux x86_64 / Latitude / DT181で地上波90秒、BS/CS、PIDフィルター、再選局、SIGTERM正常停止と各エラー分類を確認。Mac/Linuxの回帰テストとUSBモック障害試験を実施。mirakc本体との結合運用は未検証。
- 所有者の判定により今回の追加ソークは省略。v0.1.3の5時間2回の結果を本版の新たなソーク結果とは扱わない。
- 配布7ターゲットのバイナリtar.gzにはassetsを暗号化せず同梱し、ソースtar.gzには含めない。

## v0.1.3

- USB診断ログの書き込みを専用スレッドへ分離し、通常ログ出力がUSB転送の再予約を妨げないよう変更。有限キューの欠落数と書き込み時間を計測し、診断障害を検出。
- 最大200 MiB未満の診断リング、同期異常とTS検査異常の独立保存、復号チェックポイントからの再解析を追加。診断は明示指定時のみ保存。
- 受信ループの実行時間を `capture_elapsed_ms` として出力。
- Linux x86_64 / Latitude-5300 / PIX-DT181-PU0の地上波25chで、同一開発ビルドによる5時間試験を2回連続合格。合計396,687,124パケット、同期ずれ・IV待ち・CC・TEI・スクランブル残留・invalid・CRC・stream error・USBログ欠落・診断I/O障害は0。
- 所有者の合格判定により安定版へ昇格。長時間試験した `9968342` から受信コードの変更なし（版番号と文書を更新してReleaseを再ビルド）。詳細は `docs/validation.md`。
- 全期間のTS独立再検査は未実施。27chで別途発生した入力消失は原因未確定で、今回の合格範囲に含めない。

## v0.1.2

- 鍵テーブル更新時も、直前のテーブルを参照するストリームを継続復号。v0.1.1で約5分後に停止した不具合を修正。
- 受信キューを4 MiBから32 MiBへ拡張し、一時的な処理・保存遅延に備える。満杯時は引き続きエラー終了。
- 復号エラーとキュー満杯をログで区別し、鍵テーブル更新・旧世代鍵使用回数を統計に追加。
- 合成データで世代切替、旧鍵への一時的な戻り、古すぎる鍵の拒否を回帰検査。

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
| source | daemon/ctl/TSの対応source、build inputs、固定Core/PX4 source、provenance/license、manifest、libusb 1.0.30対応source |
| SHA256SUMS | 8アーカイブのSHA-256 |

各バイナリには `assets/fpga.bin`、`assets/bootstrap.bin`、`assets/validation.bin` が入ります。
元のアプリ、録画、セッションデータは含めません。ソースアーカイブには実機assetsと暗号化入力を含めません。
GitHubが自動生成するSource codeアーカイブには、リポジトリ内の暗号化入力は入りますが、復元キーは含まれません。
対応libusbソースも必要な場合は、手動同梱の `pix-userland-VERSION-source.tar.gz` を使ってください。
このprivate source archiveには、`PROVENANCE.md` に記録した固定Core/PX4 commitのsourceと各license noticeを含みます。`source-manifest.json` と `SHA256SUMS` がproduct/dependencyのsource identityとfile hashを記録します。実機assetsや暗号化入力は含めません。

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
