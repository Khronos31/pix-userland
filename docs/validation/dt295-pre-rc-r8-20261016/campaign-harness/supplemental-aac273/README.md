# Supplemental bounded AAC window audit

This isolated utility audits one explicitly requested window in a saved MPEG-TS file. It imports `ADTSStream` and `AACDecodeAudit` from the frozen parent `audit_av_adts_v2.py` without modifying that file. It does not decode video, read device state, or perform a whole-file scan.

Example invocation after the parent chooses a packet range:

```sh
python3 audit-aac-window.py /path/to/stage/capture.ts \
  --pid 273 --start-packet 107887245 --packet-count 65536 \
  --output /path/to/new-private-output
```

`--start-packet` is a zero-based packet offset. `--packet-count` is required and hard-capped at 65,536; the implementation seeks directly to `start_packet * 188` and reads exactly that range. Each invocation requires a new output directory. It writes metadata to `result.json` and private, per-epoch decoder stderr logs capped at 1 MiB each. It never writes an elementary-stream or AAC file and sends decoder stdout to `/dev/null`.

The verdict is one of `decoded_pass`, `absent`, `inconclusive`, or `fail`. `decoded_pass` requires at least one complete PES, at least one emitted ADTS frame, and clean transport/PES/ADTS/decoder checks. No frames can never pass. A missing PID is `absent`; a short or boundary-clipped stream without decoded frames is `inconclusive`. Interior framing, continuity, PES-length, TEI, scrambling, or decoder errors are `fail`. The first partial PES and final open/partial PES are counted as window-boundary evidence, not silently merged into the decoded span. Initial ADTS synchronization bytes and PCE wait state are reported separately.

After the existing whole-file audit has finished, select packet offsets from recorded phase-2 status samples, not an assumed bitrate. The intended bounded evidence points are the first packets after the committed addition, either side of the known PID 1411 configuration transition around 10,049.6–10,659.5 seconds, at least 30 minutes after addition, and near the terminal packet count. The PID 1411 configuration marker is only a timing aid: it does not establish PID 273 membership. A PID 273 appearance/disappearance boundary is assessed only when complete targeted PMTs show it on both sides. If no such transition is observed in the selected ranges, Issue #2 boundary coverage remains `unobserved`; sampled windows cannot prove that no transient transition occurred elsewhere.

No production recording was opened or analyzed while creating this utility. Synthetic tests use generated silent AAC only and remove their temporary data on completion.
