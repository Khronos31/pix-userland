# Stable リリース前検証手順

この文書は `pixd` / `pixctl` / `pix-ts` のStable releaseで再利用する準備・操作・判定・cleanupの正本です。版固有matrixと検証記録は [`release-validation/1.0.0.md`](release-validation/1.0.0.md) のように分離します。`pix-ts-legacy` の履歴/テスト結果は配布構成の確認ではありません。CLIは [`README.md`](../README.md)、DT295固有制限は [`dt295.md`](dt295.md)、過去結果は [`validation.md`](validation.md) を参照します。

## 共通の検証項目と頻度

用語は4製品で共通とする。代表機はLinux x86_64 glibc、機種はチューナー、製品はドライバ、ソークは一定時間以上の連続したTS取得を指す。時間条件は適用頻度と版別matrixに従う。PIXの通常の無指定時検証機種はPIX-DT295とPIX-DT181-PU0だが、機種初回の対象を限定しない。PIXが対象とする全機種で初回手順を個別に実施し、結果は[`release-validation/initial-model-checks.md`](release-validation/initial-model-checks.md)へ機種別に記録する。

| 区分 | 適用頻度 | PIXでの確認 |
|---|---|---|
| 機種初回 | PIXの各機種を個別に代表機で2時間連続受信し、その機種で毎release項目を代表機だけで実施する。必要時項目も通す。 | 機種ごとに異なるCLI/assets/OS対応を確認し、結果を機種別に記録する。 |
| 回帰（毎release） | 代表機2時間連続受信。全配布archiveの各OS/ABIで短時間実機回帰、`--list-json`、機種の公開同時受信能力とTS/counter/終了値を確認する。最大試験で地上/衛星3波が不足する場合は不足分を個別に短時間取得する。受信中status、正常停止とprocess/FD/IPC残留なし、CARD挿抜、USB抜去/再挿入を確認する。 | DT181-PU0はT/BS/CS、DT295は地上波の試験対象として、両機種を全配布archive/OS/ABIごとに分けて記録する。DT295を含む各試験がunsupported/nonzeroでcaptureできない場合、その候補の未達/失敗として記録する。内部CARD状態は`pixctl card-status`で確認する。外部ATR/APDU・PC/SC/WinSCard・LNBは製品機能外。 |
| 必要に応じて | LNB実測と非対応機種の拒否、複数チューナー列挙/daemon並列、変更機能に応じた機種×OS/ABIの追加確認。 | PIXはLNB給電なし。model enumerationやmulti-device変更時は該当機種で確認。 |
| 初回/major | 必要時項目を通し、代表機で全無料channelを各10秒TS取得。macOS/Windows各30分連続、残るOS/ABI各10分連続で取得する。全channel取得は代表機だけ。別途指示されたソークは別記する。 | DT295は機種としてOS/ABIごとに試験する。unsupported/nonzeroでcaptureできない候補は未達/失敗として記録する。DT181-PU0結果と分離する。 |

全配布archiveはnative OS/ABIで回帰し、代表機の全channel10秒取得を全archiveへ広げない。具体的な候補・操作・判定は以下の手順、各版の実績は[`release-validation/1.0.0.md`](release-validation/1.0.0.md)へ記録する。

## 共通実行環境表

### 環境表データ辞書

| 列 | 意味・含める属性 | 含めない属性 | 表記・許容値 |
|---|---|---|---|
| ID | OS/ABIと必要なruntime/access境界を持つ実行環境シナリオの識別子 | candidate状態、過去実績 | `E` + 番号。4製品で共通するシナリオは同じID、別runtime/access scenarioは別ID |
| OS / ABI | OS系統、CPU ABI、libcまたはOS runtime | host名、依存、実施状態 | `OS / CPU ABI / libc・runtime` の順。根拠文書にlibc記載がない場合は `未特定` |
| 依存 / driver / permission | 実行時USB library、OS側USB access手段と権限条件 | firmware、CARD API、検証結果 | `libusb:`、`USB access:`、`FD handoff:` の属性名を使う |
| firmware / 補助data | 機器へ渡す実行時firmwareと、RF channelとは別の機種固有データ | RF試験channel、archiveへの同梱・埋込完了、candidate実績 | `firmware:` と `補助data:` を分け、ない属性は `なし` |
| RF試験入力（system / channel） | 実際に受信させる放送systemと現地channel | firmware、機種data、candidate実績 | `system:` と `channel:`。対応波がない場合は `なし（理由）` |
| 製品CLIからの内蔵CARD操作 | 製品固有CLI/APIの操作範囲 | PC/SC/WinSCard adapter、実機合否 | 操作名とmodel依存条件。機能なしは `なし` |
| native PC/SC/WinSCard adapter | その行のOS標準APIへ接続する製品adapterの種類・配布形態 | 製品CLI CARD、adapter実機試験結果、他OSのadapter | OS/ABI行に該当する`Linux PC/SC:`、`macOS PC/SC:`、`Android:`または`Windows WinSCard:`だけを記載 |
| 配布artifact / 手順 | 対象archive識別子とこの正本内の該当手順 | artifact hash、host、試験結果 | `artifact: <target>; procedure: §番号` の順 |

共通IDは E02=Linux/x86_64/musl、E03=Linux/x86_64/glibc、E04=macOS/arm64、E05=Android/aarch64、E06=Android/armv7a、E07=Android/x86_64、E15-G=Linux/aarch64/glibc、E15-M=Linux/aarch64/musl、E17=Windows 11/x86_64を表す。IDはhost名や試験済み状態を表さない。PIXはLinux配布archiveがglibc/musl両環境で動作する静的musl binaryであり、別個のx86_64 musl配布targetを持たない。

| ID | OS / ABI | 依存 / driver / permission | firmware / 補助data | RF試験入力（system / channel） | 製品CLIからの内蔵CARD操作 | native PC/SC/WinSCard adapter | 配布artifact / 手順 |
|---|---|---|---|---|---|---|---|
| E03 | Linux / x86_64 / glibc | libusb: 静的1.0.30; USB access: native OSの許可済み経路 | firmware: なし; 補助data: DT181/DT295の機種固有model data | system: DT181-PU0は対応T/BS/CS、DT295はISDB-T/ISDB-S; channel: 現地channel plan（DT295例 T27/BS1） | 内部CARD statusのみ; model依存 | Linux PC/SC: なし | artifact: `linux-x86_64`; procedure: §3–§5 |
| E15-M | Linux / aarch64 / musl | libusb: 静的1.0.30; USB access: native OSの許可済み経路 | firmware: なし; 補助data: DT181/DT295の機種固有model data | system: DT181-PU0は対応T/BS/CS、DT295はISDB-T/ISDB-S; channel: 現地channel plan（DT295例 T27/BS1） | 内部CARD statusのみ; model依存 | Linux PC/SC: なし | artifact: `linux-aarch64`; procedure: §3–§5 |
| E04 | macOS / arm64 / Apple system runtime | libusb: 静的1.0.30; USB access: macOSの許可済み経路 | firmware: なし; 補助data: DT181/DT295の機種固有model data | system: DT181-PU0は対応T/BS/CS、DT295はISDB-T/ISDB-S; channel: 現地channel plan（DT295例 T27/BS1） | 内部CARD statusのみ; model依存 | macOS PC/SC: なし | artifact: `darwin-arm64`; procedure: §3–§5 |
| E05 | Android (Termux) / aarch64 / Android NDK ABI | libusb: 静的1.0.30; USB access: Termux USB permission; handoff: platform procedure | firmware: なし; 補助data: DT181/DT295の機種固有model data | system: DT181-PU0は対応T/BS/CS、DT295はISDB-T/ISDB-S; channel: 現地channel plan（DT295例 T27/BS1） | 内部CARD statusのみ; model依存 | Android: なし | artifact: `android-aarch64`; procedure: §3–§5 |
| E06 | Android (Termux) / armv7a / Android NDK ABI | libusb: 静的1.0.30; USB access: Termux USB permission; handoff: platform procedure | firmware: なし; 補助data: DT181/DT295の機種固有model data | system: DT181-PU0は対応T/BS/CS、DT295はISDB-T/ISDB-S; channel: 現地channel plan（DT295例 T27/BS1） | 内部CARD statusのみ; model依存 | Android: なし | artifact: `android-armv7a`; procedure: §3–§5 |
| E07 | Android (Termux) / x86_64 / Android NDK ABI | libusb: 静的1.0.30; USB access: Termux USB permission; handoff: platform procedure | firmware: なし; 補助data: DT181/DT295の機種固有model data | system: DT181-PU0は対応T/BS/CS、DT295はISDB-T/ISDB-S; channel: 現地channel plan（DT295例 T27/BS1） | 内部CARD statusのみ; model依存 | Android: なし | artifact: `android-x86_64`; procedure: §3–§5 |
| E17 | Windows 11 / x86_64 / Win32 | libusb: 静的1.0.30; USB access: WinUSB driver | firmware: なし; 補助data: DT181/DT295の機種固有model data | system: DT181-PU0は対応T/BS/CS、DT295はISDB-T/ISDB-S; channel: 現地channel plan（DT295例 T27/BS1） | 内部CARD statusのみ; model依存 | Windows WinSCard: なし | artifact: `windows-x64`; procedure: §3–§5 |

archive対象と試験前提を記す表であり、実装状態・候補の有無・特定版の実施結果は版別文書へ記録する。PIXの内部CARD操作はstatus照会に限り、native PC/SC/WinSCard adapterとは別の列で扱う。DT295は製品対象として各OS/ABIの試験に含める。assetsのarchiveへの埋込状態はここで判定しない。

## 共通試験シナリオ T00–T14

各IDを同じ順序・同じ6項目で実施する。$RT/$LOG/$BUS-PORT/$CHANNEL_PLANはrunごとの実値に置換し、空のcandidate専用runtime/logを使う。物理操作を依頼する各回は既定通知を行い、最大5分で応答がなければ未完了として停止する。OS/API固有の長い操作は同じT-IDから後続recipeを参照する。

### T00 候補/配布物固定

- **適用対象/頻度:** 毎releaseのcandidate作成時。
- **準備:** source SHA、CI run/attempt、archive targetとhashを固定。POSIX git rev-parse HEAD/sha256sum、Windows Get-FileHashを保存。
- **実操作/利用者への依頼:** pixd --models/--list-jsonとartifact hashを確認。candidateが無ければ停止し正式公開workflowを代用しない。
- **期待値/終了値:** provenance/hash一致の候補だけ次へ進む。候補不在は未完了。
- **停止/cleanup:** deviceを開かず、archive/logを保持。
- **記録:** source SHA、CI run/attempt、artifact ID/target/hash、取得日時、未取得理由。

### T01 環境/permission/input

- **適用対象/頻度:** 全native archive。初回は全modelごと。
- **準備:** E-ID/OS/ABI、USB access、firmware/model data、channel planを環境表と照合。
- **実操作/利用者への依頼:** OS/ABI native commandsを記録しpixd --modelsでmodelを確認する。model dataとRF試験channelを分け、archiveでのmodel dataの実現方法は版別記録に記す。外部assets pathを正本の恒久的な実行前提にしない。
- **期待値/終了値:** OS/ABIがarchiveに一致し、アクセス許可/入力が揃う。不一致はdevice操作前に止める。
- **停止/cleanup:** 権限不足やdriver変更が必要なら操作せず未実施記録。
- **記録:** E-ID/host/OS/ABI/model、USB topology/permission、firmware/data、channel plan。

### T02 列挙/selector/ready

- **適用対象/頻度:** 毎release、対象model/device。
- **準備:** 候補archiveのlist-json/models入口とdevice selectorを準備しdaemon専用runtimeを作る。
- **実操作/利用者への依頼:** pixd --modelsとpixd --list-jsonを保存しmodel/USB portを記録する。pixd --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --groupを起動し、pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --group statusとlistを実行する。
- **期待値/終了値:** JSON/portが一意、status ready、receiver数が公開能力と一致。不正selectorは成功でない。
- **停止/cleanup:** daemon通常停止しprocess/FD/IPC/socket残留を確認。
- **記録:** JSON、selector、status/list、各exit、residual。

### T03 最大同時受信

- **適用対象/頻度:** 毎release、各modelの公開最大能力。
- **準備:** 各公開receiver/deviceを一意に割り当て、各clientに別TS出力/ログを用意。
- **実操作/利用者への依頼:** 公開receiver数分のpix-ts --device usb:<BUS-PORT> --instance rc --channel <CHANNEL> --system <SYSTEM> --time <SECONDS> --output <FILE> --runtime-dir "$RT" --groupを同時起動。DT181/DT295別run。
- **期待値/終了値:** 各clientの稼働時間が重なり、各exit 0とTS/counter合格。順次取得を同時試験の代用にしない。
- **停止/cleanup:** 全client通常終了後daemon通常停止、残留なし。
- **記録:** model/receiver/system/channel、各start/end/exit、TS/counter、cleanup。

### T04 不足波/全無料channel

- **適用対象/頻度:** 毎releaseは不足波。初回/majorは全機種の代表機で全無料channel各10秒。
- **準備:** 現地channel planとmodel能力から対象system/channel一覧を作る。
- **実操作/利用者への依頼:** DT181-PU0は対応するT/BS/CS、DT295はISDB-T/ISDB-Sを対象にする。`pix-ts --channel <CHANNEL> --system <SYSTEM> --time 10 --output <FILE> --runtime-dir "$RT" --group`を各channelで実行する。DT295のISDB-Sも各OS/ABIで省略せず実行し、unsupported/nonzeroは当該候補の未達/失敗として記録する。
- **期待値/終了値:** 各対象channelのTS/exit合格。未取得は未完了、非対応波だけ根拠付きN/A。
- **停止/cleanup:** client wait/exit保存、daemon通常停止。
- **記録:** model/system/channel/全channel list、TS解析/counter/exit、未取得理由。

### T05 stream中monitor/control

- **適用対象/頻度:** 毎release。CARD/controlは製品能力に従う。
- **準備:** capture clientを起動し、別terminalでstatus/list/controlを行えるようにする。
- **実操作/利用者への依頼:** stream中に別terminalからpixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --group statusとlistを実行する。capture停止後、同じruntime/selectorでpixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --group card-statusを実行する。
- **期待値/終了値:** 照会/control時刻がstreamと対応し各response/exitを保存。機能外操作はN/A。
- **停止/cleanup:** client/daemonを通常停止し残留確認。
- **記録:** 照会/control時刻、response class、各exit、streamとの重なり。

### T06 CARD物理挿抜/復帰

- **適用対象/頻度:** CARD reader/status対応model。毎release。
- **準備:** capture停止・daemon維持。初期CARD状態とreader有無を確認。
- **実操作/利用者への依頼:** pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --group card-statusで初期状態を記録する。通知後に利用者へ抜去を依頼し、完了後に同コマンドを実行する。再度通知して同じカードの再挿入を依頼し、同コマンドで復帰状態を記録する。各依頼は最大5分待つ。ATR/reset/APDUは機能外。
- **期待値/終了値:** card-statusは初期状態でpresent、抜去後absent、再挿入後presentを返し各照会はexit 0。PIXはATR/reset/APDU操作を提供しない。応答なしは未完了。
- **停止/cleanup:** daemon通常停止、FD/IPC/runtimeを確認。
- **記録:** 依頼時刻、状態/response/exit、復帰、cleanup。

### T07 USB idle/stream切断と再取得

- **適用対象/頻度:** 毎release。USB対応deviceのidle/streamを別run。
- **準備:** `pixd --list-json`でUSB pathを特定し、streamなしでdaemonを起動してstatus/list、daemon/client PIDとUSB owner状態を記録する。capture clientは開始時にUSBを開くため、daemon-only idleではdevice handleを保持しない。
- **実操作/利用者への依頼:** daemon-only idle中に利用者へ抜去を依頼し、応答後にstatus/list、daemon診断と実際の終了値を採取する。list-jsonは列挙情報でありownership確認として扱わない。再接続を依頼して再列挙後、新runtime/daemonから短い`pix-ts` captureを実行する。別runでは`pix-ts --time "$CAPTURE_SECONDS"`中に抜去を依頼し、client/daemon診断と各実終了値を採取する。
- **期待値/終了値:** daemon-only idleではUSB ownerがいないため、抜去によるstream切断exitは発生せず、list/status差分とdaemonの実際の終了値を記録する。このidle観測をowner-held idle切断の合格にしない。stream中は切断診断とclient exit 7を確認する。time満了exit 0は切断合格でない。新daemonの短TSはexit 0とTS/counter合格。
- **停止/cleanup:** idle/streamそれぞれでowned client/daemonを通常停止してwaitし、再接続後の新daemonも通常停止する。Androidはpermission/FD再取得。
- **記録:** idle/stream、daemon/client USB owner状態、依頼/応答時刻、path、status/診断、各実終了値、再取得TS/counter、cleanup。

### T08 通常stop/cleanup

- **適用対象/頻度:** 全test終了時。
- **準備:** 各owned client/daemon PIDとruntimeを記録してから起動する。
- **実操作/利用者への依頼:** pix-ts clientを通常停止しwait "$C"、pixdを通常SIGINT/console stopしてwait。
- **期待値/終了値:** 全owned process終了、TS close、FD/IPC/socket/runtime残留なし。timeoutはfail。
- **停止/cleanup:** 無関係processへsignalしない。test専用runtimeだけ片付け失敗log保持。
- **記録:** PID/role/exit、process/FD/IPC/socket/runtime、復旧状態。

### T09 TS/counter/exit判定

- **適用対象/頻度:** 全capture/soak。
- **準備:** TS parser/SPEC条件とdaemon counterを確認できるようにする。
- **実操作/利用者への依頼:** 独立TS parserとpixd packet/byte/USB counterを対応付け、通常取得exit 0と既定TS基準を確認。
- **期待値/終了値:** 全TSが製品SPEC基準を満たし通常取得exit 0。非空file/counter 0だけでpassにしない。
- **停止/cleanup:** 元TS/hash/logを残してT08 cleanup。
- **記録:** parser/version、TS hash、sync/TEI/continuity/drop/counter、exit。

### T10 native PC/SC/WinSCard

- **適用対象/頻度:** 対応OS/APIのみ。非対応製品は理由付きN/A。
- **準備:** native OS adapter/consumerとcandidate IFD/DLL pathを用意。
- **実操作/利用者への依頼:** PIXは製品CARD status操作を持つがnative PC/SC/WinSCard adapter/APIを持たないため、このnative transaction試験は理由付きN/A。adapter/consumerを捏造して起動しない。
- **期待値/終了値:** native adapter/APIがないため理由付きN/A。PIXのcard-statusやCI smokeをnative transaction合格に読み替えない。
- **停止/cleanup:** consumerは起動しない。別の試験で起動したdaemonだけを通常停止し、system PCSC serviceを変更しない。
- **記録:** OS/ABI、module/DLL absolute path、consumer、reader/card、operation/exit。

### T11 LNB/非対応拒否

- **適用対象/頻度:** 必要時。給電対応profileだけ実測。
- **準備:** PIXはLNB給電非対応のためmeter/給電設備を接続しない。
- **実操作/利用者への依頼:** LNB controlなし。--lnaをLNBと解釈せず操作しない。
- **期待値/終了値:** PIXにLNB給電機能はないため、LNB電圧実測と拒否試験は理由付きN/A。--lnaをLNBと解釈しない。
- **停止/cleanup:** LNB操作なし。試験用daemonは通常停止する。
- **記録:** model/profile、command/exit、meter値/条件、cleanupまたはN/A根拠。

### T12 複数device/daemon/access

- **適用対象/頻度:** 必要時、複数device/selector/access変更時。
- **準備:** USB port/selectorとinstance/runtimeをdeviceごとに割り当てる。
- **実操作/利用者への依頼:** pixd --models/--list-jsonでmodel/device/portを区別。複数接続時のみ別runtimeで公開CLI capture。
- **期待値/終了値:** device/selector対応一意。別deviceの結果を混同せず、要求時は各sideを独立判定。
- **停止/cleanup:** 各client/daemon個別wait/stopしFD/IPC/socket確認。
- **記録:** device/port/PID/runtime、各exit/継続/再取得/access/cleanup。

### T13 soak/頻度

- **適用対象/頻度:** 毎releaseおよび全model初回/major。
- **準備:** 確定頻度表の対象archive/model/timeをmatrixへ展開する。
- **実操作/利用者への依頼:** 毎release代表Linux x86_64 glibc 2h/全archive native short; 初回全modelごと全無料channel 10s、macOS/Windows 30m、他ABI 10m。
- **期待値/終了値:** 指定連続時間完了かつTS条件合格。追加soakは明示ユーザー決定分のみ。
- **停止/cleanup:** T08通常stopと全resource cleanup。
- **記録:** model/OS/ABI/archive、時間、TS/counter、FD/RSS、未実施理由。

### T14 failure/release disposition

- **適用対象/頻度:** 全candidate gate完了後。
- **準備:** source/archive SHAと全T-IDの結果を版別記録へ用意。
- **実操作/利用者への依頼:** version recordへcandidate欠落/unsupported/nonzeroを未達として記録。formal release dispatchを候補試験に使わない。
- **期待値/終了値:** failure/unsupported/未実施/環境不能/candidate欠落をpassにしない。
- **停止/cleanup:** candidateと失敗証拠を保持。tag/releaseは別依頼。
- **記録:** T-ID、candidate/host/model、state、log、blocker、判定根拠。

N/Aは製品機能上該当しない項目だけに使う。OS/ABI候補のunsupported、candidate欠落、未実施、失敗、環境不能は版別結果で未達とし、passに読み替えない。手順に記した期待値は実測結果ではない。

## 1. 候補・配布物

対象versionの候補sourceから作成されたbinary archiveとsource archiveを取得し、hash・manifest・license/provenanceを照合します。pre-release候補が用意できない場合は実機gateを完了扱いにせず、その版別文書に未実施理由を記録します。正式Releaseを作成・公開するworkflowを候補作成の代用にしません。

公開後の監査を行う際は、GitHub Releaseから新規directoryへbinary/source archiveと`SHA256SUMS`を取得し、`sha256sum -c SHA256SUMS`、各archive内の`build-info.txt`が記録する固定Core SHA、source manifest/license/provenanceを照合します。これはpre-release candidate試験の代替にはなりません。

## 2. device-free gate

`.github/workflows/ci.yml` の `three-commands`、Windows runnerのpackage/runtime/EOF proofを含む候補commit上のCIを確認します。READMEのCMake proof、native/cross build、`tools/audit-binary.py`、source rebuild、license auditを実行し、出力・終了値を保存します。各候補archiveで `pixd --version`、`pixctl --version`、`pix-ts --version`、`pixd --models`、`pixd --list-json` を実行します。妥当なversion/model/JSONが返り、list-jsonが機器を所有しないことを確認します。DT295用旧standalone `--model` CLIを配布バイナリ試験に使いません。

## 3. native archiveと受信回帰

毎release、代表Linux x86_64機で2時間連続受信し、全配布archiveをnative OS/ABIで短時間回帰します。初回/major追加確認ではmacOS/Windowsは各30分、残るOS/ABIは各10分連続受信し、全無料channel各10秒は代表機だけで行います。CIのみで実機認定とはしません。

候補archive rootで作業します。USB位置とchannel値は現地の実値へ置き換えます。model dataの実現方法はその版の候補内容を記録し、外部assets pathを共通手順の必須入力にしません。

```sh
./pixd --models
./pixd --list-json
RT=$(mktemp -d "${TMPDIR:-/tmp}/pix-release.XXXXXX")
./pixd --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --group & P=$!
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --group status
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --group list
./pix-ts --instance rc --channel T27 --system isdb-t --time <SECONDS> --output t27.ts --runtime-dir "$RT" --group
```

`--models`と`--list-json`でcandidateのmodelとUSB位置を記録し、statusがfree/readyでreceiverが1基あることを確認します。最大同時受信数の1 streamを正常終了させ、client終了値、daemon packet/byte/USB counters、TS品質を確認します。DT181-PU0は利用可能な地上波・BS・CSから3波を選び、最大受信試験で未確認の波を短時間取得します。DT295は各archive/OS/ABIで同じcapture手順を実行し、unsupportedまたはnonzero終了は候補の未達/失敗として記録します。channel/TSID/slot/周波数は実値を記録します。出力非空だけで合格にせず、TS parser/独立検査と記録済みcounterで評価します。

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
./pixd --device usb:<BUS-PORT> --runtime-dir "$RT2" --instance rc --group & P2=$!
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT2" --instance rc --group status
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT2" --instance rc --group list
./pix-ts --device usb:<BUS-PORT> --instance rc --channel T27 --system isdb-t \
  --time <SECONDS> --output t27.ts --runtime-dir "$RT2" --group
```

再接続後は`pix-ts` exit 0に加えてTS sync/parser品質合格、daemon counters、通常daemon exit 0とprocess/FD/socket cleanupを記録します。AndroidはTermux USB許可/再取得、WindowsはREADMEのWinUSB構成を使います。BUSYなら別processを奪わず理由を記録します。

本製品はLNB制御を提供しません。`--lna` 等の機能をLNB給電と誤認せず、software/実電圧を試験しません。DT295は全OS/ABIで個別に試験し、unsupported/nonzero終了を候補の未達/失敗として記録します。DT181-PU0の実績をDT295へ転用しません。

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
