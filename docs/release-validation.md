# 安定版リリース前検証手順

この文書は `pixd` / `pixctl` / `pix-ts` のStable releaseで再利用する準備・操作・判定・cleanupの正本です。版固有matrixと検証記録は [`release-validation/1.0.0.md`](release-validation/1.0.0.md) のように分離します。CLIは [`README.md`](../README.md)、DT295固有制限は [`dt295.md`](dt295.md)、過去結果は [`validation.md`](validation.md) を参照します。

## 共通の検証項目と頻度

用語は4製品で共通とする。代表機はLinux x86_64 glibc、機種はチューナー、製品はドライバ、ソークは一定時間以上の連続したTS取得を指す。時間条件は下の適用頻度と版別matrixに従う。製品・機種ごとの能力、対応機能、実コマンドは続く製品固有節と手順節に記し、版別記録にはcandidateごとの実施結果を記す。

| 区分 | 適用頻度 | 共通の確認 |
|---|---|---|
| 機種初回 | 各製品の全対象機種で、機種ごとに代表機の2時間連続受信と毎release項目を実施する。必要時項目も通し、結果は機種別に初回記録へ残す。 | 当該機種の公開能力・対応機能に沿って実行し、別機種の結果で代替しない。 |
| 回帰（毎release） | 代表機で2時間連続受信する。全配布archiveをそれぞれ対応するnative OS/ABIで短時間実機回帰し、`--list-json`、機種の公開同時受信能力、TS/counter/終了値を確認する。最大同時受信で地上波・衛星の対象3波を確認できない場合は不足波を短時間取得する。受信中の状態と反復制御、正常停止、process/FD/IPC残留なし、対応機能のCARD挿抜、USB抜去/再挿入を確認する。native PC/SC/WinSCard経路がある製品・OSでは製品CLI経路と分けて確認する。 | 各archive/OS/ABIのrunと各機種の結果を分ける。未対応機能は理由付きでN/Aとし、未実施や失敗を合格に読み替えない。 |
| 必要に応じて | 対応機能ではLNB出力電圧を実測し、非対応機種の拒否を確認する。複数チューナーの列挙/daemon並列と、変更機能に応じた機種×OS/ABIの追加確認も行う。 | 適用する機種・機能・OS/ABIと実測値を記録する。 |
| 初回/major | 必要時項目を通し、代表機で全対象の無料channelを各10秒TS取得する。ソークはmacOS/Windows各30分、残るOS/ABI各10分とする。全channel取得は代表機だけで行い、別途指示されたソークは別に記録する。 | 30分/10分は各OS/ABIの個別時間であり、共通の一般定義である「一定時間以上の連続TS取得」を置き換えない。 |

全配布archiveのnative短時間回帰と代表機だけの全channel取得は別の確認である。候補の操作・期待値・cleanupは各archiveの具体手順に従い、実施状態・結果・未達は版別記録に残す。

## 製品固有の適用条件

PIXの通常の無指定時検証機種はPIX-DT295とPIX-DT181-PU0だが、機種初回の対象を限定しない。PIXが対象とする全機種で初回手順を個別に実施し、結果は[`release-validation/initial-model-checks.md`](release-validation/initial-model-checks.md)へ機種別に記録する。

| 区分 | 製品固有の確認 |
|---|---|
| 機種初回 | 機種ごとに異なるCLI/assets/OS対応を確認し、結果を機種別に記録する。 |
| 回帰（毎release） | DT181-PU0はT/BS/CS、DT295はISDB-T/ISDB-S（地上波・衛星）の試験対象として、両機種を全配布archive/OS/ABIごとに分けて記録する。DT295を含む各試験がunsupported/nonzeroでcaptureできない場合、その候補の未達/失敗として記録する。内部CARD状態は`pixctl card-status`で確認する。外部ATR/APDU・PC/SC/WinSCard・LNBは製品機能外。 |
| 必要に応じて | PIXはLNB給電なし。model enumerationやmulti-device変更時は該当機種で確認。 |
| 初回/major | DT295は機種としてOS/ABIごとに試験する。unsupported/nonzeroでcaptureできない候補は未達/失敗として記録する。DT181-PU0結果と分離する。 |

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
| 配布artifact / 手順 | 対象archive識別子とこの正本内の該当手順 | artifact hash、host、試験結果 | `artifact: <target>; procedure: [archive別手順](#3-配布アーカイブ別手順)` の順 |

共通IDは E02=Linux/x86_64/musl、E03=Linux/x86_64/glibc、E04=macOS/arm64、E05=Android/aarch64、E06=Android/armv7a、E07=Android/x86_64、E15-G=Linux/aarch64/glibc、E15-M=Linux/aarch64/musl、E17=Windows 11/x86_64を表す。IDはhost名や試験済み状態を表さない。PIXはLinux配布archiveがglibc/musl両環境で動作する静的musl binaryであり、別個のx86_64 musl配布targetを持たない。

| ID | OS / ABI | 依存 / driver / permission | firmware / 補助data | RF試験入力（system / channel） | 製品CLIからの内蔵CARD操作 | native PC/SC/WinSCard adapter | 配布artifact / 手順 |
|---|---|---|---|---|---|---|---|
| E02 | Linux / x86_64 / musl | libusb: 静的1.0.30; USB access: native musl runtimeの許可済み経路 | firmware: なし; 補助data: DT181/DT295の機種固有model data | system: DT181-PU0は対応T/BS/CS、DT295はISDB-T/ISDB-S; channel: 現地channel plan（DT295例 T27/BS1） | 内部CARD statusのみ; model依存 | Linux PC/SC: なし | artifact: `linux-x86_64`; procedure: [archive別手順](#3-配布アーカイブ別手順) |
| E03 | Linux / x86_64 / glibc | libusb: 静的1.0.30; USB access: native OSの許可済み経路 | firmware: なし; 補助data: DT181/DT295の機種固有model data | system: DT181-PU0は対応T/BS/CS、DT295はISDB-T/ISDB-S; channel: 現地channel plan（DT295例 T27/BS1） | 内部CARD statusのみ; model依存 | Linux PC/SC: なし | artifact: `linux-x86_64`; procedure: [archive別手順](#3-配布アーカイブ別手順) |
| E15-G | Linux / aarch64 / glibc | libusb: 静的1.0.30; USB access: native OSの許可済み経路 | firmware: なし; 補助data: DT181/DT295の機種固有model data | system: DT181-PU0は対応T/BS/CS、DT295はISDB-T/ISDB-S; channel: 現地channel plan（DT295例 T27/BS1） | 内部CARD statusのみ; model依存 | Linux PC/SC: なし | artifact: `linux-aarch64`; procedure: [archive別手順](#3-配布アーカイブ別手順) |
| E15-M | Linux / aarch64 / musl | libusb: 静的1.0.30; USB access: native OSの許可済み経路 | firmware: なし; 補助data: DT181/DT295の機種固有model data | system: DT181-PU0は対応T/BS/CS、DT295はISDB-T/ISDB-S; channel: 現地channel plan（DT295例 T27/BS1） | 内部CARD statusのみ; model依存 | Linux PC/SC: なし | artifact: `linux-aarch64`; procedure: [archive別手順](#3-配布アーカイブ別手順) |
| E04 | macOS / arm64 / Apple system runtime | libusb: 静的1.0.30; USB access: macOSの許可済み経路 | firmware: なし; 補助data: DT181/DT295の機種固有model data | system: DT181-PU0は対応T/BS/CS、DT295はISDB-T/ISDB-S; channel: 現地channel plan（DT295例 T27/BS1） | 内部CARD statusのみ; model依存 | macOS PC/SC: なし | artifact: `darwin-arm64`; procedure: [archive別手順](#3-配布アーカイブ別手順) |
| E05 | Android (Termux) / aarch64 / Android NDK ABI | libusb: 静的1.0.30; USB access: Termux USB permission; handoff: platform procedure | firmware: なし; 補助data: DT181/DT295の機種固有model data | system: DT181-PU0は対応T/BS/CS、DT295はISDB-T/ISDB-S; channel: 現地channel plan（DT295例 T27/BS1） | 内部CARD statusのみ; model依存 | Android: なし | artifact: `android-aarch64`; procedure: [archive別手順](#3-配布アーカイブ別手順) |
| E06 | Android (Termux) / armv7a / Android NDK ABI | libusb: 静的1.0.30; USB access: Termux USB permission; handoff: platform procedure | firmware: なし; 補助data: DT181/DT295の機種固有model data | system: DT181-PU0は対応T/BS/CS、DT295はISDB-T/ISDB-S; channel: 現地channel plan（DT295例 T27/BS1） | 内部CARD statusのみ; model依存 | Android: なし | artifact: `android-armv7a`; procedure: [archive別手順](#3-配布アーカイブ別手順) |
| E07 | Android (Termux) / x86_64 / Android NDK ABI | libusb: 静的1.0.30; USB access: Termux USB permission; handoff: platform procedure | firmware: なし; 補助data: DT181/DT295の機種固有model data | system: DT181-PU0は対応T/BS/CS、DT295はISDB-T/ISDB-S; channel: 現地channel plan（DT295例 T27/BS1） | 内部CARD statusのみ; model依存 | Android: なし | artifact: `android-x86_64`; procedure: [archive別手順](#3-配布アーカイブ別手順) |
| E17 | Windows 11 / x86_64 / Win32 | libusb: 静的1.0.30; USB access: WinUSB driver | firmware: なし; 補助data: DT181/DT295の機種固有model data | system: DT181-PU0は対応T/BS/CS、DT295はISDB-T/ISDB-S; channel: 現地channel plan（DT295例 T27/BS1） | 内部CARD statusのみ; model依存 | Windows WinSCard: なし | artifact: `windows-x64`; procedure: [archive別手順](#3-配布アーカイブ別手順) |

archive対象と試験前提を記す表であり、実装状態・候補の有無・特定版の実施結果は版別文書へ記録する。PIXの内部CARD操作はstatus照会に限り、native PC/SC/WinSCard adapterとは別の列で扱う。DT295は製品対象として各OS/ABIの試験に含める。assetsのarchiveへの埋込状態はここで判定しない。

## 端末nodeと環境状態の退避・復元（T01前）

Termuxの`termux-usb -l`はUSB node path文字列のJSON配列を返す（例: `["/dev/bus/usb/001/004"]`）。配列要素をVID/PID辞書として扱わない。次をbootstrap時と再接続後に毎回実行し、選択結果・一覧をLOGに残す。製品/機種ごとに`TARGET_USB`と`EXPECTED_NODES`を表のUSB IDと当該stage（loader/runtime、単機能/primary+sibling）から設定する。ASICENはstage遷移ごとに期待ID/数を切り替える。0件、JSON型不正、対象一致数の不一致、sysfsで対象pathを対応付けできない場合はpermissionを要求せず停止する。無関係nodeは一覧に記録して対象選択から除外する。

```sh
: "${TARGET_USB:?set model/stage VID:PID, e.g. 3275:0080}"
: "${EXPECTED_NODES:?set exact node count from model/stage}"
case "$TARGET_USB" in [0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]) ;; *) echo "invalid VID:PID" >&2; exit 2;; esac
case "$EXPECTED_NODES" in ''|*[!0-9]*|0) echo "invalid expected node count" >&2; exit 2;; esac
termux-usb -l > "$LOG/termux-usb-paths.json"
SYSFS_ROOT=${SYSFS_ROOT:-/sys/bus/usb/devices}
python - "$LOG/termux-usb-paths.json" "$TARGET_USB" "$EXPECTED_NODES" "$LOG/termux-usb-map.json" "$SYSFS_ROOT" <<'PY'
import glob, json, os, re, sys
src, target, expected, out, sysroot = sys.argv[1:]
paths = json.load(open(src, encoding="utf-8"))
if not isinstance(paths, list) or any(not isinstance(p, str) for p in paths):
    raise SystemExit("termux-usb -l must be a JSON array of USB path strings")
matches, rows = [], []
for path in paths:
    m = re.fullmatch(r"/dev/bus/usb/(\d{3})/(\d{3})", path)
    row = {"path": path, "vid_pid": None, "product": None}
    if m:
        bus, dev = (str(int(v)) for v in m.groups())
        for node in glob.glob(sysroot + "/*"):
            try:
                if open(node + "/busnum").read().strip() == bus and open(node + "/devnum").read().strip() == dev:
                    vid = open(node + "/idVendor").read().strip().lower()
                    pid = open(node + "/idProduct").read().strip().lower()
                    row["vid_pid"] = vid + ":" + pid
                    product = node + "/product"
                    row["product"] = open(product).read().strip() if os.path.exists(product) else ""
                    break
            except OSError:
                continue
    rows.append(row)
    if row["vid_pid"] == target.lower():
        matches.append(path)
json.dump({"api_paths": paths, "mapped": rows, "target": target.lower(), "selected": matches}, open(out, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
if len(matches) != int(expected):
    raise SystemExit(f"expected {expected} {target} node(s), found {len(matches)}; see {out}")
PY
cat "$LOG/termux-usb-map.json"
python3 -c 'import json,sys; [print(p) for p in json.load(open(sys.argv[1]))["selected"]]' "$LOG/termux-usb-map.json" > "$LOG/termux-usb-selected-paths.txt"
```

PIXはsingle-device runで`TARGET_USB=06b8:102b`（DT181-PU0）または`06b8:103b`（DT295）、`EXPECTED_NODES=1`とする。T12の2-device runは同じ対象VID:PIDを選び`EXPECTED_NODES=2`にする。

結果が期待数と完全一致した場合だけ、JSONの`selected`に記録された各pathへ`termux-usb -r <path>`（または製品固有の`-r -e`）を実行する。USB permission後にdaemonを開始する。再接続時は古いpath/FDを使わず、同じ列挙・sysfs照合を再実行する。sysfs未読・node未対応・無許可候補複数は合格ではなく未達として記録する。

環境依存状態を変える前に、承認済みの試験用account/add-on、保護されたrun専用LOG、復元可能な退避を用意する。次は手順を記載したものであり、今回の作業では実行しない。

- **E02 HAOS/Supervisor options:** `/addons/self/info`の`options`は対象addon自身のapp recordに保存されたraw optionsで、`/options/config`のdefaults・secret解決後の値とは異なる。Supervisor APIの実装は[apps.py](https://github.com/home-assistant/supervisor/blob/main/supervisor/api/apps.py#L1620-L1700)および[options endpoints](https://github.com/home-assistant/supervisor/blob/main/supervisor/api/apps.py#L1832-L2015)を参照。対象addon内で認可済みtokenを使い、raw表現はmode 600のprivate backupにだけ保存する。値を画面・共有log・evidenceへ出さない。validateの`valid`がtrue以外、または読み出し/HTTP/JSON処理が失敗した場合はoptions POST前に終了する。Supervisorは本試験で操作しない。
  ```sh
  set -eu
  umask 077
  mkdir -m 700 -p "$LOG/private"
  : "${SUPERVISOR_TOKEN:?run inside the target add-on with its token}"
  API=http://supervisor/addons/self
  curl -fsS -H "Authorization: Bearer $SUPERVISOR_TOKEN" "$API/info" -o "$LOG/private/info.before.json"
  python3 - "$LOG/private/info.before.json" "$LOG/private/options.before.json" <<'PY'
  import json, sys
  x=json.load(open(sys.argv[1], encoding="utf-8")); x=x.get("data", x)
  options=x.get("options")
  if not isinstance(options, dict): raise SystemExit("raw app options unavailable")
  json.dump(options, open(sys.argv[2], "w", encoding="utf-8"), sort_keys=True)
  PY
  chmod 600 "$LOG/private/"*.json
  cp "$LOG/private/options.before.json" "$LOG/private/options.restore.json"
  cp "$LOG/private/options.before.json" "$LOG/private/options.trial.json"
  : "${TRIAL_OPTION_KEY:?set one approved non-secret top-level option key}"
  : "${TRIAL_OPTION_JSON:?set its approved JSON value}"
  python3 - "$LOG/private/options.trial.json" "$TRIAL_OPTION_KEY" "$TRIAL_OPTION_JSON" <<'PY'
  import json, sys
  p, key, value=sys.argv[1:]
  options=json.load(open(p, encoding="utf-8"))
  if key not in options: raise SystemExit("trial key is not an existing option")
  options[key]=json.loads(value)
  json.dump(options, open(p, "w", encoding="utf-8"), sort_keys=True)
  PY
  python3 -m json.tool "$LOG/private/options.trial.json" > /dev/null
  python3 - "$LOG/private/options.trial.json" "$LOG/private/options-trial.request.json" <<'PY'
  import json, sys
  json.dump({"options":json.load(open(sys.argv[1], encoding="utf-8"))}, open(sys.argv[2], "w", encoding="utf-8"))
  PY
  # /options/validate accepts the raw options object; only /options uses the {"options": ...} request schema.
  cp "$LOG/private/options.trial.json" "$LOG/private/options-trial.validate.json"
  chmod 600 "$LOG/private/"*.json
  curl -fsS -H "Authorization: Bearer $SUPERVISOR_TOKEN" -H 'Content-Type: application/json' --data-binary @"$LOG/private/options-trial.validate.json" "$API/options/validate" -o "$LOG/private/options-trial.validation-response.json"
  if ! python3 -c 'import json,sys; x=json.load(open(sys.argv[1])); x=x.get("data",x); sys.exit(0 if x.get("valid") is True else 1)' "$LOG/private/options-trial.validation-response.json"; then echo 'options validation rejected; no trial POST was sent' >&2; exit 1; fi
  curl -fsS -H "Authorization: Bearer $SUPERVISOR_TOKEN" -H 'Content-Type: application/json' --data-binary @"$LOG/private/options-trial.request.json" "$API/options" -o "$LOG/private/options-trial.post.json"
  ```
  run終了後、保存したraw optionsを同じ`/options` endpointへ戻し、`/info`のraw `options`とprivate backupの完全一致を確認する。`/options/config`のvalidated値との比較で復元を判定しない。
  ```sh
  set -eu
  python3 - "$LOG/private/options.restore.json" "$LOG/private/options-restore.request.json" <<'PY'
  import json, sys
  json.dump({"options":json.load(open(sys.argv[1], encoding="utf-8"))}, open(sys.argv[2], "w", encoding="utf-8"))
  PY
  curl -fsS -H "Authorization: Bearer $SUPERVISOR_TOKEN" -H 'Content-Type: application/json' --data-binary @"$LOG/private/options-restore.request.json" "$API/options" -o "$LOG/private/options-restore.post.json"
  curl -fsS -H "Authorization: Bearer $SUPERVISOR_TOKEN" "$API/info" -o "$LOG/private/info.after.json"
  python3 - "$LOG/private/info.after.json" "$LOG/private/options.after.json" <<'PY'
  import json, sys
  x=json.load(open(sys.argv[1], encoding="utf-8")); x=x.get("data", x)
  json.dump(x.get("options"), open(sys.argv[2], "w", encoding="utf-8"), sort_keys=True)
  PY
  cmp "$LOG/private/options.restore.json" "$LOG/private/options.after.json"
  ```


optionsを適用した後と元値へ戻した後に、それぞれ別の認可済みHAOS CLI sessionから対象add-onだけを再起動する。HA Core/mirakcは再起動しない。

```sh
ADDON_SLUG='<target-addon-slug>'
ha addons restart "$ADDON_SLUG"
```

- **E03 Linux module/udev/group:**対象udev rule fileの元内容/不在状態、対象userのsupplementary groups、対象USB nodeのmode/UID/GIDを保存する。`DEVICE_NODE`と`TARGET_USB`は同一候補機器の実値に限定し、sysfs pathとVID/PIDが一致しない場合は何も適用せず停止する。rule再読込時のtriggerはそのsysfs device pathだけへ指定する。変更は別途承認された試験でのみ実施する。
  ```sh
  set -eu
  TEST_USER='<non-root-test-user>'; LOG='<private-run-log>'
  DEVICE_NODE='/dev/bus/usb/BBB/AAA'; TARGET_USB='<selected-vid>:<selected-pid>'
  for f in /etc/modprobe.d/smsusb-blacklist.conf /etc/udev/rules.d/99-usb-access.rules; do
    n=$(basename "$f")
    if [ -e "$f" ]; then cp -a "$f" "$LOG/$n.before"; printf 'present\n' > "$LOG/$n.state"; else printf 'absent\n' > "$LOG/$n.state"; fi
  done
  id "$TEST_USER" > "$LOG/test-user.before.txt"; PRIMARY_GROUP=$(id -gn "$TEST_USER")
  id -nG "$TEST_USER" | python3 -c 'import sys; p=sys.argv[1]; print(",".join(g for g in sys.stdin.read().split() if g != p))' "$PRIMARY_GROUP" > "$LOG/groups.before.txt"
  test -c "$DEVICE_NODE"
  SYSFS_PATH=$(udevadm info --query=path --name="$DEVICE_NODE")
  SYSFS_DEVICE="/sys$SYSFS_PATH"
  test -f "$SYSFS_DEVICE/idVendor" -a -f "$SYSFS_DEVICE/idProduct"
  ACTUAL_USB="$(cat "$SYSFS_DEVICE/idVendor"):$(cat "$SYSFS_DEVICE/idProduct")"
  test "$(printf '%s' "$ACTUAL_USB" | tr '[:upper:]' '[:lower:]')" = "$(printf '%s' "$TARGET_USB" | tr '[:upper:]' '[:lower:]')"
  stat -c '%a %u %g' "$DEVICE_NODE" > "$LOG/usb-node.before.txt"
  printf '%s\n' "$SYSFS_PATH" > "$LOG/usb-sysfs-path.before.txt"
  # 承認済みrunでのみ適用する。適用は隔離subshell内で行い、途中失敗でも親shellの復元へ戻る:
  apply_trial_access() (
    set -eu
    : "${BLACKLIST_MODULES:?set only the modules required by the selected product/model}"
    : > /etc/modprobe.d/smsusb-blacklist.conf || exit $?
    for module in $BLACKLIST_MODULES; do printf 'blacklist %s\n' "$module" >> /etc/modprobe.d/smsusb-blacklist.conf || exit $?; done
    VID=${TARGET_USB%:*}; PID=${TARGET_USB#*:}
    printf 'SUBSYSTEM=="usb", ATTR{idVendor}=="%s", ATTR{idProduct}=="%s", MODE="0660", GROUP="video"\n' "$VID" "$PID" > /etc/udev/rules.d/99-usb-access.rules || exit $?
    udevadm control --reload-rules || exit $?
    udevadm trigger --action=change "$SYSFS_DEVICE" || exit $?
    udevadm settle || exit $?
    usermod -aG video "$TEST_USER" || exit $?
  )
  if apply_trial_access; then APPLY_RC=0; else APPLY_RC=$?; fi
  printf '%s\n' "$APPLY_RC" > "$LOG/access-apply.rc"
  RESTORE_RC=0
  set +e
  for f in /etc/modprobe.d/smsusb-blacklist.conf /etc/udev/rules.d/99-usb-access.rules; do
    n=$(basename "$f")
    if [ "$(cat "$LOG/$n.state")" = present ]; then
      cp -a "$LOG/$n.before" "$f" || RESTORE_RC=1
      cmp "$LOG/$n.before" "$f" || RESTORE_RC=1
    else
      rm -f "$f" || RESTORE_RC=1
      test ! -e "$f" || RESTORE_RC=1
    fi
  done
  udevadm control --reload-rules || RESTORE_RC=1
  if test -c "$DEVICE_NODE" && test "$(udevadm info --query=path --name="$DEVICE_NODE")" = "$SYSFS_PATH" && test "$(cat "$SYSFS_DEVICE/idVendor"):$(cat "$SYSFS_DEVICE/idProduct")" = "$ACTUAL_USB"; then :; else RESTORE_RC=1; fi
  udevadm trigger --action=change "$SYSFS_DEVICE" || RESTORE_RC=1
  udevadm settle || RESTORE_RC=1
  if set -- $(cat "$LOG/usb-node.before.txt") && test "$#" -eq 3; then
    chown "$2:$3" "$DEVICE_NODE" || RESTORE_RC=1
    chmod "$1" "$DEVICE_NODE" || RESTORE_RC=1
  else RESTORE_RC=1; fi
  if stat -c '%a %u %g' "$DEVICE_NODE" > "$LOG/usb-node.after.txt"; then cmp "$LOG/usb-node.before.txt" "$LOG/usb-node.after.txt" || RESTORE_RC=1; else RESTORE_RC=1; fi
  if GROUPS_BEFORE=$(cat "$LOG/groups.before.txt"); then
    if [ -n "$GROUPS_BEFORE" ]; then usermod -G "$GROUPS_BEFORE" "$TEST_USER" || RESTORE_RC=1; else usermod -G '' "$TEST_USER" || RESTORE_RC=1; fi
    id "$TEST_USER" > "$LOG/test-user.after.txt" || RESTORE_RC=1
    if PRIMARY_AFTER=$(id -gn "$TEST_USER") && id -nG "$TEST_USER" | python3 -c 'import sys; p=sys.argv[1]; print(",".join(g for g in sys.stdin.read().split() if g != p))' "$PRIMARY_AFTER" > "$LOG/groups.after.txt"; then cmp "$LOG/groups.before.txt" "$LOG/groups.after.txt" || RESTORE_RC=1; else RESTORE_RC=1; fi
  else RESTORE_RC=1; fi
  printf '%s\n' "$RESTORE_RC" > "$LOG/access-restore.rc"
  set -e
  if [ "$APPLY_RC" -ne 0 ]; then exit "$APPLY_RC"; fi
  if [ "$RESTORE_RC" -ne 0 ]; then echo 'access state restoration incomplete; retain evidence and stop' >&2; exit "$RESTORE_RC"; fi
  ```

- **E07 Android stay-awake:**初回ADB readが非0/空/不正値なら書込み前に停止する。`null`は未設定、十進整数は保存値として区別する。権限・readback失敗を無視しない。次は端末設定に触れないsynthetic確認用の状態遷移であり、実Androidへは実行しない。
  ```sh
  set -eu
  LOG='<private-run-log>'
  if ! BEFORE=$(adb shell settings get global stay_on_while_plugged_in); then echo 'initial Android settings read failed; no write attempted' >&2; exit 1; fi
  BEFORE=$(printf '%s' "$BEFORE" | tr -d '\r')
  case "$BEFORE" in null) printf 'absent\n' > "$LOG/stay-awake.before-state.txt";; ''|*[!0-9]*) echo 'initial Android settings value is invalid; no write attempted' >&2; exit 1;; *) printf 'present\n' > "$LOG/stay-awake.before-state.txt";; esac
  printf '%s\n' "$BEFORE" > "$LOG/stay-awake.before.txt"
  : "${TRIAL_VALUE:?set an approved decimal test value}"
  case "$TRIAL_VALUE" in ''|*[!0-9]*) echo 'trial value must be decimal' >&2; exit 2;; esac
  if adb shell settings put global stay_on_while_plugged_in "$TRIAL_VALUE"; then TRIAL_RC=0; else TRIAL_RC=$?; fi
  if [ "$BEFORE" = null ]; then adb shell settings delete global stay_on_while_plugged_in; else adb shell settings put global stay_on_while_plugged_in "$BEFORE"; fi
  if ! AFTER=$(adb shell settings get global stay_on_while_plugged_in); then echo 'Android settings readback failed' >&2; exit 1; fi
  AFTER=$(printf '%s' "$AFTER" | tr -d '\r')
  case "$AFTER" in null) printf 'absent\n' > "$LOG/stay-awake.after-state.txt";; ''|*[!0-9]*) echo 'Android settings readback is invalid' >&2; exit 1;; *) printf 'present\n' > "$LOG/stay-awake.after-state.txt";; esac
  printf '%s\n' "$AFTER" > "$LOG/stay-awake.after.txt"
  cmp "$LOG/stay-awake.before-state.txt" "$LOG/stay-awake.after-state.txt"
  cmp "$LOG/stay-awake.before.txt" "$LOG/stay-awake.after.txt"
  test "$TRIAL_RC" -eq 0
  ```

## 共通試験シナリオ T00–T14

### run別candidateの取得と作業directory

この手順書commitとcandidate source SHAは一致不要。候補は既存`release.yml` runのtarget artifactから取得し、runのhead SHA、attempt、結論、artifact ID/name/expiryと各jobの結論を確認する。`release` jobが成功して公開済みのrunは候補取得に使わない。新規workflow dispatchや正式Releaseは実行しない。候補jobがない/expiredなら未実施で止める。

```sh
set -eu
gh run list --repo Khronos31/pix-userland --workflow release.yml --limit 20
RUN_ID='<successful-candidate-run-id>'; TARGET='<linux-x86_64|linux-aarch64|darwin-arm64|android-aarch64|android-armv7a|android-x86_64|windows-x64>'
RUN_JSON=$(gh run view "$RUN_ID" --repo Khronos31/pix-userland --json headSha,attempt,conclusion)
SOURCE_SHA=$(printf '%s\n' "$RUN_JSON" | python3 -c 'import json,sys; print(json.load(sys.stdin)["headSha"])')
printf '%s\n' "$RUN_JSON" | python3 -c 'import json,sys; r=json.load(sys.stdin); assert r["conclusion"] == "success" and r["headSha"] and r["attempt"] > 0'
test -n "$SOURCE_SHA"
gh run view "$RUN_ID" --repo Khronos31/pix-userland --json jobs --jq '.jobs[] | [.name,.conclusion] | @tsv'
RELEASE_JOB=$(gh run view "$RUN_ID" --repo Khronos31/pix-userland --json jobs --jq '.jobs[] | select(.name == "release") | .conclusion')
test "$RELEASE_JOB" != success
gh api "repos/Khronos31/pix-userland/actions/runs/$RUN_ID/artifacts" --jq '.artifacts[] | [.id,.name,.expired] | @tsv'
WORK=$(mktemp -d "${TMPDIR:-$HOME}/pix-run-${RUN_ID}.XXXXXX"); chmod 700 "$WORK"
ARTIFACT_ID=$(gh api "repos/Khronos31/pix-userland/actions/runs/$RUN_ID/artifacts" --jq ".artifacts[] | select(.name == \"$TARGET\" and .expired == false) | .id")
test -n "$ARTIFACT_ID"; DIGEST=$(gh api "repos/Khronos31/pix-userland/actions/runs/$RUN_ID/artifacts" --jq ".artifacts[] | select(.id == $ARTIFACT_ID) | .digest"); EXPECTED=${DIGEST#sha256:}
gh api "repos/Khronos31/pix-userland/actions/artifacts/$ARTIFACT_ID/zip" > "$WORK/artifact.zip"
if command -v sha256sum >/dev/null; then ACTUAL=$(sha256sum "$WORK/artifact.zip" | awk '{print $1}'); else ACTUAL=$(shasum -a 256 "$WORK/artifact.zip" | awk '{print $1}'); fi
test "$ACTUAL" = "$EXPECTED"; mkdir "$WORK/download"; unzip -q "$WORK/artifact.zip" -d "$WORK/download"
ARCHIVE=$(find "$WORK/download" -maxdepth 1 -type f -name 'pix-userland-*.tar.gz' -print -quit)
test -n "$ARCHIVE"; if command -v sha256sum >/dev/null; then sha256sum "$ARCHIVE"; else shasum -a 256 "$ARCHIVE"; fi
mkdir "$WORK/candidate"; tar -xzf "$ARCHIVE" -C "$WORK/candidate"
D=$(find "$WORK/candidate" -mindepth 1 -maxdepth 1 -type d -name 'pix-userland-*' -print -quit)
test -n "$D"; LOG="$WORK/log"; RT="$WORK/runtime"; mkdir -m 700 "$LOG" "$RT"; cd "$D"; export PATH="$D:$PATH"
python3 -c 'import json,sys; m=json.load(open("release-info.json")); print(json.dumps(m,sort_keys=True)); assert m.get("commit") == sys.argv[1]' "$SOURCE_SHA"
```

Windows uses a new empty `$D`, `Expand-Archive`, and `Get-FileHash -Algorithm SHA256`. The target artifact digest from GitHub is checked before extraction; record the inner archive SHA-256 and `release-info.json.commit` as separate provenance evidence. This workflow artifact does not provide an inner `SHA256SUMS`. Windows helper commands use Python 3.11+ (`python --version`). Termux uses `$PREFIX/tmp` or `$HOME`, not OS `/tmp`; install/check `gh`, Python 3.11+, `tar`, `unzip`, `coreutils` using `pkg install gh python tar unzip coreutils` only after host approval. `termux-usb -l` output is recorded, and `termux-usb -r <path>` is limited to a path identified as the target tuner. Linux access is observed with `id`, `stat`, and `lsusb -t`; permissions/udev changes require separate approval, saved original state, and restoration. Do not change HAOS/Supervisor settings.

Windows PowerShellの実行例:

```powershell
$ErrorActionPreference = 'Stop'
$RunId = '<successful-candidate-run-id>'; $Target = 'windows-x64'
$Work = Join-Path $HOME "pix-run-$RunId"; $Download = Join-Path $Work 'download'; $D = Join-Path $Work 'candidate'; $LOG = Join-Path $Work 'log'; $RT = Join-Path $Work 'runtime'
if (Test-Path $Work) { throw "refusing to reuse $Work" }
New-Item -ItemType Directory -Path $Download,$D,$LOG,$RT | Out-Null
$run = gh run view $RunId --repo Khronos31/pix-userland --json headSha,attempt,conclusion | ConvertFrom-Json
if ($run.conclusion -ne 'success') { throw 'candidate run is not successful' }
$CandidateSourceSha = $run.headSha
gh run view $RunId --repo Khronos31/pix-userland --json jobs --jq '.jobs[] | [.name,.conclusion] | @tsv'
$releaseJob = gh run view $RunId --repo Khronos31/pix-userland --json jobs --jq '.jobs[] | select(.name == "release") | .conclusion'
if ($releaseJob -eq 'success') { throw 'published Release run cannot be used as a candidate' }
gh api "repos/Khronos31/pix-userland/actions/runs/$RunId/artifacts" --jq '.artifacts[] | [.id,.name,.expired] | @tsv'
$artifact = (gh api "repos/Khronos31/pix-userland/actions/runs/$RunId/artifacts" --jq ".artifacts[] | select(.name == \"$Target\" and .expired == false) | [.id,.digest] | @tsv") -split "`t"
if ($artifact.Count -ne 2 -or -not $artifact[0] -or -not $artifact[1]) { throw 'candidate artifact missing, expired, or ambiguous' }
$expected = $artifact[1] -replace '^sha256:', ''
$ArtifactZip = Join-Path $Download 'artifact.zip'
$token = gh auth token
try {
  $headers = @{ Authorization = "Bearer $token"; Accept = 'application/vnd.github+json' }
  Invoke-WebRequest -Uri "https://api.github.com/repos/Khronos31/pix-userland/actions/artifacts/$($artifact[0])/zip" -Headers $headers -OutFile $ArtifactZip
} finally { Remove-Variable token,headers -ErrorAction SilentlyContinue }
$actual = (Get-FileHash -Algorithm SHA256 $ArtifactZip).Hash.ToLowerInvariant()
if ($actual -ne $expected) { throw 'GitHub candidate artifact digest mismatch' }
Expand-Archive -LiteralPath $ArtifactZip -DestinationPath $Download
$ARCHIVE = Get-ChildItem $Download -Filter 'pix-userland-*.tar.gz' | Select-Object -First 1
if (-not $ARCHIVE) { throw 'candidate archive missing' }
Get-FileHash -Algorithm SHA256 $ARCHIVE.FullName
tar -xzf $ARCHIVE.FullName -C $D
if ($LASTEXITCODE -ne 0) { throw 'candidate archive extraction failed' }
$D = Get-ChildItem $D -Directory | Select-Object -First 1
if (-not $D) { throw 'candidate payload directory missing' }; $D = $D.FullName
$info = Get-Content (Join-Path $D 'release-info.json') -Raw | ConvertFrom-Json
if ($info.commit -ne $CandidateSourceSha) { throw 'candidate source commit mismatch' }
$ArchiveSha256 = (Get-FileHash -Algorithm SHA256 $ARCHIVE.FullName).Hash.ToLowerInvariant()
[ordered]@{ run_id=$RunId; source_sha=$CandidateSourceSha; attempt=[int]$run.attempt; run_conclusion=$run.conclusion; release_job_conclusion=$releaseJob; target=$Target; artifact_id=$artifact[0]; artifact_digest=$expected; archive_sha256=$ArchiveSha256 } | ConvertTo-Json | Set-Content (Join-Path $LOG 'candidate-lock.json')
Set-Location $D
```

各IDを同じ順序・同じ6項目で実施する。$RT/$LOG/$BUS-PORT/$CHANNEL_PLANはrunごとの実値に置換し、空のcandidate専用runtime/logを使う。物理操作を依頼する各回は既定通知を行い、最大5分で応答がなければ未完了として停止する。OS/API固有の長い操作は同じT-IDから後続recipeを参照する。

### T00 候補/配布物固定

- **適用対象/頻度:** 毎releaseのcandidate作成時。
- **準備:** source SHA、CI run/attempt、archive targetとhashを固定。POSIXではgit rev-parse HEADを保存し、macOSはshasum -a 256、Linuxはsha256sum、WindowsはGet-FileHashでhashを確認する。
- **実操作/利用者への依頼:** 固定candidateを取得し、macOSでは`shasum -a 256 -c SHA256SUMS`、Linuxでは`sha256sum -c SHA256SUMS`、Windowsでは`Get-FileHash -Algorithm SHA256`でarchive hashを照合する。対象archive個別のdigestもmacOSでは`shasum -a 256 "$ARCHIVE"`、Linuxでは`sha256sum "$ARCHIVE"`で表示し、manifest entryと一致することを確認する。pixd --models/--list-jsonとmanifest/source refを確認する。candidateが無ければ停止し正式公開workflowを代用しない。
- **期待値/終了値:** provenance/hash一致の候補だけ次へ進む。候補不在は未完了。
- **停止/cleanup:** deviceを開かず、archive/logを保持。
- **記録:** source SHA、CI run/attempt、artifact ID/target/hash、取得日時、未取得理由。

### T01 実行環境・アクセス権・入力

- **適用対象/頻度:** 全native archive。初回は全modelごと。
- **準備:** E-ID/OS/ABI、USB access、firmware/model data、channel planを環境表と照合。
- **実操作/利用者への依頼:** OS/ABI native commandsを記録しpixd --modelsでmodelを確認する。model dataとRF試験channelを分け、archiveでのmodel dataの実現方法は版別記録に記す。外部assets pathを正本の恒久的な実行前提にしない。
- **期待値/終了値:** OS/ABIがarchiveに一致し、アクセス許可/入力が揃う。不一致はdevice操作前に止める。
- **停止/cleanup:** 権限不足やdriver変更が必要なら操作せず未実施記録。
- **記録:** E-ID/host/OS/ABI/model、USB topology/permission、firmware/data、channel plan。

### T02 列挙・selector・ready確認

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

### T04 不足波・全無料channel

- **適用対象/頻度:** 毎releaseは不足波。初回/majorは全機種の代表機で全無料channel各10秒。
- **準備:** 現地channel planとmodel能力から対象system/channel一覧を作る。
- **実操作/利用者への依頼:** DT181-PU0は対応するT/BS/CS、DT295はISDB-T/ISDB-Sを対象にする。`pix-ts --channel <CHANNEL> --system <SYSTEM> --time 10 --output <FILE> --runtime-dir "$RT" --group`を各channelで実行する。DT295のISDB-Sも各OS/ABIで省略せず実行し、unsupported/nonzeroは当該候補の未達/失敗として記録する。
- **期待値/終了値:** 各対象channelのTS/exit合格。未取得は未完了、非対応波だけ根拠付きN/A。
- **停止/cleanup:** client wait/exit保存、daemon通常停止。
- **記録:** model/system/channel/全channel list、TS解析/counter/exit、未取得理由。

### T05 受信中の状態確認・制御

- **適用対象/頻度:** 毎release。CARD/controlは製品能力に従う。
- **準備:** capture clientを起動し、別terminalでstatus/list/controlを行えるようにする。
- **実操作/利用者への依頼:** stream中に別terminalからpixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --group statusとlistを実行する。capture停止後、同じruntime/selectorでpixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --group card-statusを実行する。
- **期待値/終了値:** 照会/control時刻がstreamと対応し各response/exitを保存。機能外操作はN/A。
- **停止/cleanup:** client/daemonを通常停止し残留確認。
- **記録:** 照会/control時刻、response class、各exit、streamとの重なり。

### T06 CARD物理挿抜・復帰

- **適用対象/頻度:** CARD reader/status対応model。毎release。
- **準備:** capture停止・daemon維持。初期CARD状態とreader有無を確認。
- **実操作/利用者への依頼:** pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --group card-statusで初期状態を記録する。通知後に利用者へ抜去を依頼し、完了後に同コマンドを実行する。再度通知して同じカードの再挿入を依頼し、同コマンドで復帰状態を記録する。各依頼は最大5分待つ。ATR/reset/APDUは機能外。
- **期待値/終了値:** card-statusは初期状態でpresent、抜去後absent、再挿入後presentを返し各照会はexit 0。PIXはATR/reset/APDU操作を提供しない。応答なしは未完了。
- **停止/cleanup:** daemon通常停止、FD/IPC/runtimeを確認。
- **記録:** 依頼時刻、状態/response/exit、復帰、cleanup。

### T07 USB待機中・受信中の切断と再取得

- **適用対象/頻度:** 毎release。USB対応deviceのidle/streamを別run。
- **準備:** `pixd --list-json`でUSB pathを特定し、候補archiveとmodel固有dataでdaemonを起動する。`pixd`は`Family::create`成功後にIPC serviceを開始する。DT295はassets読込後に`ReceiverAdapter::initialize`から`usb_open_model`でdeviceをopen/claimし、DT181はassets読込後に`dt181_session_open`からdeviceをopen/claimしてfirmware/session初期化する。正常起動してstreamなしのstatusが取得できるidle daemonは、両modelともUSB保持中である。object生成だけをopen/claimの根拠にせず、この成功経路でidleへ入る。
- **実操作/利用者への依頼:** USB保持中・stream clientなしのidleで利用者へ抜去を依頼し、応答後に`pixctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc --group status`と`list`、daemon診断、daemon processの実状態/終了値を採取する。通常停止・wait後、再接続を依頼し、`pixd --list-json`で再列挙してから新runtime/daemonを起動しstatus/listと短い`pix-ts` captureを実行する。別runでは`pix-ts --time "$CAPTURE_SECONDS"`中に抜去を依頼し、client/daemon診断と各実終了値を採取する。
- **期待値/終了値:** idleでは抜去時にstream clientがないためstream client exit 7は期待値にしない。固定sourceはidle抜去時のdaemon自動終了値や`pixctl status`の特定応答を定義していないため、その実際のresponse/exit/daemon状態を記録し、list-jsonやdaemon生存だけを切断検知の合格証拠にしない。stream runは切断診断とclient exit 7を確認し、time満了exit 0は切断合格でない。新daemonの短TSはexit 0とTS/counter合格。
- **停止/cleanup:** idle/streamそれぞれでowned client/daemonを通常停止してwaitし、終了値、process/FD/socketを確認する。再接続後の新daemonも通常停止する。Androidはpermission/FD再取得。
- **記録:** model/idle/stream、USB open/claimを含む起動条件、依頼/応答時刻、path、status response/exit、daemon診断/process実状態と終了値、stream client exit、再取得TS/counter、cleanup。

OS/ABI別の具体操作は [USB切断・LNBの環境別手順](#pix-os-t07-recipes) を参照する。

### T08 通常停止・cleanup

- **適用対象/頻度:** 全test終了時。
- **準備:** 各owned client/daemon PIDとruntimeを記録してから起動する。
- **実操作/利用者への依頼:** pix-ts clientを通常停止しwait "$C"、pixdを通常SIGINT/console stopしてwait。
- **期待値/終了値:** 全owned process終了、TS close、FD/IPC/socket/runtime残留なし。timeoutはfail。
- **停止/cleanup:** 無関係processへsignalしない。test専用runtimeだけ片付け失敗log保持。
- **記録:** PID/role/exit、process/FD/IPC/socket/runtime、復旧状態。

### 共通helperの固定取得（T09前提）

T09を実行する前に、候補sourceとは独立した共通runbook helperをSiano runbookのimmutable commit `c2378ea3c52b3e415ec6c7acfea4b8f758b46eda`から取得する。candidate checkout/archive内のhelper有無に依存しない。POSIX/Termuxでは次を一度実行し、以後の全captureで`$TOOL/ts-check.py`を使う。

```sh
RUNBOOK_REF='c2378ea3c52b3e415ec6c7acfea4b8f758b46eda'; HELPER_SHA256='dbb4826e3eb4f0cb13850b59864c7219b8929822976c9e213d3948edbe450f05'
TOOL="$WORK/tooling"; mkdir -m 700 -p "$TOOL"
gh api -H 'Accept: application/vnd.github.raw' "repos/Khronos31/siano-userland/contents/docs/release-validation/ts-check.py?ref=$RUNBOOK_REF" > "$TOOL/ts-check.py"
if command -v sha256sum >/dev/null 2>&1; then printf '%s  %s\n' "$HELPER_SHA256" "$TOOL/ts-check.py" | sha256sum -c -; else printf '%s  %s\n' "$HELPER_SHA256" "$TOOL/ts-check.py" | shasum -a 256 -c -; fi
printf '%s\n' "$RUNBOOK_REF $HELPER_SHA256" > "$LOG/ts-helper-version.txt"
```

Windows PowerShell 5.1でも同じ固定版を新しいtooling directoryへ取得し、hash一致後にのみ使う。

```powershell
$RunbookRef = 'c2378ea3c52b3e415ec6c7acfea4b8f758b46eda'; $HelperSha256 = 'dbb4826e3eb4f0cb13850b59864c7219b8929822976c9e213d3948edbe450f05'
$Tool = Join-Path $Work 'tooling'; New-Item -ItemType Directory -Path $Tool | Out-Null
$Helper = Join-Path $Tool 'ts-check.py'
Invoke-WebRequest -UseBasicParsing -Uri "https://raw.githubusercontent.com/Khronos31/siano-userland/$RunbookRef/docs/release-validation/ts-check.py" -OutFile $Helper
$Actual = (Get-FileHash -LiteralPath $Helper -Algorithm SHA256).Hash.ToLowerInvariant()
if ($Actual -ne $HelperSha256) { throw 'runbook TS helper hash mismatch' }
"$RunbookRef $Actual" | Set-Content (Join-Path $LOG 'ts-helper-version.txt')
```

helperのversionはrunbook Git commitとSHA-256の組で固定する。candidate source SHA、artifact digest、helper ref/hashを別々に記録する。

### T09 TS・counter・終了値判定

- **適用対象/頻度:** 全capture/soak。
- **準備:** Python 3.11以上を`python3 --version`で確認し、固定取得した`$TOOL/ts-check.py`（Windowsは`$Helper`）を使う。候補ごとに新規作成した`$LOG`を使う。
- **実操作/利用者への依頼:** POSIXでは各TSに`python3 $TOOL/ts-check.py "$LOG/<capture>.ts" > "$LOG/<capture>.ts-check.json"`を実行する。Windowsは同じhelperを`python $Helper "$LOG\<capture>.ts"`で実行し、標準出力を`$LOG`へ保存する。Termuxは`python`で同じhelperを呼び出す。候補daemonのpacket/byte/USB counterとclient exitも記録する。
- **期待値/終了値:** null PIDはcontinuity判定から除外する。同一PIDの隣接payload duplicate pairはPCR timestamp以外の全byte一致時だけ1組を許容し、duplicate indexをJSONへ記録する。3個目、内容差、非連続再送はcontinuity error。retune境界を確認できるrunはzero-basedの最初の出力packet indexを`--retune-boundary-at`へ渡し、境界後の各PID初回packetだけCC比較を除外する。TEI、sync、framing、invalid adaptationは除外しない。PCR reserved bits/extension違反とinvalid adaptationはfail。全TSが製品SPEC基準を満たし通常取得exit 0。非空file/counter 0だけでpassにしない。
- **停止/cleanup:** 元TS/hash/logを残してT08 cleanup。
- **記録:** Python version、helperのGit SHA/hash、TS hash、helperのJSON/終了値、duplicate/null/invalid packet index、sync/TEI/continuity/drop/counter、client exit。

Windows PowerShellでの固定helper呼出し:

```powershell
$helper = Join-Path $Tool 'ts-check.py'; $ts = Join-Path $LOG '<capture>.ts'
$json = Join-Path $LOG '<capture>.ts-check.json'; $err = Join-Path $LOG '<capture>.ts-check.err'
& python $helper $ts 1> $json 2> $err; $parserRc = $LASTEXITCODE
$parserRc | Set-Content (Join-Path $LOG '<capture>.ts-check.rc')
if ($parserRc -ne 0) { throw "TS parser failed: $parserRc" }
```

### T10 native PC/SC・WinSCard

- **適用対象/頻度:** 対応OS/APIのみ。非対応製品は理由付きN/A。
- **準備:** native OS adapter/consumerとcandidate IFD/DLL pathを用意。
- **実操作/利用者への依頼:** PIXは製品CARD status操作を持つがnative PC/SC/WinSCard adapter/APIを持たないため、このnative transaction試験は理由付きN/A。adapter/consumerを捏造して起動しない。
- **期待値/終了値:** native adapter/APIがないため理由付きN/A。PIXのcard-statusやCI smokeをnative transaction合格に読み替えない。
- **停止/cleanup:** consumerは起動しない。別の試験で起動したdaemonだけを通常停止し、system PCSC serviceを変更しない。
- **記録:** OS/ABI、module/DLL absolute path、consumer、reader/card、operation/exit。

### T11 LNB・非対応要求の拒否

- **適用対象/頻度:** 必要時。給電対応profileだけ実測。
- **準備:** PIXはLNB給電非対応のためmeter/給電設備を接続しない。
- **実操作/利用者への依頼:** LNB controlなし。--lnaをLNBと解釈せず操作しない。
- **期待値/終了値:** PIXにLNB給電機能はないため、LNB電圧実測と拒否試験は理由付きN/A。--lnaをLNBと解釈しない。
- **停止/cleanup:** LNB操作なし。試験用daemonは通常停止する。
- **記録:** model/profile、command/exit、meter値/条件、cleanupまたはN/A根拠。

### T12 複数device・daemon・access

- **適用対象/頻度:** 必要時、複数device/selector/access変更時。
- **準備:** USB port/selectorとinstance/runtimeをdeviceごとに割り当てる。
- **実操作/利用者への依頼:** pixd --models/--list-jsonでmodel/device/portを区別。複数接続時のみ別runtimeで公開CLI capture。Androidは`termux-usb -l`で2つのnodeを記録し、各nodeへ`termux-usb -r -e "./pixd --instance a --runtime-dir $RT_A --group" <node-A>`および`termux-usb -r -e "./pixd --instance b --runtime-dir $RT_B --group" <node-B>`を別Termux sessionから実行する。daemonが受け取るFDは別々で、clientは各instance/runtimeを指定する。
- **期待値/終了値:** device/selector対応一意。別deviceの結果を混同せず、要求時は各sideを独立判定。
- **停止/cleanup:** 各client/daemon個別wait/stopしFD/IPC/socket確認。
- **記録:** device/port/PID/runtime、各exit/継続/再取得/access/cleanup。

### T13 連続受信・頻度

- **適用対象/頻度:** 毎releaseおよび全model初回/major。
- **準備:** 確定頻度表の対象archive/model/timeをmatrixへ展開する。
- **実操作/利用者への依頼:** 毎release代表Linux x86_64 glibc 2h/全archive native short; 初回全modelごと全無料channel 10s、macOS/Windows 30m、他ABI 10m。
- **期待値/終了値:** 指定連続時間完了かつTS条件合格。追加soakは明示ユーザー決定分のみ。
- **停止/cleanup:** T08通常stopと全resource cleanup。
- **記録:** model/OS/ABI/archive、時間、TS/counter、FD/RSS、未実施理由。

### T14 失敗・リリース判定

- **適用対象/頻度:** 全candidate gate完了後。
- **準備:** source/archive SHAと全T-IDの結果を版別記録へ用意。
- **実操作/利用者への依頼:** version recordへcandidate欠落/unsupported/nonzeroを未達として記録。formal release dispatchを候補試験に使わない。
- **期待値/終了値:** failure/unsupported/未実施/環境不能/candidate欠落をpassにしない。
- **停止/cleanup:** candidateと失敗証拠を保持。tag/releaseは別依頼。
- **記録:** T-ID、candidate/host/model、state、log、blocker、判定根拠。

N/Aは製品機能上該当しない項目だけに使う。OS/ABI候補のunsupported、candidate欠落、未実施、失敗、環境不能は版別結果で未達とし、passに読み替えない。手順に記した期待値は実測結果ではない。

## 対象アーカイブ・環境から全試験への対応

各正式archive/E-IDで適用されるT00–T14を独立して実行する。試験項目の準備、操作、期待値/終了値、停止/cleanup、記録は「共通試験シナリオ T00–T14」を唯一の定義とし、下表はarchiveごとのOS/ABI環境手順への参照を示す。異なるarchive、libc、OS/ABI、USB access boundaryの結果で代替しない。機能対象外はT本文と製品固有条件に理由を記し、未実施・unsupported・候補未入手・環境不足は未達として記録する。

| archive / E-ID / 実行環境とprocedure |
|---|
| `linux-x86_64` / [E03](#3-配布アーカイブ別手順) / Linux x86_64/glibc |
| `linux-x86_64` / [E02](#3-配布アーカイブ別手順) / Linux x86_64/musl |
| `linux-aarch64` / [E15-G](#3-配布アーカイブ別手順) / Linux aarch64/glibc |
| `linux-aarch64` / [E15-M](#3-配布アーカイブ別手順) / Linux aarch64/musl |
| `darwin-arm64` / [E04](#3-配布アーカイブ別手順) / macOS arm64 |
| `android-aarch64` / [E05](#android-termux-の実行) / Android Termux/aarch64 |
| `android-armv7a` / [E06](#android-termux-の実行) / Android Termux/armv7a |
| `android-x86_64` / [E07](#android-termux-の実行) / Android Termux/x86_64 |
| `windows-x64` / [E17](#windows-11-x86_64の実行) / Windows 11 x86_64 |

## 0. 共通配布アーカイブと共通手順

各冊の「共通実行環境表」は、Stable release gateで確認する正式配布artifactとnative/canonical runtimeの対応matrixである。環境IDはOS/ABIとUSB access boundaryを識別し、host名や試験状態を表さない。正式な対象archiveはREADME/SPECと各版のmatrixに従う。

| 共通要素 | 役割 |
|---|---|
| 環境ID | OS/ABIとruntime/access条件の識別子。host名・実施状態は版別記録に残す。 |
| 配布artifact matrix | 上の環境表で正式archiveと試験環境を対応付ける。追加の移植性確認環境はこのmatrixへ含めない。 |
| T00–T14 | 共通の試験項目・操作・期待値・cleanup・記録項目を示す。 |
| 共通コマンド | CLIやOS固有の実コマンドは製品ごとに異なる。各環境表の「配布artifact / 手順」リンク先と続くrecipeを使い、製品間で同じCLIを仮定しない。 |
| 状態語彙 | 実施状態は下の共通語彙を版別記録で使う。試験結果の合否とは別に記録する。 |

### 検証状態の共通語彙

| 状態 | 意味 |
|---|---|
| `継承` | 同一model・runtime/access path・featureで、artifact同一性または非影響の根拠がある既存証拠を適用する。 |
| `今回再検証` | 今回のcandidateで対象を実行し、結果を記録する。 |
| `未認定` | 対象の現行証拠がなく、合格を示せない。 |
| `対象外` | 製品仕様の対象外に限る。未実施・環境不能・candidate欠落・失敗には使わない。 |

## 1. 候補・配布物

対象versionの候補sourceから作成されたbinary archiveとsource archiveを取得し、hash・manifest・license/provenanceを照合します。pre-release候補が用意できない場合は実機gateを完了扱いにせず、その版別文書に未実施理由を記録します。正式Releaseを作成・公開するworkflowを候補作成の代用にしません。

公開後の監査を行う際は、GitHub Releaseから新規directoryへbinary/source archiveと`SHA256SUMS`を取得し、macOSでは`shasum -a 256 -c SHA256SUMS`、Linuxでは`sha256sum -c SHA256SUMS`、各archive内の`build-info.txt`が記録する固定Core SHA、source manifest/license/provenanceを照合します。これはpre-release candidate試験の代替にはなりません。

## 2. 機器非接続gate

`.github/workflows/ci.yml` の `three-commands`、Windows runnerのpackage/runtime/EOF proofを含む候補commit上のCIを確認します。READMEのCMake proof、native/cross build、`tools/audit-binary.py`、source rebuild、license auditを実行し、出力・終了値を保存します。各候補archiveで `pixd --version`、`pixctl --version`、`pix-ts --version`、`pixd --models`、`pixd --list-json` を実行します。妥当なversion/model/JSONが返り、list-jsonが機器を所有しないことを確認します。DT295用旧standalone `--model` CLIを配布バイナリ試験に使いません。

## 3. 配布アーカイブ別手順

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

<a id="pix-os-t07-recipes"></a>
## 4. USB切断とLNB

### Windows 11 x86_64の実行

Windowsの候補は既存runの`windows-x64` artifact内にある`windows-x64` tar.gzである。公開済みReleaseを使わず、候補runの`release` jobが未実行/skipであることを確認する。事前に対象tunerの既存WinUSB bindingをread-onlyで記録し、driver install/changeは行わない。

```powershell
$ErrorActionPreference = 'Stop'
$RunId = '<same-run-id-as-T00>'; $T00LockPath = '<T00-log-directory>\candidate-lock.json'
if (-not (Test-Path -LiteralPath $T00LockPath)) { throw 'T00 candidate lock missing' }
$lock = Get-Content -LiteralPath $T00LockPath -Raw | ConvertFrom-Json
if ([string]$lock.run_id -ne $RunId) { throw 'E17 RunId differs from T00 candidate lock' }
$Work = Join-Path $HOME "pix-e17-$RunId"; $Download = Join-Path $Work 'download'; $Extract = Join-Path $Work 'extract'
if (Test-Path $Work) { throw "refusing to reuse existing path: $Work" }
New-Item -ItemType Directory -Path $Download,$Extract,(Join-Path $Work 'runtime'),(Join-Path $Work 'logs') | Out-Null
$run = gh run view $RunId --repo Khronos31/pix-userland --json headSha,attempt,conclusion | ConvertFrom-Json
if ($run.conclusion -ne 'success' -or $run.headSha -ne $lock.source_sha -or [int]$run.attempt -ne [int]$lock.attempt) { throw 'candidate run source, attempt, or conclusion differs from T00 lock' }
$releaseJob = gh run view $RunId --repo Khronos31/pix-userland --json jobs --jq '.jobs[] | select(.name == "release") | .conclusion'
if (-not $releaseJob -or $releaseJob -eq 'null' -or $releaseJob -eq 'success' -or $releaseJob -ne $lock.release_job_conclusion) { throw 'release job conclusion differs from T00 lock or is published' }
$artifactRows = @(gh api "repos/Khronos31/pix-userland/actions/runs/$RunId/artifacts" --jq ".artifacts[] | select(.name == \"$($lock.target)\" and .expired == false) | [.id,.digest] | @tsv")
if ($artifactRows.Count -ne 1) { throw 'candidate artifact missing, expired, or ambiguous' }
$artifact = $artifactRows[0] -split "`t"
$artifactDigest = $artifact[1] -replace '^sha256:', ''
if ($lock.target -ne 'windows-x64' -or $artifact[0] -ne [string]$lock.artifact_id -or $artifactDigest -ne [string]$lock.artifact_digest) { throw 'candidate artifact differs from T00 lock' }
$ArtifactZip = Join-Path $Download 'artifact.zip'; $token = gh auth token
try {
  $headers = @{ Authorization = "Bearer $token"; Accept = 'application/vnd.github+json' }
  Invoke-WebRequest -UseBasicParsing -Uri "https://api.github.com/repos/Khronos31/pix-userland/actions/artifacts/$($artifact[0])/zip" -Headers $headers -OutFile $ArtifactZip
} finally { Remove-Variable token,headers -ErrorAction SilentlyContinue }
$actualDigest = (Get-FileHash -LiteralPath $ArtifactZip -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualDigest -ne $artifactDigest) { throw 'downloaded candidate artifact digest mismatch' }
Expand-Archive -LiteralPath $ArtifactZip -DestinationPath $Download
$Archive = Get-ChildItem $Download -Filter 'pix-userland-*-windows-x64.tar.gz' | Select-Object -First 1
if (-not $Archive) { throw 'candidate archive missing' }
$archiveSha256 = (Get-FileHash -LiteralPath $Archive.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
if ($archiveSha256 -ne [string]$lock.archive_sha256) { throw 'candidate archive differs from T00 lock' }
tar -xzf $Archive.FullName -C $Extract
if ($LASTEXITCODE -ne 0) { throw 'candidate extraction failed' }
$D = Get-ChildItem $Extract -Directory | Select-Object -First 1
if (-not $D) { throw 'candidate payload directory missing' }; $D = $D.FullName
$releaseInfo = Get-Content (Join-Path $D 'release-info.json') -Raw | ConvertFrom-Json
if ($releaseInfo.commit -ne [string]$lock.source_sha) { throw 'candidate release-info commit differs from T00 lock' }
Set-Location $D
Get-FileHash -Algorithm SHA256 .\pixd.exe, .\pixctl.exe, .\pix-ts.exe
& .\pixd.exe --models
& .\pixd.exe --list-json
$os = Get-CimInstance Win32_OperatingSystem
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
[pscustomobject]@{ Caption=$os.Caption; Version=$os.Version; Build=$os.BuildNumber; OSArchitecture=$os.OSArchitecture; ProcessorArchitecture=$cpu.Architecture }
$RT = Join-Path $Work 'runtime'; $LOG = Join-Path $Work 'logs'
$selector = 'usb:BUS-PORT'
```

別PowerShellでdaemonを起動してそのconsoleを維持する。T02の`status`/`list`は2つ目のconsoleから確認する。

```powershell
Set-Location 'C:\pix-e17-<run-id>'
$D = (Get-Location).Path
$RT = Join-Path $D 'runtime'
$LOG = Join-Path $D 'logs'
$selector = 'usb:BUS-PORT'
& .\pixd.exe --device $selector --runtime-dir $RT --instance rc --group
```

capture consoleでは各runのmodel、channel、system、有限秒数、出力fileを指定する。DT181-PU0とDT295を別runとして全archiveで実施する。unsupported/nonzeroは未達である。

```powershell
Set-Location 'C:\pix-e17-<run-id>'
$D = (Get-Location).Path
$RT = Join-Path $D 'runtime'
$LOG = Join-Path $D 'logs'
$selector = 'usb:BUS-PORT'
& .\pixctl.exe --device $selector --runtime-dir $RT --instance rc --group status
& .\pixctl.exe --device $selector --runtime-dir $RT --instance rc --group list
$p = Start-Process -FilePath (Join-Path $D 'pix-ts.exe') -ArgumentList @('--device',$selector,'--instance','rc','--channel','T27','--system','isdb-t','--time','30','--output',(Join-Path $D 'capture.ts'),'--runtime-dir',$RT,'--group') -PassThru -NoNewWindow
$p.WaitForExit()
$p.Refresh()
$p.ExitCode
Get-FileHash -Algorithm SHA256 (Join-Path $D 'capture.ts')
```

T04は`$CHANNEL_PLAN`に対象channelを並べ、各channel/modelを独立captureとして実行する。T05は別consoleのstatus/listを受信中にも実行する。T06はcaptureを止めdaemonを維持し、利用者の抜去前後に`card-status`を記録する。外部ATR/APDUは本製品の対象外であり、内部CARDがないOSを理由に試験を省略しない。

```powershell
$LOG = Join-Path $D 'logs'
New-Item -ItemType Directory -Path $LOG | Out-Null
$CHANNEL_PLAN = @('<channel-1>','<channel-2>')
foreach ($channel in $CHANNEL_PLAN) {
  $system = if ($channel -match '^T\d+$') { 'isdb-t' } elseif ($channel -match '^(BS|CS)\d+') { 'isdb-s' } else { throw "Unsupported channel format: $channel" }
  $out = Join-Path $LOG ($channel + '.ts')
  $p = Start-Process -FilePath (Join-Path $D 'pix-ts.exe') -ArgumentList @('--device',$selector,'--instance','rc','--channel',$channel,'--system',$system,'--time','10','--output',$out,'--runtime-dir',$RT,'--group') -PassThru -NoNewWindow -Wait
  [pscustomobject]@{ Channel=$channel; System=$system; ExitCode=$p.ExitCode; File=$out }
}
& (Join-Path $D 'pixctl.exe') --device $selector --runtime-dir $RT --instance rc --group status
& (Join-Path $D 'pixctl.exe') --device $selector --runtime-dir $RT --instance rc --group list
& (Join-Path $D 'pixctl.exe') --device $selector --runtime-dir $RT --instance rc --group card-status
```

T05はcaptureを別PowerShellで実行し、status/listをcapture中に反復する。T06はcaptureを起動しない別runとしてdaemonを維持し、card-statusを抜去・再挿入の前後で保存する。T09は各TSでTSDuck `tsanalyze`を実行し、TS SHA-256と製品SPECのsync/TEI/continuityを記録する。T10/T11は本製品に外部PC/SC/WinSCard/LNB APIがないため理由付きN/Aとする。

```powershell
$CONTROL_CAPTURE_SECONDS = '<finite-seconds-from-release-matrix>'
$monitor = Start-Process -FilePath (Join-Path $D 'pix-ts.exe') -ArgumentList @('--device',$selector,'--instance','rc','--channel','T27','--system','isdb-t','--time',$CONTROL_CAPTURE_SECONDS,'--output',(Join-Path $LOG 't05-monitor.ts'),'--runtime-dir',$RT,'--group') -PassThru -NoNewWindow
while (-not $monitor.HasExited) {
  & (Join-Path $D 'pixctl.exe') --device $selector --runtime-dir $RT --instance rc --group status 2>&1 | Tee-Object -Append (Join-Path $LOG 't05-status.txt')
  & (Join-Path $D 'pixctl.exe') --device $selector --runtime-dir $RT --instance rc --group list 2>&1 | Tee-Object -Append (Join-Path $LOG 't05-list.txt')
  Start-Sleep -Seconds 5
  $monitor.Refresh()
}
$monitor.Refresh(); $monitor.ExitCode | Out-File (Join-Path $LOG 't05-client.rc')
```

T06は別runでclientを起動せずdaemonだけreadyにし、各照会のresponseと終了値を保存する。利用者へカード抜去・再挿入を依頼し、各操作後に同じ`card-status`を実行する。各依頼の完了待ちは最大5分とし、応答なしは未完了とする。

```powershell
$cardCommand = Join-Path $D 'pixctl.exe'
& $cardCommand --device $selector --runtime-dir $RT --instance rc --group card-status 2>&1 | Tee-Object (Join-Path $LOG 't06-before.txt')
$LASTEXITCODE | Out-File (Join-Path $LOG 't06-before.rc')
# 利用者へカード抜去を依頼し、完了後に状態を記録する。
& $cardCommand --device $selector --runtime-dir $RT --instance rc --group card-status 2>&1 | Tee-Object (Join-Path $LOG 't06-removed.txt')
$LASTEXITCODE | Out-File (Join-Path $LOG 't06-removed.rc')
# 利用者へ同じカードの再挿入を依頼し、完了後に状態を記録する。
& $cardCommand --device $selector --runtime-dir $RT --instance rc --group card-status 2>&1 | Tee-Object (Join-Path $LOG 't06-reinserted.txt')
$LASTEXITCODE | Out-File (Join-Path $LOG 't06-reinserted.rc')
```

T07ではidle runとstream runを分ける。物理抜去直前にstatusでidle/streamingを確定し、利用者へ依頼して最大5分待つ。stream clientは別PowerShellで前景実行し、選択した観測窓内の自然終了値とstderrの`stream-end`/`packets-written`を保存する。自然終了exit 7が切断、exit 0はtime満了であり、観測窓を超えて動作中ならclient consoleでCtrl+Cを入力し、timeout未達として終了値を記録する。daemonを通常終了してUSBを戻した後、再列挙したpathで新しいruntime/daemonを作りstatus/listと短いT27受信を行う。終了後は全client/daemonを個別にwaitし、process/runtime/socketを記録する。T00–T14の頻度・期待値・cleanup・証拠項目は各T見出しのまま適用する。

```powershell
$CAPTURE_SECONDS = '<finite-capture-duration-seconds-from-release-matrix>'
$OBSERVATION_LIMIT_SECONDS = '<finite-cut-observation-window-seconds>'
$CLIENT_FINISH_LIMIT_SECONDS = [int]$CAPTURE_SECONDS + 30
$client = Start-Process -FilePath (Join-Path $D 'pix-ts.exe') -ArgumentList @('--device',$selector,'--instance','rc','--channel','T27','--system','isdb-t','--time',$CAPTURE_SECONDS,'--output',(Join-Path $LOG 'usb-cut.ts'),'--runtime-dir',$RT,'--group') -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $LOG 'usb-cut.out') -RedirectStandardError (Join-Path $LOG 'usb-cut.err')
if (-not $client.WaitForExit([int]$OBSERVATION_LIMIT_SECONDS * 1000)) {
  'timeout: capture still active at observation limit' | Out-File (Join-Path $LOG 'usb-cut-observation.txt')
  if (-not $client.WaitForExit([int]$CLIENT_FINISH_LIMIT_SECONDS * 1000)) {
    'client exceeded finite capture plus 30-second cleanup bound; terminate owned client and mark T07 incomplete' | Out-File (Join-Path $LOG 'usb-cut-client-timeout.txt')
    Stop-Process -Id $client.Id -ErrorAction SilentlyContinue
    if (-not $client.WaitForExit(5000)) { 'owned client did not exit within 5-second cleanup bound' | Out-File (Join-Path $LOG 'usb-cut-client-cleanup.txt') }
  }
}
$client.Refresh()
if ($client.HasExited) { $client.ExitCode | Out-File (Join-Path $LOG 'usb-cut.rc') } else { 'unavailable: owned client did not exit' | Out-File (Join-Path $LOG 'usb-cut.rc') }
```

受信を別PowerShellで起動し、streaming中にstatusを確認してから利用者へ抜去を依頼する。観測窓が満了してもclientが動作中ならtimeout/failを記録し、有限`--time`+30秒の終了上限まで待つ。上限超過時はowned clientだけを停止して5秒bounded waitでcleanupし、client終了値を取得できない場合も記録する。いずれも通常daemon停止へ進み、T07を未完了とする。観測窓内に自然終了したexit 7だけを切断結果として評価し、exit 0やtimeoutをpassにしない。

T08ではclient終了後にdaemon consoleでCtrl+Cを入力し、daemonの終了値、process一覧、runtime内socketを保存する。daemon終了後は同じgroup modeのstatusを再実行し、endpoint不在の戻り値を確認する。T09では`Get-Command tsanalyze`と`tsanalyze --version`を保存して各TSに実行し、188-byte同期、TEI、continuityを製品SPEC基準で判定する。utilityがない場合は環境不能/未実施でありpassにしない。T10とT11は製品機能外として理由付きN/Aを記録する。T12は異なるUSB portで別consoleのdaemon A/Bとclient A/Bを起動し、A切断時のclient exit 7とBの継続・exit 0を個別記録する。T13は同じcaptureを頻度表にある各OS/ABIの指定時間で実行し、開始・5秒ごと・終了時のTS/counter/RSS/process handle countを保存する。Windowsのhandle countはPOSIX FD数と同一視せず、FD欄は取得不能として明示する。T14は全T-IDの候補/host/model/state/evidence pathを版別記録へ集約し、unsupported・未実施・失敗をpassにしない。

```powershell
$RT_A = Join-Path $D 'runtime-a'; $RT_B = Join-Path $D 'runtime-b'
$PORT_A = '<enumerated-port-a>'; $PORT_B = '<enumerated-port-b>'
New-Item -ItemType Directory -Path $RT_A, $RT_B | Out-Null
# 別daemon console Aで実行する。
Set-Location 'C:\pix-e17-<run-id>'
$DA = Join-Path (Get-Location).Path 'pixd.exe'; $RT_A = Join-Path (Get-Location).Path 'runtime-a'
$PORT_A = '<enumerated-port-a>'
& $DA --device "usb:$PORT_A" --runtime-dir $RT_A --instance a --group
```

```powershell
# 別daemon console Bで実行する。
Set-Location 'C:\pix-e17-<run-id>'
$DB = Join-Path (Get-Location).Path 'pixd.exe'; $RT_B = Join-Path (Get-Location).Path 'runtime-b'
$PORT_B = '<enumerated-port-b>'
& $DB --device "usb:$PORT_B" --runtime-dir $RT_B --instance b --group
```

別client consoleでBを開始し、次にAを開始する。A切断中もB captureを継続させ、両clientの終了値を個別に記録する。

```powershell
Set-Location 'C:\pix-e17-<run-id>'
$D = (Get-Location).Path; $B = Join-Path $D 'pix-ts.exe'
$PORT_A = '<enumerated-port-a>'; $PORT_B = '<enumerated-port-b>'
$S = '<finite-seconds-from-release-matrix>'
$b = Start-Process -FilePath $B -ArgumentList @('--device',"usb:$PORT_B",'--instance','b','--channel','T27','--system','isdb-t','--time',$S,'--output',(Join-Path $D 'b.ts'),'--runtime-dir',(Join-Path $D 'runtime-b'),'--group') -PassThru -NoNewWindow
$a = Start-Process -FilePath $B -ArgumentList @('--device',"usb:$PORT_A",'--instance','a','--channel','T27','--system','isdb-t','--time',$S,'--output',(Join-Path $D 'a.ts'),'--runtime-dir',(Join-Path $D 'runtime-a'),'--group') -PassThru -NoNewWindow
# Aがstreaming中であることをstatusで確認し、利用者へA側deviceの抜去を依頼する。
$a.WaitForExit(); $a.Refresh(); $a.ExitCode | Out-File (Join-Path $D 'a.rc')
$b.WaitForExit(); $b.Refresh(); $b.ExitCode | Out-File (Join-Path $D 'b.rc')
```

T13は上のdaemon/client起動方法で`$S`を頻度表の対象秒数にし、5秒間隔でTS/counter/process/RSS/handle countを採る。Windows handle countはPOSIX FD数の代替合格条件ではなく、FD欄は取得不能として記録する。

```powershell
Set-Location 'C:\pix-e17-<run-id>'
$D = (Get-Location).Path; $B = Join-Path $D 'pix-ts.exe'; $C = Join-Path $D 'pixctl.exe'
$RT = Join-Path $D 'runtime'; $LOG = Join-Path $D 'logs'; $S = '<seconds-from-frequency-table>'
$selector = 'usb:BUS-PORT'
$soak = Start-Process -FilePath $B -ArgumentList @('--device',$selector,'--instance','rc','--channel','T27','--system','isdb-t','--time',$S,'--output',(Join-Path $LOG 'soak.ts'),'--runtime-dir',$RT,'--group') -PassThru -NoNewWindow
while (-not $soak.HasExited) {
  $soak.Refresh(); Get-Process -Id $soak.Id | Select-Object Id,ProcessName,WorkingSet64,HandleCount | Export-Csv -NoTypeInformation -Append (Join-Path $LOG 't13-process.csv')
  Get-Process pixd -ErrorAction SilentlyContinue | Select-Object Id,ProcessName,WorkingSet64,HandleCount | Export-Csv -NoTypeInformation -Append (Join-Path $LOG 't13-process.csv')
  & $C --device $selector --runtime-dir $RT --instance rc --group status 2>&1 | Out-File -Append (Join-Path $LOG 't13-status.txt')
  $LASTEXITCODE | Out-File -Append (Join-Path $LOG 't13-status.rc')
  Start-Sleep -Seconds 5
}
$soak.Refresh(); $soak.ExitCode | Out-File (Join-Path $LOG 't13-client.rc')
Get-FileHash -Algorithm SHA256 (Join-Path $LOG 'soak.ts')
Get-ChildItem -LiteralPath $RT -Force
```

### Android (Termux) の実行

E05/E06/E07ごとに対応するABI archiveを展開し、Termux:APIと`termux-api`を使う。`termux-usb -l`で実nodeを記録する。`pixd --list-json`や`--device`は`--fd`と併用できないため、Androidは`termux-usb -r -e`でdaemon自身を起動し、渡されたFDでUSBを開く。以下をdaemon起動に使い、clientは別Termux sessionから同じruntime/instance/groupを指定する。

```sh
RT="$PREFIX/tmp/pix-<run-id>"
mkdir -p "$RT" && chmod 700 "$RT"
termux-usb -l
termux-usb -r -e "./pixd --instance rc --runtime-dir $RT --group" /dev/bus/usb/BBB/AAA
```

再接続後は`termux-usb -l`で新nodeを確認し、新しいruntimeを作成して同じlaunchを行う。古いFDを再利用しない。T00–T14は各Android ABI archiveごとに同じ期待値・終了値・cleanup・記録を適用する。`--models`/archive model data情報はdevice-free candidate inspectionとして行い、USB FDでの列挙可否を代用しない。

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

抜去完了の応答を受けた時点で観測窓を開始します。窓内に自然終了しなければtimeout/failを記録し、`SIGINT`でclientを通常停止します。どちらの場合も`wait`で実終了値を保存し、exit 0は切断合格にしません。

```sh
printf '%s' 'USB抜去完了後にEnter: '
IFS= read -r _
sleep "$OBSERVATION_LIMIT_SECONDS"
if kill -0 "$C" 2>/dev/null; then
  printf '%s\n' timeout > "$LOG/cut-observation.txt"
  kill -INT "$C"
fi
if wait "$C"; then rc=0; else rc=$?; fi
printf '%s\n' "$rc" > "$LOG/cut.rc"
cat "$LOG/cut.stderr" > "$LOG/cut.stderr.saved"
```

旧daemonを通常停止し、process/FD/runtime socketを確認してから、ユーザーへ同じ物理portへの再接続を依頼します。Linux/macOS/Windowsは`pixd --list-json`で再列挙し、新しいruntimeでdaemonを起動します。Androidは`termux-usb -l`で新nodeを記録し、`termux-usb -r -e`で新しいFDをdaemonへ渡して新しいruntimeで起動します。いずれもstatus/listを確認し、既知の無料T27の短TSを取得します。

```sh
RT2=$(mktemp -d "${TMPDIR:-/tmp}/pix-reconnect.XXXXXX")
./pixd --device usb:<BUS-PORT> --runtime-dir "$RT2" --instance rc --group & P2=$!
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT2" --instance rc --group status
./pixctl --device usb:<BUS-PORT> --runtime-dir "$RT2" --instance rc --group list
./pix-ts --device usb:<BUS-PORT> --instance rc --channel T27 --system isdb-t \
  --time <SECONDS> --output t27.ts --runtime-dir "$RT2" --group
```

Androidでは上の`RT2`を新しいrun固有pathへ置き換え、再接続後のTermux USB nodeを使ってdaemonだけを次のように起動します。client consoleでは同じ`RT2`/`rc`/`--group`を指定して上の`pixctl`と`pix-ts`を実行します。

```sh
RT2="$PREFIX/tmp/pix-reconnect-<run-id>"
mkdir -p "$RT2" && chmod 700 "$RT2"
termux-usb -l
termux-usb -r -e "./pixd --instance rc --runtime-dir $RT2 --group" /dev/bus/usb/BBB/AAA
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
