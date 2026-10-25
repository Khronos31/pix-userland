# Stable リリース前検証手順

この文書は `pixd` / `pixctl` / `pix-ts` のStable releaseで再利用する準備・操作・判定・cleanupの正本です。版固有matrixと検証記録は [`release-validation/1.0.0.md`](release-validation/1.0.0.md) のように分離します。`pix-ts-legacy` の履歴/テスト結果は配布構成の確認ではありません。CLIは [`README.md`](../README.md)、DT295固有制限は [`dt295.md`](dt295.md)、過去結果は [`validation.md`](validation.md) を参照します。

## 共通の検証項目と頻度

用語は4製品で共通とする。代表機はLinux x86_64 glibc、機種はチューナー、製品はドライバ、ソークは30分以上の連続TS取得を指す。PIXの標準機種はPIX-DT295とPIX-DT181-PU0の両方で、結果は[`release-validation/initial-model-checks.md`](release-validation/initial-model-checks.md)に機種別に記録する。

| 区分 | 適用頻度 | PIXでの確認 |
|---|---|---|
| 機種初回 | DT295とDT181-PU0それぞれを代表機で2時間連続受信し、毎release項目を代表機だけで実施する。必要時項目も通す。 | 機種ごとに異なるCLI/assets/OS対応を確認。 |
| 回帰（毎release） | 代表機2時間連続受信。全配布archiveの各OS/ABIで短時間実機回帰、`--list-json`、最大能力の同時受信とTS/counter/終了値を確認する。最大試験で地上/衛星3波が不足する場合は不足分を個別に短時間取得する。受信中status、正常停止とprocess/FD/IPC残留なし、CARD挿抜、USB抜去/再挿入を確認する。 | 1チューナー/1 receiverで同時stream上限は1。DT181-PU0はT/BS/CSの短時間取得、DT295はREADMEの確認済みmacOS地上波範囲を適用する。内部CARD状態は`pixctl card-status`で確認する。外部ATR/APDU・PC/SC/WinSCard・LNBは製品機能外。両機種を別々に記録する。 |
| 必要に応じて | LNB実測と非対応機種の拒否、複数チューナー列挙/daemon並列、変更機能に応じた機種×OS/ABIの追加確認。 | PIXはLNB給電なし。model enumerationやmulti-device変更時は該当機種で確認。 |
| 初回/major | 必要時項目を通し、代表機で全無料channelを各10秒TS取得。macOS/Windows各30分連続、残るOS/ABI各10分連続で取得する。全channel取得は代表機だけ。別途指示されたソークは別記する。 | DT295は対応OS/asset条件に従い、DT181-PU0結果と分離する。 |

全配布archiveはnative OS/ABIで回帰し、代表機の全channel10秒取得を全archiveへ広げない。具体的な候補・操作・判定は以下の手順、各版の実績は[`release-validation/1.0.0.md`](release-validation/1.0.0.md)へ記録する。

## 1. 候補・配布物

現行CIには7 binaryすべてを含むpre-release candidate artifactがありません。通常CIはLinux x86_64 packageとsourceを、Windows workflowはWindows packageを個別に生成します。release.ymlはmain限定の `workflow_dispatch` で全targetをpackageし、Release作成・asset upload・公開まで行います。これをpre-release候補としてdispatchすると正式Releaseを公開するため、回帰工程では実行しません。候補artifact一式がないので、実機回帰の実行可能性は現CIのままでは未達です。candidate artifact追加とtag自動公開への変更はCI/source変更になるため、このdocs-only PRでは行わず、親の判断事項とします。

公開後の監査を行う際は、GitHub Releaseから新規directoryへbinary/source archiveと`SHA256SUMS`を取得し、`sha256sum -c SHA256SUMS`、各archive内の`build-info.txt`が記録する固定Core SHA、source manifest/license/provenanceを照合します。これはpre-release candidate試験の代替にはなりません。

## 2. device-free gate

`.github/workflows/ci.yml` の `three-commands`、Windows runnerのpackage/runtime/EOF proofを含む候補commit上のCIを確認します。READMEのCMake proof、native/cross build、`tools/audit-binary.py`、source rebuild、license auditを実行し、出力・終了値を保存します。各候補archiveで `pixd --version`、`pixctl --version`、`pix-ts --version`、`pixd --models`、`pixd --list-json` を実行します。妥当なversion/model/JSONが返り、list-jsonが機器を所有しないことを確認します。DT295用旧standalone `--model` CLIを配布バイナリ試験に使いません。

## 3. native archiveと受信回帰

毎release、代表Linux x86_64機で2時間連続受信し、全配布archiveをnative OS/ABIで短時間回帰します。初回/major追加確認ではmacOS/Windowsは各30分、残るOS/ABIは各10分連続受信し、全無料channel各10秒は代表機だけで行います。CIのみで実機認定とはしません。

候補archive rootで作業し、配布assetsを読み込ませます。USB位置とchannel値は現地の実値へ置き換えます。

```sh
./pixd --models
./pixd --list-json
RT=$(mktemp -d "${TMPDIR:-/tmp}/pix-release.XXXXXX")
./pixd --device usb:<BUS-PORT> --assets ./assets --runtime-dir "$RT" --instance rc --group & P=$!
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --group status
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --group list
./pix-ts --instance rc --channel T27 --system isdb-t --time <SECONDS> --output t27.ts --runtime-dir "$RT" --group
```

`--models`と`--list-json`でcandidateのmodelとUSB位置を記録し、statusがfree/readyでreceiverが1基あることを確認します。最大同時受信数の1 streamを正常終了させ、client終了値、daemon packet/byte/USB counters、TS品質を確認します。DT181-PU0は利用可能な地上波・BS・CSから3波を選び、最大受信試験で未確認の波を短時間取得します。DT295は現行READMEと[`dt295.md`](dt295.md)で確認済みのmacOS arm64・T27・target 0に範囲を限定し、それ以外を成功扱いしません。channel/TSID/slot/周波数は実値を記録します。出力非空だけで合格にせず、TS parser/独立検査と記録済みcounterで評価します。

受信中は別端末で状態を反復観測し、clientを通常終了して同じdaemon/runtimeから再取得します。

```sh
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --group status
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --group list
```

USB抜去/再挿入はidle時と受信中で別々に行い、実際のclient/daemon表示と終了値を記録します。抜去後はdaemonを通常終了し、挿し直した後に`pixd --list-json`で再列挙し、新daemon/runtimeからstatus/listと短時間TSを取り直します。期待されるexit値はREADMEとcandidate実測に照合し、自動hotplug再取得は仮定しません。

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

版ごとの適用頻度・対象・追加確認は版別文書へ記録します。candidate archiveごとの安定した手順と合否判定は本書の各節に従います。
