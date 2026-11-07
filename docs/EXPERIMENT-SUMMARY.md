# PIX-DT295 + denpa 実験総括

最終更新: 2026-11-01

## 1. 目的

PIX-DT295 チューナーのオンボード H.264 変換を Raspberry Pi + denpa で実用化し、
denpa をフォークして画質プリセットを UI から選べるようにする。

## 2. 達成したこと

### 2.1 オンボード H.264 変換 (pix-userland)

- 4 プリセット + passthrough を実機で実測済み:
  - `720p8m`: 1280×720 Main@3.1, 7,644 kbps
  - `720p4m`: 1280×720, 3,508 kbps
  - `480p15`: 720×480, 1,248 kbps
  - `240p03`: 320×240, 268 kbps
  - `passthrough`: MPEG-2 そのまま
- プリセット切替: `~/dt295-transcode-profile.sh` + エージェント `http://192.168.100.2:25252/presets`
- PSI 中断原因 (同一バージョン PMT 書き換え) の特定と再取り込み実装
- Unit B (VA29037030) を録画用、Unit A (VA07036111) を実験用に隔離

### 2.2 denpa フォーク (rk2kk/denpa)

- プリセット UI: コーデック選択肢に `チューナー: 720p 8Mbps` 等を統合
- `/api/tuner-preset` GET/POST エンドポイント
- `liveCodecs()` 統合 (タブレットでも同じ表示)
- **映像コピー経路** (`DENPA_LIVE_COPY=1`): チューナーの H.264 を `-c:v copy` で写す
  - CPU: 90.8% → 4-10% (1 コア)
  - 系全体: 45% → 25%
  - 画面: 放送から -0.4 秒
  - 音声は AAC で再エンコード (esds に AudioSpecificConfig が必要なため)
  - フォールバック: コピーが 0 パケットなら自動でエンコードへ
- denpa を v2.20.0 に更新 (upstream 116 コミットマージ)
- 字幕 PID のオプショナル化 (`?` 付き map)
- デコードエラー無視 + 破損パケット破棄 (`-err_detect ignore_err`, `-fflags +discardcorrupt`)
- 字幕なしでの再試行 (初回字幕失敗が致命的だった問題を修正)

### 2.3 pix-userland

- PR #40 作成 (e4dc18b)、checks 0 件
- マージ: upstream (Khronos31) の 243 コミットを統合 (v0.1.1〜v0.1.4)
- DT295 live policy (dt295_live_policy.c) の PSI 許容ロジックを CS にも適用
- CS H.264 トランスコードの既知問題を docs/dt295.md に文書化

## 3. 試行したが解決できなかった問題

### 3.1 CS (スカパー) の H.264 トランスコード

**症状**: CS 視聴時に途切れ (stuttering) が発生。GR/BS は正常。

**調査結果**:
1. CS の H.264 ストリームは SPS/PPS/IDR を **ストリーム開始時のみ送信**（GOP 3 秒以上）
2. **5.1ch AAC オーディオが corrupt** (`Decode error rate 1 exceeds maximum 0.666667`)
3. **複数 MPEG-2 プログラムの混在** → PMT 書き換えが不完全
4. デコードエラーが連続発生 (`Invalid data found when processing input`)

**試した対策 (全て不成功)**:
- IDR キャッシュ + 先送り → mid-GOP フレームの参照関係で効果なし
- 新規 lease で CS → コピーは字幕 PID で失敗
- 字幕 PID オプショナル化 → コピーは通るがデコードエラーは残る
- デコードエラー無視 + 破損パケット破棄 → 途切れ軽減せず
- レジスタ調整 (V_PID_OUT=0x1FFD, PMT_MODE=7) → 効果なし
- SPS/PPS 偽装 → 初期化は早まるが途切れは解消しない
- upstream pix-userland 更新 (v0.1.4) → PMT 処理改善されたが CS の SPS/PPS 未送信は解消せず

**結論**: CS の H.264 トランスコードは tuner ファームウェア (`trc_m_h.bin`) の CS 固有のバグ。pix-userland/denpa/agent では解決不可。

**解決策**: CS 視聴時は passthrough (MPEG-2) を使用。tuner が MPEG-2 を出力し、denpa がクリーンな入力から H.264 に再エンコード（途切れなし）。

### 3.2 CS の途切れ (passthrough 後)

passthrough 後も denpa で CS が視聴できない問題が発生。原因:
- denpa の service filter が CS の複数プログラム TS を正しく処理できない
- 5.1ch AAC のデコードエラー

**対策**: CS 視聴時は denpa で MPEG-2 コーデックを選択（WASM デコーダ）。H.264 を選ぶと Pi が再エンコード（入力クリーン、途切れなし）。

### 3.3 BonDriver for PIX-DT295

- 存在しない。EDCB/TVTest エコシステムは PT1/PT2/PT3 等をサポート
- pix-userland から BonDriver を作成することは理論上可能だが、大規模プロジェクト（数週間〜数ヶ月）
- 現在の Pi + denpa 構成が BonDriver 不要で TV 視聴を実現

### 3.4 TBS6281SEv2_drv からの参考

- TBS6281SE は DVB-T2/C 対応の PCIe チューナー（欧州/中国向け）
- PIX-DT295 は ISDB-T/S 対応の USB チューナー（日本向け）
- ハードウェア・ドライバ・プロトコルが完全に異なるため、直接的な参考にはならない
- TBS6281SEv2_drv の forks: 0 件

## 4. 現在の構成

| コンポーネント | 状態 |
|---|---|
| Raspberry Pi 5 | 稼働中 |
| tuner-agent | dt295-agent:local (0.3.0-dt295) |
| denpa | denpa-fork:prod (v2.20.0 + DT295 パッチ) |
| pix-ts-legacy | v0.1.4 (upstream merge 済み) |
| CS プロファイル | passthrough (MPEG-2) |
| GR/BS プロファイル | 720p8m (H.264, 7,644 kbps) |
| DENPA_LIVE_COPY | 1 (有効) |

## 5. 推奨事項

1. CS 視聴時は passthrough を維持し、denpa で MPEG-2 または H.264 を選択
2. GR/BS は H.264 トランスコード + コピー経路で低負荷・高画質
3. BonDriver の開発は現実的ではないため、Pi + denpa 構成を継続
4. tuner ファームウェアの CS バグは PIXELA がサポート終了のため修正不可