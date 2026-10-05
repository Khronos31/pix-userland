# pix-userland / pix-ts

> [!CAUTION]
> 本プロジェクトは非公開の個人用リポジトリです。リポジトリやリリースを外部へ公開しないでください。

## 概要

pix-userland は、ピクセラ製 USB チューナーを libusb で直接制御し、188 バイトの MPEG-TS パケットを出力する C11 実装のユーザー空間 CLI ツール（`pix-ts`）です。

単一チューナーを制御し、選択した伝送ストリーム全体の TS パケットを出力します。

なお、常駐デーモン、予約録画、GUI、内蔵 IC カードリーダー／PC/SC API の制御、ソフトウェアによる B25 / MULTI2 デスクランブル機能は備えていません。また、特定サービスの分離、PID フィルター、標準入力による動的な再選局（`--control`）、ブラウザからの WebUSB 制御も未実装です。

## 対応機器・動作環境

### 対応機種

- **CaptyTV Hi-Vision PIX-DT181-PU0**
  - USB VID:PID: `06b8:102b`
  - 対応放送波: 地上デジタル放送、BS デジタル放送、110 度 CS デジタル放送
  - 本ツールで動作を確認している唯一の実機対応モデルです。

> [!NOTE]
> PIX-DT141、PIX-DT195、PIX-DT295 など、他の CaptyTV や StationTV シリーズは未対応かつ未検証です。製品名が同じでも、この実装での動作は未確認です。

### 配布ターゲットと検証状況

バイナリは 7 種類のターゲット向けに配布しています。すべて tar.gz 形式で、libusb 1.0.30 を静的リンクしています。Linux 向けバイナリは musl による完全静的リンクとなっており、glibc 環境と musl 環境の両方でそのまま動作します。一般のバイナリ利用者がコンパイラやビルドツールを準備する必要はありません。

| ターゲット | リンク方式・依存関係 | 検証状況 |
|---|---|---|
| **darwin-arm64** (macOS) | OS 標準ライブラリに動的依存 | DT181 実機にて C 版による地上・BS・110 度 CS の選局・受信・TS 復元を確認済み。CI 単体テストを通過。 |
| **linux-x86_64** | musl 完全静的（glibc / musl 両対応） | Latitude上で実機受信を確認。v0.1.3採用コードで地上波25ch・5時間2回を合格。詳細は[検証記録](docs/validation.md)。 |
| **linux-aarch64** | musl 完全静的（glibc / musl 両対応） | ビルドおよび検査範囲のみ。実機未検証。CI 単体テストを通過。 |
| **android-aarch64** | Android NDK r27d / API 24（Bionic 動的依存） | Termux 向け CLI。クロスビルドと ELF 検査のみ確認、実機未検証。 |
| **android-armv7a** | Android NDK r27d / API 24（Bionic 動的依存） | 同上。 |
| **android-x86_64** | Android NDK r27d / API 24（Bionic 動的依存） | 同上。 |
| **windows-x64** | OS 標準 DLL（Win32 API）に動的依存 | ビルドおよび検査範囲のみ。別途 libusb DLL は不要ですが機器への WinUSB ドライバ設定が必須。CI 単体テストを通過。 |

- **注意事項・未検証項目**:
  - 長時間受信の確認範囲はLinux x86_64・地上波25chの5時間2回です。受信中の USB ケーブル抜去、スリープからの復帰処理は未検証です。
  - Linux 向けの udev / mdev 設定例を用意していますが、新規権限ルールによる実機でのホットプラグ動作は未検証です。
  - Android 向けは Termux の端末環境で動かす CLI バイナリであり、Android アプリ（APK）ではありません。実行には Termux 上で `Termux:API` アプリと `termux-api` パッケージ（`termux-usb` コマンドを含む）が必要です。

## 必要条件

### 機器・受信環境

- **B-CAS カード**: チューナー本体に有効な B-CAS カードを挿入してください。
- **アンテナ線**: 地上波端子と BS/CS 端子が独立しています。受信する放送波の端子へ正しく接続してください。
- **LNB 電源**: PIX-DT181-PU0 はアンテナ給電（LNB 電源供給）に対応していません。共同受信設備や他の機器（テレビ・レコーダー等）、ブースターからすでにアンテナへ給電されている場合はそのまま受信できます。給電されていない単独アンテナ等の環境では、別途給電機器が必要です。なお、外部から給電されている環境であれば、本機側で給電をオフにする設定や操作は不要です。
  - ※ `--lna` オプションはチューナー内部の受信増幅器（LNA）の切り替え設定であり、アンテナへの LNB 電源ではありません（既定値: 地上波は off、BS/CS は on）。

### 機器用データ（assets）

実機チューナーを動作させるには、FPGA ファームウェアおよび認証データの計 3 ファイルが必要です。

- `fpga.bin`（DT181 用、699,673 バイト）
- `bootstrap.bin`（16 バイト）
- `validation.bin`（64 バイト）

バイナリ配布パッケージには、アーカイブ内の `assets/` ディレクトリに上記 3 ファイルを同梱しています。
ソースアーカイブにはこれらの実機用データを含んでいません。自身で準備する場合は、正規に所有する StationTV LE 2.3.3.1 を対象に、補助ツール `tools/prepare-assets.py` を用いて生成します。このツールは StationTV LE 2.3.3.1 のバイナリを SHA-256 で照合して認証データを抽出しますが、旧アプリ単体から FPGA を抽出するわけではないため、別途入手した既存の FPGA ファイルを `--fpga` オプションで渡す必要があります。

```sh
python3 tools/prepare-assets.py --app '/path/to/StationTV LE.app' --fpga '/path/to/pixy_fpga_102b.bin'
```

### 実行時ライブラリ

配布バイナリは libusb 1.0.30 を静的リンクしているため、システムへの libusb や pkg-config の導入は不要です。コンパイラ等のビルド環境を用意する必要もありません。
- **Linux**: musl で完全静的リンクしているため、追加の共有ライブラリなしで glibc / musl のどちらの環境でも起動します。
- **Windows**: libusb を静的リンクしているため別途 libusb DLL は不要ですが、OS 標準の DLL には動的依存します。また、デバイスドライバとして WinUSB の関連付けが必要です。

## 導入方法

### 配布アーカイブの構成

非公開リポジトリの Release（非 Draft）から、利用環境に合わせたバイナリパッケージ（`pix-userland-*-<target>.tar.gz`）を取得して展開します。ソースアーカイブ（`pix-userland-*-source.tar.gz`）および `SHA256SUMS` も同リリースに配置しています。

配布アーカイブの同梱内容:
- `pix-ts`（Windows 向けパッケージは `pix-ts.exe`）
- `assets/`（`fpga.bin`, `bootstrap.bin`, `validation.bin`）
- `README.md`, `docs/`, `packaging/`, `licenses/`

> [!IMPORTANT]
> `pix-ts` は実行ファイルの配置場所を基準にした assets の自動探索をしません。既定では**カレントディレクトリ相対**の `./assets` を参照します。
> 展開したアーカイブルートを作業ディレクトリにしてコマンドを実行するか、別ディレクトリから起動する場合は `--assets /path/to/assets` で assets ディレクトリの絶対パスを必ず指定してください。

### Linux: USB デバイスのアクセス権限設定

Linux で一般ユーザーが USB チューナーを直接操作するには、デバイスノードへのアクセス権限（`video` グループなど）が必要です。また、カーネルドライバが既にインターフェースを掴んでいる場合は、起動時に `--detach-kernel-driver` を指定して明示的に解放します。

#### udev の場合
1. `packaging/70-pix-userland.rules` を `/etc/udev/rules.d/` に配置します。
2. チューナーを利用するユーザーを `video` グループに追加します。
3. udev ルールを再読み込みするかマシンを再起動し、チューナーを再接続します。

#### BusyBox mdev / Alpine Linux の場合
1. `packaging/mdev.conf` に記載のルールを、既存の汎用 USB ルールより前に追加します。
2. 起動時に接続済みの機器に対しては、root 権限で以下を実行して権限を設定します。
   ```sh
   packaging/pix-usb-permissions --scan
   ```
   sysfs を走査し、VID:PID が `06b8:102b` と一致する USB デバイスノードのみに `root:video` / `0660` を設定します。

### Windows: WinUSB ドライバの割り当て

Windows で使用する場合は、Zadig などのユーティリティを使い、接続した PIX-DT181-PU0 のドライバを WinUSB に設定してください。

## 最短の使用例

展開したアーカイブルートに移動した状態で実行します。

### 認識されている対応機器の確認
```sh
./pix-ts --list
```
Windows (PowerShell) の場合:
```powershell
.\pix-ts.exe --list
```

### 地上デジタル放送の受信
物理チャンネル 27ch を 15 秒間受信し、`terrestrial.ts` に保存します。
```sh
./pix-ts -c 27 -t 15 -o terrestrial.ts
```
- `-c` にはリモコンの番号やサービス番号ではなく、物理チャンネル（13〜52）を指定します。
- 物理チャンネル 27ch は今回の東京地域での受信確認例です。お住まいの地域に合わせて適切な物理チャンネル番号を指定してください。
- `-o` で指定した出力先ファイルは上書きしません。同名のファイルが既に存在する場合はエラー終了します。

Windows (PowerShell) での保存例:
```powershell
.\pix-ts.exe -c 27 -t 15 -o terrestrial.ts
```

### BS デジタル放送の受信
BS 15ch（TSID: `0x40f1`）を 15 秒間受信し、`bs.ts` に保存します。
```sh
./pix-ts -s bs -c 15 --tsid 0x40f1 -t 15 -o bs.ts
```
- BS 15ch および TSID `0x40f1` は実際に受信を確認した例です。衛星放送の物理チャンネルや TSID は衛星のトランスポンダ編成等で変化し得るため、最新の放送編成等を確認して指定してください。
- `--tsid` を省略した場合は、該当のトランスポンダで最初に検出した TS を受信します。

### 110 度 CS デジタル放送の受信
CS 22ch（TSID: `0x7160`）を 15 秒間受信し、`cs.ts` に保存します。
```sh
./pix-ts -s cs -c 22 --tsid 0x7160 -t 15 -o cs.ts
```
- CS 22ch および TSID `0x7160` も実際に受信を確認した例です。BS と同様に放送編成等を確認して指定してください。

### 別のディレクトリから実行する場合
作業ディレクトリがアーカイブルート以外の場合は、`--assets` でディレクトリのパスを明示します。
```sh
/path/to/pix-ts --assets /path/to/assets -c 27 -t 15 -o terrestrial.ts
```

### 標準出力と標準エラー出力
`-o` を省略するか `-o -` を指定すると、標準出力へ TS パケットを出力します。診断メッセージや終了時の統計情報はすべて標準エラー出力へ送られます。
```sh
./pix-ts -s bs -c 15 --tsid 0x40f1 -t 30 > capture.ts 2> capture.log
```
- `-t` を省略した場合は継続して受信し、SIGINT（Ctrl+C）または SIGTERM で停止します。
- Windows の PowerShell ではバージョンによってリダイレクト時にバイナリデータが破損することがあるため、ファイル保存には `-o` オプションを使用してください。

### Android (Termux) での受信例
Termux 環境では `termux-usb` でデバイスのファイルディスクリプタを取得し、引数として渡して受信します。実行には `Termux:API` アプリおよび `termux-api` パッケージが必要です。
```sh
# /dev/bus/usb/001/002 は実際の USB デバイスノードに合わせて変更してください
termux-usb -r -e './pix-ts --assets ./assets -c 27 -t 15 -o terrestrial.ts' /dev/bus/usb/001/002
```

### 取得した TS の検査
取得した TS ファイル（188 バイトパケット）の構造やパケット整合性を検査します（チューナーの接続や assets は不要です）。
```sh
./pix-ts --inspect capture.ts
```

## CLI 仕様

### 書式
```text
pix-ts --system terrestrial|bs|cs --channel N [options]
```

### オプション一覧

| オプション | 引数 | 説明 |
|---|---|---|
| `-c, --channel` | `N` | 物理チャンネル番号を指定します（地上波: 13〜52、BS: 奇数 1〜23、CS: 偶数 2〜24）。リモコン番号やサービス番号ではありません。 |
| `-s, --system` | `TYPE` | 放送波の種別を指定します（`terrestrial`（既定値）、`bs`、`cs`）。 |
| `--tsid` | `N` | 衛星放送の TSID を 10 進数または 16 進数（`0x...`）で指定します。省略時は最初に検出した TS を選択します。 |
| `-t, --time` | `SECONDS` | 受信時間を秒単位で指定します。省略時は SIGINT / SIGTERM で停止するまで継続受信します。 |
| `-o, --output` | `PATH` | 出力先ファイルパスを指定します。既存ファイルへの上書きはしません。省略時または `-` の場合は標準出力に出力します。 |
| `-a, --assets` | `DIR` | 機器データ（`fpga.bin`, `bootstrap.bin`, `validation.bin`）のあるディレクトリを指定します（既定値: `./assets`）。 |
| `-d, --device` | `INDEX` | 検出した対応 USB デバイスの 0 始まりのインデックスを指定します（既定値: `0`）。 |
| `--fd` | `FD` | Android / Termux から渡された借用 USB ファイルディスクリプタを指定します（引数末尾の整数でも指定可能）。Windows では未対応です。`--list` や 0 以外の `--device` とは併用できません。 |
| `--lna` | `on / off` | 内部受信増幅器（LNA）を設定します（既定値: 地上波は `off`、BS/CS は `on`）。LNB 電源ではありません。 |
| `--detach-kernel-driver` | なし | カーネルドライバが対象インターフェースにアタッチされている場合、明示的にデタッチします。 |
| `--list` | なし | 機器を初期化せずに、認識されている対応 USB デバイスの一覧を表示します。 |
| `--inspect` | `FILE` | 既存の 188 バイト TS ファイルの整合性を検証します（USB 機器や assets は不要）。 |
| `--decode RAW --context FILE` | `RAW`, `FILE` | USB 転送の生キャプチャデータとコンテキストファイルからオフラインで TS を復元します（出力先は `-o` で指定）。※ 暗号化放送 TS 一般を復号する機能ではありません。 |
| `--version` | なし | バージョン情報を表示します。 |
| `-h, --help` | なし | ヘルプを表示します。 |

### 終了コード

| コード | 意味 |
|---|---|
| `0` | 正常終了。受信および検査の実行時は、取得した TS パケットの整合性（PAT/PMT 等）を確認します。 |
| `2` | コマンドライン引数の誤り、または端末画面への直接バイナリ出力防止による中断。 |
| `3` | ローカルファイルやメモリ確保などのローカルエラー。 |
| `4` | USB 通信、初期化、選局、セッション制御エラー。 |
| `8` | TS 検証エラー、ストリームエラー（出力が 3 秒以上停滞、受信キュー満杯など）。 |
| `130` | 初期化処理中の割り込み検知（SIGINT 等）。 |

### 受信・ストリーム出力の仕様

- **伝送ストリーム全体の出力**: 選択したチャンネル（トランスポンダ）の伝送ストリーム全体（188 バイトパケット）を出力します。特定サービス（番組）の分離や PID フィルターによる間引きはしません。
- **異常検知と停止**: 後段パイプの詰まりなどにより TS 出力が 3 秒以上進まない場合、または内部の受信キューが満杯になった場合は、パケットの欠落を放置せずエラー（コード 8）として受信を停止し、終了処理へ進みます。
- **末尾端数パケットの破棄**: 受信終了時に発生した 188 バイト未満の端数データは TS 出力から除外して破棄し、標準エラー出力の JSON 統計情報（`trailing_bytes`）に記録します。正常終了（コード 0）であっても端数パケットが出力ファイルに混入することはありません。
- **USB 転送暗号の復元**: 旧公式アプリがハードウェアとの間で行っていた独自の USB 転送暗号（セッション確立、鍵更新、AES 転送暗号）を内部で再現して TS を復元します。放送波自体の暗号に対するソフトウェア B-CAS / MULTI2 デスクランブル機能は含みません。

### Android / Termux 連携の仕様

`--fd FD` オプション（または引数末尾の整数）を指定すると、通常のデバイス探索を省略し、`libusb_wrap_sys_device` を使って渡されたファイルディスクリプタを直接制御します。受け取ったファイルディスクリプタの VID/PID を確認したうえで動作し、終了時も呼び出し元が所有するファイルディスクリプタは閉じません。
なお、`--fd` は Windows 環境では利用できません。また、`--list` や 0 以外の `--device` とは併用できません。

## ビルド方法

配布バイナリを利用する場合はビルド環境の準備は不要です。ソースからビルドする場合の手順を以下に示します。

### 通常ビルド（動的リンク）

C11 対応コンパイラ、make、pkg-config、および libusb 開発ヘッダー・ライブラリが必要です。

```sh
# macOS (Homebrew)
brew install libusb pkg-config
make
make test

# Linux (glibc / musl): 各ディストリビューションのパッケージマネージャで libusb 開発パッケージを導入
make
make test
```
- `CC`, `CFLAGS`, `LDFLAGS`, `USB_CFLAGS`, `USB_LIBS` 変数は make 実行時に上書き可能です。
- 大容量ファイルを扱うため `_FILE_OFFSET_BITS=64` が有効になっています。

### ポータブルビルド（静的リンクスクリプト）

各種プラットフォーム向けに、libusb 1.0.30 を静的リンクしたバイナリを `build/` に生成するスクリプトを用意しています。
`tools/build-portable.sh` は、source アーカイブに同梱されている `third_party/libusb-1.0.30.tar.bz2` があればそれを優先して使用し、無ければ自動でダウンロードします。いずれの場合も公式未改変ソースの SHA-256 チェックサムを照合します。
なお、改変した libusb をリンクする場合は、Makefile の `USB_CFLAGS` / `USB_LIBS` を指定して通常ビルドを行います（詳細は [docs/releases.md](docs/releases.md) を参照してください）。

```sh
# ネイティブ環境（Linux: glibc 動的 + libusb 静的、macOS: Apple 標準ライブラリ動的 + libusb 静的）
./tools/build-portable.sh native

# musl Linux 環境（Alpine Linux などの musl コンパイラ環境で完全静的バイナリを作成）
./tools/build-portable.sh musl

# Android (Termux) 向けクロスビルド（Bionic 動的依存）
ANDROID_NDK_HOME=/path/to/android-ndk-r27d ANDROID_ABI=aarch64 ./tools/build-portable.sh android
```
- `musl` モードは glibc 環境から自動変換するスクリプトではありません。必ず Alpine Linux などの musl ネイティブ環境で実行してください。
- Android クロスビルドでは `ANDROID_NDK_HOME` の指定が必須です。`ANDROID_ABI` は `aarch64`（既定値）、`armv7a`、`x86_64` から選択できます（ターゲット API レベルは 24）。

### Windows でのビルド

MSYS2 の MINGW64 環境でビルドします。環境内に gcc, make, curl, p7zip 等のツールが必要です。
```sh
./tools/build-windows.sh
```
libusb 1.0.30 を静的リンクした Windows x64 用の実行ファイル（`pix-ts.exe`）を生成します。

## 関連ドキュメント

詳細な設計情報や検証記録については、以下のドキュメントを参照してください。

- [architecture.md](docs/architecture.md): 内部構造、USB 転送暗号の復元シーケンス、実装設計の解説
- [validation.md](docs/validation.md): 実機および CI における検証範囲、テスト結果の詳細
- [releases.md](docs/releases.md): プライベート Release 向けクロスビルド手順、CI パイプラインの詳細

## ライセンス・権利表記

- 本プロジェクトの自作ソースコードには **MIT License** を適用しています（本リポジトリ自体は非公開です）。
- プロジェクト全体を一括して MIT License と表記することはできません。静的リンクおよび同梱される各コンポーネントには、それぞれのライセンスが適用されます。
  - **libusb**: LGPL-2.1-or-later（詳細は `COPYING.libusb` を参照してください）
  - **musl libc**（Linux ポータブルバイナリ）: musl の配布条件（MIT 互換）
  - 各ライブラリの権利表示および詳細は `THIRD_PARTY_NOTICES.md` および `licenses/` を確認してください。
- 手動同梱の source アーカイブ（`pix-userland-*-source.tar.gz`）には、再ビルド用のアプリソースコードやスクリプトに加え、静的リンクに対応した libusb のソースコードを同梱しています（GitHub が自動生成する Source code アーカイブとは内容が異なります）。
- 機器用データ（`assets/` 内の `fpga.bin`, `bootstrap.bin`, `validation.bin`）はピクセラ製機器・ソフトウェアに由来するバイナリデータであり、本プロジェクトの MIT License は適用されません。ソースアーカイブにも含まれません。
