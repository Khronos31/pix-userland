# Releases

現行3コマンド配布の候補回帰手順は [`release-validation.md`](release-validation.md) を参照してください。以下の履歴的検証記録と候補ごとの達成状況を、将来候補の合格として扱わないでください。

## 3コマンド配布への移行（pixd / pixctl / pix-ts）

配布する公開コマンドは `pixd`、`pixctl`、`pix-ts` の3つです。`core-integration/` の CMake が、固定した dtv-userland-core（`core-integration/dtv-userland-core.revision`）のソースから各製品として再ビルド・静的リンクします。従来の standalone C CLI は `make` で `pix-ts-legacy` として生成する内部診断・テスト用で、配布しません（同名の旧 `pix-ts` を誤って配布しないための分離です）。以下の v0.1.4 以前の記述は、従来 standalone CLI を配布していた Release の記録です。

`include/version.h` の版は変更していません。この移行は正式 Release・タグ・版更新を含みません。

GitHubが生成するSource codeアーカイブと `tools/package-release.py source` が作成する対応ソースパッケージは別物です。`.gitattributes` により自動アーカイブでは `firmware/manifest.json` とビルド用ソースを残し、許可済みバイナリ `firmware/dt181.bin`、`firmware/dt295.bin`、保存IVを除外します。対応ソースパッケージはオフライン再ビルドに使う許可済みファームウェア、manifest、保存metadata/IVを明示的に含みます。暗号化入力 `.private/assets.enc` はGitHub自動アーカイブには含まれますが、対応ソースパッケージには含めません。

### 3コマンド配布の検証状況

| 対象 | 配線 | 確認済み | 未確認 |
|---|---|---|---|
| Linux（通常 CI） | `.github/workflows/ci.yml` の `three-commands` | ubuntu-24.04 で3コマンドと CMake の3 proof をビルド・実行し、linux-x86_64 パッケージを作って展開後の3コマンドを実行、source アーカイブを展開してネットワークなしで再ビルド | 実機 |
| linux-x86_64 / linux-aarch64（musl） | `tools/build-portable.sh musl`、release workflow | glibc 静的リンクで3コマンドの link と `audit-binary.py` を通過（musl 自体ではない） | Alpine でのビルドは release workflow のみ。linux-aarch64 は未ビルド |
| darwin-arm64 | `tools/build-portable.sh native` | なし | ビルド・audit |
| android-aarch64 / armv7a / x86_64 | `tools/build-portable.sh android` | NDK r27.0（CI は r27d）で3 ABI とも3コマンドをクロスビルドし `audit-binary.py` を通過 | 実行・実機・r27d |
| windows-x64 | `tools/build-windows.sh`、`windows.yml` | 固定llvm-mingw 20250910 UCRTでCore・製品・source libusbから3PEをcross build、PE/import監査 | Windows runnerのpackage/runtime/EOF proofはCIで別実行。実機受信は未確認 |

実機（DT181 / DT295）での受信は、この配布接続の検証範囲に含めません。

リポジトリは所有者が承認・実施済みのPUBLIC状態です。可視性は変更しません。CIは開始時、成果物保存前、機器データ復元前、Releaseへの保存・発行前に対象リポジトリとPUBLIC状態を確認します（`tools/check-private.sh` は履歴上の名前を維持）。素材の追加許可や新たな権利判断を意味しません。
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
| source | `core-integration/` を含む自作ソース、ビルド/梱包スクリプト、固定Coreのsource snapshot（`third_party/dtv-userland-core/`）、ライセンス、libusb 1.0.30対応ソース、`source-manifest.json`、`SHA256SUMS` |
| SHA256SUMS | 8アーカイブのSHA-256 |

各バイナリには `assets/fpga.bin`、`assets/bootstrap.bin`、`assets/validation.bin` が入ります。
元のアプリ、録画、セッションデータは含めません。ソースアーカイブには実機assetsと暗号化入力を含めません。
GitHubが自動生成するSource codeアーカイブには、リポジトリ内の暗号化入力は入りますが、復元キーは含まれません。
対応libusbソースも必要な場合は、手動同梱の `pix-userland-VERSION-source.tar.gz` を使ってください。
バイナリは各アーカイブの `pixd`、`pixctl`、`pix-ts`（Windows は `.exe`）で、`licenses/dtv-userland-core-*` に固定Coreの `LICENSE`、adapterの `COPYING`、`PROVENANCE.json` を含みます。`build-info.txt` は固定Coreのコミット（`dtv-userland-core <40桁>`）を記録します。
sourceアーカイブの `source-manifest.json` は、全ファイルのSHA-256、製品とCoreのcommit/tree、libusbアーカイブのSHA-256を記録し、アーカイブ内の `SHA256SUMS` と一致します。

## CIの実行

Actions画面の **Release** をmainから手動実行します。
`include/version.h` のVERSIONからタグ名を生成します。同じタグ/Releaseの再利用は拒否します。
全ビルド成功後、同じコミットを対象に一時Draftへファイルを揃え、検査後にDraftを解除します。
失敗してDraftだけ残った場合は、その内容を確認してから整理・再実行してください。
`pixd` / `pixctl` / `pix-ts` をビルドして `tools/audit-binary.py` で3つとも検査します。WindowsはLinuxで固定chain/source libusbからcross buildし、Windows runnerでpackageを展開して基本CLIを実行します。通常の `windows.yml` は合成USB fixtureによるruntime・UTF-8 asset/output・TS byte一致・EOF停止・cleanup・同じnamespaceへの再起動も検証します。従来Make testsは通常CIのPOSIXとWindows別jobに維持し、CMake全体統合は別工程です。AndroidはクロスビルドとELF検査を行います。
`tools/verify-release.py` は7バイナリそれぞれの3コマンド、従来CLIの混入なし、Coreのpinとlicense通知、sourceの閉包（manifest・SHA256SUMS・Core snapshot）を検査します。
CIに実機は接続していないため、CI成功は各OSでの受信を保証しません。

## 実機データの保管

所有者が許可した既存assetsだけを最終Releaseバイナリに同梱します。PUBLIC追従で追加の素材・鍵・driver表・rawを増やしません。
`.private/assets.enc` はAES-256-GCMで認証暗号化した入力です。復元キーはGitHub Actions secretの `PIX_ASSETS_KEY` に保存し、Gitには入れません。
キーはランダム32バイトをBase64化したものです。生成や更新時にログへ出力しないでください。
`tools/assets-bundle.py seal ASSET_DIR .private/assets.enc` はこの環境変数を読み取ります。
キーを更新する場合は暗号化ファイルとActions secretをセットで更新します。

最終Releaseジョブだけが復元し、3つの名前・サイズを検証してバイナリアーカイブへ追加します。
中間CI artifactsはassetsなしです。復元データをコンソールへ表示しません。
ソースやテストデータは自作/合成のものを使い、メーカー由来assetsにMITライセンスを付けません。

## 再ビルドと再リンク

`release-info.json` にコミット・バージョン・対象・静的libusb・assets同梱の有無・固定Coreのコミットを記録します。
`build-info.txt` はコンパイラと固定Coreのコミットなどの情報です。ルートの自作ソースはMIT、libusbはLGPL-2.1以降です。静的リンクされるCoreのhost adapterの一部はGPL-2.0-onlyで、各ソースのSPDX表記と同梱の `COPYING` に従います。
その他は `THIRD_PARTY_NOTICES.md` と `licenses/` を参照してください。

sourceアーカイブにはCoreの固定sourceが `third_party/dtv-userland-core/` に含まれ、`core-integration/CMakeLists.txt` が自動でそれを使います（取得は不要）。
`third_party/libusb-1.0.30.tar.bz2` は `tools/build-portable.sh` が優先利用します。
公式ソースはSHA-256を照合します。通常のソースcheckoutでは、未配置なら公式サイトから取得します。
コンパイル・単体テストに実機assetsは不要です。

sourceアーカイブを展開し、ネットワークなしで3コマンドを再ビルドして確認する例:

```sh
tools/check-source-rebuild.sh pix-userland-VERSION-source.tar.gz /path/to/empty-work-dir
```

Windows向けもLinux x86_64で同じ対応sourceから3PEを再buildできます。先に固定llvm-mingw archiveをcacheへ用意します（compilerはビルド環境の前提で、source packageには同梱しません）。Coreとlibusbはsource内の固定版を使い、この入口は追加downloadを拒否します。

```sh
LLVM_MINGW_CACHE=/path/to/verified-toolchain-cache \
  tools/check-source-rebuild.sh pix-userland-VERSION-source.tar.gz /path/to/empty-work-dir windows-x64
```

cache内の `llvm-mingw-20250910-ucrt-ubuntu-22.04-x86_64.tar.xz` は毎回固定SHA-256を照合し、新しく展開します。3PEの再build/監査とWindows上での実行は別々に確認します。

別のCore checkoutを使う場合は `DTV_USERLAND_CORE_SOURCE_DIR` を指定します。Gitリポジトリなら固定コミットとの一致を確認し、ローカルで意図的に異なる版を使うときだけ `PIX_CORE_ALLOW_UNPINNED=1` を付けます。

改変したlibusbとの再リンク例（POSIX。改変版のarchiveを明示した場合だけ固定チェックサムを迂回します）:

```sh
# sourceアーカイブを展開したルートで実行
mkdir -p build/relink
tar -xjf third_party/libusb-1.0.30.tar.bz2 -C build/relink
# build/relink/libusb-1.0.30 の必要箇所を変更
tar -cjf build/relink/libusb-modified.tar.bz2 -C build/relink libusb-1.0.30
LIBUSB_SOURCE_TARBALL="$PWD/build/relink/libusb-modified.tar.bz2" \
LIBUSB_ALLOW_MODIFIED=1 tools/build-portable.sh musl
for program in pixd pixctl pix-ts; do
  python3 tools/audit-binary.py linux-x86_64 "build/musl/$program"
done
```

このrecipeはlibusbを再ビルドし、3コマンドを同じarchiveから再リンクします。公式archiveとsource checkoutは既定で公式SHA-256を必須とします。Windowsでは改変libusb source archiveを `LIBUSB_SOURCE_ARCHIVE=/path/to/libusb-modified.tar.bz2 LIBUSB_ALLOW_MODIFIED=1 tools/build-windows.sh` で指定し、同じ固定chainで再buildします。toolchainのhash照合は迂回しません。AndroidはNDK toolchainでlibusbと3コマンドをビルドし、macOSはIOKit/CoreFoundation/Security frameworkをリンクします。署名や独自の再リンク制限は設けません。
