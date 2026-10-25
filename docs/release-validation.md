# Stable リリース前検証手順

この文書は `pixd` / `pixctl` / `pix-ts` のStable releaseで再利用する準備・操作・判定・cleanupの正本です。版固有matrixと検証記録は [`release-validation/1.0.0.md`](release-validation/1.0.0.md) のように分離します。`pix-ts-legacy` の履歴/テスト結果は配布構成の確認ではありません。CLIは [`README.md`](../README.md)、DT295固有制限は [`dt295.md`](dt295.md)、過去結果は [`validation.md`](validation.md) を参照します。

## 共通の検証項目と頻度

用語は4製品で共通とする。代表機はLinux x86_64 glibc、機種はチューナー、製品はドライバ、ソークは一定時間以上の連続したTS取得を指す。時間条件は適用頻度と版別matrixに従う。PIXの通常の無指定時検証機種はPIX-DT295とPIX-DT181-PU0だが、機種初回の対象を限定しない。PIXが対象とする全機種で初回手順を個別に実施し、結果は[`release-validation/initial-model-checks.md`](release-validation/initial-model-checks.md)へ機種別に記録する。

| 区分 | 適用頻度 | PIXでの確認 |
|---|---|---|
| 機種初回 | PIXの各機種を個別に代表機で2時間連続受信し、その機種で毎release項目を代表機だけで実施する。必要時項目も通す。 | 機種ごとに異なるCLI/assets/OS対応を確認し、結果を機種別に記録する。 |
| 回帰（毎release） | 代表機2時間連続受信。全配布archiveの各OS/ABIで短時間実機回帰、`--list-json`、機種の公開同時受信能力とTS/counter/終了値を確認する。最大試験で地上/衛星3波が不足する場合は不足分を個別に短時間取得する。受信中status、正常停止とprocess/FD/IPC残留なし、CARD挿抜、USB抜去/再挿入を確認する。 | DT181-PU0はT/BS/CS、DT295は地上波の試験対象として、両機種を全配布archive/OS/ABIごとに分けて記録する。DT295の現source platform gateとprivate package経路は結果に記録し、他OSを対象外として扱わない。内部CARD状態は`pixctl card-status`で確認する。外部ATR/APDU・PC/SC/WinSCard・LNBは製品機能外。 |
| 必要に応じて | LNB実測と非対応機種の拒否、複数チューナー列挙/daemon並列、変更機能に応じた機種×OS/ABIの追加確認。 | PIXはLNB給電なし。model enumerationやmulti-device変更時は該当機種で確認。 |
| 初回/major | 必要時項目を通し、代表機で全無料channelを各10秒TS取得。macOS/Windows各30分連続、残るOS/ABI各10分連続で取得する。全channel取得は代表機だけ。別途指示されたソークは別記する。 | DT295は機種としてOS/ABIごとに試験し、source gateとpackage経路を結果に記録する。DT181-PU0結果と分離する。 |

全配布archiveはnative OS/ABIで回帰し、代表機の全channel10秒取得を全archiveへ広げない。具体的な候補・操作・判定は以下の手順、各版の実績は[`release-validation/1.0.0.md`](release-validation/1.0.0.md)へ記録する。

## 共通実行環境表

### 環境表データ辞書

| 列 | 意味・含める属性 | 含めない属性 | 表記・許容値 |
|---|---|---|---|
| ID | OS/ABIと必要なruntime/access境界を持つ実行環境シナリオの識別子 | candidate状態、過去実績 | `E` + 番号。4製品で共通するシナリオは同じID、別runtime/access scenarioは別ID |
| OS / ABI | OS系統、CPU ABI、libcまたはOS runtime | host名、依存、実施状態 | `OS / CPU ABI / libc・runtime` の順。根拠文書にlibc記載がない場合は `未特定` |
| 依存 / driver / permission | 実行時USB library、OS側USB access手段と権限条件 | firmware、CARD API、検証結果 | `libusb:`、`USB access:`、`FD handoff:` の属性名を使う |
| firmware / input | 実行時に必要なfirmwareまたは製品入力と放送方式 | archiveへの同梱・埋込完了、candidate実績 | `firmware:` と `input:` を分け、該当しない属性は `なし` |
| 製品CLIからの内蔵CARD操作 | 製品固有CLI/APIの操作範囲 | PC/SC/WinSCard adapter、実機合否 | 操作名とmodel依存条件。機能なしは `なし` |
| native PC/SC/WinSCard adapter | OS標準APIへ接続する製品adapterの種類・配布形態 | 製品CLI CARD、adapter実機試験結果 | `Linux PC/SC:`、`macOS PC/SC:`、`Android:`、`Windows WinSCard:` を列挙 |
| 配布artifact / 手順 | 対象archive識別子とこの正本内の該当手順 | artifact hash、host、試験結果 | `artifact: <target>; procedure: §...` の順 |

共通IDは E02=Linux/x86_64/musl、E03=Linux/x86_64/glibc、E04=macOS/arm64、E05=Android/aarch64、E06=Android/armv7a、E07=Android/x86_64、E15-G=Linux/aarch64/glibc、E15-M=Linux/aarch64/musl、E17=Windows 11/x86_64を表す。IDはhost名や試験済み状態を表さない。PIXはLinux配布archiveがglibc/musl両環境で動作する静的musl binaryであり、別個のx86_64 musl配布targetを持たない。

| ID | OS / ABI | 依存 / driver / permission | firmware / input | 製品CLIからの内蔵CARD操作 | native PC/SC/WinSCard adapter | 配布artifact / 手順 |
|---|---|---|---|---|---|---|
| E03 | Linux / x86_64 / glibc | libusb: 静的1.0.30; USB access: native OSの許可済み経路 | firmware: なし; input: DT181/DT295 model assets | 内部CARD statusのみ; model依存 | Linux PC/SC: なし; macOS PC/SC: なし; Android: なし; Windows WinSCard: なし | artifact: `linux-x86_64`; procedure: §3–§5 |
| E15-M | Linux / aarch64 / musl | libusb: 静的1.0.30; USB access: native OSの許可済み経路 | firmware: なし; input: DT181/DT295 model assets | 内部CARD statusのみ; model依存 | Linux PC/SC: なし; macOS PC/SC: なし; Android: なし; Windows WinSCard: なし | artifact: `linux-aarch64`; procedure: §3–§5 |
| E04 | macOS / arm64 / Apple system runtime | libusb: 静的1.0.30; USB access: macOSの許可済み経路 | firmware: なし; input: DT181/DT295 model assets | 内部CARD statusのみ; model依存 | Linux PC/SC: なし; macOS PC/SC: なし; Android: なし; Windows WinSCard: なし | artifact: `darwin-arm64`; procedure: §3–§5 |
| E05 | Android (Termux) / aarch64 / Android NDK ABI | libusb: 静的1.0.30; USB access: Termux USB permission; handoff: platform procedure | firmware: なし; input: DT181/DT295 model assets | 内部CARD statusのみ; model依存 | Linux PC/SC: なし; macOS PC/SC: なし; Android: なし; Windows WinSCard: なし | artifact: `android-aarch64`; procedure: §3–§5 |
| E06 | Android (Termux) / armv7a / Android NDK ABI | libusb: 静的1.0.30; USB access: Termux USB permission; handoff: platform procedure | firmware: なし; input: DT181/DT295 model assets | 内部CARD statusのみ; model依存 | Linux PC/SC: なし; macOS PC/SC: なし; Android: なし; Windows WinSCard: なし | artifact: `android-armv7a`; procedure: §3–§5 |
| E07 | Android (Termux) / x86_64 / Android NDK ABI | libusb: 静的1.0.30; USB access: Termux USB permission; handoff: platform procedure | firmware: なし; input: DT181/DT295 model assets | 内部CARD statusのみ; model依存 | Linux PC/SC: なし; macOS PC/SC: なし; Android: なし; Windows WinSCard: なし | artifact: `android-x86_64`; procedure: §3–§5 |
| E17 | Windows 11 / x86_64 / Win32 | libusb: 静的1.0.30; USB access: WinUSB driver | firmware: なし; input: DT181/DT295 model assets | 内部CARD statusのみ; model依存 | Linux PC/SC: なし; macOS PC/SC: なし; Android: なし; Windows WinSCard: なし | artifact: `windows-x64`; procedure: §3–§5 |

archive対象と試験前提を記す表であり、実装状態・候補の有無・特定版の実施結果は版別文書へ記録する。PIXの内部CARD操作はstatus照会に限り、native PC/SC/WinSCard adapterとは別の列で扱う。DT295は製品対象として各OS/ABIの試験に含め、未実装やpackage未接続を対象外へ読み替えない。assetsのarchiveへの埋込状態はここで判定しない。

## 共通試験シナリオ

シナリオ本文は全製品で「適用頻度/対象能力、準備/起動、操作/利用者への依頼、期待値、停止/cleanup、記録」の順に記す。HAOS/SCS上で物理操作を依頼する場合、各依頼前にCodexは`beep`、Claude Codeは`vibe`を実行し、依頼から最大5分待つ。期限内に操作がなければ、その工程を未完了として停止する。

### A. 列挙と最大同時受信

| 適用頻度 / 対象能力 | 準備 / 起動 | 操作 / 利用者への依頼 | 期待値 | 停止 / cleanup | 記録 |
|---|---|---|---|---|---|
| 毎release。各modelの公開receiver数。DT181-PU0はT/BS/CS、DT295はOS/ABIごとに試験し現在のsource gate/package経路を記録 | candidate assets、現地`BUS-PORT`、空の`$RT`を用意し、`pixd --list-json`と`pixd --models`で照合 | `pixd --device usb:<BUS-PORT> --assets ./assets --runtime-dir "$RT" --instance rc --group`起動後、同じruntime/instance/groupの`pixctl ... status`と`pixctl ... list`をそれぞれ確認し、公開receiverごとの`pix-ts --instance rc --channel <CHANNEL> --system <SYSTEM> --time <SECONDS> --output <FILE> --runtime-dir "$RT" --group`を同時実行 | 各要求streamのclient終了値、TS同期/parser結果、daemon packet/byte/USB countersを確認。source gateでunsupportedとなった条件もarchive/model/OSごとに記録し、対象外扱いしない | clientを通常終了後、`pixd`を通常停止しprocess/FD/runtime socket残留を確認 | archive SHA、model、OS/ABI、channel、USB selector、CLI/exit、TS/counters、cleanup |

受信中status/listの反復確認は次のCLIを使い、実行時刻をstream開始/終了とともに記録する。

```sh
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --group status
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --group list
```

### B. 内部CARD status抜去・再挿入

適用: 内部reader statusを公開するmodel。外部ATR/reset/APDU/PC/SC/WinSCard操作は製品機能外。受信を正常停止してdaemonを維持した状態で行う。

```sh
./pixctl --runtime-dir "$RT" --device usb:<BUS-PORT> --instance rc --group card-status
```

1. `card-status`で挿入時の内部状態を記録する。
2. 親は「対象機のカードを抜いてください」と利用者へ依頼し、依頼から最大5分待つ。応答がなければ工程を未完了として中断する。抜去後に同じ`card-status`を実行し、不在状態を期待する。

```sh
./pixctl --runtime-dir "$RT" --device usb:<BUS-PORT> --instance rc --group card-status
```

3. 親は「同じカードを再挿入してください」と依頼し、最大5分待つ。再挿入後に`card-status`を実行し、挿入状態への復帰を期待する。

```sh
./pixctl --runtime-dir "$RT" --device usb:<BUS-PORT> --instance rc --group card-status
```

4. daemonを通常停止し、process/FD/runtime socketを確認する。status出力以外のATR/APDUデータを作らない。

各物理操作の依頼時刻、出力、終了値、状態遷移、cleanupを記録する。CARD statusとstream同時動作を主張する結果は、候補archiveを使った別試験記録へ記す。

### C. USB抜去・再接続

| 適用頻度 / 対象能力 | 準備 / 起動 | 操作 / 利用者への依頼 | 期待値 | 停止 / cleanup | 記録 |
|---|---|---|---|---|---|
| 毎release。対応modelのidle時とstreaming時 | `pixd --list-json`でportを固定し、idle runとstream runを分ける。stream時は`--time <CAPTURE_SECONDS>`を設定し、有限観測窓`<OBSERVATION_LIMIT_SECONDS>`を事前に決めて記録する | 各runで親が対象USBを抜くよう依頼し、依頼時刻を記録する。stream時は観測窓が残りcapture時間より短い状態で抜去し、自然終了と終了値を観測する。時間値は各試験で選ぶ観測条件で、製品の固定秒数要件ではない | stream中の切断はclient exit 7と`USB_IO`/`DISCONNECTED` stream-end診断を期待する。`--time`満了によるexit 0は切断合格ではない。観測窓を越えてclientが残る場合はtimeout/failとして記録し、SIGINTで通常cleanupする。再接続後は新daemonのstatus/listと短TSを確認し、client exit 0とTS品質合格を必要とする | client/旧daemonを通常終了し、終了値・process/FD/socketを確認。再接続後の新daemonも通常終了してcleanup。Termuxはpermission/FDを再取得 | 物理操作と観測窓、capture deadline、port、stream-end診断、client/daemon exit、TS/counter/解析、再列挙、cleanup |

### D. teardown・結果

受信clientを通常終了し、`kill -TERM "$P"`、`wait "$P"`でdaemon終了を待ち、status/process/runtime socketのcleanupを確認する。頻度ごとの実績、未実施、失敗、対象外は版別文書へ記録する。

## 1. 候補・配布物

対象versionの候補sourceから作成されたbinary archiveとsource archiveを取得し、hash・manifest・license/provenanceを照合します。pre-release候補が用意できない場合は実機gateを完了扱いにせず、その版別文書に未実施理由を記録します。正式Releaseを作成・公開するworkflowを候補作成の代用にしません。

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
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --group status
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --group list
./pix-ts --instance rc --channel T27 --system isdb-t --time <SECONDS> --output t27.ts --runtime-dir "$RT" --group
```

`--models`と`--list-json`でcandidateのmodelとUSB位置を記録し、statusがfree/readyでreceiverが1基あることを確認します。最大同時受信数の1 streamを正常終了させ、client終了値、daemon packet/byte/USB counters、TS品質を確認します。DT181-PU0は利用可能な地上波・BS・CSから3波を選び、最大受信試験で未確認の波を短時間取得します。DT295は各archive/OS/ABIで試験し、現在のsource gateがunsupportedを返す条件とcapture可能なmacOS arm64・T27・target 0の実測を分けて記録します。channel/TSID/slot/周波数は実値を記録します。出力非空だけで合格にせず、TS parser/独立検査と記録済みcounterで評価します。

受信中は別端末で状態を反復観測し、clientを通常終了して同じdaemon/runtimeから再取得します。

```sh
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --group status
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --group list
```

USB抜去/再挿入はidle時と受信中で別々に行い、実際のclient/daemon表示と終了値を記録します。抜去後はdaemonを通常終了し、挿し直した後に`pixd --list-json`で再列挙し、新daemon/runtimeからstatus/listと短時間TSを取り直します。期待されるexit値はREADMEとcandidate実測に照合し、自動hotplug再取得は仮定しません。

PIX内部カードは以下の `card-status` による挿入状態だけを確認します。外部ATR/reset/APDUやPC/SC/WinSCardは製品機能外です。captureを通常停止しdaemonを起動したまま行います。各物理操作の直前にHAOS/SCS上でCodexは`beep`、Claude Codeは`vibe`を実行し、「カードを抜いてください」または「同じカードを再挿入してください」と依頼します。各依頼から最大5分待ち、応答がない場合は工程を未完了として中断します。抜去後にstatus不在、再挿入後にpresent復帰を確認します。

```sh
./pixctl --runtime-dir "$RT" --device usb:<BUS-PORT> --instance rc --group card-status
```

## 4. USB切断とLNB

USB idleとstream中にユーザーがdeviceを抜き、client/daemonの実際の切断表示・終了値を保存します。stream切断は通常time満了と区別するため、試験ごとに`CAPTURE_SECONDS`とそれより短い`OBSERVATION_LIMIT_SECONDS`を選び、抜去時刻と残りcapture時間を記録します。これらは観測用parameterで、製品の時間受入要件ではありません。clientが指定時間の満了前に自然終了した場合、pinned Coreのexit mapに従う切断終了はexit 7（`USB_IO`または`DISCONNECTED`）で、`pix-ts` stderrの`stream-end`診断と`packets-written`も記録します。exit 0は通常の`--time`満了なので切断合格にしません。有限観測窓内に終了しない場合はtimeout/failを記録し、`SIGINT`でclientを通常停止して終了値を保存します。自動hotplug再取得は期待しません。

stream runでは、物理抜去依頼の前にstatusでstreamingを確認し、candidateに対する有限captureを開始します。観測窓の値は各実施記録へ明記します。

```sh
CAPTURE_SECONDS=<finite-capture-duration>
OBSERVATION_LIMIT_SECONDS=<finite-cut-observation-window>
./pix-ts --device usb:<BUS-PORT> --instance rc --channel T27 --system isdb-t \
  --time "$CAPTURE_SECONDS" --output cut.ts --runtime-dir "$RT" --group \
  >"$LOG/cut.stdout" 2>"$LOG/cut.stderr" & C=$!
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --group status
# streaming確認後、残りcapture時間が観測窓より長い間に利用者へ抜去を依頼
```

依頼後は事前に選んだ観測窓内でclientが自然終了するかを見て、`wait "$C"`の実終了値とstderrを保存します。指定時間の満了後にexit 0したrunは通常time満了として切断試験を無効扱いにします。

旧daemonを通常停止し、process/FD/runtime socketを確認してから、ユーザーへ同じ物理portへの再接続を依頼します。`pixd --list-json`で再列挙した後、新しいruntimeでdaemonを起動してstatus/listを確認し、既知の無料T27の短TSを取得します。

```sh
RT2=$(mktemp -d "${TMPDIR:-/tmp}/pix-reconnect.XXXXXX")
./pixd --device usb:<BUS-PORT> --assets ./assets --runtime-dir "$RT2" --instance rc --group & P2=$!
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT2" --instance rc --group status
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT2" --instance rc --group list
./pix-ts --device usb:<BUS-PORT> --instance rc --channel T27 --system isdb-t \
  --time <SECONDS> --output t27.ts --runtime-dir "$RT2" --group
```

再接続後は`pix-ts` exit 0に加えてTS sync/parser品質合格、daemon counters、通常daemon exit 0とprocess/FD/socket cleanupを記録します。AndroidはTermux USB許可/再取得、WindowsはREADMEのWinUSB構成を使います。BUSYなら別processを奪わず理由を記録します。

本製品はLNB制御を提供しません。`--lna` 等の機能をLNB給電と誤認せず、software/実電圧を試験しません。DT295のcapture gateとprivate assets finalize対象は現状macOS arm64に限られるため、全OS/ABIでのDT295結果は各行に分けて記録し、DT181の実績から認定を広げません。

## 5. soak・cleanup・記録

代表Linux x86_64機で2時間連続受信し、開始・定期・終了時にTS/parser品質、packet/counter、process/FD/RSS、channel、終了値を保存します。未完了やエラーは失敗/未確認で残します。

`pix-ts`を通常終了し、`pixd`へSIGTERMを送り、status/process/runtime socket cleanupを確認します。`pixd`終了後もruntime rootを残したまま同じ`--group` accessでstatus照会し、IPC endpointの消失と接続失敗を対応付けます。

```sh
kill -TERM "$P"
if wait "$P"; then daemon_rc=0; else daemon_rc=$?; fi
printf '%s\n' "$daemon_rc" > "$LOG/daemon.rc"
find "$RT" -type s -print > "$LOG/socket-after-stop.txt"
if ./pixctl --runtime-dir "$RT" --instance rc --device usb:<BUS-PORT> --group status > "$LOG/status-after-stop.txt" 2>&1; then rc=0; else rc=$?; fi
printf '%s\n' "$rc" > "$LOG/status-after-stop.rc"
rmdir "$RT"
```

`wait`は条件式内で捕捉するため、daemonが非zero終了しても`set -e`相当のshellでも後続を打ち切らず、他のcommandが終了値を上書きする前に`$LOG/daemon.rc`へ保存します。正常停止の期待値はdaemon exit 0、空のsocket一覧、同じgroup modeの`pixctl`がendpoint不在を示す`connect failed (3)`でexit 3です。実測`daemon.rc`が`0`であることをsocket/status記録と並べて確認します。別access modeの接続失敗をcleanupの証拠に使いません。

記録にはcandidate/Core SHA、workflow run/attempt、archive SHA、host/OS/arch/model、assets識別（秘密値を含めない）、USB selector、channel、コマンド、人的操作、期待/実測、ログ、cleanup、対象外/未確認を含めます。全gate後の結果記録commit前にsource/build inputが候補から変わっていないことを確認します。CIの手動Release workflow、tag、公開操作は実行しません。

版ごとの適用頻度・対象・追加確認は版別文書へ記録します。candidate archiveごとの安定した手順と合否判定は本書の各節に従います。
