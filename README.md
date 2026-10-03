# pix-userland / pix-ts

PIXELA USBチューナーをlibusbで直接制御し、188バイトのMPEG-TSを出力するC11のCLIです。
現在の実機対応は **CaptyTV Hi-Vision PIX-DT181-PU0 (`06b8:102b`)** の地上波・BS・110度CSです。
Mac直接接続で、C版による3波の選局・受信・TS復元を確認しました。

**非公開専用プロジェクトです。リポジトリ・Releaseを公開しないでください。**

実行時の外部ライブラリはlibusbだけです。POSIX環境ではpthread、WindowsではWin32 APIを使用し、
OpenSSL、Python、PC/SC、libudev、systemdには直接依存しません。
通常のシステムlibusbがlibudevを使う場合もあります。libudevも外すには、下記の
`tools/build-portable.sh` で `--disable-udev` のlibusbを静的リンクします。

## すぐに使う

このセッションのMacにはビルド済み `./pix-ts` と、準備済みデータへの `assets/` リンクがあります。
このディレクトリに移動して実行します。今回のMacではsudo不要でした。

```sh
./pix-ts --list
./pix-ts -c 27 -t 15 -o terrestrial.ts
./pix-ts -s bs -c 15 --tsid 0x40f1 -t 15 -o bs.ts
./pix-ts -s cs -c 22 --tsid 0x7160 -t 15 -o cs.ts
```

`-c` は物理チャンネルです。BSの101などのサービス番号ではありません。
BS/CSの `--tsid` を省略すると、そのトランスポンダで検出した先頭のTSを選びます。
`-t` 省略時は継続受信し、SIGINT（Ctrl+C）／SIGTERMで停止します。
ファイルは上書きしません。TSは選択した伝送ストリーム全体で、サービスの分離はしません。

標準出力にはTSだけ、診断と最後のJSON統計は標準エラーに出します。

```sh
./pix-ts -s bs -c 15 --tsid 0x40f1 -t 30 > capture.ts 2> capture.log
./pix-ts -s bs -c 15 --tsid 0x40f1 | ffplay -vst p:101:v:0 -ast p:101:a:0 -i -
./pix-ts --inspect capture.ts
```

映像が1440×1080でもSAR=4:3なら表示比率は16:9です。変換時もSARを尊重してください。

**LNAとLNB:** `--lna on|off` は内部の受信増幅器です。既定は地上波OFF、BS/CS ON。
この機種はLNB給電に非対応です。外部給電の環境で、給電OFF操作は不要です。

## ビルド

C11コンパイラ、make、pkg-config、libusb開発用ファイルが必要です。
libusb 1.0.29/1.0.30でビルド確認済みです。

```sh
# macOS
brew install libusb pkg-config
make
make test

# glibc / musl Linux: 各環境のlibusb開発パッケージを用意して同じ操作
make
make test
```

既定のビルドでリンクするのはlibusbとOS標準ライブラリです。
`CC`、`CFLAGS`、`LDFLAGS`、`USB_CFLAGS`、`USB_LIBS`を上書きできます。
`_FILE_OFFSET_BITS=64` を有効にしています。

### libudevなし・静的libusb

```sh
# ネイティブ環境。Linuxではglibc、MacではAppleのシステムライブラリを使用
./tools/build-portable.sh native

# Alpine等のmusl環境で実行。完全静的のLinux実行ファイルを作る
./tools/build-portable.sh musl

# Android/Bionic向け。Android NDKを別途用意する
ANDROID_NDK_HOME=/path/to/android-ndk-r27d ./tools/build-portable.sh android
```

スクリプトはlibusb 1.0.30を取得し、固定SHA-256を照合してからビルドします。
`musl` モードはmuslコンパイラ環境で実行してください。glibcをmuslへ自動変換するものではありません。
Androidは既定でaarch64/API 24。`ANDROID_ABI=x86_64` または `armv7a` も選択できますが、
Release CIで3種ともクロスビルドとELF検査を行います。出力は `build/` 以下です。

## 機器用データ

FPGAと旧アプリ由来の認証データは、ソースアーカイブには含めません。
`assets/` に次の3ファイルが必要です。別の場所は `--assets DIR` で指定できます。

- `fpga.bin` — DT181用699,673バイト
- `bootstrap.bin` — 16バイト
- `validation.bin` — 64バイト

所有するStationTV LE 2.3.3.1から準備する補助ツールを同梱しています。
Pythonはこの準備作業だけに使用し、受信時には不要です。

```sh
python3 tools/prepare-assets.py --app '/path/to/StationTV LE.app' --fpga '/path/to/pixy_fpga_102b.bin'
```

抽出元バイナリはSHA-256で固定版と照合します。配布対象はCソースとツールです。
実機用データ、録画TS、セッション用データは `.gitignore` とソース梱包の対象外です。

## udev / mdev

どちらも実行時の必須サービスではなく、USBデバイスノードの権限を与える手段です。
libusbがudevなしでビルドされていても、udev管理の `/dev/bus/usb` を利用できます。

udevでは `packaging/70-pix-userland.rules` を `/etc/udev/rules.d/` に配置し、受信ユーザーを
`video` グループに追加して、ルール再読込後に再接続します。

mdevでは `packaging/mdev.conf` のルールを、既存設定の一般USBルールより前に追加します。
起動時に接続済みの機器には、`packaging/pix-usb-permissions --scan` をrootで実行できます。
これはsysfsで `06b8:102b` と確認できたUSB文字デバイスだけに `root:video / 0660` を設定します。
BusyBoxの模擬sysfs/dev環境で、対象のみ変更されることを検査済みです。
実機でのmdevホットプラグ一式は未検証です。

## Android / Termux

Android側でUSBアクセス権を取得し、開いたfdを渡します。`--fd N` と末尾の整数引数を受け付けます。
fdモードではlibusbのデバイス探索を止め、`libusb_wrap_sys_device` を使います。
受け取ったfdのVID/PIDを確認し、終了時も呼び出し元所有のfdを閉じません。

```sh
# 実際のUSBパスに置換。Termux:APIとtermux-apiパッケージが必要です
termux-usb -r -e './pix-ts --assets ./assets -c 27 -t 15 -o terrestrial.ts' /dev/bus/usb/001/002
```

Bionic版aarch64はNDK r27dでクロスビルド・ELF依存確認済みです。
Android実機での権限ダイアログ、fd引き渡し、チューナー受信は未検証です。

## 復号と出力

本機内のB-CASカードを使う処理と、USB転送の独自暗号は別です。
`pix-ts` は旧アプリのセッション確立・鍵更新・AES転送暗号を再現します。
ソフト側のB-CAS/MULTI2デスクランブル処理はありません。
今回のNHK総合・NHK BS・QVCでは、復元後のTSをそのまま再生できました。
未契約番組や未確認の機器モードまで視聴を保証するものではありません。

## 終了とエラー

- 0: 正常終了。取得できたTSのPAT/PMTも確認。
- 2: CLIの指定誤り／端末へのバイナリ出力。
- 3: ファイル、メモリ等のローカルエラー。
- 4: USB、初期化、選局、セッション制御エラー。
- 8: TS検証エラー、受信キュー満杯、USBストリーム／出力エラー。
- 130: 初期化中の割り込みが検出された場合。

TS出力が3秒以上進まない場合はエラー終了します。欠落を黙って成功扱いしません。
最後の188バイト未満の途中パケットはTSに出力せず、`trailing_bytes` に記録します。
停止・取り外し・長時間運用には引き続き検証余地があります。

## 現在の範囲

DT181の単一チューナー・全TS出力が対象です。DT141/195/295は未対応・未検証。
GUI、予約録画、常駐デーモン、標準入力による再選局、PIDフィルター、4K/8Kは未実装です。
Windows x64版はlibusbのWinUSBバックエンドを使用します。対象機器にWinUSBドライバを
関連付ける必要があります。Windowsでの受信は未検証です。ブラウザのWebUSB対応には
非同期API向けの状態管理など追加実装が必要です。

構成は [architecture.md](docs/architecture.md)、検証範囲は [validation.md](docs/validation.md) を参照してください。

命名・CLIの参考: [siano-userland](https://github.com/Khronos31/siano-userland)、
[px4-userland](https://github.com/Khronos31/px4-userland)。これらのドライバ実装は取り込んでいません。
ライセンスはMIT。静的リンク版のlibusbには別途LGPL-2.1以降が適用されます。

## Private Release CI

7プラットフォームのバイナリとソース・SHA256SUMSを、Privateリポジトリ内のDraft Releaseに保存します。
対象と手順は [releases.md](docs/releases.md) を参照してください。CIは実機用データを使いません。
