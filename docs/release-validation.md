# 現行 Stable 候補のリリース前回帰手順

この手順は現行の配布3コマンド `pixd` / `pixctl` / `pix-ts` に適用します。`pix-ts-legacy` の履歴/テスト結果は配布構成の確認ではありません。CLIは [`README.md`](../README.md)、DT295固有制限は [`dt295.md`](dt295.md)、過去結果は [`validation.md`](validation.md) を参照します。

## 1. 候補・配布物

候補source commitのCI/package成果物を取得し、run/ref/SHA、artifact、build-infoの固定Core SHA、archive hash、source manifest、license/provenanceを記録します。7 binary tar.gzはLinux x86_64/aarch64、macOS arm64、Android aarch64/armv7a/x86_64、Windows x64です。source archiveを含め、空directoryへ展開してファイルを確認します。

```sh
mkdir candidate && cd candidate
tar -xf <candidate-artifact.tar.gz>
sha256sum -c SHA256SUMS
```

同一runのpackage重複性、各target binary audit、source archiveからネットワークなしの再build、Core source revision/provenanceとlicense一致を確認します。PIXの現在の `release.yml` は `workflow_dispatch` 専用でmain ref・既存tag/releaseなしを要求し、tag pushでの自動buildではありません。現行CIの実態として記録し、このrunbook作業中はworkflowを実行しません。自動tag-build公開へ変える仕様/CI判断は親へ報告し、ここでは文書から事実を捏造しません。

## 2. device-free gate

`.github/workflows/ci.yml` の `three-commands`、Windows runnerのpackage/runtime/EOF proofを含む候補commit上のCIを確認します。READMEのCMake proof、native/cross build、`tools/audit-binary.py`、source rebuild、license auditを実行し、出力・終了値を保存します。各候補archiveで `pixd --version`、`pixctl --version`、`pix-ts --version`、`pixd --models`、`pixd --list-json` を実行します。妥当なversion/model/JSONが返り、list-jsonが機器を所有しないことを確認します。DT295用旧standalone `--model` CLIを配布バイナリ試験に使いません。

## 3. native archiveと受信回帰

最低soak時間はLinux x86_64代表機2時間、macOS arm64 30分、Windows x86_64 30分、他の全配布binary各10分。2時間は各対応機種が一度以上およびreleaseごとの代表Linux x86_64機で実施します。全配布binaryはそのnative OS/ABIで短時間回帰し、major versionでは各OS/archで短時間TSを実施します。実機が未確認のOS/ABIは未確認のまま記録し、CIだけで受信認定しません。

候補archive rootで作業し、配布assetsを読み込ませます。USB位置とchannel値は現地の実値へ置き換えます。

```sh
RT=$(mktemp -d "${TMPDIR:-/tmp}/pix-release.XXXXXX")
./pixd --device usb:<BUS-PORT> --assets ./assets --runtime-dir "$RT" --instance rc --group & P=$!
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --group status
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --group list
./pix-ts --instance rc --channel T27 --system isdb-t --time 10 --output t27.ts --runtime-dir "$RT" --group
```

statusがfree/ready、receiverが存在すること、T27が正常終了しTSが同期した188-byte packet列であること、daemon packet/byte/USB countersとTS品質を確認します。現地で受信可能な全地上波channel、全無料BS/CS対象を各10秒取得し、channel/TSID/slot/周波数を実値で記録します。無料CS raw TSを対象外にしない一方、CS22の複数ECM問題は1.0.0の範囲外としてその既知Issueだけを別記します。受信後は通常終了し同じdaemon/runtimeから再取得して再利用を確認します。出力非空だけで合格にせず、TS parser/独立検査と記録済みcounterで評価します。

PIX内部カードは以下の `card-status` による挿入状態だけを確認します。外部ATR/reset/APDUやPC/SC/WinSCardは非対応で実施しません。カードを抜いてstatus不在、再挿入後presentへ戻ることはcaptureを止めdaemon idleにした状態で行います。streamとカード再挿入の共存は未確認のため未達として明記します。

```sh
./pixctl --runtime-dir "$RT" --device usb:<BUS-PORT> --group card-status
```

## 4. USB切断とLNB

USB idleとstream中にユーザーがdeviceを抜き、client/daemonの実際の切断表示・終了値を保存します。自動hotplug再取得を期待しません。通常cleanup後、同じ物理portを戻して再列挙し、新しい `pixd` を起動してstatus/list/T27を確認します。AndroidはTermux USB許可/再取得、WindowsはREADMEのWinUSB構成を使います。BUSYなら別processを奪わず理由を記録します。

本製品はLNB制御を提供しません。`--lna` 等の機能をLNB給電と誤認せず、software/実電圧を試験しません。DT295 assets/model経路は対応するnative macOS archiveと [`dt295.md`](dt295.md) の条件に従い、他OSのDT181実績から認定を広げません。

## 5. soak・cleanup・記録

各archiveをnative OS/ABIで規定時間連続受信し、開始・定期・終了時にTS/parser品質、packet/counter、process/FD/RSS、channel、終了値を保存します。各地域channelの10秒回帰はこのsoakとは別に実施します。未完了やエラーは失敗/未確認で残します。

`pix-ts`を正常終了し、`pixd`へSIGTERMを送り、status/process/runtime socket cleanupを確認します。

```sh
kill -TERM "$P"
wait "$P"
./pixctl --runtime-dir "$RT" --device usb:<BUS-PORT> status
rmdir "$RT"
```

記録にはcandidate/Core SHA、workflow run/attempt、archive SHA、host/OS/arch/model、assets識別（秘密値を含めない）、USB selector、channel、コマンド、人的操作、期待/実測、ログ、cleanup、対象外/未確認を含めます。全gate後の結果記録commit前にsource/build inputが候補から変わっていないことを確認します。CIの手動Release workflow、tag、公開操作は実行しません。
