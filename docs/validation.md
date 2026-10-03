# Validation — 2026-10-03

## 実機受信（C版）

PIX-DT181-PU0、B-CASカード挿入、ユーザーのアンテナ外部給電環境。
Apple Silicon / macOS 26.6.2 arm64、sudoなし。従来のPython版とは別のC実装で確認。

| 系統 | 選局 | 時間 | 出力TSパケット | 同期ずれ / TEI / CC / PSI CRCエラー |
|---|---|---:|---:|---|
| 地上波 | 27ch / 557142857 Hz | 15秒 | 164864 | すべて0 |
| BS | 15ch / 1318000000 Hz / TSID 0x40f1 | 15秒 | 197631 | すべて0 |
| CS | 22ch / 2013000000 Hz / TSID 0x7160 | 15秒 | 363563 | すべて0 |

受信した3つのTSはPython試作の独立した検査器でも同じパケット数とエラー0を確認。
機器転送用AESを復元し、放送TSのスクランブルフラグは0。
停止時の188バイト未満の断片は除去。各ログの `trailing_bytes` は欠落検出件数ではない。

最初のC試験ではFIFOクリア前に受信を予約して1バイトの同期ずれが発生。
クリア → 読み取り予約 → ストリーム開始の順に修正した後、上表の3試験が成功。

追加の実機確認:

- libusbを静的リンクしたMac版でCS22を10秒取得し、検査成功。
- stdoutの受け手を途中で閉じると、書き込みエラーを検出して終了コード8。
- 受信中のSIGTERMでストリーム停止・USB解放まで完了し、終了コード0。
- 実機のUSB抜去、スリープ復帰、長時間連続運用は未検証。

## ソフトウェア検査

- AES-128: FIPS-197既知ベクトルの暗号化・復号。
- RSA: セッションごとの1024-bit生成・公開/秘密演算往復。
- 生成したRSAに対しPythonの独立した多倍長演算で20入力の往復検査。
- Python cryptographyで生成した合成USB/AESベクトルとC出力を一致検査。
- USB断片の長さ1, 2, 7, 37, 187, 188, 189, 376, 752バイトで同じ復元結果。
- 制御パケット除去、IV未到着時の保留、不正なキー表サイズの拒否。
- MPEG-2 CRC既知値、PAT/PMT、重複TS、連続番号欠落、discontinuity_indicator。
- CLIの不正な数値、チャンネル、オプション組み合わせ。
- macOSのAddressSanitizer/UndefinedBehaviorSanitizerで上記テストとCS実データ検査を完了。
- glibc Linuxで `-Werror` ビルド・単体テスト。

## プラットフォームごとの到達点

| 環境 | ビルド/実行 | C版のUSB受信 |
|---|---|---|
| macOS arm64 | ビルド、単体/実データ/サニタイザ、静的libusb版実行 | 地上波・BS・CS確認済み |
| Linux x86_64 glibc | Latitude-5300でビルド/テスト、BS実データ検査 | 未検証（チューナーはMacへ移動済み） |
| Linux x86_64 musl | Alpine 3.22で完全静的ビルド/テスト、Latitude上でBS実データ検査 | 未検証 |
| Android aarch64 Bionic | NDK r27d / API 24でクロスビルド、ELF検査 | 未検証 |
| udev | 06b8:102b限定の権限ルールを用意 | 新ルール適用の実機試験は未実施 |
| BusyBox mdev | Alpineコンテナ内の模擬sysfs/devで対象限定の権限変更を検査 | 実機ホットプラグは未検証 |
| Linux aarch64 musl | CIネイティブビルド/単体テスト/ELF依存検査 | 未検証 |
| Android armv7a / x86_64 | CIクロスビルド/ELF依存検査 | 未検証 |
| Windows x64 | CIビルド/単体テスト/PE依存検査。v0.1.1からlibusb静的 | 未検証 |
| WebUSB | 未実装 | 未実装 |

静的libusb版の依存確認:

- Mac: AppleのlibSystem/libobjcとIOKit/CoreFoundation/Securityのみ。Homebrewのdylib参照なし。
- glibc: libcとELFローダーのみ。libudev/libusbの共有ライブラリ参照なし。
- musl: PT_INTERP/動的依存なしの完全静的ELF。
- Bionic: `/system/bin/linker64`、libc.so/libdl.so/liblog.soのみ。

Androidコンパイルは成功後、出力先が別コンテナのroot所有だったためコピー段階で一度停止。
生成済みバイナリを確認して保存し、出力ディレクトリの所有権を修正した。
これはAndroid実行試験の代わりにはならない。

## 参考と実装元

主要なプロトコル根拠はユーザー所有のStationTV LE 2.3.3.1バイナリと、このセッションの実機試験。
元のPython実装は `../pixy-ts/`、旧調査資料はユーザーの `pixela-stationtv-notes`。

CLI/移植方針の参考として読んだリポジトリ:

- Khronos31/siano-userland: `01dcf87ae3da8baa2ce339deec18aa960183cb1b`
- Khronos31/px4-userland: `cf38742618bb02db41a95def619fbff50e9eb0f3`

これらのGPLドライバコードは本実装に取り込んでいない。

## v0.1.1の配布検査

全7種類でlibusbを静的リンク。Windows版にlibusb DLLは同梱しません。CIで実行ファイルのアーキテクチャとOS標準依存を検査します。最終Private Releaseのバイナリにだけ3つの実機assetsを追加し、ソースには含めません。ライセンス表示と同一コミットの対応ソースも検査します。
