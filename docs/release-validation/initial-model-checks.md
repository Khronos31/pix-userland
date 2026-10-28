# PIX 初回機種チェック

この初回手順はPIXの対象全機種に適用する。各機種について代表Linux x86_64 glibcで2時間連続受信し、その機種で毎release回帰項目と必要時項目を実施する。機種ごとに結果を分け、他機種の結果を継承しない。通常の無指定時検証機種はPIX-DT295とPIX-DT181-PU0だが、初回対象の限定ではない。今後追加される機種にも同じ共通手順を適用する。

## PIX-DT295

| 項目 | 実施内容と結果 | 状態 |
|---|---|---|
| 連続受信 | macOS arm64 local candidate `c21f526` / Core `154e9de`でT27を30分連続取得。client exit 0、19,239,695 packets、USB errors 0。raw TSは`/dev/null`で、payload qualityは解析していない。 | Mac 30分の一部のみ。Linux x86_64 2時間初回条件は未実施 |
| cleanup / 再受信 | daemon close時`USB_IO`。続く10秒captureはexit 7、0 bytes。 | 未達 |

## PIX-DT181-PU0

1.0.0 candidateでの初回チェックは未実施。旧baseline手順はruntime path contextにより`INVALID_ARGUMENT`となったため、今回candidateの実績として扱わない。

## 共通の未達

両機種のLinux x86_64 2時間連続TS、毎release回帰項目、必要時項目、final candidate archiveによる各OS/ABI回帰は未実施。初回機種チェックは両機種とも未認定とする。
