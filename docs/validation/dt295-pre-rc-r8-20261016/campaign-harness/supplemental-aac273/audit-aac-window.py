#!/usr/bin/env python3
"""Audit AAC/PES framing in one explicitly bounded saved-TS packet window."""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

PACKET_SIZE = 188
MAX_PACKET_COUNT = 65536
MAX_PES_BYTES = 16 * 1024 * 1024
MAX_EPOCHS = 16
MAX_STDERR_BYTES_PER_EPOCH = 1024 * 1024
WINDOW_TIMEOUT_SECONDS = 90
PROCESS_CLOSE_TIMEOUT_SECONDS = 10

PARENT = Path(__file__).resolve().parent.parent
FROZEN_AUDIT = PARENT / "audit_av_adts_v2.py"
_SPEC = importlib.util.spec_from_file_location("frozen_audit_av_adts_v2", FROZEN_AUDIT)
if _SPEC is None or _SPEC.loader is None:
    raise RuntimeError("cannot load frozen audit_av_adts_v2.py")
_AV = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_AV)


class BoundedAACDecodeAudit(_AV.AACDecodeAudit):
    """Reuse the frozen frame/PCE accounting and decoder process a bounded way."""

    def __init__(self, out: Path):
        super().__init__(out)
        self.out = out
        self.processes = []
        self._lock = threading.Lock()
        self.timed_out = False
        self._watchdog = threading.Timer(WINDOW_TIMEOUT_SECONDS, self._kill_all)
        self._watchdog.daemon = True
        self._watchdog.start()

    def _kill_all(self):
        self.timed_out = True
        with self._lock:
            processes = list(self.processes)
        for proc in processes:
            if proc.poll() is None:
                try:
                    proc.kill()
                except OSError:
                    pass

    @staticmethod
    def _drain_stderr(pipe, log, state):
        try:
            while True:
                chunk = pipe.read(8192)
                if not chunk:
                    break
                state["total"] += len(chunk)
                remaining = max(0, MAX_STDERR_BYTES_PER_EPOCH - state["retained"])
                if remaining:
                    part = chunk[:remaining]
                    log.write(part)
                    state["retained"] += len(part)
                state["truncated"] += max(0, len(chunk) - remaining)
        finally:
            pipe.close()

    def _start_decoder(self, frame):
        if len(self.epochs) + int(self.epoch is not None) > MAX_EPOCHS:
            raise ValueError("AAC configuration epoch limit exceeded")
        e = self.epoch
        log_path = self.out / f"aac-epoch-{e['epoch']:03d}.log"
        log = log_path.open("xb")
        proc = subprocess.Popen(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "aac",
             "-i", "pipe:0", "-f", "null", "-"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            bufsize=0, start_new_session=True)
        state = {"total": 0, "retained": 0, "truncated": 0}
        reader = threading.Thread(target=self._drain_stderr,
                                  args=(proc.stderr, log, state), daemon=True)
        reader.start()
        e.update(proc=proc, log=log, log_path=log_path,
                 stderr_reader=reader, stderr_state=state,
                 decoder_started=True, broken_pipe=False,
                 pce_frame_index=frame["index"] if e["signature"][3] == 0 else None,
                 pce_es_byte_offset=frame["offset"] if e["signature"][3] == 0 else None)
        with self._lock:
            self.processes.append(proc)

    def _finish_epoch(self):
        e = self.epoch
        if e is None:
            return
        e["pce_wait_pending"] = e["signature"][3] == 0 and not e["decoder_started"]
        if e["proc"] is not None:
            try:
                e["proc"].stdin.close()
            except (BrokenPipeError, OSError):
                e["broken_pipe"] = True
            try:
                e["decode_exit"] = e["proc"].wait(timeout=PROCESS_CLOSE_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                e["decoder_timed_out"] = True
                e["proc"].kill()
                e["decode_exit"] = e["proc"].wait()
            e["stderr_reader"].join(timeout=PROCESS_CLOSE_TIMEOUT_SECONDS)
            if e["stderr_reader"].is_alive():
                e["decoder_timed_out"] = True
                e["proc"].kill()
                e["stderr_reader"].join(timeout=1)
            e["log"].flush()
            e["log"].close()
            e["error_log_bytes"] = e["stderr_state"]["retained"]
            e["error_log_truncated_bytes"] = e["stderr_state"]["truncated"]
            e["stderr_total_bytes"] = e["stderr_state"]["total"]
            if (e["decode_exit"] != 0 or e["error_log_bytes"] or
                    e["error_log_truncated_bytes"] or e["broken_pipe"] or
                    e.get("decoder_timed_out")):
                self.decode_failures.append(e["epoch"])
        else:
            e.update(decode_exit=None, error_log_bytes=0,
                     error_log_truncated_bytes=0, stderr_total_bytes=0,
                     broken_pipe=False, decoder_timed_out=False)
        e["broadcast_audio_seconds"] = e["stream_samples"] / e["sample_rate_hz"]
        self.cumulative_audio_seconds += e["broadcast_audio_seconds"]
        e.pop("proc", None)
        e.pop("log", None)
        e.pop("log_path", None)
        e.pop("stderr_reader", None)
        e.pop("stderr_state", None)
        self.epochs.append(e)
        self.epoch = None

    def finish_bounded(self, stream):
        try:
            result = self.finish(stream)
        finally:
            self._watchdog.cancel()
        result["decoder_timed_out"] = self.timed_out or any(
            epoch.get("decoder_timed_out", False) for epoch in self.epochs)
        return result

    def abort(self):
        self._kill_all()
        try:
            self._finish_epoch()
        finally:
            self._watchdog.cancel()


def _valid_pes_header(pes: bytes):
    if len(pes) < 3:
        return None, "PES prefix incomplete"
    if pes[:3] != b"\x00\x00\x01":
        return None, "invalid PES prefix"
    if len(pes) < 9:
        return None, "PES fixed header incomplete"
    if pes[6] & 0xC0 != 0x80:
        return None, "invalid MPEG-2 PES marker bits"
    pts_dts = (pes[7] >> 6) & 3
    if pts_dts == 1:
        return None, "reserved PTS_DTS_flags value"
    optional_len = pes[8]
    if 9 + optional_len > len(pes):
        return None, "PES optional header exceeds packet"
    if pts_dts == 2 and optional_len < 5:
        return None, "PES PTS header is truncated"
    if pts_dts == 3 and optional_len < 10:
        return None, "PES PTS/DTS header is truncated"
    declared = int.from_bytes(pes[4:6], "big")
    expected = declared + 6 if declared else None
    if expected is not None and expected != len(pes):
        return expected, ("PES declared length underflow" if len(pes) < expected
                          else "PES exceeds declared length")
    return expected, None


def _read_packet_window(ts_path: Path, start_packet: int, packet_count: int,
                        pid: int, stream, row):
    size = ts_path.stat().st_size
    packet_total, trailing_bytes = divmod(size, PACKET_SIZE)
    if trailing_bytes:
        raise ValueError("input TS has trailing non-packet bytes")
    if start_packet < 0 or packet_count < 1 or packet_count > MAX_PACKET_COUNT:
        raise ValueError("require start_packet >= 0 and packet_count in 1..65536")
    if start_packet + packet_count > packet_total:
        raise ValueError("requested packet window exceeds saved TS length")

    selected_packets = payload_packets = pusi_count = 0
    first_pid_packet = None
    last_pid_packet = None
    last_cc = None
    last_payload = None
    leading_packets = 0
    started = False
    active = None
    final_open_pes = 0
    final_partial_pes = 0
    maximum_pes_bytes = 0
    packet_errors = []
    pes_rows = []
    state = {"rows": {pid: row}, "invalid": [], "violations": []}
    t0 = time.monotonic()

    def finish_active(end_kind):
        nonlocal active, maximum_pes_bytes, final_partial_pes
        if active is None:
            return
        pes = bytes(active["data"])
        maximum_pes_bytes = max(maximum_pes_bytes, len(pes))
        expected, issue = _valid_pes_header(pes)
        if issue:
            if end_kind == "window_end" and issue in (
                    "PES prefix incomplete", "PES fixed header incomplete",
                    "PES optional header exceeds packet",
                    "PES PTS header is truncated", "PES PTS/DTS header is truncated"):
                final_partial_pes += 1
                active = None
                return
            pes_rows.append({"start_packet_1based": active["start_packet_1based"],
                             "actual_bytes": len(pes), "expected_bytes": expected,
                             "error": issue, "boundary": end_kind})
            active = None
            return
        # Reuse the frozen parser for PES-to-ES extraction and ADTS/PCE handling.
        _AV._parse_pes(pid, active["start_packet_1based"], pes, state, None)
        if state["invalid"] or state["violations"] or row["audio_poisoned"]:
            active = None
            return
        active = None

    with ts_path.open("rb") as f:
        f.seek(start_packet * PACKET_SIZE, os.SEEK_SET)
        remaining = packet_count
        packet_number = start_packet
        while remaining:
            take = min(8192, remaining)
            block = f.read(take * PACKET_SIZE)
            if len(block) != take * PACKET_SIZE:
                raise ValueError("short read inside requested packet window")
            for off in range(0, len(block), PACKET_SIZE):
                if time.monotonic() - t0 > WINDOW_TIMEOUT_SECONDS:
                    raise TimeoutError("packet-window processing exceeded time limit")
                q = block[off:off + PACKET_SIZE]
                packet_number += 1
                if q[0] != 0x47:
                    packet_errors.append({"packet_1based": packet_number,
                                          "error": "transport sync byte"})
                    continue
                this_pid = ((q[1] & 0x1F) << 8) | q[2]
                if this_pid != pid:
                    continue
                selected_packets += 1
                if first_pid_packet is None:
                    first_pid_packet = packet_number
                last_pid_packet = packet_number
                if q[1] & 0x80:
                    packet_errors.append({"packet_1based": packet_number, "error": "TEI"})
                if q[3] & 0xC0:
                    packet_errors.append({"packet_1based": packet_number,
                                          "error": "transport scrambling"})
                afc = (q[3] >> 4) & 3
                if afc == 0:
                    packet_errors.append({"packet_1based": packet_number,
                                          "error": "adaptation_field_control=0"})
                    continue
                off_payload = 4
                discontinuity = False
                if afc & 2:
                    adaptation_len = q[4]
                    if adaptation_len > 183 or 5 + adaptation_len > PACKET_SIZE:
                        packet_errors.append({"packet_1based": packet_number,
                                              "error": "adaptation field overrun"})
                        continue
                    if adaptation_len:
                        discontinuity = bool(q[5] & 0x80)
                    off_payload = 5 + adaptation_len
                    if afc == 2 and off_payload != PACKET_SIZE:
                        packet_errors.append({"packet_1based": packet_number,
                                              "error": "adaptation-only packet has trailing bytes"})
                        continue
                    if afc == 3 and off_payload >= PACKET_SIZE:
                        packet_errors.append({"packet_1based": packet_number,
                                              "error": "adaptation field leaves no payload"})
                        continue
                has_payload = bool(afc & 1) and off_payload < PACKET_SIZE
                if not has_payload:
                    if discontinuity:
                        last_cc = last_payload = None
                    continue
                payload = q[off_payload:]
                cc = q[3] & 0x0F
                if discontinuity:
                    last_cc = last_payload = None
                if last_cc is not None and cc != ((last_cc + 1) & 0x0F):
                    packet_errors.append({"packet_1based": packet_number,
                                          "error": "continuity counter",
                                          "previous_cc": last_cc, "current_cc": cc})
                last_cc, last_payload = cc, payload
                payload_packets += 1
                pusi = bool(q[1] & 0x40)
                if pusi:
                    pusi_count += 1
                    if active is not None:
                        finish_active("next_pusi")
                    started = True
                    active = {"start_packet_1based": packet_number,
                              "data": bytearray(payload)}
                elif not started:
                    leading_packets += 1
                    continue
                elif active is not None:
                    active["data"].extend(payload)
                if active is not None:
                    if len(active["data"]) > MAX_PES_BYTES:
                        pes_rows.append({"start_packet_1based": active["start_packet_1based"],
                                         "actual_bytes": len(active["data"]),
                                         "error": "PES exceeds 16 MiB bound"})
                        active = None
                        started = False
                        continue
                    if len(active["data"]) >= 6 and active["data"][:3] == b"\x00\x00\x01":
                        declared = int.from_bytes(active["data"][4:6], "big")
                        expected = declared + 6 if declared else None
                        if expected is not None and len(active["data"]) > expected:
                            pes_rows.append({"start_packet_1based": active["start_packet_1based"],
                                             "actual_bytes": len(active["data"]),
                                             "expected_bytes": expected,
                                             "error": "PES exceeds declared length"})
                            active = None
                            started = False
            remaining -= take

    if active is not None:
        pes = bytes(active["data"])
        expected, issue = _valid_pes_header(pes)
        if issue is None:
            if expected is None:
                final_open_pes += 1
            else:
                finish_active("window_end")
        elif issue in ("PES prefix incomplete", "PES fixed header incomplete",
                       "PES optional header exceeds packet", "PES PTS header is truncated",
                       "PES PTS/DTS header is truncated", "PES declared length underflow"):
            final_open_pes += 1
            final_partial_pes += 1
        else:
            pes_rows.append({"start_packet_1based": active["start_packet_1based"],
                             "actual_bytes": len(pes), "expected_bytes": expected,
                             "error": issue, "boundary": "window_end"})
            active = None

    return {"packet_total": packet_total, "selected_pid_packets": selected_packets,
            "selected_pid_payload_packets": payload_packets,
            "selected_pid_pusi_packets": pusi_count,
            "selected_pid_first_packet_1based": first_pid_packet,
            "selected_pid_last_packet_1based": last_pid_packet,
            "leading_partial_pes_packets_omitted": leading_packets,
            "complete_pes_count": row["complete_pes"],
            "invalid_pes_headers": state["invalid"],
            "pes_length_violations": state["violations"],
            "interior_pes_errors": pes_rows,
            "final_open_pes_count": final_open_pes,
            "final_partial_pes_count": final_partial_pes,
            "maximum_pes_bytes": maximum_pes_bytes,
            "ts_packet_errors": packet_errors,
            "global_sync_errors_in_window": sum(1 for x in packet_errors
                                                 if x["error"] == "transport sync byte"),
            "continuity_window_initial_pid_packet_unchecked": True}


def audit_window(ts_path: Path, *, pid: int, start_packet: int, packet_count: int,
                 output_dir: Path):
    if not 0 <= pid <= 0x1FFE:
        raise ValueError("pid must be in 0..8190 (NULL PID is excluded)")
    if start_packet < 0:
        raise ValueError("start_packet is a zero-based nonnegative packet offset")
    if packet_count < 1 or packet_count > MAX_PACKET_COUNT:
        raise ValueError(f"packet_count must be in 1..{MAX_PACKET_COUNT}")
    if output_dir.exists():
        raise FileExistsError("output directory already exists")
    os.umask(0o077)
    output_dir.mkdir(mode=0o700, parents=True, exist_ok=False)
    decoder_dir = output_dir / "decoder"
    decoder_dir.mkdir(mode=0o700)
    stream = _AV.ADTSStream()
    audio = BoundedAACDecodeAudit(decoder_dir)
    row = {"pid": pid, "kind": "aac", "complete_pes": 0, "es_bytes": 0,
           "stream": stream, "audio": audio, "audio_poisoned": None}
    try:
        scan = _read_packet_window(ts_path, start_packet, packet_count,
                                   pid, stream, row)
    except BaseException:
        audio.abort()
        raise
    prefinish_framing_error = row["audio_poisoned"]
    try:
        stream.finish()
    except ValueError as exc:
        row["audio_poisoned"] = row["audio_poisoned"] or str(exc)
    adts = audio.finish_bounded(stream)
    framing_errors = [x for x in adts["decode_failures"]
                      if x != "no-adts-sync" and not isinstance(x, int)]
    decoder_epoch_errors = [e for e in adts["epochs"]
                             if (e.get("decode_exit") not in (None, 0) or
                                 e.get("error_log_bytes", 0) or
                                 e.get("error_log_truncated_bytes", 0) or
                                 e.get("broken_pipe") or e.get("decoder_timed_out"))]
    no_initial_sync_with_audio = (row["audio_poisoned"] == "no initial ADTS synchronization found"
                                  and row["complete_pes"] > 0 and row["es_bytes"] >= 7)
    errors_present = bool(scan["ts_packet_errors"] or scan["invalid_pes_headers"] or
                          scan["pes_length_violations"] or scan["interior_pes_errors"] or
                          prefinish_framing_error or no_initial_sync_with_audio or framing_errors or
                          decoder_epoch_errors or adts["decoder_timed_out"])
    if errors_present:
        verdict = "fail"
    elif scan["selected_pid_packets"] == 0:
        verdict = "absent"
    elif adts["frames_emitted"] == 0:
        verdict = "inconclusive"
    elif not scan["complete_pes_count"] or not adts["frames_seen"]:
        verdict = "inconclusive"
    else:
        verdict = "decoded_pass"
    result = {
        "schema": "dt295-bounded-aac-window-v1",
        "verdict": verdict,
        "analysis_valid": verdict != "fail",
        "input": {"ts_name": ts_path.name, "pid": pid,
                  "start_packet_zero_based": start_packet,
                  "packet_count_requested": packet_count,
                  "packet_count_cap": MAX_PACKET_COUNT},
        "window": scan,
        "audio": {"es_bytes": row["es_bytes"], **adts,
                  "pce_wait_pending_epoch_count": sum(
                      bool(e.get("pce_wait_pending")) for e in adts["epochs"]),
                  "framing_error": row["audio_poisoned"]},
        "verdict_basis": {
            "no_frames_never_pass": True,
            "decoded_pass_requires_complete_PES_ADTS_frames_and_clean_decoder": True,
            "leading_partial_PES_is_reported_as_window_boundary": True,
            "final_open_or_partial_PES_is_reported_as_window_boundary": True,
            "full_run_issue2_absence_or_presence_not_inferred_from_one_window": True},
        "limitations": [
            "Only the explicit packet window was read; no whole-file walk was performed.",
            "Initial PID continuity before this window is unknown.",
            "This reports one stream window and cannot establish that no unobserved PMT transition occurred elsewhere.",
            "A clean AAC decode is not a full recording quality or route/register readback verdict."],
    }
    (output_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n",
                                            encoding="utf-8")
    print(json.dumps({"verdict": verdict, "pid": pid,
                      "start_packet_zero_based": start_packet,
                      "packets_read": packet_count,
                      "complete_pes_count": scan["complete_pes_count"],
                      "adts_frames_seen": adts["frames_seen"],
                      "adts_frames_emitted": adts["frames_emitted"]}, ensure_ascii=False))
    return result


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("ts", type=Path, help="saved transport-stream file")
    ap.add_argument("--pid", required=True, type=int)
    ap.add_argument("--start-packet", required=True, type=int,
                    help="zero-based packet offset to seek to")
    ap.add_argument("--packet-count", required=True, type=int,
                    help=f"packets to read, maximum {MAX_PACKET_COUNT}")
    ap.add_argument("--output", required=True, type=Path,
                    help="new private directory; metadata and bounded decoder logs only")
    args = ap.parse_args(argv)
    try:
        result = audit_window(args.ts, pid=args.pid, start_packet=args.start_packet,
                              packet_count=args.packet_count, output_dir=args.output)
    except (OSError, ValueError, TimeoutError) as exc:
        print(f"AAC window audit: {exc}", file=sys.stderr)
        return 1
    return 0 if result["verdict"] != "fail" else 1


if __name__ == "__main__":
    raise SystemExit(main())
