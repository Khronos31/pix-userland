# 現行 Stable 候補のリリース前回帰手順

この手順は現行の配布3コマンド `pixd` / `pixctl` / `pix-ts` に適用します。`pix-ts-legacy` の履歴/テスト結果は配布構成の確認ではありません。CLIは [`README.md`](../README.md)、DT295固有制限は [`dt295.md`](dt295.md)、過去結果は [`validation.md`](validation.md) を参照します。

## 1. 候補・配布物

現行CIには7 binaryすべてを含むpre-release candidate artifactがありません。通常CIはLinux x86_64 packageとsourceを、Windows workflowはWindows packageを個別に生成します。release.ymlはmain限定の `workflow_dispatch` で全targetをpackageし、Release作成・asset upload・公開まで行います。これをpre-release候補としてdispatchすると正式Releaseを公開するため、回帰工程では実行しません。候補artifact一式がないので、実機回帰の実行可能性は現CIのままでは未達です。candidate artifact追加とtag自動公開への変更はCI/source変更になるため、このdocs-only PRでは行わず、親の判断事項とします。

公開後の監査を行う際は、GitHub Releaseから新規directoryへbinary/source archiveと`SHA256SUMS`を取得し、`sha256sum -c SHA256SUMS`、各archive内の`build-info.txt`が記録する固定Core SHA、source manifest/license/provenanceを照合します。これはpre-release candidate試験の代替にはなりません。

## 2. device-free gate

`.github/workflows/ci.yml` の `three-commands`、Windows runnerのpackage/runtime/EOF proofを含む候補commit上のCIを確認します。READMEのCMake proof、native/cross build、`tools/audit-binary.py`、source rebuild、license auditを実行し、出力・終了値を保存します。各候補archiveで `pixd --version`、`pixctl --version`、`pix-ts --version`、`pixd --models`、`pixd --list-json` を実行します。妥当なversion/model/JSONが返り、list-jsonが機器を所有しないことを確認します。DT295用旧standalone `--model` CLIを配布バイナリ試験に使いません。

## 3. native archiveと受信回帰

各releaseのSoakでは代表Linux x86_64機で2時間連続受信します。対応機種ごとのqualificationでは各機種自体を一度以上2時間試験し、releaseごとの繰り返し条件ではありません。major versionでは全OS/architectureごとにnative binaryで短時間TSを行います。CIのみで実機認定とはしません。

候補archive rootで作業し、配布assetsを読み込ませます。USB位置とchannel値は現地の実値へ置き換えます。

```sh
RT=$(mktemp -d "${TMPDIR:-/tmp}/pix-release.XXXXXX")
./pixd --device usb:<BUS-PORT> --assets ./assets --runtime-dir "$RT" --instance rc --group & P=$!
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --group status
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --group list
./pix-ts --instance rc --channel T27 --system isdb-t --time <SECONDS> --output t27.ts --runtime-dir "$RT" --group
```

statusがfree/ready、receiverが存在すること、T27が正常終了しTSが同期した188-byte packet列であること、daemon packet/byte/USB countersとTS品質を確認します。変更に関係するchannelを選び、channel/TSID/slot/周波数を実値で記録します。受信後は通常終了し同じdaemon/runtimeから再取得して再利用を確認します。出力非空だけで合格にせず、TS parser/独立検査と記録済みcounterで評価します。

PIX内部カードは以下の `card-status` による挿入状態だけを確認します。外部ATR/reset/APDUやPC/SC/WinSCardは非対応で実施しません。カードを抜いてstatus不在、再挿入後presentへ戻ることはcaptureを止めdaemon idleにした状態で行います。streamとカード再挿入の共存は未確認のため未達として明記します。

```sh
./pixctl --runtime-dir "$RT" --device usb:<BUS-PORT> --group card-status
```

## 4. USB切断とLNB

USB idleとstream中にユーザーがdeviceを抜き、client/daemonの実際の切断表示・終了値を保存します。自動hotplug再取得を期待しません。通常cleanup後、同じ物理portを戻して再列挙し、新しい `pixd` を起動してstatus/list/T27を確認します。AndroidはTermux USB許可/再取得、WindowsはREADMEのWinUSB構成を使います。BUSYなら別processを奪わず理由を記録します。

本製品はLNB制御を提供しません。`--lna` 等の機能をLNB給電と誤認せず、software/実電圧を試験しません。DT295 assets/model経路は対応するnative macOS archiveと [`dt295.md`](dt295.md) の条件に従い、他OSのDT181実績から認定を広げません。

## 5. soak・cleanup・記録

代表Linux x86_64機で2時間連続受信し、開始・定期・終了時にTS/parser品質、packet/counter、process/FD/RSS、channel、終了値を保存します。未完了やエラーは失敗/未確認で残します。

`pix-ts`を正常終了し、`pixd`へSIGTERMを送り、status/process/runtime socket cleanupを確認します。

```sh
kill -TERM "$P"
wait "$P"
./pixctl --runtime-dir "$RT" --device usb:<BUS-PORT> status
rmdir "$RT"
```

記録にはcandidate/Core SHA、workflow run/attempt、archive SHA、host/OS/arch/model、assets識別（秘密値を含めない）、USB selector、channel、コマンド、人的操作、期待/実測、ログ、cleanup、対象外/未確認を含めます。全gate後の結果記録commit前にsource/build inputが候補から変わっていないことを確認します。CIの手動Release workflow、tag、公開操作は実行しません。

## 6. 1.0.0 初回matrix（この版だけ）

以下の初回版matrixの時間/targetは後続releaseへ自動継承しません。1.0.0では代表Linux x86_64を2時間、macOS arm64とWindows x86_64を各30分、その他全配布binaryを各10分試験します。全7 binaryをnative OS/architectureで短時間回帰し、全実地域地上波channelおよび全無料BS/CS targetを各10秒raw TS取得します。無料CSを省略せず、CS22複数ECMの既知Issueだけを1.0.0対象外として別記します。

channel、TSID、slot、周波数を現地計画と機種設定から試験前に固定し、上の `--time <SECONDS>` をこのmatrixでは `10` に置き換えます。Linux x86_64代表機2時間に加え、各機種自体の一度以上2時間qualification、major versionごとの全OS/architecture短TSを別記します。
