# pix-userland / pix-ts

> [!CAUTION]
> 本プロジェクトは非公開の個人用リポジトリです。リポジトリやリリースを外部へ公開しないでください。

## 概要

pix-userland は、ピクセラ製 USB チューナー向けのユーザー空間ソフトウェアです。公開コマンドは、USB 機器を所有する `pixd`、daemon の状態や機器・内部カード状態を調べる `pixctl`、TS を受信する `pix-ts` の3つです。

`pix-ts` は選択した伝送ストリームを 188 バイトの MPEG-TS パケットとして出力します。PIX CARD 機能は内部カードの挿入状態照会に限られ、内蔵 IC カードリーダー／PC/SC API の制御や、ソフトウェアによる B25 / MULTI2 デスクランブル機能は備えていません。特定サービスの分離、予約録画、GUI、ブラウザからの WebUSB 制御も未実装です。

## 配布コマンド（pixd / pixctl / pix-ts）

配布アーカイブに入る公開コマンドは次の3つです（Windows 向けパッケージは `.exe`）。

| コマンド | 役割 |
|---|---|
| `pixd` | USB 機器を所有する常駐 daemon です。`--models` で対応機種、`--list-json` で接続中の機器を表示します。 |
| `pixctl` | daemon の状態・機器構成・内部カード挿入状態を調べる制御 CLI です（`list` / `status` / `card-status`）。 |
| `pix-ts` | daemon から TS を受け取り、標準出力またはファイルへ出力するクライアントです。 |

引数は各コマンドの `--help`、版は `--version` で確認できます。3コマンドは `core-integration/` の CMake でビルドし、固定した [dtv-userland-core](https://github.com/Khronos31/dtv-userland-core) のソース（`core-integration/dtv-userland-core.revision`）から静的リンクします。

従来の standalone C 実装（`make` が生成する `pix-ts-legacy`）は内部診断と既存テスト用で、配布アーカイブには含めません。公開 `pix-ts` とは引数も出力も異なります。以下の「最短の使用例」「CLI 仕様」は従来 standalone CLI の記録であり、コマンド名を `pix-ts-legacy` に読み替えています。

## 対応機器・動作環境

### 対応機種

- **CaptyTV Hi-Vision PIX-DT181-PU0**
  - USB VID:PID: `06b8:102b`
  - 対応放送波: 地上デジタル放送、BS デジタル放送、110 度 CS デジタル放送
  - 既存のリリースで実機対応を確認しているモデルです。

> [!NOTE]
> PIX-DT141、PIX-DT195 など、他の CaptyTV や StationTV シリーズは未対応かつ未検証です。PIX-DT295 の C 経路は開発中で、macOS arm64・地上波 T27・ターゲット0では 15 秒、5 分、5 時間の受信と保存 TS 検査を確認しました。5 時間の TS は 194,054,187 パケット（36,482,187,156 バイト）で、CC、TEI、scramble、invalid、section CRC の各異常は 0 件でした。PID 273 の追加イベントも実機診断で確認しました（機器レジスター readback は未実施）。追加音声の出現・消失境界における部分 PES の Issue #2 は未解決です。DT295 はまだ RC や配布パッケージに含まれません。各 OS の検証範囲と制限は[DT295 の状況](docs/dt295.md)を参照してください。

### 配布ターゲットと検証状況

> [!NOTE]
> 下の表と r8 の記述は、従来 standalone CLI を配布した既存 Release の記録です。`pixd` / `pixctl` / `pix-ts` の3コマンド配布は、この表の検証済みという意味にはなりません。3コマンドの配布接続の状況は [docs/releases.md](docs/releases.md) の「3コマンド配布の検証状況」を参照してください。

バイナリは 7 種類のターゲット向けに配布しています。すべて tar.gz 形式で、libusb 1.0.30 を静的リンクしています。Linux 向けバイナリは musl による完全静的リンクとなっており、glibc 環境と musl 環境の両方でそのまま動作します。一般のバイナリ利用者がコンパイラやビルドツールを準備する必要はありません。

| ターゲット | リンク方式・依存関係 | 検証状況 |
|---|---|---|
| **darwin-arm64** (macOS) | OS 標準ライブラリに動的依存 | DT181 実機にて C 版による地上・BS・110 度 CS の選局・受信・TS 復元を確認済み。CI 単体テストを通過。 |
| **linux-x86_64** | musl 完全静的（glibc / musl 両対応） | Latitude上で実機受信を確認。v0.1.3採用コードで地上波25ch・5時間2回を合格。詳細は[検証記録](docs/validation.md)。 |
| **linux-aarch64** | musl 完全静的（glibc / musl 両対応） | r8 はクロスビルドと ELF 形式・依存関係検査のみ。実行・実機未検証。 |
| **android-aarch64** | Android NDK r27d / API 24（Bionic 動的依存） | Termux 向け CLI。r8 はクロスビルドと ELF 検査のみ。実行・実機未検証。 |
| **android-armv7a** | Android NDK r27d / API 24（Bionic 動的依存） | r8 はクロスビルドと ELF 検査のみ。実行・実機未検証。 |
| **android-x86_64** | Android NDK r27d / API 24（Bionic 動的依存） | r8 はクロスビルドと ELF 検査のみ。実行・実機未検証。 |
| **windows-x64** | OS 標準 DLL（Win32 API）に動的依存 | r8 はクロスビルドと PE 形式・依存関係検査のみ。実行・実機未検証。別途 libusb DLL は不要ですが機器への WinUSB ドライバ設定が必須。 |

r8 の7ターゲット確認では、native macOS と Linux x64 でテストを実行しました。Linux ARM64、Windows x64、Android の3 ABI はクロスビルドと形式・依存関係検査のみで、各バイナリは実行していません。これは既存 Release の対応範囲を変更するものではありません。

- **注意事項・未検証項目**:
  - 既存 DT181 の長時間受信確認は Linux x86_64・地上波 T25 の5時間録画2回です（詳細は[検証記録](docs/validation.md)）。DT295 r8 の macOS arm64・T27・ターゲット0の5時間録画は上記の別試験です。受信中の USB ケーブル抜去、スリープからの復帰処理はいずれも未検証です。
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

DT295用C経路は専用の `dt295.assets` を使います。DT181の3ファイルとは互換性がありません。DT295の構成、必要入力、実機確認範囲と制約については[DT295 の状況](docs/dt295.md)を参照してください。

### 実行時ライブラリ

配布バイナリは libusb 1.0.30 を静的リンクしているため、システムへの libusb や pkg-config の導入は不要です。コンパイラ等のビルド環境を用意する必要もありません。
- **Linux**: musl で完全静的リンクしているため、追加の共有ライブラリなしで glibc / musl のどちらの環境でも起動します。
- **Windows**: libusb を静的リンクしているため別途 libusb DLL は不要ですが、OS 標準の DLL には動的依存します。また、デバイスドライバとして WinUSB の関連付けが必要です。

## 導入方法

### 配布アーカイブの構成

非公開リポジトリの Release（非 Draft）から、利用環境に合わせたバイナリパッケージ（`pix-userland-*-<target>.tar.gz`）を取得して展開します。ソースアーカイブ（`pix-userland-*-source.tar.gz`）および `SHA256SUMS` も同リリースに配置しています。

配布アーカイブの同梱内容:
- `pixd`, `pixctl`, `pix-ts`（Windows 向けパッケージは `.exe`）
- `build-info.txt`（固定した Core のコミットとビルド環境）
- `assets/`（`fpga.bin`, `bootstrap.bin`, `validation.bin`）
- `README.md`, `docs/`, `packaging/`, `licenses/`

> [!IMPORTANT]
> `pixd` は実行ファイルの配置場所を基準にした assets の自動探索をしません。既定では**カレントディレクトリ相対**の `./assets` を参照します。
> 展開したアーカイブルートを作業ディレクトリにして `pixd` を起動するか、別ディレクトリから起動する場合は `--assets /path/to/assets` で assets ディレクトリの絶対パスを必ず指定してください。`pixctl` と `pix-ts` は assets を読みません。

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

## 最短の使用例（従来 standalone CLI）

> [!NOTE]
> この節と「CLI 仕様」は従来 standalone C CLI の記録です。`make` で生成する `pix-ts-legacy` を使い、配布される `pix-ts`（`--device` / `--instance` を取るクライアント）とは別のコマンドです。

展開したアーカイブルートに移動した状態で実行します。

### 認識されている対応機器の確認
```sh
./pix-ts-legacy --list
```
Windows (PowerShell) の場合:
```powershell
.\pix-ts-legacy.exe --list
```

### 地上デジタル放送の受信
物理チャンネル 27ch を 15 秒間受信し、`terrestrial.ts` に保存します。
```sh
./pix-ts-legacy -c 27 -t 15 -o terrestrial.ts
```
- `-c` にはリモコンの番号やサービス番号ではなく、物理チャンネル（13〜62）を指定します。
- 物理チャンネル 27ch は今回の東京地域での受信確認例です。お住まいの地域に合わせて適切な物理チャンネル番号を指定してください。
- `-o` で指定した出力先ファイルは上書きします。同名のファイルが既に存在する場合は上書きします。

Windows (PowerShell) での保存例:
```powershell
.\pix-ts-legacy.exe -c 27 -t 15 -o terrestrial.ts
```

### BS デジタル放送の受信
BS 15ch（TSID: `0x40f1`）を 15 秒間受信し、`bs.ts` に保存します。
```sh
./pix-ts-legacy -s bs -c 15 --tsid 0x40f1 -t 15 -o bs.ts
```
- BS 15ch および TSID `0x40f1` は実際に受信を確認した例です。衛星放送の物理チャンネルや TSID は衛星のトランスポンダ編成等で変化し得るため、最新の放送編成等を確認して指定してください。
- `--tsid` を省略した場合は、該当のトランスポンダで最初に検出した TS を受信します。

### 110 度 CS デジタル放送の受信
CS 22ch（TSID: `0x7160`）を 15 秒間受信し、`cs.ts` に保存します。
```sh
./pix-ts-legacy -s cs -c 22 --tsid 0x7160 -t 15 -o cs.ts
```
- CS 22ch および TSID `0x7160` も実際に受信を確認した例です。BS と同様に放送編成等を確認して指定してください。

### 別のディレクトリから実行する場合
作業ディレクトリがアーカイブルート以外の場合は、`--assets` でディレクトリのパスを明示します。
```sh
/path/to/pix-ts-legacy --assets /path/to/assets -c 27 -t 15 -o terrestrial.ts
```

### 標準出力と標準エラー出力
`-o` を省略するか `-o -` を指定すると、標準出力へ TS パケットを出力します。診断メッセージや終了時の統計情報はすべて標準エラー出力へ送られます。
```sh
./pix-ts-legacy -s bs -c 15 --tsid 0x40f1 -t 30 > capture.ts 2> capture.log
```
- `-t` を省略した場合は継続して受信し、SIGINT（Ctrl+C）または SIGTERM で停止します。
- Windows の PowerShell ではバージョンによってリダイレクト時にバイナリデータが破損することがあるため、ファイル保存には `-o` オプションを使用してください。

### Android (Termux) での受信例
Termux 環境では `termux-usb` でデバイスのファイルディスクリプタを取得し、引数として渡して受信します。実行には `Termux:API` アプリおよび `termux-api` パッケージが必要です。
```sh
# /dev/bus/usb/001/002 は実際の USB デバイスノードに合わせて変更してください
termux-usb -r -e './pix-ts-legacy --assets ./assets -c 27 -t 15 -o terrestrial.ts' /dev/bus/usb/001/002
```

### 取得した TS の検査
取得した TS ファイル（188 バイトパケット）の構造やパケット整合性を検査します（チューナーの接続や assets は不要です）。
```sh
./pix-ts-legacy --inspect capture.ts
```

## CLI 仕様（従来 standalone CLI）

### mirakc / siano-ts 互換の使用方法

`--channel T25`、`--channel BS01_0`、`--channel CS02` に対応します。
数値と従来の `--system bs|cs` も使用できます。BSの末尾は受信機から取得した
TSID配列のスロット番号（0〜7）で、空スロットはエラーになります。
`--tsid` の明示指定は名前のスロットに優先します。

`-f/--freq HZ`（衛星では中間周波数）、`-p/--pid PID`（繰り返し可、8192は全PID）、
`-v/--verbose`、`--list-json`、`--control`、`--fail-on-drop` を追加しました。
`--device` はインデックス、`BUS:ADDRESS`、`BUS-PORT[.PORT...]`、`serial:TEXT`、
`usb:BUS-PORT[.PORT...]` を受け付けます。`serial:` は接続中の機器のserialと完全一致
（大文字小文字を保持）で照合し、一致なし・重複時は先頭へfallbackせず失敗します。
`--list-json` は `enclosures` / `ungrouped_usb_devices` 形式で、1台につき
`systems: ["ISDB-T","ISDB-S"]` の受信機1基を列挙します（DT181・DT295ともT/S両対応。
現在の受信経路の検証・対応OS範囲は別で、DT295のC経路はmacOS地上波のみ確認済みです）。
`serial` はdescriptorの文字列またはnull、`serial_unique` は現在の列挙内での
一意性（true/false、判定不能はnull）です。

mirakc設定例（展開先とUSB位置は実際の環境に置換）:

```yaml
tuners:
  - name: pix-dt181
    types: [GR, BS, CS]
    command: /opt/pix-userland/pix-ts-legacy --assets /opt/pix-userland/assets --device 1-2 --channel {{{channel}}} --time {{{duration}}} {{{extra_args}}}
    decoded: true
```

channelsには `channel: T25`、`channel: BS01_0`、`channel: CS02` のように設定します。
service IDによる分離はmirakc側で行います。本機内のB-CAS処理後のTSを出力するため
`decoded: true` とします。`--time -` は無期限受信で、SIGTERMに対応します。
受信時のstdoutは188バイトTSのみ、ログはstderrです。実際のmirakcとの結合運用は未検証です。

`--control` は改行区切りの `channel T25`、`channel BS01_0`、`tune 545142857`、
`quit` を受け付け、EOFでも終了します。初期選局を省略すると最初のコマンドを待ちます。
時間指定・診断保存との併用はできません。

機器固有の差異:

- `-F/--firmware` はPIX用FPGAファイルで、siano用rioファームウェアは使用できません。
  bootstrap/validationは引き続き `--assets` から読みます。配布assetsの暗号化・復号手順はありません。
- 地上波は13〜62ch、LNB電源とソフトB25復号は非対応です。
- ログ文言・統計JSONはPIX固有です。終了コードはsiano/px4の共通分類に合わせています。
- PIDフィルターは復号・全入力検査後に適用し、統計のpacketsはフィルター前の数です。
- `--fail-on-drop` は互換引数です。pix-ts-legacyは常にキュー欠落をエラー終了させます。


### 書式
```text
pix-ts-legacy --system terrestrial|bs|cs --channel N [options]
```

### オプション一覧

| オプション | 引数 | 説明 |
|---|---|---|
| `-c, --channel` | `TOKEN` | `T27` / `BS01_0` / `CS02` または物理チャンネル番号を指定します（地上波: 13〜62、BS: 奇数 1〜23、CS: 偶数 2〜24）。リモコン番号やサービス番号ではありません。 |
| `-s, --system` | `TYPE` | 放送波の種別を指定します（`terrestrial`（既定値）、`bs`、`cs`）。 |
| `--tsid` | `N` | 衛星放送の TSID を 10 進数または 16 進数（`0x...`）で指定します。省略時は最初に検出した TS を選択します。 |
| `-t, --time` | `SECONDS` | 受信時間を秒単位で指定します。`-` または省略時は SIGINT / SIGTERM で停止するまで継続受信します。 |
| `-o, --output` | `PATH` | 出力先ファイルパスを指定します。既存ファイルは上書きします。省略時または `-` の場合は標準出力に出力します。 |
| `--model` | `dt181 / dt295` | 機種を指定します（既定値: `dt181`）。実験的 DT295 は macOS・地上波・ターゲット0のみです。 |
| `-a, --assets` | `DIR` | 機器データのあるディレクトリを指定します（既定値: `./assets`）。DT181 は3ファイル、実験的 DT295 は `dt295.assets` を読み込みます。 |
| `-d, --device` | `SPEC` | インデックス（既定値: `0`）、`BUS:ADDRESS`、`BUS-PORT[.PORT...]`、`serial:TEXT`、`usb:BUS-PORT[.PORT...]` を指定します。 |
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
| `3` | 対象機器が見つからない。 |
| `4` | 機器が使用中、またはカーネルドライバーが所有。 |
| `5` | 信号ロック待ちタイムアウト、または指定衛星TSが見つからない。 |
| `7` | USB通信・機器プロトコル・切断エラー。 |
| `8` | TS整合性、キュー満杯、出力進捗停滞。 |
| `9` | B-CASカード未挿入。 |
| `10` | FPGA・認証データの欠損、不正サイズ、初期化時の拒否。 |
| `70` | 内部エラー、メモリ・スレッド・ローカルファイル・診断I/Oエラー。 |

正常なSIGINT/SIGTERM停止は初期化中も0です。ただし停止処理で新たな障害を検知した場合は該当コードになります。
最初の障害分類を保持し、後続の終了処理の障害で置き換えません。
px4のコード6はIPCに固有で、本ツールには該当する経路がありません。
sianoには汎用エラー1もありますが、pix-ts-legacyでは原因に応じて上記へ分類します。

地上波53〜62chの番号・周波数変換は対応していますが、この帯域の実機受信は未検証です。
以前の52ch上限はソフトウェアの入力検証であり、ハード制約を示す根拠ではありません。

### 受信・ストリーム出力の仕様

- **伝送ストリーム全体の出力**: 選択したチャンネル（トランスポンダ）の伝送ストリーム全体（188 バイトパケット）を出力します。特定サービス（番組）の分離はしません。`--pid` 指定時は復号後に指定PIDのみを出力します。
- **異常検知と停止**: 後段パイプの詰まりなどにより TS 出力が 3 秒以上進まない場合、または内部の受信キューが満杯になった場合は、パケットの欠落を放置せずエラー（コード 8）として受信を停止し、終了処理へ進みます。
- **末尾端数パケットの破棄**: 受信終了時に発生した 188 バイト未満の端数データは TS 出力から除外して破棄し、標準エラー出力の JSON 統計情報（`trailing_bytes`）に記録します。正常終了（コード 0）であっても端数パケットが出力ファイルに混入することはありません。
- **USB 転送暗号の復元**: 旧公式アプリがハードウェアとの間で行っていた独自の USB 転送暗号（セッション確立、鍵更新、AES 転送暗号）を内部で再現して TS を復元します。放送波自体の暗号に対するソフトウェア B-CAS / MULTI2 デスクランブル機能は含みません。

### Android / Termux 連携の仕様

`--fd FD` オプション（または引数末尾の整数）を指定すると、通常のデバイス探索を省略し、`libusb_wrap_sys_device` を使って渡されたファイルディスクリプタを直接制御します。受け取ったファイルディスクリプタの VID/PID を確認したうえで動作し、終了時も呼び出し元が所有するファイルディスクリプタは閉じません。
なお、`--fd` は Windows 環境では利用できません。また、`--list` や 0 以外の `--device` とは併用できません。

## ビルド方法

配布バイナリを利用する場合はビルド環境の準備は不要です。ソースからビルドする場合の手順を以下に示します。

### 3コマンドのビルド（pixd / pixctl / pix-ts）

C++17 対応コンパイラ、CMake 3.16 以上、pkg-config、libusb 開発ヘッダー・ライブラリが必要です。固定した Core のソースは、ソースアーカイブでは `third_party/dtv-userland-core/` から、通常の checkout では GitHub から取得します。

```sh
cmake -S core-integration -B build/cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake --parallel 4
ctest --test-dir build/cmake --output-on-failure
```

`pixd`、`pixctl`、`pix-ts` が `build/cmake/` に生成されます。ローカルの Core を使う場合は `-DPIX_CORE_SOURCE_DIR=/path/to/dtv-userland-core` を指定します（固定コミットとの一致は確認されません）。

### 従来 standalone CLI のビルド（内部診断・テスト用）

C11 対応コンパイラ、make、pkg-config、および libusb 開発ヘッダー・ライブラリが必要です。`make` が生成するのは `pix-ts-legacy` で、配布アーカイブには含まれません。

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

各種プラットフォーム向けに、libusb 1.0.30 を静的リンクした `pixd` / `pixctl` / `pix-ts` を `build/` に生成するスクリプトを用意しています。従来 standalone CLI のテスト（`make test`）も同時に実行しますが、その成果物は `build/` に出力しません。CMake が必要です（Alpine では `cmake git python3` も追加）。
`tools/build-portable.sh` は、source アーカイブに同梱されている `third_party/libusb-1.0.30.tar.bz2` があればそれを優先して使用し、無ければ自動でダウンロードします。いずれの場合も公式未改変ソースの SHA-256 チェックサムを照合します。
なお、改変した libusb をリンクする場合は、改変した libusb のソースアーカイブを `LIBUSB_SOURCE_TARBALL` と `LIBUSB_ALLOW_MODIFIED=1` で指定して同じスクリプトを実行します（詳細は [docs/releases.md](docs/releases.md) を参照してください）。

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

Linux x86_64 で固定 llvm-mingw 20250910 UCRT を使いクロスビルドします。make, curl, tar, xz, bzip2, CMake, pkg-config, Python 3 が必要です。Core・製品・libusb 1.0.30 を同じ toolchain でソースからビルドし、他ABIの静的ライブラリは混ぜません。
```sh
./tools/build-windows.sh
```
libusb 1.0.30 を静的リンクした Windows x64 用の実行ファイル（`pixd.exe` / `pixctl.exe` / `pix-ts.exe`）を生成する手順です。

`pixd --exit-on-stdin-eof` は親の標準入力が閉じたときに停止・解放します。Windowsの引数とasset/output pathはUTF-8から変換し、TSはbinaryで書き込みます。クロスビルド・PE監査とWindows runner上の実行proofは別々に確認します。対応sourceからの追加取得なし再ビルドは [docs/releases.md](docs/releases.md) を参照してください。

## 関連ドキュメント

詳細な設計情報や検証記録については、以下のドキュメントを参照してください。

- [architecture.md](docs/architecture.md): 内部構造、USB 転送暗号の復元シーケンス、実装設計の解説
- [validation.md](docs/validation.md): 実機および CI における検証範囲、テスト結果の詳細
- [releases.md](docs/releases.md): プライベート Release 向けクロスビルド手順、CI パイプラインの詳細

## ライセンス・権利表記

- 本プロジェクトの自作ソースコードには **MIT License** を適用しています（本リポジトリ自体は非公開です）。
- プロジェクト全体を一括して MIT License と表記することはできません。静的リンクおよび同梱される各コンポーネントには、それぞれのライセンスが適用されます。
  - **libusb**: LGPL-2.1-or-later（詳細は `COPYING.libusb` を参照してください）
  - **dtv-userland-core**: ルートは MIT、一部の host adapter は GPL-2.0-only（各ソースの SPDX 表記に従います）。配布バイナリには静的リンクされ、`licenses/dtv-userland-core-*` に Core の `LICENSE`・`COPYING`・`PROVENANCE.json` を同梱します
  - **musl libc**（Linux ポータブルバイナリ）: musl の配布条件（MIT 互換）
  - 各ライブラリの権利表示および詳細は `THIRD_PARTY_NOTICES.md` および `licenses/` を確認してください。
- 手動同梱の source アーカイブ（`pix-userland-*-source.tar.gz`）には、再ビルド用のアプリソースコードやスクリプトに加え、静的リンクに対応した libusb のソースコードを同梱しています（GitHub が自動生成する Source code アーカイブとは内容が異なります）。
- 機器用データ（`assets/` 内の `fpga.bin`, `bootstrap.bin`, `validation.bin`）はピクセラ製機器・ソフトウェアに由来するバイナリデータであり、本プロジェクトの MIT License は適用されません。ソースアーカイブにも含まれません。
