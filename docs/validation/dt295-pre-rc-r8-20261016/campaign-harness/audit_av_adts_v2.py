"""Offline 5-minute AV audit with strict ADTS framing and configuration epochs.

Only a single initial ADTS synchronization scan is allowed. Every later frame
must begin exactly where its predecessor declares; header configuration changes
start a new ffmpeg decoder epoch rather than being mistaken for byte corruption.
No elementary-stream files are written.
"""
import argparse
import json
import os
import subprocess
from pathlib import Path


class ADTSStream:
    MAX_FRAME = 8191

    def __init__(self):
        self.buf = bytearray()
        self.locked = False
        self.poisoned = None
        self.sync_bytes_omitted = 0
        self.total_es_bytes = 0
        self.frame_index = 0
        self.trailing_partial_bytes = 0

    @staticmethod
    def parse_header(data):
        if len(data) < 7:
            return None
        if data[0] != 0xff or data[1] & 0xf6 != 0xf0:
            raise ValueError("invalid ADTS sync/layer")
        sample_index = (data[2] >> 2) & 15
        if sample_index >= len(SAMPLE_RATES):
            raise ValueError("reserved ADTS sample-rate index")
        frame_len = ((data[3] & 3) << 11) | (data[4] << 3) | (data[5] >> 5)
        header_len = 7 if data[1] & 1 else 9
        if frame_len < header_len or frame_len > ADTSStream.MAX_FRAME:
            raise ValueError("invalid ADTS frame length")
        channels = ((data[2] & 1) << 2) | (data[3] >> 6)
        signature = ((data[1] >> 3) & 1, (data[2] >> 6) & 3, sample_index, channels)
        raw_blocks = (data[6] & 3) + 1
        return frame_len, header_len, signature, raw_blocks

    def feed(self, data):
        if self.poisoned:
            raise ValueError(self.poisoned)
        # Processing in small slices ensures no untrusted PES can inflate the
        # retained framing buffer; only one ADTS frame plus six header bytes is
        # needed between calls.
        frames = []
        view = memoryview(data)
        for off in range(0, len(view), 32768):
            self.total_es_bytes += min(32768, len(view) - off)
            self.buf.extend(view[off:off + 32768])
            try:
                self._consume(frames)
            except ValueError as exc:
                self.poisoned = str(exc)
                self.buf.clear()
                raise
        return frames

    def _consume(self, frames):
        while True:
            if not self.locked:
                found = None
                # At stream start only, skip bytes until a complete plausible
                # ADTS frame. No later byte is searched for sync.
                for i in range(max(0, len(self.buf) - 6)):
                    if self.buf[i] != 0xff:
                        continue
                    try:
                        h = self.parse_header(self.buf[i:i + 7])
                    except ValueError:
                        continue
                    if h is None:
                        break
                    frame_len = h[0]
                    if len(self.buf) - i < frame_len:
                        self.sync_bytes_omitted += i
                        del self.buf[:i]
                        return
                    found = (i, h)
                    break
                if found is None:
                    drop = max(0, len(self.buf) - 6)
                    self.sync_bytes_omitted += drop
                    del self.buf[:drop]
                    return
                pos, _ = found
                self.sync_bytes_omitted += pos
                del self.buf[:pos]
                self.locked = True
            if len(self.buf) < 7:
                return
            try:
                h = self.parse_header(self.buf)
            except ValueError as exc:
                raise ValueError("ADTS framing error after initial synchronization: " + str(exc))
            if h is None:
                return
            frame_len, header_len, signature, raw_blocks = h
            if len(self.buf) < frame_len:
                return
            start_offset = self.total_es_bytes - len(self.buf)
            frame = bytes(self.buf[:frame_len])
            del self.buf[:frame_len]
            index = self.frame_index
            self.frame_index += 1
            pce_first = header_len < frame_len and (frame[header_len] >> 5) == 5
            frames.append(dict(index=index, offset=start_offset,
                               signature=signature, raw_blocks=raw_blocks,
                               pce_first=pce_first, data=frame))

    def finish(self):
        self.trailing_partial_bytes = len(self.buf)
        if self.poisoned:
            raise ValueError(self.poisoned)
        if not self.locked:
            raise ValueError("no initial ADTS synchronization found")
        self.buf.clear()


SAMPLE_RATES = (96000, 88200, 64000, 48000, 44100, 32000, 24000,
                22050, 16000, 12000, 11025, 8000, 7350)


class AACDecodeAudit:
    def __init__(self, out):
        self.out = out
        self.epochs = []
        self.epoch = None
        self.next_log = 0
        self.transitions = []
        self.decode_failures = []
        self.frames_seen = 0
        self.frames_emitted = 0
        self.frames_before_pce = 0
        self.bytes_before_pce = 0
        self.cumulative_audio_seconds = 0.0

    def _finish_epoch(self):
        e = self.epoch
        if e is None:
            return
        e["pce_wait_pending"] = e["signature"][3] == 0 and not e["decoder_started"]
        if e["proc"] is not None:
            try:
                e["proc"].stdin.close()
            except BrokenPipeError:
                e["broken_pipe"] = True
            e["decode_exit"] = e["proc"].wait()
            e["log"].close()
            e["error_log_bytes"] = e["log_path"].stat().st_size
            if e["decode_exit"] != 0 or e["error_log_bytes"] or e["broken_pipe"]:
                self.decode_failures.append(e["epoch"])
        else:
            e.update(decode_exit=None, error_log_bytes=0, broken_pipe=False)
        e["broadcast_audio_seconds"] = e["stream_samples"] / e["sample_rate_hz"]
        self.cumulative_audio_seconds += e["broadcast_audio_seconds"]
        e.pop("proc", None)
        e.pop("log", None)
        e.pop("log_path", None)
        self.epochs.append(e)
        self.epoch = None

    def _new_epoch(self, frame):
        if self.epoch is not None:
            old = self.epoch
            self._finish_epoch()
            self.transitions.append(dict(frame_index=frame["index"], es_byte_offset=frame["offset"],
                                         broadcast_audio_time_seconds=round(self.cumulative_audio_seconds, 6),
                                         from_signature=list(old["signature"]),
                                         to_signature=list(frame["signature"])))
        sig = frame["signature"]
        e = dict(epoch=len(self.epochs), signature=sig,
                 mpeg_id=sig[0], profile=sig[1], sample_rate_index=sig[2],
                 sample_rate_hz=SAMPLE_RATES[sig[2]], channel_configuration=sig[3],
                 first_frame_index=frame["index"], first_es_byte_offset=frame["offset"],
                 last_frame_index=frame["index"], last_es_byte_offset=frame["offset"],
                 complete_frames=0, frames_before_pce=0, bytes_before_pce=0,
                 stream_samples=0, raw_data_block_counts={},
                 pce_frame_index=None, pce_es_byte_offset=None,
                 decoder_started=False, proc=None)
        self.epoch = e

    def _start_decoder(self, frame):
        e = self.epoch
        log_path = self.out / f"aac-epoch-{e['epoch']:03d}.log"
        log = log_path.open("wb")
        proc = subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "error",
                                 "-f", "aac", "-i", "pipe:0", "-f", "null", "-"],
                                stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                stderr=log)
        e.update(proc=proc, log=log, log_path=log_path,
                 decoder_started=True,
                 broken_pipe=False,
                 pce_frame_index=frame["index"] if e["signature"][3] == 0 else None,
                 pce_es_byte_offset=frame["offset"] if e["signature"][3] == 0 else None)

    def feed(self, frame):
        self.frames_seen += 1
        if self.epoch is None or frame["signature"] != self.epoch["signature"]:
            self._new_epoch(frame)
        e = self.epoch
        e["complete_frames"] += 1
        e["stream_samples"] += frame["raw_blocks"] * 1024
        e["raw_data_block_counts"][str(frame["raw_blocks"])] = (
            e["raw_data_block_counts"].get(str(frame["raw_blocks"]), 0) + 1)
        e["last_frame_index"] = frame["index"]
        e["last_es_byte_offset"] = frame["offset"]
        if not e["decoder_started"]:
            if e["signature"][3] == 0 and not frame["pce_first"]:
                e["frames_before_pce"] += 1
                e["bytes_before_pce"] += len(frame["data"])
                self.frames_before_pce += 1
                self.bytes_before_pce += len(frame["data"])
                return
            self._start_decoder(frame)
        try:
            e["proc"].stdin.write(frame["data"])
            e["frames_emitted"] = e.get("frames_emitted", 0) + 1
            self.frames_emitted += 1
        except BrokenPipeError:
            e["broken_pipe"] = True

    def finish(self, stream):
        self._finish_epoch()
        if not stream.locked:
            self.decode_failures.append("no-adts-sync")
        if any(e["pce_wait_pending"] for e in self.epochs):
            self.decode_failures.extend(e["epoch"] for e in self.epochs if e["pce_wait_pending"])
        return dict(epochs=self.epochs, transitions=self.transitions,
                    frames_seen=self.frames_seen, frames_emitted=self.frames_emitted,
                    frames_before_pce=self.frames_before_pce,
                    bytes_before_pce=self.bytes_before_pce,
                    initial_sync_bytes_omitted=stream.sync_bytes_omitted,
                    trailing_partial_bytes=stream.trailing_partial_bytes,
                    decode_failures=sorted(set(self.decode_failures)))


def _write_video(row, es, video_codec):
    if not row["started"]:
        combined = row["probe"] + es
        if video_codec == "mpegvideo":
            offset = combined.find(b"\0\0\1\xb3")
        else:
            offset = next((i for i in range(len(combined) - 3)
                           if combined[i:i + 3] == b"\0\0\1" and combined[i + 3] & 31 == 7), -1)
        if offset < 0:
            row["leading_bytes_omitted"] += max(0, len(combined) - 3)
            row["probe"] = combined[-3:]
            return
        row["leading_bytes_omitted"] += offset
        row["started"] = True
        row["probe"] = b""
        es = combined[offset:]
    if es and not row["broken_pipe"]:
        try:
            row["proc"].stdin.write(es)
        except BrokenPipeError:
            row["broken_pipe"] = True


def _parse_pes(pid, begin, pes, state, video_codec):
    row = state["rows"][pid]
    if len(pes) < 9 or pes[:3] != b"\0\0\1" or 9 + pes[8] > len(pes):
        state["invalid"].append(dict(pid=pid, start_packet=begin, bytes=len(pes)))
        return
    declared = int.from_bytes(pes[4:6], "big")
    if declared and declared + 6 != len(pes):
        state["violations"].append(dict(pid=pid, start_packet=begin,
                                        actual=len(pes), expected=declared + 6))
    es = bytes(pes[9 + pes[8]:])
    row["complete_pes"] += 1
    row["es_bytes"] += len(es)
    if row["kind"] == "video":
        _write_video(row, es, video_codec)
        return
    if row["audio_poisoned"]:
        return
    try:
        frames = row["stream"].feed(es)
        for frame in frames:
            row["audio"].feed(frame)
    except ValueError as exc:
        row["audio_poisoned"] = str(exc)


def audit(directory, video=256, audio=272, label="adts-v2-reviewed", packets=None,
          video_codec="mpegvideo"):
    os.umask(0o077)
    out = directory / label
    out.mkdir(mode=0o700, exist_ok=False)
    limit = packets if packets is not None else (directory / "capture.ts").stat().st_size // 188
    rows = {}
    for pid, codec, kind in ((video, video_codec, "video"), (audio, "aac", "aac")):
        if kind == "aac":
            rows[pid] = dict(pid=pid, kind=kind, complete_pes=0, es_bytes=0)
            rows[pid]["stream"] = ADTSStream()
            rows[pid]["audio"] = AACDecodeAudit(out)
            rows[pid]["audio_poisoned"] = None
        else:
            log_path = out / f"{pid}.log"
            log = log_path.open("wb")
            proc = subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "error",
                                     "-f", codec, "-i", "pipe:0", "-progress",
                                     str(out / f"{pid}.progress"), "-f", "null", "-"],
                                    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                    stderr=log)
            rows[pid] = dict(pid=pid, kind=kind, complete_pes=0, es_bytes=0,
                             started=False, probe=b"", proc=proc, log=log,
                             log_path=log_path, broken_pipe=False,
                             leading_bytes_omitted=0)
    state = dict(rows=rows, invalid=[], violations=[])
    active, packet = {}, 0
    try:
        with (directory / "capture.ts").open("rb") as f:
            while packet < limit:
                block = f.read(188 * min(8192, limit - packet))
                if not block:
                    raise ValueError("input shorter than frozen packet count")
                if len(block) % 188:
                    raise ValueError("partial TS packet")
                for off in range(0, len(block), 188):
                    q = block[off:off + 188]
                    packet += 1
                    pid = ((q[1] & 31) << 8) | q[2]
                    if pid not in rows or not q[3] & 16:
                        continue
                    start = 5 + q[4] if q[3] & 32 else 4
                    if start > 188:
                        raise ValueError("adaptation field overrun")
                    if q[1] & 64:
                        if pid in active:
                            _parse_pes(pid, *active.pop(pid), state, video_codec)
                        active[pid] = (packet, bytearray())
                    if pid in active:
                        active[pid][1].extend(q[start:])
                        if len(active[pid][1]) > 16 * 1024**2:
                            raise ValueError("PES exceeds bounded 16MiB")
    finally:
        for row in rows.values():
            if row["kind"] == "video":
                try:
                    row["proc"].stdin.close()
                except BrokenPipeError:
                    row["broken_pipe"] = True
                row["decode_exit"] = row["proc"].wait()
                row["log"].close()
                row["error_log_bytes"] = row["log_path"].stat().st_size
    for row in rows.values():
        for key in ("proc", "log", "log_path"):
            row.pop(key, None)
    result_rows = []
    for row in rows.values():
        if row["kind"] == "aac":
            try:
                row["stream"].finish()
            except ValueError as exc:
                row["audio_poisoned"] = row["audio_poisoned"] or str(exc)
            row["adts"] = row["audio"].finish(row["stream"])
            row["adts"]["framing_error"] = row["audio_poisoned"]
        result_rows.append({k: v for k, v in row.items()
                            if k not in ("kind", "probe", "stream", "audio", "audio_poisoned")})
    # Pending PES packets are intentionally omitted at capture end.
    result = dict(streams=result_rows, pes_length_violations=state["violations"],
                  invalid_pes_headers=state["invalid"], packets_examined=packet,
                  requested_packets=limit, final_open_pes_omitted=True,
                  interior_corruption_preserved=True)
    valid = not state["violations"] and not state["invalid"]
    for r in result_rows:
        if rows[r["pid"]]["kind"] == "video":
            valid = valid and r["complete_pes"] > 0 and r["decode_exit"] == 0
            valid = valid and r["error_log_bytes"] == 0 and not r["broken_pipe"]
        else:
            valid = valid and r["complete_pes"] > 0
            valid = valid and r["adts"]["framing_error"] is None
            valid = valid and not r["adts"]["decode_failures"]
            valid = valid and all(e["decode_exit"] == 0 and e["error_log_bytes"] == 0
                                  and not e["broken_pipe"]
                                  for e in r["adts"]["epochs"] if e["decode_exit"] is not None)
    result["valid"] = valid
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"valid": result["valid"], "packets_examined": packet,
                      "pes_length_violations": len(state["violations"]),
                      "invalid_pes_headers": len(state["invalid"]),
                      "streams": [{"pid": r["pid"], "complete_pes": r["complete_pes"],
                                   "decode_exit": r.get("decode_exit"),
                                   "error_log_bytes": r.get("error_log_bytes"),
                                   "adts_frames": r.get("adts", {}).get("frames_seen"),
                                   "config_epochs": len(r.get("adts", {}).get("epochs", [])),
                                   "config_transitions": r.get("adts", {}).get("transitions", []),
                                   "framing_error": r.get("adts", {}).get("framing_error")}
                                  for r in result_rows]}, ensure_ascii=False))
    return result


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("directory", type=Path)
    ap.add_argument("--video", type=int, default=256)
    ap.add_argument("--audio", type=int, default=272)
    ap.add_argument("--video-codec", choices=("mpegvideo", "h264"), default="mpegvideo")
    ap.add_argument("--label", default="adts-v2-reviewed")
    ap.add_argument("--packets", type=int)
    a = ap.parse_args()
    result = audit(a.directory, a.video, a.audio, a.label, a.packets, a.video_codec)
    raise SystemExit(0 if result["valid"] else 1)
