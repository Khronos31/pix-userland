# pix-userland

`pix-userland` は、ピクセラ製 USB チューナーに対応した、ユーザー空間で動作するドライバおよびツール群です。カーネルモジュールを使用せず、ユーザー空間から選局、MPEG-TS ストリームの取得、および機器の状態照会を行います。

公開コマンドは、USB 機器を所有・管理するデーモン `pixd`、デーモンの状態や機器・内部カード状態を調べる `pixctl`、MPEG-TS を受信・出力するクライアント `pix-ts` の 3 つです。共通基盤 `dtv-userland-core` を静的リンクし、統一された IPC アーキテクチャで動作します。

## 対応機種・動作環境

### 対応機種

本ソフトウェアがプロファイルとして識別する機種は以下のとおりです。

| 機種 | USB ID | 対応放送波 | 受信部 | 状況と検証範囲 |
|---|---|---|---|---|
| CaptyTV Hi-Vision PIX-DT181-PU0 | `06b8:102b` | 地上デジタル、BS デジタル、110 度 CS デジタル | 1 系統（排他受信） | 実機検証済み（地上・BS・110度CS）。同時受信は 1 ストリームのみ。 |
| PIX-DT295 | `06b8:103b` | 地上デジタル、BS デジタル、110 度 CS デジタル | 1 系統（排他受信） | プロファイルおよび stream バックエンド対応。OS 別の検証範囲や制約は [DT295 の状況](docs/dt295.md) を参照。 |

- **PIX-DT181-PU0**: 実機検証済みの主対象モデルです。地上波端子と BS/CS 端子を備え、1 系統の受信部で地上・BS・110度CSのいずれかを受信できます（同時受信は 1 ストリームのみ）。
- **PIX-DT295**: プロファイルおよび stream バックエンド（Linux, macOS, Windows）に対応しています。実機での長時間検証範囲や動的 PMT 追従などの制約事項については [DT295 の状況](docs/dt295.md) を参照してください。
- その他の CaptyTV や StationTV シリーズ（PIX-DT141、PIX-DT195 など）は未対応・未検証です。

`pixd --models` を実行すると、USB へのアクセスを行わずに静的な対応機種名と VID:PID の一覧を表示します。

### 動作環境（配布ターゲット）

バイナリパッケージは以下の 7 種類のターゲット向けに提供しています。libusb 1.0.30 を静的リンクしており、一般の利用環境でコンパイラや開発用ライブラリを準備する必要はありません。

| 対象 OS / 環境 | リンク方式・依存関係 | 構成と備考 |
|---|---|---|
| **Linux x86_64** | musl 完全静的リンク | glibc 環境および musl 環境の両方でそのまま動作します。 |
| **Linux aarch64** | musl 完全静的リンク | 同上 |
| **macOS arm64** | OS 標準ライブラリに動的依存 | libusb 静的リンク。Homebrew 等の追加ライブラリは不要です。 |
| **Android aarch64** | Bionic 動的依存 / libc++ 静的リンク | Termux 環境用 CLI バイナリ（API 24、16KiB ページ対応）。 |
| **Android armv7a** | Bionic 動的依存 / libc++ 静的リンク | 同上 |
| **Android x86_64** | Bionic 動的依存 / libc++ 静的リンク | 同上 |
| **Windows x64** | OS 標準 DLL に動的依存 | libusb 静的リンク。別途 WinUSB ドライバの割り当てが必要です。 |

各プラットフォームのビルドおよび検証状況の詳細は [検証記録](docs/validation.md) および [リリース検証手順](docs/release-validation.md) を参照してください。

## 必要条件

### 機器・受信環境

- **B-CAS カード**: チューナー本体のカードスロットに有効な B-CAS カードを挿入してください。
- **アンテナ接続**: 地上波端子と BS/CS 端子が独立しています。受信する放送波に合わせた端子へ同軸ケーブルを接続してください。
- **LNB 電源（アンテナ給電）**: PIX-DT181-PU0 はアンテナ給電（LNB 15V 電源供給）に対応していません。共同受信設備やブースター、他のテレビ・レコーダー等からアンテナへ給電されている環境ではそのまま受信できます。給電されていない単独アンテナ等の環境では、別途給電機器が必要です。なお、外部から給電されている環境において、本機側で給電を無効化する特別な設定は不要です。

### ファームウェア・機器用データ

実機チューナーを動作させるには、機種に応じたファームウェアデータ（DT181 用の `dt181.bin`、および DT295 用の `dt295.bin`）が必要です。

- **PIX-DT181-PU0**: 単一のファームウェアファイル `dt181.bin`（形式 `P181A1`、699,773 バイト、SHA-256 `a8532036b2995c869f713714b85609d2d21508f8848b96e5c7528ab75f3dd7a0`）。従来の 3 つの個別ファイル（FPGA ビットストリーム、ブートストラップ、認証データ）を単一ファイルへ統合・検証する形式です。
- **PIX-DT295**: 単一のファームウェアファイル `dt295.bin`（形式 `D295A1`、1,839,100 バイト、SHA-256 `5ea05c9d7a217a6184524479ec9bb259f8d0cd162fc71803ed830d2b3d50f11e`）。

**配布バイナリにはこれらの必要なファームウェアデータがあらかじめ組み込まれています（埋め込み方式）。** 通常の利用環境で外部ファイルを用意したり、コマンドラインで指定したりする必要はありません。

ソースから自身でビルドする場合、または開発・診断目的で埋め込みを無効にする場合（`PIX_EMBEDDED_ASSETS=OFF`）は、正規に所有するソフトウェアから生成スクリプト（DT181 は `tools/prepare-assets.py`、DT295 は `tools/prepare-dt295-assets.py`）を用いて単一ファームウェアファイルを準備してください。
外部ファイルを指定してデーモンを起動する際は、`pixd` の `--firmware <FILE>`（または `-F <FILE>`）オプションを使用します。ファイル名や拡張子は任意でよく、ファイル内容（形式ヘッダ、機種識別、SHA-256 ハッシュ）に基づいて検証されます。

### 実行時ライブラリ

配布バイナリは libusb 1.0.30 を静的リンクしているため、システムへの libusb インストールは不要です。
- **Linux**: musl による完全静的リンクのため、追加パッケージなしで glibc / musl 双方の環境で動作します。
- **macOS**: libusb を静的リンクしており、実行時に読み込むのは macOS 標準のシステムライブラリのみです。
- **Android (Termux)**: Termux 用のコマンドライン実行ファイルです（Android 向け APK アプリケーションではありません）。USB デバイスへのアクセスには `Termux:API` アプリおよび `termux-api` パッケージが必要です。
- **Windows**: libusb を静的リンクしているため `libusb-1.0.dll` は不要ですが、対象チューナーに WinUSB ドライバを割り当てる必要があります。

## 導入方法

### 配布アーカイブの展開

利用環境に合わせた配布アーカイブ（`pix-userland-*-<target>.tar.gz`）を取得し、任意のディレクトリに展開します。

配布アーカイブの構成:
- `pixd`, `pixctl`, `pix-ts`（Windows 向けは `.exe`）
- `build-info.txt`
- `README.md`, `docs/`, `packaging/`, `licenses/`

ファームウェアデータは実行ファイルに組み込まれているため、外部ファームウェアファイルの配置や展開作業は不要です。展開した実行ファイルをそのまま実行できます。

### Linux: USB デバイスのアクセス権限設定

Linux で一般ユーザーが USB チューナーにアクセスするには、USB デバイスノードへのアクセス権限が必要です。

#### udev の設定例
`/etc/udev/rules.d/70-pix-userland.rules` を作成し、以下のルールを記述します（`packaging/70-pix-userland.rules` に同等のファイルを用意しています）。

```udev
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="06b8", ATTR{idProduct}=="102b", MODE="0660", GROUP="video"
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="06b8", ATTR{idProduct}=="103b", MODE="0660", GROUP="video"
```

実行ユーザーを `video` グループに追加し、udev ルールを再読み込みしたあと、チューナーを再接続してください。

```sh
sudo usermod -aG video "$USER"
sudo udevadm control --reload-rules
```

#### BusyBox mdev / Alpine Linux の場合
`packaging/mdev.conf` の設定を mdev 設定に追加するか、起動時に `packaging/pix-usb-permissions --scan` を実行して権限を設定します。

### Windows: WinUSB ドライバの割り当て

Windows で使用する場合は、Zadig などのユーティリティを使い、接続したチューナーのドライバを WinUSB に設定してください。

## 最短の使用例

製品コマンドは、デバイスを保持・管理するデーモン `pixd`、状態確認などを行う `pixctl`、MPEG-TS ストリームを受信する `pix-ts` で構成されます。

### 1. 接続機器の確認

接続中の機器を一覧表示します。

```sh
./pixd --list-json
```

出力される JSON から、対象機器の USB ポートパス（`usb:BUS-PORT`）を確認します。

### 2. デーモンの起動

USB ポートパスとインスタンス名を指定してデーモンを起動します。

```sh
runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/pix-userland.XXXXXX")
./pixd --device usb:1-2 --instance tuner --runtime-dir "$runtime_dir" &
pixd_pid=$!
```

### 3. デーモンの状態確認

別の端末またはバックグラウンド起動後に、デーモンの稼働状態や内部カードの挿入状態を確認します。

```sh
# 機器および受信部の一覧照会
./pixctl --runtime-dir "$runtime_dir" --instance tuner list

# デーモンの稼働状態照会
./pixctl --runtime-dir "$runtime_dir" --instance tuner status

# 内部 B-CAS カードの挿入状態照会
./pixctl --runtime-dir "$runtime_dir" --instance tuner card-status
```

### 4. MPEG-TS の受信

`pix-ts` クライアントを使い、指定したチャンネルの MPEG-TS ストリームを受信します。

```sh
# 地上波 27ch を 15 秒間受信し、terrestrial.ts に保存
./pix-ts --runtime-dir "$runtime_dir" --instance tuner \
  --channel T27 --time 15 --output terrestrial.ts

# BS 15ch（スロット0）を 15 秒間受信し、bs.ts に保存
./pix-ts --runtime-dir "$runtime_dir" --instance tuner \
  --channel BS15_0 --time 15 --output bs.ts

# CS 22ch を 15 秒間受信し、cs.ts に保存
./pix-ts --runtime-dir "$runtime_dir" --instance tuner \
  --channel CS22 --time 15 --output cs.ts
```

- `--output -` を指定するか `--output` を省略すると、MPEG-TS パケットを標準出力へ出力します（診断メッセージやログは標準エラー出力へ送られます）。
- `--time` を省略した場合は継続して受信し、SIGINT（Ctrl+C）または SIGTERM で停止します。

### 5. デーモンの停止

受信が完了したら、デーモンプロセスへ SIGINT または SIGTERM を送信して正常停止させます。

```sh
kill "$pixd_pid"
wait "$pixd_pid"
rm -rf "$runtime_dir"
```

### Android (Termux) での実行

Termux 環境では、Termux:API の `termux-usb` コマンドで USB のファイルディスクリプタを取得し、同梱のランチャー `packaging/pix-termux` を介して実行します。

```sh
# termux-usb -l でデバイスパスを確認
termux-usb -r -e './packaging/pix-termux --system isdb-t --channel T27 --time 15 -o terrestrial.ts' /dev/bus/usb/001/002
```

ランチャーがデーモン（`pixd --fd`）の起動、状態確認、`pix-ts` による受信、および終了時の後始末を一括して行います。詳細は [Android / Termux](docs/android-termux.md) を参照してください。

## 受信部とチャンネル・CLI 仕様

### 受信部（Receiver）

各機種の受信部番号は `0` です（地デジ・BS・CS で共用）。
`pix-ts --receiver` の指定を省略した場合は既定値 `0` が使用されます。未搭載の受信部番号を指定した場合はエラー（終了コード `3`）となります。

### チャンネル指定

`px4-userland` や `siano-userland` と共通のチャンネル表記に対応しています。

- **地上デジタル放送（ISDB-T）**: `T13`〜`T62`、または `13`〜`62`
- **BS デジタル放送（ISDB-S）**: `BS<nn>[_<slot>]`（例: `BS15_0`）
  - スロット番号は 0〜7 を指定できます。
  - `--tsid <N>` を明示指定した場合は、スロット番号より TSID 指定を優先します。
- **110 度 CS デジタル放送（ISDB-S）**: `CS<nn>`（例: `CS22`）
  - `--tsid <N>` で TSID を指定できます。

放送波種別を明示する場合は `--system isdb-t` または `--system isdb-s`（あるいは `-s terrestrial|bs|cs`）を指定します。
また、周波数を直接指定する `-f, --freq <HZ>`（衛星の場合は中間周波数）にも対応しています。

### 主要オプション一覧

#### `pixd`（デバイス管理デーモン）
- `--device <SPEC>`: 保持する USB デバイスを指定します（`usb:BUS-PORT`、インデックス、`serial:TEXT` 等）。
- `--instance <TOKEN>`: デーモンのインスタンス識別名を指定します。
- `--runtime-dir <DIR>`: UNIX ドメインソケット等のランタイムディレクトリを指定します。
- `--firmware <FILE>`, `-F <FILE>`: 外部ファームウェアファイルを指定します（埋め込み無効ビルドでは必須、埋め込み有効ビルドでは省略可。任意ファイル名に対応）。
- `--group`: 共有グループアクセスを有効にします。
- `--exit-on-stdin-eof`: 標準入力が閉じた（EOF）際に自動停止します（親プロセス監視用）。
- `--fd <FD>`: オープン済みの USB ファイルディスクリプタを渡します（Android / Termux 用）。
- `--models`: 静的な対応機種一覧を表示します。
- `--list-json`: 接続中の機器を JSON 形式で一覧表示します。

※ 配布バイナリはファームウェアデータを内蔵しているため、通常利用で `--firmware` オプションは不要です。埋め込みを無効にしてビルドした場合（`PIX_EMBEDDED_ASSETS=OFF`）は `--firmware <FILE>` または `-F <FILE>` で対象機種の単一ファームウェアファイルを指定してください。指定されたファイルの内容が不正な場合や機種が一致しない場合は起動時にエラーとなり、内蔵データへのフォールバックは行われません。

#### `pixctl`（制御・状態照会ツール）
- サブコマンド:
  - `list`: デバイス構成および公開受信部の一覧を表示します。
  - `status`: デーモンの稼働状態やストリーム状態を表示します。
  - `card-status`: 内部 B-CAS カードの挿入状態を表示します。
- 接続先指定: `--instance <TOKEN>`、`--runtime-dir <DIR>`、`--device <SPEC>`

#### `pix-ts`（MPEG-TS 受信ツール）
- `-c, --channel <TOKEN>`: 受信チャンネルを指定します。
- `-s, --system <SYSTEM>`: 放送波種別を指定します（`isdb-t`, `isdb-s` 等）。
- `-f, --freq <HZ>`: 受信周波数を Hz 単位で指定します。
- `--tsid <N>`: 衛星放送の TSID を指定します（10 進数または `0x` プレフィックスの 16 進数）。
- `-t, --time <SECONDS>`: 受信時間を秒単位で指定します（省略時は停止シグナルまで継続）。
- `--packet-count <N>`: 受信する MPEG-TS パケット数を指定します。
- `-o, --output <PATH>`: 出力ファイルパスを指定します（省略時または `-` で標準出力）。
- `--receiver <N>`: 受信部番号を指定します（既定値: `0`）。
- `-p, --pid <PID>`: 出力する TS パケットの PID をフィルタリングします（複数指定可、既定値は全 PID）。
- `--control`: 標準入力から改行区切りの選局コマンド（`channel <TOKEN>` / `tune <HZ>` / `quit`）を受け付けて動的に選局します。
- `--fail-on-drop`: 受信キュー溢れや出力詰まりを検知した時点で即座に出力を停止し終了します。

### 終了コード

エラー処理および終了コードは、他製品（`px4-userland`, `asicen-userland`, `siano-userland`）と共通の分類体系を採用しています。

| コード | 意味 |
|---|---|
| `0` | 正常終了。 |
| `2` | コマンドライン引数の不正、または端末画面への直接バイナリ出力防止による中断。 |
| `3` | 対象機器が見つからない、未準備、または非対応。 |
| `4` | デバイスが使用中、またはカーネルドライバが所有。 |
| `5` | 信号ロック待ちタイムアウト、または指定された衛星 TS が見つからない。 |
| `6` | IPC 通信エラー。 |
| `7` | USB 通信エラー、機器プロトコル異常、または受信中の USB 切断。 |
| `8` | TS 整合性異常、キュー満杯、または出力パイプ停滞による drop。 |
| `9` | B-CAS カード未挿入。 |
| `10` | ファームウェア・認証データの欠損、不正、または初期化拒否。 |
| `70` | 内部エラー（メモリ確保失敗、スレッド生成失敗など）。 |

## CARD 機能（内部カード状態照会）

`pixctl card-status` により、チューナー内部の B-CAS カードの物理挿入状態（`present` / `absent`）を照会できます。

```sh
./pixctl --runtime-dir "$runtime_dir" --instance tuner card-status
```

> [!IMPORTANT]
> 本ソフトウェアにおける CARD 機能は内部カードの挿入状態照会に限られます。
> `px4-userland` や `asicen-userland` のような PC/SC IFD プラグイン（`pcscd` 用のスマートカードリーダー API）は提供していません。
> また、ソフトウェアによる B25 / MULTI2 デスクランブル機能も備えていません。
> PIX-DT181-PU0 等はハードウェア内部で B-CAS カードを用いた処理を行うため、正常に復元された TS が出力されますが、PC/SC 経由で外部の B25 ライブラリ等から直接カードを制御することはできません。

## 注意事項・既知の制限

- **同時受信ストリームの制限**: 同時に受信できるストリームは 1 系統のみです。地上波端子と BS/CS 端子がありますが、排他利用となります。
- **LNB 電源非対応**: チューナー本体からのアンテナ給電（LNB 15V）は行えません。BS/CS アンテナに給電が必要な環境では、外部の給電機器（テレビ、レコーダー、ブースター等）を併用してください。
- **特定サービス分離・録画管理機能**: 選択した伝送ストリーム全体の 188 バイト MPEG-TS パケットを出力します。特定番組（Service ID）の分離、EPG 取得、予約録画管理、GUI、WebUSB 等の機能は備えていません。
- **従来の standalone CLI (`pix-ts-legacy`) について**: ソースツリーの `make` でビルドされる `pix-ts-legacy` は内部診断および既存テスト用であり、通常配布パッケージには含まれません。公開された正式な利用手順には `pixd` / `pixctl` / `pix-ts` を使用してください。

## ソースからのビルド

配布バイナリを利用する場合はビルド環境の準備は不要です。ソースからビルドする場合の手順を以下に示します。

### 3コマンドのビルド（pixd / pixctl / pix-ts）

C++17 対応コンパイラ、CMake 3.16 以上、pkg-config、および libusb 1.0 の開発用ヘッダが必要です。共通基盤 `dtv-userland-core` は固定コミットから自動取得して静的リンクします。

配布バイナリと同様にファームウェアデータを埋め込んでビルドする場合は、あらかじめ `firmware/` に必要なファイル（`dt181.bin` および `dt295.bin`）を配置した上で、`-DPIX_EMBEDDED_ASSETS=ON` を指定します。配置先が標準パスと異なる場合は、`-DPIX_DT181_FIRMWARE_FILE=<FILE>` および `-DPIX_DT295_FIRMWARE_FILE=<FILE>` で個別ファイルを指定できます。

```sh
cmake -S core-integration -B build/cmake -DCMAKE_BUILD_TYPE=Release -DPIX_EMBEDDED_ASSETS=ON
cmake --build build/cmake --parallel 4
ctest --test-dir build/cmake --output-on-failure
```

ビルドが完了すると、`build/cmake/` に `pixd`、`pixctl`、`pix-ts` が生成されます。
埋め込みを行わない診断ビルド（`PIX_EMBEDDED_ASSETS=OFF`、既定値）では、`pixd` の起動時に `--firmware <FILE>`（または `-F <FILE>`）で外部ファームウェアファイルを指定します。

### ポータブルビルド（静的リンクスクリプト）

各種プラットフォーム向けに、libusb 1.0.30 を静的リンクしたバイナリを `build/` に生成するスクリプトを用意しています。環境変数 `PIX_EMBEDDED_ASSETS=ON` を設定することで埋め込みバイナリをビルドできます（ファームウェアファイルは既定で `firmware/dt181.bin` と `firmware/dt295.bin` を参照しますが、`PIX_DT181_FIRMWARE_FILE` および `PIX_DT295_FIRMWARE_FILE` で任意パスの指定も可能です）。

```sh
# ネイティブ環境（Linux: glibc 動的 + libusb 静的、macOS: システムライブラリ動的 + libusb 静的）
PIX_EMBEDDED_ASSETS=ON ./tools/build-portable.sh native

# musl Linux 環境（Alpine Linux 等の musl 環境で完全静的バイナリを作成）
PIX_EMBEDDED_ASSETS=ON ./tools/build-portable.sh musl

# Android (Termux) 向けクロスビルド
PIX_EMBEDDED_ASSETS=ON ANDROID_NDK_HOME=/path/to/android-ndk-r27d ANDROID_ABI=aarch64 ./tools/build-portable.sh android
```

### Windows 向けクロスビルド

Linux x86_64 環境にて、llvm-mingw UCRT ツールチェーンを用いてクロスビルドを行います。

```sh
PIX_EMBEDDED_ASSETS=ON ./tools/build-windows.sh
```

libusb 1.0.30 を静的リンクした Windows x64 用の実行ファイル（`pixd.exe`, `pixctl.exe`, `pix-ts.exe`）が生成されます。

## 関連ドキュメント・リリース作業

詳細な設計情報、検証記録、リリース作業手順については以下のドキュメントを参照してください。

- [architecture.md](docs/architecture.md): 内部アーキテクチャ、USB 転送暗号の復元シーケンスの解説
- [release-validation.md](docs/release-validation.md): リリース前の実機検証手順および判定基準
- [validation.md](docs/validation.md): 実機および CI における検証記録の詳細
- [releases.md](docs/releases.md): 配布バイナリのビルド環境および CI パイプラインの詳細
- [dt295.md](docs/dt295.md): PIX-DT295 の開発状況および制限事項

## ライセンス・権利表記

- 本プロジェクトの自作ソースコードには **MIT License** を適用しています（詳細は [LICENSE](LICENSE) を参照してください）。
- 静的リンクおよび同梱される各コンポーネントには、それぞれのライセンスが適用されます。
  - **libusb**: LGPL-2.1-or-later（詳細は `COPYING.libusb` を参照してください）
  - **dtv-userland-core**: ルートは MIT、一部の host adapter は GPL-2.0-only（各ソースの SPDX 表記に従います）。配布バイナリには静的リンクされ、`licenses/dtv-userland-core-*` に Core の `LICENSE`・`COPYING`・`PROVENANCE.json` を同梱します
  - **musl libc**（Linux ポータブルバイナリ）: musl の配布条件（MIT 互換）
  - 各ライブラリの権利表示および詳細は `THIRD_PARTY_NOTICES.md` および `licenses/` を確認してください。
- 配布バイナリに組み込まれるファームウェアデータ（`dt181.bin`, `dt295.bin`）はピクセラ製機器・ソフトウェアに由来するバイナリデータであり、バイナリへの埋め込みによってその由来や権利関係が変更されることはありません。本プロジェクトの MIT License は適用されず、ソースアーカイブにも含まれません。
