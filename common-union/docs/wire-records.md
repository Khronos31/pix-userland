# UDTV 1.0 型付きレコードコーデック (dtv::wire::records)

本ドキュメントは、`dtv_userland_wire` ターゲットに含まれる型付きレコードコーデック（ヘッダー: `include/dtv/userland/records.h`、実装: `src/records.cpp`）のバイナリ符号化規約、エラー契約、およびコンテキスト検証仕様を解説する。

本モジュールは、20 バイト固定長のエンベロープに続く UDTV 1.0 ペイロード（HELLO, ACQUIRE, LIST, STATUS, TUNE, RELEASE, イベント, カード操作等）を対象とした有界なシリアライザ/デシリアライザ群を提供する。動的メモリ確保は行わず、ホストの構造体レイアウト（パディングやエンディアン）に依存しないポータブルなバイナリ変換を実施する。

---

## 1. 符号化規約 (Encoding Conventions)

ワイヤフォーマット上の全整数値はリトルエンディアンで格納される。

| データ型 | バイト表現 | 規則 |
|---|---|---|
| `u8` / `u16` / `u32` / `u64` / `i32` | 固定長 | リトルエンディアンバイト列 |
| `bool` | 1 バイト (`u8`) | `0`（偽）または `1`（真）。それ以外の値は拒否 |
| `UUID` / `epoch` / `token` | 16 バイト | 固定長バイナリオブジェクト |
| `str[N]` | 2 バイト長さ + UTF-8 列 | 先頭 2 バイト（`u16`）にバイト長、続いて最大 $N$ バイトの UTF-8。途中の NUL バイト禁止 |
| `bytes[N]` | 4 バイト長さ + バイナリ列 | 先頭 4 バイト（`u32`）にバイト長、続いて最大 $N$ バイトのバイト列 |
| `arr<T,N>` | 2 バイト要素数 + 要素列 | 先頭 2 バイト（`u16`）に要素数、続いて最大 $N$ 個の型付き要素 |
| `opt<T>` | 1 バイトタグ + 値 | タグが `0` の場合は値なし、`1` の場合は直後に値が後続。それ以外のタグは拒否 |

すべての制御系レコード構造体は、固定容量のコンテナ（`WString`, `BBytes`, `BArray`, `Optional`）を用いて値型として表現される。
唯一の例外は `TsData`（ストリームデータイベント）であり、最大 1 MiB のパケット配列をコピーせずに入力スパン（`input.data + 28`）を別名参照（alias）する。そのため、`TsData::packets` の参照先は入力バッファの生存期間内でのみ有効であり、非同期ワーカや別スレッドへ保持する場合は呼び出し元側で所有権を持つバッファへコピーする必要がある。

---

## 2. コーデックのエラー契約

デコーダは、与えられた入力スパンが仕様と整合することを要求する。エラーの性質に応じて以下のエラーコードが厳格に区別される：

### 2.1 エラーの分類
- **`PROTOCOL_ERROR` (構文レベルの破綻)**:
  ワイヤ上の構造そのものの不正。全体のサイズ不足や不整合、配列要素数の上限超過、不正なオプションタグ（0/1 以外）、UTF-8 規則違反、およびレコード末尾の余剰バイト（trailing bytes）などが該当する。
- **`INVALID_ARGUMENT` (値レベル・順序レベルの不正)**:
  サイズや構文は適法であるが意味的に範囲外の値。未定義の enum 値、予約ビットの立脚、0 禁止フィールドでの 0 値、宣言セット外のメトリック ID などが該当する。
  また、**`StreamCounters`, `ReceiverCounters`, `Measurements` における要素 ID の昇順違反や重複 ID**（前要素以下の ID）も、局所コーデックにより `INVALID_ARGUMENT` として拒否される。
- **`RESOURCE_LIMIT` (制御ペイロード上限超過)**:
  構文的に正しい制御レコードであるが、符号化後のサイズが制御ペイロード上限値 `kMaxControlPayload`（65,536 バイト）を超過する場合に、バッファへの書き込み前に返される。呼び出し元のバッファ不足を示す `BUFFER_TOO_SMALL` とは明確に区別される。
- **エンコード時の一括計測保証**:
  通常の制御レコードエンコーダは、最初にレコード全体のサイズを事前計測（カウントパス）する。呼び出し元の出力バッファが不足している場合は何も書き込まずに `BUFFER_TOO_SMALL` を返すため、不完全なレコードが途中で切り捨てられて出力されることはない。

### 2.2 `TsData` エンコーダの例外仕様
`TsData`（`encode_ts_data`）は制御系レコードとは異なり、以下の独立した実装規約を持つ：
- **カウントパス不使用**: カウントパスは使用せず、直接出力容量（`kTsDataPrefixSize + value.packets.size`、すなわち `28 + packets.size` バイト）を検査する。
- **制御上限の除外とバイト長の区別**: 制御ペイロード上限（65,536 バイト）は適用されない。TS パケット列のサイズ `packets.size` は 188 の倍数かつ `kMaxTsPacketBytes`（1,048,476 バイト）以下であることが要求され、28 バイトのヘッダ接頭辞を含めたワイヤペイロード全体が出力バッファ容量に収まるかを検査する。例えば 400 パケットの場合、`packets.size` は 75,200 バイト（400 × 188）であり、28 バイト prefix を含む符号化出力長は 75,228 バイトとなる。このように 65,536 バイトを超えるデータであっても正しくエンコードされる（なお、75,228 バイトを直接 `packets.size` に渡した場合は 188 の倍数ではないため `INVALID_ARGUMENT` となる）。
- **入力検証と戻り値**: `stream_id` が 0、パケットポインタが無効（`packets.data == nullptr` かつ `packets.size != 0` の不正な組合せ）、サイズが 188 の倍数でない、または最大長を超過している場合は `INVALID_ARGUMENT`、出力バッファ不足は `BUFFER_TOO_SMALL` を返す（なお `ByteView{nullptr, 0U}` のようにサイズ 0 の空パケット列は適法に受理され、28 バイトのヘッダ接頭辞のみがエンコードされる）。

---

## 3. バリデーションの分離 (構文チェックとコンテキスト検証)

バイトコーデックはレコード単体の局所的な整合性のみを検証する。スナップショットの公開や操作のディスパッチを行う前に、以下のコンテキストバリデータを実行して状態参照を検証する必要がある：

### 3.1 構成要素の個別バリデータ
- `validate_error_record`, `validate_scope`
- `validate_capabilities`, `validate_receiver_descriptor`, `validate_reader_descriptor`, `validate_topology`, `validate_limits`
- `validate_measurements`: メトリック ID セットが `measurement_mask` と完全一致し、各メトリックが present かつ unknown または valid であること（unsupported は禁止）。
- `validate_stream_counters` (ID5, ID6) / `validate_receiver_counters` (ID1..4): カウンタ飽和ルール（`overflow=true` の場合は `value == UINT64_MAX` 必須）を検証。
- `validate_binding`: `function_id` がスナップショット内に実在することを検証。
- **`validate_list_response`**:
  デバイス一覧応答の構造を検証する。**LIST 内における機能、受信機、リーダーの各 ID 重複**は、局所コーデックではなくこのバリデータによって `INVALID_ARGUMENT` で拒否される。

### 3.2 総合状態検証 (`validate_status_response`)
`validate_status_response(value, StatusContext{&list})` は、STATUS 応答を発行する前に呼び出される包括的な検証パスである。

```cpp
struct StatusContext final {
    const ListResponse* list = nullptr; // カノニカルな LIST スナップショットへのポインタ
};
```

`StatusContext` はカノニカルな同じエポックの `ListResponse` へのポインタを保持する構造体であり、null ポインタはカノニカルコンテキストが存在しないことを表す。本バリデータは以下を厳密に検査する：
1. **エポックの一致**: STATUS のコンテキストエポックがカノニカルな LIST のエポックと等しいこと。
2. **メンバーシップとデスクリプタ照合**: 応答内の全機能、受信機、リーダー ID がカノニカル LIST 内に重複なく存在すること。受信機（`ReceiverDescriptor`）はカノニカルデスクリプタと `systems`, `binding`, `capabilities` が完全一致し、リーダー（`ReaderDescriptor`）はカノニカルデスクリプタと `binding` が完全一致すること（リーダーには `systems` や `capabilities` のフィールドおよび比較は存在しない）。
3. **スコープ整合性**:
   - 全体（`enclosure`）スコープの場合、カノニカル LIST の全機能・全受信機・全リーダー集合と過不足なく一致すること。
   - 部分スコープの場合、対象の受信機/リーダーがカノニカルメンバーであること。
4. **制限値と時刻**: `Limits.max_concurrent_streams` が公開受信機数以下であること、および各種観測時刻が `snapshot.observed_at_ns` 以前であること。
5. **保持ストリーム**: 保持された最終ストリームターゲットが存在する場合、当該受信機が現在も `leased` 状態にあること。

カノニカルコンテキストが欠落している（`list == nullptr`）場合、またはエポックが一致しない場合、関数は直ちに `INVALID_ARGUMENT` を返す。部分的なスナップショット自身の主張によってカノニカル LIST を代替することはできない。

---

## 4. 本モジュール単体に含まれない境界

接続状態の管理（HELLO 先頭、要求 ID 順序、多重要求排他）は [docs/control-connection.md](control-connection.md) の `ControlConnection` が担当する。
また、ソケット I/O、タイマー制御、およびデーモン/ハードウェアの物理動作は各製品デーモンの責務である。
