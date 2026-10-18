# UDTV 1.0 制御接続シリアライザ (dtv::control)

本ドキュメントは、`dtv_userland_control` ターゲット（ヘッダー: `include/dtv/userland/control_connection.h`、実装: `src/control_connection.cpp`）が提供する UDTV 1.0 制御接続管理モジュールの設計、プロトコル規則、および利用手順を解説する開発者向け文書である。

---

## 1. 概要と責務境界

`ControlConnection` は、UDTV 1.0 プロトコルの制御接続（Control Connection）を処理するためのポータブルなシリアライザ兼状態判定クラスである。

- **1 接続につき 1 インスタンス**: 制御接続 1 本に対して 1 つの `ControlConnection` オブジェクトを割り当て、所有者（製品デーモン）のイベントループから駆動する。
- **リソースを所有しない**: ソケット、ファイルディスクリプタ、OS スレッド、タイマー、および実バックエンドサービスを内部に保持しない。I/O 多重化、スケジューリング、タイムアウト管理、および実際のハードウェア/サービス操作はすべて呼び出し元が担当する。
- **プロトコル規則の強制**: ハンドシェイク手順、単一未完了要求（one-outstanding-request）の制約、要求 ID の厳格な単調増加、およびデーモンエポックの照合をライブラリレベルで厳密に検証する。
- **受入済み 6 操作のサポート**:
  `HELLO`, `LIST`, `STATUS`, `ACQUIRE`, `RELEASE`, `TUNE` の 6 操作をサポートする。HELLO は `accept()` 内で直接処理され、残る 5 操作（LIST, STATUS, ACQUIRE, RELEASE, TUNE）が所有型の `ControlRequest` 構造体へデコードされてディスパッチされる。
- **製品接続と移行の境界**:
  実製品 `px4-userland` のデーモンにおいて、本モジュールは同一のポーリング/ワーカ/エンジン経路へ 6 操作が接続された内部増分（increment）として機能する。ただし、このエンドポイントにおいて `CARD`、ストリームデータプレーン、および既存クライアントの移行は未完であり、`START`/`STOP`/`ATTACH`/`CARD` 操作や旧クライアントの完全な動作を保証するものではない。

---

## 2. 接続ライフサイクルとプロトコル規則

### 2.1 HELLO ハンドシェイクの先頭必須

制御接続が開かれた後、クライアントが最初に送信する要求フレームは必ず `HELLO`（オペコード `0x0001`）でなければならない。
- HELLO より前に他の要求が届いた場合、プロトコル違反として即座に応答なしで切断（`InboundAction::close`）する。
- 確立済みの接続に対して 2 回目の HELLO が送信された場合も同様に切断する。

### 2.2 単一未完了要求 (One-Outstanding-Request)

UDTV 1.0 制御プロトコルでは、1 つの接続上で同時に処理できる要求は 1 件に制限される。
- 要求を受理（`InboundAction::respond` または `InboundAction::dispatch`）した時点で `outstanding_` 状態となり、応答の送信が完了して呼び出し元が `complete()` を呼ぶまで、後続の要求フレームは一切受け付けない。
- 要求処理中に次のフレームが到達した場合、プロトコルエラーとして接続を切断する。

### 2.3 要求 ID の厳格な単調増加

各要求フレームの `request_id` は、直前の要求の ID よりも厳格に大きな値（`request_id > last_request_id_`）でなければならない。
- 同一の要求 ID や過去の要求 ID を持つフレームを受信した場合は、プロトコル違反として切断する。
- 要求 ID はクライアント側で採番され、正常応答およびエラー応答にそのまま引き継がれる。

### 2.4 デーモンエポックの照合 (STALE_HANDLE)

接続確立後の通常操作（`LIST`, `STATUS`, `ACQUIRE`, `RELEASE`, `TUNE`）は、デーモンのライフタイムエポック（`ServerHello::epoch`）と一致している必要がある。
- 要求内のエポックが一致しない場合、古いデーモンインスタンスに対する要求とみなされ、ワイヤエラーコード `kWireErrorStaleHandle` (16) を含むエラー応答（`error_response`）が返される。

---

## 3. 主要データ構造

### 3.1 サーバ設定 (`ServerHello`)

デーモン全体で不変の共通パラメータを保持し、接続確立時に参照される。

```cpp
struct ServerHello final {
    Uuid epoch{};             // デーモンの起動単位を識別する 16 バイト UUID
    Producer producer;        // デーモンのプロデューサ識別情報
    std::uint64_t features = 0U; // デーモンがネゴシエーション対象として提供する機能ビットマスク
};
```

クライアントが要求した `required_features` に、サーバが提供していない機能ビットが含まれていた場合、HELLO 要求は `UNSUPPORTED` エラー応答とともに拒否され、接続は切断される。

### 3.2 要求分類とデコード結果 (`ControlRequest`)

`accept()` が `InboundAction::dispatch` を返したとき、`request` にデコード済みの値が格納される。ディスパッチ対象は以下の 5 操作である：

```cpp
enum class ControlRequestKind : std::uint8_t {
    list,
    status,
    acquire,
    release,
    tune,
};

struct ControlRequest final {
    ControlRequestKind kind = ControlRequestKind::list;
    std::uint16_t opcode = 0U;
    std::uint32_t request_id = 0U;
    ListRequest list{};
    StatusRequest status{};
    AcquireRequest acquire{};
    ReleaseRequest release{};
    TuneRequest tune{};
};
```

- HELLO は `ControlRequest` には格納されず、`accept()` 内で直接処理されて `InboundAction::respond` を返す。
- `ControlRequest` はメモリフットプリントが大きいため（特に `TuneRequest` は固定長の PID 配列を保持する）、接続ごとではなくイベントループあたり 1 つ保持することが推奨される。

### 3.3 受信処理結果 (`Inbound`)

```cpp
enum class InboundAction : std::uint8_t {
    respond,   // 応答フレームが出力バッファに書き込まれた（送信が必要）
    dispatch,  // 要求が正常にデコードされた（サービス呼び出しが必要）
    close,     // プロトコル違反（応答なしで即時切断が必要）
};

struct Inbound final {
    InboundAction action = InboundAction::close;
    std::size_t reply_size = 0U;    // respond 時の応答フレーム長
    bool close_after_reply = false; // respond 送信完了後に切断すべきか
    std::uint32_t code = 0U;        // エラーコード（正常時は 0）
};
```

---

## 4. API と利用手順

### 4.1 要求フレームの受付と分類 (`accept`)

完全な 1 フレーム（ヘッダー 20 バイト + ペイロード）を受信した時点で呼び出す。

```cpp
Inbound accept(const Frame& frame, ControlRequest& request,
               MutableByteView output) noexcept;
```

- `output`: 応答フレームを書き込むための出力バッファ。最大制御ペイロードに対応するため、`kHeaderSize + kMaxControlPayload`（20 + 65,536 = 65,556 バイト）以上のサイズが必要である。
- 戻り値に応じた処理：
  - `InboundAction::respond`: `output.data` に格納された `inbound.reply_size` バイトをソケットから送信する。HELLO の正常応答や各種エラー応答が含まれる。`close_after_reply` が true の場合は送信後にソケットを閉じる。送信完了後に `complete()` を呼び出す。
  - `InboundAction::dispatch`: `request` の内容に基づいて製品側のサービスロジック（受信機リースや選局など）を呼び出し、結果を応答フレームとしてエンコードして送信する。送信完了後に `complete()` を呼び出す。
  - `InboundAction::close`: 直ちにソケットを閉じる。

### 4.2 PID フィルタ機能のネゴシエーションゲート

`TUNE` 要求を処理する際、要求構造体内の `pids.present` が true（特定の PID フィルタリングが要求されている）であるにもかかわらず、HELLO ハンドシェイクで `PID_FILTER` 機能ビットがネゴシエートされていない場合、`accept()` はディスパッチを行わず、即座に出力バッファへ型付きの `UNSUPPORTED` エラー応答を生成し、`InboundAction::respond` を返す。

### 4.3 処理完了通知 (`complete`)

```cpp
void complete() noexcept;
```

応答フレームの送信が完了した時点で必ず呼び出す。これにより `outstanding_` 状態が解除され、接続は次の要求を受け入れ可能な待機状態に戻る。

---

## 5. 応答フレーム生成ヘルパー群

呼び出し元が `dispatch` された要求を処理した後、応答をワイヤフレームとして出力するための各種ヘルパー関数が提供されている。

### 5.1 汎用レコードフレーム生成 (`encode_record_frame`)

型付きレコードをエンベロープ付きフレーム（正常応答は `Kind::response`、エラーは `Kind::error_response`）としてエンコードする。

```cpp
template <typename Record>
Result<std::size_t> encode_record_frame(Kind kind, std::uint16_t opcode,
                                        std::uint32_t request_id, const Record& value,
                                        Result<std::size_t> (*encode)(const Record&,
                                                                      MutableByteView) noexcept,
                                        MutableByteView output) noexcept;
```

### 5.2 エラー応答フレーム生成 (`encode_error_frame`)

指定された要求 ID、エポック、エラーコード、操作名、および対象スコープから、エラー応答フレームを生成する。

```cpp
Result<std::size_t> encode_error_frame(std::uint16_t opcode, std::uint32_t request_id,
                                       const Uuid& epoch, std::uint32_t code,
                                       const char* operation, const Scope& target,
                                       MutableByteView output) noexcept;
```

### 5.3 STATUS 応答の段階的構築

STATUS 応答はサイズが大きく動的情報を含むため、以下のヘルパーを用いて段階的に安全に生成する：

1. **スコープ検証 (`check_status_scope`)**:
   要求された `Scope`（全機能、個別受信機、個別リーダー）がカノニカルな `ListResponse` に存在するか検証する。
2. **骨格の構築 (`build_status_shell`)**:
   カノニカルな LIST 情報、コンテキスト、スナップショット時刻から、不変部分（プロデューサ情報、トポロジ、カノニカルデスクリプタ一覧など）をあらかじめ設定した `StatusResponse` 構造体の骨格を生成する。動的フィールド（ロック状態、信号品質、受信機リース状態など）のみを呼び出し元が更新する。
3. **完全検証とエンコード (`encode_status_frame`)**:
   構築した `StatusResponse` を `validate_status_response` でカノニカル LIST と照合して厳格に検証した上でフレームを書き出す。未検証の不整合データがワイヤ上に出力されることはない。

### 5.4 レシーバリース状態 (`leased` 配列) 観測の意味と限界

受信機エンジン（`dtv::receiver`）の `ReceiverEngineStatus` には、各スロットのアクティブなリース有無を示す `leased` 真偽値配列が含まれている。
- **観測値の性質**: これはレジストリミューテックス保護下でコピーされたスロットごとのアクティブリース有無の真偽値（`lease.active` の投影）にすぎず、所有者識別子、lease ID、stream ID、トークンなどは保持しない。
- **ライフサイクルとの関係**: RELEASE 要求（`release_lease`）の開始時、第 1 のレジストリミューテックス区間では保留チケット破棄・現行チューン無効化・`cleanup` 操作状態の設定が行われるが、Lease 自体はこの時点では消去されない。その後スロットごとの `receiver_mutex` 取得を待機し、mutex 取得後の第 2 レジストリミューテックス区間で `Lease{}` に初期化された時点で初めて `false` となる。先行する別操作が `receiver_mutex` を保持している間は RELEASE 待機中であっても `leased=true` と観測され得る。また、`leased=false` は Lease 消去時点で現れ、その後のハードウェア detach や物理リソース解放の完了指標ではない。
- **適用範囲**: 本配列は STATUS 応答等の生成時に各スロットのリース有無を参照するために有用であるが、これ単体をもってストリームターゲットの同一性や世代を検証したり、公開ストリームライフサイクルの完成や物理的な静止（quiescence）を担保したりすることはできない。
