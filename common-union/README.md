# DTV ユーザー空間共通基盤 (dtv-userland-core)

本リポジトリは、各種 DTV ユーザー空間ドライバ・デーモン（`px4-userland`、`asicen-userland`、`pix-userland`、`siano-userland` など）で共有される C++17 共通基盤ライブラリを提供する。エラー/Result 型、ロギング、モックトランスポート、UDTV 1.0 ワイヤ層、デバイス識別子コーデック、選局解決器、制御接続ディスパッチャ、および内部互換エンジン群（受信機管理、カード所有権管理、パケットストリーム蓄積）で構成される。

本ライブラリは独立したデーモン、ハードウェアトランスポート、完全なスマートカードプロトコルスタック、あるいは特定ハードウェアのアダプタそのものではない。各製品が固定のソースコミット（本仕様が基準とする C++ コード基盤コミット: `643b5af25aed20210ba45a30eed41ec3bbc3739c`、tree `667ae3c1db3268339d1fe795573eeedfe599ed7d`。ドキュメント統合や後続のクライアント統合等によってリポジトリの最新 HEAD は前進するため、コード基準点と動的な最新コミットは区別される）を参照し、ビルド時に静的ライブラリとしてリンクして利用する。

---

## 1. コンポーネント構成とターゲット

本リポジトリは、常設の基本ターゲット（コア export）と、必要に応じて有効化するオプトインの内部エンジンターゲットに分かれている。

### 1.1 常設ターゲット (デフォルトで常時ビルド・提供)

常設ターゲットはいずれも例外（`-fno-exceptions` / `/EHs-c-`）および RTTI（`-fno-rtti` / `/GR-`）を無効化してコンパイルされ、オペレーティングシステム固有 API や USB ライブラリへの依存を持たない。

| ターゲット名 | CMake エクスポート名 | リンク依存関係 | 主な役割 |
|---|---|---|---|
| `dtv_userland_core` | `dtv::core` | なし | 基盤: `Error` / `Result<T>` 型、軽量ロギング、抽象トランスポート Seam、モックトランスポート |
| `dtv_userland_wire` | `dtv::wire` | `dtv::core` | UDTV 1.0 ワイヤ層: 20 バイト固定エンベロープ、断片化対応フレーム受信機（`FrameReceiver`）、型付きレコードコーデック（`records.h`） |
| `dtv_userland_identity` | `dtv::identity` | `dtv::core` | 識別子コーデック: `serial:` / `usb:` セレクタ構文解析、インスタンス名正規化、LANGID / USB 文字列デスクリプタ復号、`DTVI` 自動識別符号化 |
| `dtv_userland_tuning` | `dtv::tuning` | `dtv::wire` | 選局入力解決器: チャンネル文字列（地上波・BS・CS）または周波数（Hz）を入力とし、放送波種別・要求 Hz・スロット/TSID セレクションを純粋解決 |
| `dtv_userland_json` | `dtv::json` | `dtv::wire` | JSON シリアライザ: 検証済み `StatusResponse` をコンパクトな UTF-8 JSON 文字列へ変換（動的メモリ確保なし、バッファ境界保証） |
| `dtv_userland_control` | `dtv::control` | `dtv::wire` | 制御接続シリアライザ: UDTV 1.0 制御接続のハンドシェイク、単一未完了要求管理、6 操作（HELLO/LIST/STATUS/ACQUIRE/RELEASE/TUNE）のデコードと応答生成 |

### 1.2 オプトイン内部エンジンターゲット (デフォルト OFF)

特定の製品アダプタが内部実装を共有するために用いるコンポーネントであり、必要な製品ビルドでのみ CMake オプションを指定して有効化する。

| ターゲット名 | CMake オプション | 依存関係 | 主な役割 |
|---|---|---|---|
| `dtv_userland_receiver` (`dtv::receiver`) | `DTV_USERLAND_BUILD_RECEIVER` (既定: OFF) | `dtv::core`, `Threads::Threads` | 受信機ライフサイクル互換エンジン: 受信機リース、選局管理、ストリーム接続状態マシン、スロットのリース有無（`leased` 配列）の観測を提供 |
| `dtv_userland_capture` (`dtv::capture`) | `DTV_USERLAND_BUILD_CAPTURE` (既定: OFF) | `dtv::core`, `Threads::Threads` | パケットストリームセッション互換ストレージ: 188 バイト TS パケットのリングバッファ、世代チケット（`incarnation`）による遅延リーダー保護、終了状態ドレイン管理 |
| `dtv_userland_card` (`dtv::card`) | `DTV_USERLAND_BUILD_CARD` (既定: OFF) | `dtv::core` (INTERFACE) | カード所有権管理エンジン: 複数セッション間でのスマートカード所有権調停、トランザクション排他、ATR キャッシュ管理（ヘッダーオンリー） |

- **スレッド依存性**: `dtv::receiver` および `dtv::capture` のみが `Threads::Threads`（C++11 スレッド・同期プリミティブ）をリンクする。
- **キャプチャとレシーバの独立性**: `dtv::capture` は `dtv::receiver` に依存しない。そのため、受信機エンジンを無効化したキャプチャ単独（capture-only）の構成が可能である。

---

## 2. CMake によるビルドと利用方法

### 2.1 ビルドとテスト

本リポジトリ単体でのビルドおよび単体テストの実行は CMake を用いて行う。

```sh
# 基本ターゲットおよび単体テストのビルド
cmake -B build -S . \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON

cmake --build build
ctest --test-dir build --output-on-failure
```

オプションの内部エンジンを含む全テストをビルドする場合は、対象オプションを有効化する。

```sh
cmake -B build-all -S . \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON \
    -DDTV_USERLAND_BUILD_RECEIVER=ON \
    -DDTV_USERLAND_BUILD_CAPTURE=ON \
    -DDTV_USERLAND_BUILD_CARD=ON

cmake --build build-all
ctest --test-dir build-all --output-on-failure
```

### 2.2 製品側でのインポート (`find_package`)

インストール済みの本共通基盤パッケージを利用する場合、製品側の `CMakeLists.txt` で `find_package` を呼び出す。

```cmake
# 常設ターゲットの利用 (core, wire, identity, tuning, json, control がインポートされる)
find_package(dtv_userland_core CONFIG REQUIRED)

# オプションコンポーネントを要求する場合
find_package(dtv_userland_core CONFIG REQUIRED COMPONENTS receiver capture control card)
```

製品アダプタのターゲットに対しては、必要なライブラリをリンクする。

```cmake
target_link_libraries(my_daemon PRIVATE
    dtv::core
    dtv::wire
    dtv::identity
    dtv::tuning
    dtv::json
    dtv::control
    dtv::receiver  # COMPONENTS receiver 指定時
    dtv::capture   # COMPONENTS capture 指定時
    dtv::card      # COMPONENTS card 指定時
)
```

---

## 3. 主要サブシステムの設計契約

### 3.1 制御接続ディスパッチャ (`dtv::control`)

`ControlConnection`（`include/dtv/userland/control_connection.h`）は、1 つの制御接続に 1 つ割り当てられ、呼び出し元プロセスのイベントループから駆動されるシリアライザ兼要求分類器である。
自らはソケットやタイマーを保持せず、プロトコル規約（HELLO 先頭必須、単一未完了要求ハンドシェイク、厳格な要求 ID 単調増加、エポック一致検証）を強制する。
受入済みの 6 操作（`HELLO`, `LIST`, `STATUS`, `ACQUIRE`, `RELEASE`, `TUNE`）をサポートし、HELLO は内部で直接応答フレームを生成して返し、残る 5 操作を所有型の `ControlRequest` にデコードしてディスパッチする。TUNE において PID フィルタが指定された際にハンドシェイクで `PID_FILTER` が合意されていなければ `UNSUPPORTED` エラーを即座に返す。

実製品 `px4-userland` のデーモン実装において、本モジュールは同一のポーリング/ワーカ/エンジン経路へ 6 操作が接続された内部増分（increment）として機能する。ただし、この制御エンドポイントにおいて `CARD`、ストリームデータプレーン、および既存クライアントの移行は未完成であり、`START`/`STOP`/`ATTACH`/`CARD` 操作や旧クライアントの完全な動作を保証するものではない。
詳細な仕様と利用方法は [docs/control-connection.md](docs/control-connection.md) を参照のこと。

### 3.2 パケットストリームセッション蓄積 (`dtv::capture`)

`PacketStreamSession`（`include/dtv/userland/detail/packet_stream_session.h`）は、188 バイト固定長の完全 TS パケットをスレッド安全にキューイングし、リーダーへ提供する共通ストレージである。
`Attachment` による完全一致判定、世代番号（`incarnation`）を記録する `ReadTicket` と 3 段階の読み取りヘルパー（`begin_read_locked`, `read_wake_locked`, `finish_read_locked`）による遅延リーダー保護、およびセッション終了時の安全なドレイン機能を提供する。
本コンポーネントは内部パケット蓄積を担当するものであり、プロデューサスレッドの完全な静止（quiescence）や借用ファイルディスクリプタの返却保証、あるいは最終的な v1 ストリーム契約の完了を意味するものではない。
詳細は [docs/capture.md](docs/capture.md) を参照のこと。

### 3.3 受信機ライフサイクルと選局エントランス (`dtv::receiver`)

`ReceiverEngine`（`include/dtv/userland/detail/receiver_engine.h`）は、最大 8 スロットの受信機リース、状態遷移、および選局を管理する内部互換エンジンである。
選局処理には以下の 2 つの系統（`TuneEntrance`）が用意されている：

1. **厳密周波数選局 (`TuneEntrance::strict_hz`)**:
   `HzTuneRequest` を介して正確な 64 ビット周波数（Hz 単位、1..2^53-1）を受け取るエントランス。呼び出し元がプロファイルに基づいて事前検証した結果を消費し、受信機ごとに定められたレディネス検証ポリシー（`lock_poll`、`terminal_lock`、`commands`、`ts_sync`）に従ってバックエンド選局を実行する。
2. **従来互換選局 (`TuneEntrance::legacy_khz`)**:
   従来の kHz 単位（`frequency_khz`）を受け取る互換エントランス。従来の選局コールバックをそのまま呼び出す。

また、`ReceiverEngineStatus` には各スロットのアクティブなリース有無を示す `leased` 真偽値配列が含まれており、外部から参照できる。

### 3.4 スマートカード所有権管理 (`dtv::card`)

`CardOwnershipEngine`（`include/dtv/userland/detail/card_ownership_engine.h`）は、B-CAS / スマートカードに対する排他的なセッション所有権、トランザクション管理、カード抜き差し検知、ATR キャッシュ管理を提供するテンプレートクラスである。
本エンジンは抽象化されたバイト列およびセッション Seam を扱い、ハードウェア制御や特定カードプロトコル（T=1 等）の実装は製品側アダプタに委ねられている。

### 3.5 ワイヤ層およびレコードコーデック (`dtv::wire`)

UDTV 1.0 プロトコルのバイナリ表現を処理する。
- 20 バイト固定長のエンベロープ仕様（マジック `UDTV`、メジャー 1、マイナー 0）およびペイロード上限（TS_DATA: 1,048,504 バイト、制御系: 65,536 バイト）の詳細は [docs/wire-subset.md](docs/wire-subset.md) を参照。
- 型付きレコード群（HELLO, ACQUIRE, LIST, STATUS, TUNE, RELEASE, イベント, カードレコード等）のバイト表現、コンテキスト検証、および TS_DATA の例外仕様は [docs/wire-records.md](docs/wire-records.md) を参照。

### 3.6 デバイス識別子コーデック (`dtv::identity`)

製品スラグ、シリアル番号（`serial:<string>`）、USB トポロジ（`usb:<path>[,<path>...]`）から正規化されたデバイスインスタンス名（`auto-<32hex>`、16 バイト短縮ダイジェストの 32 桁 16 進表現、接頭辞込み計 37 文字）を決定論的に導出する。
構文仕様および復号ルールは [docs/identity.md](docs/identity.md) を参照。

### 3.7 選局入力解決器 (`dtv::tuning`)

CLI や設定ファイルから与えられるチャンネル文字列や周波数入力を、ワイヤ上のシステム種別、要求 Hz、スロット/TSID セレクションへ変換する純粋解決器である。
文法定義と境界条件は [docs/tuning.md](docs/tuning.md) を参照。

---

## 4. ライセンスと出所の境界

本共通基盤に含まれる承認済みの基本モジュール、ワイヤ層実装、および新規追加されたビルド・パッケージング・テスト・ドキュメントファイル群には、ルートの **MIT ライセンス**（SPDX-License-Identifier: MIT）が適用される。

このライセンス適用は、本リポジトリ内の承認済みソース範囲に限定され、各製品リポジトリ（`px4-userland`、`asicen-userland` 等）の GPL-2.0-only コード、未記載のソースファイル、サードパーティ製素材、あるいは将来の GPL アダプタを再ライセンスするものではない。
各ファイルの詳細な出所、ハッシュ値、および除外範囲については [PROVENANCE.json](PROVENANCE.json) に記録されている。
