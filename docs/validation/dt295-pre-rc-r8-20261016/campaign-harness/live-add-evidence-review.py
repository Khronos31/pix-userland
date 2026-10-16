#!/usr/bin/env python3
"""Bounded, offline-only review of DT295 live-PID-add evidence."""
from __future__ import annotations
import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

PACKET = 188
NULL_PID = 0x1FFF
MAX_EVENTS = 64
MAX_WINDOW = 65536
DEFAULT_WINDOW = 32768
THIRTY_MINUTES_MS = 1_800_000
PCR_MODULUS = 1 << 33

CRC_TABLE = []
for _i in range(256):
    _v = _i << 24
    for _ in range(8):
        _v = ((_v << 1) ^ (0x04C11DB7 if _v & 0x80000000 else 0)) & 0xFFFFFFFF
    CRC_TABLE.append(_v)

def crc32_mpeg(data: bytes) -> int:
    v = 0xFFFFFFFF
    for x in data:
        v = ((v << 8) & 0xFFFFFFFF) ^ CRC_TABLE[(v >> 24) ^ x]
    return v

def u16(b: bytes, i: int) -> int:
    return (b[i] << 8) | b[i + 1]

class Assembler:
    def __init__(self):
        self.buf = bytearray()
        self.last_cc = None
        self.last_packet = None
        self.sections = []
        self.section_records = []
        self.buf_start = None
        self.packet_no = 0
        self.errors = defaultdict(int)

    def consume(self, many=True):
        while len(self.buf) >= 3:
            if self.buf[0] == 0xFF:
                self.buf.clear()
                return
            size = 3 + (((self.buf[1] & 15) << 8) | self.buf[2])
            if size > 4096 or size < 3:
                self.errors["section_length"] += 1
                self.buf.clear()
                return
            if len(self.buf) < size:
                return
            s = bytes(self.buf[:size])
            del self.buf[:size]
            if len(s) < 8 or crc32_mpeg(s) != 0:
                self.errors["crc"] += 1
            else:
                self.sections.append(s)
                self.section_records.append((s, self.buf_start if self.buf_start is not None else self.packet_no))
            self.buf_start = self.packet_no if self.buf else None
            if not many:
                self.buf.clear()
                return

    def feed(self, p: bytes, packet_no: int):
        self.packet_no = packet_no
        if len(p) != PACKET or p[0] != 0x47:
            self.errors["sync"] += 1
            self.buf.clear()
            self.last_cc = None
            return
        if p[1] & 0x80:
            self.errors["tei"] += 1
            self.buf.clear()
            self.last_cc = None
            return
        afc = (p[3] >> 4) & 3
        if not afc:
            self.errors["adaptation_control"] += 1
            return
        off = 4
        if afc & 2:
            n = p[4]
            if n > 183:
                self.errors["adaptation_length"] += 1
                self.buf.clear()
                return
            if n and p[5] & 0x80:
                self.buf.clear()
            off = 5 + n
        if not afc & 1 or off >= PACKET:
            return
        cc = p[3] & 15
        if self.last_cc is not None:
            if p == self.last_packet:
                return
            if cc != ((self.last_cc + 1) & 15):
                self.errors["continuity"] += 1
                self.buf.clear()
        self.last_cc, self.last_packet = cc, p
        payload = p[off:]
        if p[1] & 0x40:
            pointer = payload[0]
            if pointer + 1 > len(payload):
                self.errors["pointer"] += 1
                self.buf.clear()
                return
            if self.buf:
                self.buf.extend(payload[1:pointer + 1])
                self.consume(False)
            self.buf = bytearray(payload[pointer + 1:])
            self.buf_start = packet_no if self.buf else None
            self.consume()
        elif self.buf:
            self.buf.extend(payload)
            self.consume(False)

def complete_groups(records, tid):
    groups, complete = {}, []
    for s, packet_no in records:
        if s[0] != tid or len(s) < 12 or not s[1] & 0x80 or not s[5] & 1:
            continue
        ext, version, no, last = u16(s, 3), (s[5] >> 1) & 31, s[6], s[7]
        if no > last:
            continue
        key = (ext, version, last)
        parts = groups.setdefault(key, {})
        parts[no] = (s, packet_no)
        if len(parts) == last + 1 and all(i in parts for i in range(last + 1)):
            complete.append({"extension": ext, "version": version,
                             "sections": [parts[i][0] for i in range(last + 1)],
                             "packet_1based": packet_no})
    return complete

def parse_pat(group):
    programs = {}
    for s in group["sections"]:
        end, pos = len(s) - 4, 8
        while pos + 4 <= end:
            program, pid = u16(s, pos), u16(s, pos + 2) & 0x1FFF
            if program:
                programs[program] = pid
            pos += 4
        if pos != end:
            return None
    return {"tsid": group["extension"], "version": group["version"], "programs": programs}

def descriptors_ca(s, start, end):
    p, found = start, []
    while p < end:
        if p + 2 > end:
            return None
        tag, n = s[p], s[p + 1]
        p += 2
        if p + n > end:
            return None
        if tag == 9 and n >= 4:
            pid = u16(s, p + 2) & 0x1FFF
            if pid != NULL_PID:
                found.append(pid)
        p += n
    return found

def parse_pmt(group):
    streams, ecms, pcrs = {}, [], set()
    for s in group["sections"]:
        if len(s) < 16:
            return None
        end, pcr = len(s) - 4, u16(s, 8) & 0x1FFF
        pcrs.add(pcr)
        info = u16(s, 10) & 0xFFF
        pos = 12
        if pos + info > end:
            return None
        ca = descriptors_ca(s, pos, pos + info)
        if ca is None:
            return None
        ecms.extend(ca)
        pos += info
        while pos < end:
            if pos + 5 > end:
                return None
            typ, pid, n = s[pos], u16(s, pos + 1) & 0x1FFF, u16(s, pos + 3) & 0xFFF
            pos += 5
            if pos + n > end:
                return None
            ca = descriptors_ca(s, pos, pos + n)
            if ca is None:
                return None
            ecms.extend(ca)
            if pid != NULL_PID:
                streams[pid] = typ
            pos += n
        if pos != end:
            return None
    return {"program": group["extension"], "version": group["version"],
            "streams": streams, "ecm_pids": sorted(set(ecms)), "pcr_pids": sorted(pcrs)}

def window_packets(path: Path, start: int, count: int):
    with path.open("rb") as f:
        f.seek(start * PACKET)
        b = f.read(count * PACKET)
    b = b[:len(b) - len(b) % PACKET]
    return [b[i:i + PACKET] for i in range(0, len(b), PACKET)]

def scan_tables(path: Path, start: int, count: int):
    packets = window_packets(path, start, count)
    pat_a = Assembler()
    for i, p in enumerate(packets):
        if len(p) == PACKET and ((p[1] & 31) << 8 | p[2]) == 0:
            pat_a.feed(p, start + i + 1)
    pat_groups = complete_groups(pat_a.section_records, 0)
    pat_history = [{**parse_pat(g), "packet_1based": g["packet_1based"]}
                   for g in pat_groups if parse_pat(g)]
    pat = pat_history[-1] if pat_history else None
    pmts, pmt_history, errors = {}, defaultdict(list), defaultdict(int)
    for k, v in pat_a.errors.items():
        errors["pat_" + k] += v
    if pat:
        pid_program = {}
        for table in pat_history:
            for program, pid in table["programs"].items():
                pid_program[pid] = program
        assemblers = {pid: Assembler() for pid in pid_program}
        for i, p in enumerate(packets):
            if len(p) == PACKET:
                pid = ((p[1] & 31) << 8) | p[2]
                if pid in assemblers:
                    assemblers[pid].feed(p, start + i + 1)
        for pid, a in assemblers.items():
            parsed = [{**parse_pmt(g), "packet_1based": g["packet_1based"], "pmt_pid": pid}
                      for g in complete_groups(a.section_records, 2) if parse_pmt(g)]
            program = pid_program[pid]
            parsed = [p for p in parsed if p["program"] == program]
            if parsed:
                pmt_history[program].extend(parsed)
                pmts[program] = parsed[-1]
            for k, v in a.errors.items():
                errors[f"pmt{pid}_{k}"] += v
    expected_pmts = pat["programs"] if pat else {}
    complete = bool(pat) and all(pmt_history.get(program) and
                                 pmt_history[program][-1]["pmt_pid"] == pid
                                 for program, pid in expected_pmts.items())
    for program in pmt_history:
        pmt_history[program].sort(key=lambda x: x["packet_1based"])
    return {"start_packet_1based": start + 1, "packets_scanned": len(packets),
            "pat": pat, "pmts": pmts, "pat_history": pat_history,
            "pmt_history": dict(pmt_history),
            "all_pat_pmts_complete": complete,
            "errors": dict(errors)}

def pcr_value(p):
    b = p[6:12]
    base = (b[0] << 25) | (b[1] << 17) | (b[2] << 9) | (b[3] << 1) | (b[4] >> 7)
    ext = ((b[4] & 1) << 8) | b[5]
    return base * 300 + ext if ext < 300 else None

def metrics(path: Path, start: int, count: int, pid: int, pcr_pid):
    packets = window_packets(path, start, count)
    total = valid = payload = header_bad = cc_bad = 0
    last_cc, last_packet, pcrs = None, None, []
    for i, p in enumerate(packets):
        if p[0] != 0x47:
            continue
        this_pid, afc = ((p[1] & 31) << 8) | p[2], (p[3] >> 4) & 3
        if this_pid == pid:
            total += 1
            if p[1] & 0x80 or not afc:
                header_bad += 1
            else:
                valid += 1
                payload += bool(afc & 1)
                cc = p[3] & 15
                if last_cc is not None and p != last_packet and cc != ((last_cc + 1) & 15):
                    cc_bad += 1
                last_cc, last_packet = cc, p
        if pcr_pid is not None and this_pid == pcr_pid and afc & 2 and p[4] >= 7 and p[5] & 0x10:
            value = pcr_value(p)
            if value is not None:
                pcrs.append({"packet_1based": start + i + 1, "ticks_27mhz": value})
    return {"window_start_packet_1based": start + 1, "window_packets_scanned": len(packets),
            "pid": pid, "pid_packets": total, "pid_valid_headers": valid,
            "pid_payload_packets": payload, "pid_header_errors": header_bad,
            "pid_continuity_errors_in_window": cc_bad, "pcr_pid": pcr_pid, "pcr_samples": pcrs}

def read_diagnostics(path: Path):
    if path.stat().st_size > 64 * 1024 * 1024:
        raise ValueError("diagnostic file exceeds 64 MiB review bound")
    events, statuses, malformed = [], [], 0
    with path.open(encoding="utf-8") as f:
        for line_no, line in enumerate(f, 1):
            if len(line) > 64 * 1024:
                raise ValueError(f"diagnostic line {line_no} exceeds 64 KiB")
            try:
                x = json.loads(line)
            except (json.JSONDecodeError, UnicodeError):
                malformed += 1
                continue
            if not isinstance(x, dict):
                continue
            if x.get("event") in ("dt295_live_pid_add", "dt295_live_add_summary", "end"):
                events.append(x)
            elif x.get("model") == "DT295" and type(x.get("phase")) is int:
                statuses.append(x)
            if len(events) > MAX_EVENTS + 4:
                raise ValueError("diagnostic event count exceeds bounded limit")
    return events, statuses, malformed

def duplicates(values):
    seen, dup = set(), set()
    for x in values:
        if x in seen:
            dup.add(x)
        seen.add(x)
    return sorted(dup)

def diag_summary(items):
    adds = [x for x in items if x.get("event") == "dt295_live_pid_add"]
    sums = [x for x in items if x.get("event") == "dt295_live_add_summary"]
    ends = [x for x in items if x.get("event") == "end"]
    errors = []
    for i, x in enumerate(adds):
        for field, value in (("phase", 2), ("previously_present", 0),
                             ("existing_routes_and_ecms_preserved", 1)):
            if type(x.get(field)) is not int or x[field] != value:
                errors.append({"event_index": i, "field": field})
        for field in ("after_packets", "pid", "slot", "ecm_slot", "ecm_pid"):
            if type(x.get(field)) is not int or x[field] < 0:
                errors.append({"event_index": i, "field": field})
    count = sums[-1].get("live_add_count") if sums else None
    return {"adds": adds, "add_event_count": len(adds),
            "summary_event_count": len(sums), "summary_count": count,
            "summary_matches_events": len(sums) == 1 and type(count) is int and count == len(adds),
            "duplicate_pids": duplicates([x["pid"] for x in adds if type(x.get("pid")) is int]),
            "duplicate_slots": duplicates([x["slot"] for x in adds if type(x.get("slot")) is int]),
            "field_errors": errors, "end_event_count": len(ends),
            "exit_code": ends[-1].get("exit") if ends else None}

def streams(scan):
    result = {}
    for program, pmt in scan.get("pmts", {}).items():
        for pid, typ in pmt["streams"].items():
            result[pid] = {"program": int(program), "stream_type": typ}
    return result

def targeted_pmts_complete(scan, programs):
    """Require current complete PMTs only for programs relevant to this add."""
    pat = scan.get("pat")
    if not pat:
        return False
    current = pat.get("programs", {})
    for program in programs:
        pmt = scan.get("pmts", {}).get(program)
        expected_pid = current.get(program)
        if not pmt or expected_pid is None or pmt.get("pmt_pid") != expected_pid:
            return False
    return bool(programs)

def status_bracket(statuses, point, final_status, packet_count):
    phase2 = [s for s in statuses if s.get("phase") == 2 and type(s.get("packets")) is int
              and type(s.get("elapsed_ms")) is int]
    before = [s for s in phase2 if s["packets"] <= point]
    after = [s for s in phase2 if s["packets"] >= point]
    lo = max(before, key=lambda s: s["packets"]) if before else None
    hi = min(after, key=lambda s: s["packets"]) if after else None
    exact = bool(final_status and final_status.get("packets") == packet_count
                 and final_status.get("confirmed_bytes") == packet_count * PACKET)
    if not (lo and hi and final_status):
        return {"status": "inconclusive", "packet_mapping_exact": exact,
                "before_status_packets": lo.get("packets") if lo else None,
                "after_status_packets": hi.get("packets") if hi else None,
                "before_status_elapsed_ms": lo.get("elapsed_ms") if lo else None,
                "after_status_elapsed_ms": hi.get("elapsed_ms") if hi else None}
    bound = max(0, final_status["elapsed_ms"] - hi["elapsed_ms"])
    return {"status": "confirmed_30m" if bound >= THIRTY_MINUTES_MS else "under_30m_or_inconclusive",
            "packet_mapping_exact": exact, "before_status_packets": lo["packets"],
            "after_status_packets": hi["packets"], "before_status_elapsed_ms": lo["elapsed_ms"],
            "after_status_elapsed_ms": hi["elapsed_ms"], "final_elapsed_ms": final_status["elapsed_ms"],
            "conservative_elapsed_after_event_ms": bound,
            "uncertainty_ms": max(0, hi["elapsed_ms"] - lo["elapsed_ms"])}

def analyze(ts_path: Path, diag_path: Path, window: int):
    size = ts_path.stat().st_size
    packet_count, trailing = divmod(size, PACKET)
    items, statuses, malformed_lines = read_diagnostics(diag_path)
    diag = diag_summary(items)
    adds = diag["adds"]
    if len(adds) > MAX_EVENTS:
        raise ValueError("more than 64 add events")
    initial = scan_tables(ts_path, 0, min(packet_count, window))
    tail_start = max(0, packet_count - window)
    terminal = scan_tables(ts_path, tail_start, min(packet_count, window))
    final_terminal = next((x for x in reversed(statuses) if x.get("phase") in (3, 4)), None)
    final_success = final_terminal if final_terminal and final_terminal.get("phase") == 3 else None
    packet_mapping_exact = bool(final_success and final_success.get("packets") == packet_count
                                and final_success.get("confirmed_bytes") == packet_count * PACKET)
    results = []
    for event in adds:
        boundary = event.get("after_packets")
        if type(boundary) is not int or boundary <= 0:
            results.append({"event": event, "evidence": "inconclusive", "reason": "invalid after_packets",
                            "contradiction_observed": False,
                            "table_and_packet_evidence_complete": False,
                            "runtime_after_add_30m": "unobserved_or_inconclusive",
                            "status_duration_after_event": {"status": "inconclusive"},
                            "pcr_elapsed_between_limited_windows_seconds": None})
            continue
        index = min(boundary, packet_count)
        pre_start, post_start = max(0, index - window), min(index, packet_count)
        before = scan_tables(ts_path, pre_start, min(window, index - pre_start))
        after = scan_tables(ts_path, post_start, min(window, packet_count - post_start))
        event_start = max(0, index - window)
        event_window = scan_tables(ts_path, event_start,
                                   min(packet_count - event_start, MAX_WINDOW, window * 2))
        pid = event.get("pid") if type(event.get("pid")) is int else -1
        timelines = event_window.get("pmt_history", {})
        programs = sorted(int(p) for p, rows in timelines.items()
                          if any(pid in row["streams"] and row["packet_1based"] <= boundary for row in rows))
        transitions = []
        for program_id in programs:
            timeline = timelines.get(program_id, [])
            new_rows = [row for row in timeline
                        if row["packet_1based"] <= boundary and pid in row["streams"]]
            new_row = new_rows[-1] if new_rows else None
            old_rows = [row for row in timeline if new_row and row["packet_1based"] < new_row["packet_1based"]
                        and pid not in row["streams"]]
            old_row = old_rows[-1] if old_rows else None
            transitions.append({"program": program_id, "old": old_row, "new": new_row})
        program = programs[0] if programs else None
        first_transition = transitions[0] if transitions else {}
        old_row, new_row = first_transition.get("old"), first_transition.get("new")
        after_pmt = after.get("pmts", {}).get(program) if program is not None else None
        pmt = after_pmt or new_row
        before_has = bool(new_row and old_row is None)
        after_complete = targeted_pmts_complete(after, programs)
        ecms_before = old_row.get("ecm_pids") if old_row else None
        ecms_after = new_row.get("ecm_pids") if new_row else None
        pcr_pid = next((x for x in (pmt or {}).get("pcr_pids", []) if x != NULL_PID), None)
        metric = metrics(ts_path, post_start, min(window, packet_count - post_start), pid, pcr_pid)
        bracket = status_bracket(statuses, boundary, final_terminal, packet_count)
        pcr_samples = list(metric["pcr_samples"])
        if pcr_pid is not None and tail_start > post_start + metric["window_packets_scanned"]:
            tail_metric = metrics(ts_path, tail_start, min(window, packet_count - tail_start), pid, pcr_pid)
            pcr_samples.extend(tail_metric["pcr_samples"])
        pcr_seconds = None
        if len(pcr_samples) >= 2:
            pcr_seconds = (((pcr_samples[-1]["ticks_27mhz"] // 300 -
                             pcr_samples[0]["ticks_27mhz"] // 300) % PCR_MODULUS) / 90000.0)
        status_ms = bracket.get("conservative_elapsed_after_event_ms", 0)
        runtime_30m = status_ms >= THIRTY_MINUTES_MS
        fields_ok = (event.get("phase") == 2 and event.get("previously_present") == 0
                     and event.get("existing_routes_and_ecms_preserved") == 1)
        transitions_ok = bool(transitions) and all(x["old"] is not None and x["new"] is not None
                                                    for x in transitions)
        ecm_pid = event.get("ecm_pid", 0)
        ecm_binding_matches = all(
            ecm_pid == 0 or (x["old"] and x["new"] and
                             ecm_pid in x["old"]["ecm_pids"] and ecm_pid in x["new"]["ecm_pids"])
            for x in transitions)
        ecm_sets_unchanged = bool(transitions) and all(
            x["old"] and x["new"] and x["old"]["ecm_pids"] == x["new"]["ecm_pids"]
            for x in transitions)
        event_tables_complete = targeted_pmts_complete(event_window, programs)
        pmt_ok = (packet_mapping_exact and event_tables_complete and after_complete
                  and transitions_ok and ecm_binding_matches and metric["pid_valid_headers"] > 0)
        results.append({
            "event": event, "event_fields_valid": fields_ok,
            "contradiction_observed": not fields_ok or
            (packet_mapping_exact and metric["pid_header_errors"] > 0) or
            (packet_mapping_exact and transitions_ok and not ecm_sets_unchanged),
            "table_and_packet_evidence_complete": bool(pmt_ok),
            "event_packet_to_saved_ts_mapping_exact": packet_mapping_exact,
            "event_adjacent_ts_window_is_unanchored": not packet_mapping_exact,
            "before_window": before, "after_window": after,
            "event_window_target_pmts_complete": event_tables_complete,
            "programs_advertising_added_pid": programs,
            "program_transitions": [{"program": x["program"],
                                     "old_packet_1based": x["old"].get("packet_1based") if x["old"] else None,
                                     "new_packet_1based": x["new"].get("packet_1based") if x["new"] else None,
                                     "ecms_before": x["old"]["ecm_pids"] if x["old"] else None,
                                     "ecms_after": x["new"]["ecm_pids"] if x["new"] else None,
                                     "ecms_unchanged": bool(x["old"] and x["new"] and
                                                             x["old"]["ecm_pids"] == x["new"]["ecm_pids"])}
                                    for x in transitions],
            "event_ecm_pid_matches_each_program": ecm_binding_matches,
            "event_ecm_sets_unchanged_for_each_program": ecm_sets_unchanged,
            "old_complete_pmt_packet_1based": old_row.get("packet_1based") if old_row else None,
            "new_complete_pmt_packet_1based": new_row.get("packet_1based") if new_row else None,
            "added_pid_absent_in_old_pmt": bool(old_row and pid not in old_row["streams"]),
            "added_pid_present_in_new_pmt": bool(new_row and pid in new_row["streams"]),
            "added_pid_packet_evidence": metric,
            "program": program, "stream_type": pmt["streams"].get(pid) if pmt else None,
            "program_ecms_before": ecms_before, "program_ecms_after": ecms_after,
            "program_ecms_unchanged": ecms_before is not None and ecms_before == ecms_after,
            "pcr_elapsed_between_limited_windows_seconds": pcr_seconds,
            "status_duration_after_event": bracket,
            "runtime_after_add_30m": "confirmed" if runtime_30m else "unobserved_or_inconclusive",
            "post_add_30m_observed": bool(runtime_30m)})
    duplicate = bool(diag["duplicate_pids"] or diag["duplicate_slots"])
    end_ok = bool(final_success and diag["exit_code"] == 0)
    metadata_ok = not diag["field_errors"] and not duplicate and (
        diag["summary_matches_events"] or (not adds and not diag["summary_event_count"]))
    if not adds:
        verdict, reason = "inconclusive", "no live-add event observed; normal short-term tests may continue"
    elif duplicate or diag["field_errors"] or any(x["contradiction_observed"] for x in results):
        verdict, reason = "contradicted", "duplicate PID/slot or event/header evidence contradicts the claim"
    elif not metadata_ok or not end_ok:
        verdict, reason = "inconclusive", "summary mismatch, incomplete diagnostics, or non-successful recording end"
    elif not all(x["table_and_packet_evidence_complete"] for x in results):
        verdict, reason = "inconclusive", "bounded windows lack complete PAT/PMT or added-PID packet evidence"
    else:
        verdict, reason = "confirmed_with_source_guard_limit", "event and bounded TS evidence agree; no register readback"
    five_hour = "overall_five_hour_duration_is_a_separate_audit_gate"
    minimum_post_ms = None
    for x in results:
        status_ms = x["status_duration_after_event"].get("conservative_elapsed_after_event_ms")
        if type(status_ms) is int:
            observed = status_ms
        else:
            continue
        minimum_post_ms = observed if minimum_post_ms is None else min(minimum_post_ms, observed)
    analysis_valid = not malformed_lines and not duplicate and not diag["field_errors"] and not any(
        x["contradiction_observed"] for x in results)
    if diag["end_event_count"] and (diag["add_event_count"] or diag["summary_event_count"]) \
            and not diag["summary_matches_events"]:
        analysis_valid = False
    min_seconds = None if minimum_post_ms is None else minimum_post_ms // 1000
    if not analysis_valid:
        fix_path = "invalid"
    elif not adds:
        fix_path = "unobserved"
    elif diag["exit_code"] not in (None, 0):
        fix_path = "inconclusive"
    elif (min_seconds is not None and min_seconds >= 1800
          and metadata_ok and end_ok
          and all(x.get("table_and_packet_evidence_complete") for x in results)):
        fix_path = "observed_pass"
    else:
        fix_path = "inconclusive"
    live_count = diag["summary_count"] if type(diag["summary_count"]) is int else (
        diag["add_event_count"] if not diag["end_event_count"] else None)
    return {
        "schema": "dt295-live-add-evidence-review-v1",
        "analysis_valid": analysis_valid,
        "inputs": {"ts_name": ts_path.name, "diagnostics_name": diag_path.name,
                   "ts_size_bytes": size, "ts_packets": packet_count, "trailing_bytes": trailing,
                   "window_packets": window, "packet_to_saved_ts_mapping_exact": packet_mapping_exact},
        "diagnostics": {k: v for k, v in diag.items() if k != "adds"},
        "diagnostic_malformed_line_count": malformed_lines,
        "initial_window": initial, "terminal_window": terminal,
        "live_add_events": results, "live_add_verdict": verdict, "live_add_verdict_reason": reason,
        "runtime_after_add_30m": "confirmed" if results and all(
            x["runtime_after_add_30m"] == "confirmed" for x in results) else "unobserved_or_inconclusive",
        "five_hour_fix_path_verdict": five_hour,
        "fix_path_verdict": fix_path,
        "live_add_count": live_count,
        "minimum_post_add_seconds_observed": min_seconds,
        "quality_verdict": "not_assessed; local PSI/header windows are not a full quality audit",
        "route_ecm_limit": "preservation is based on fixed source guard and committed diagnostic event, not tuner register readback",
        "startup_slot_limit": "initial PMT inventory is observational and cannot establish exact startup slots",
        "runtime_basis": "post-add duration uses conservative DT295 status elapsed time only; PCR differences are metadata cross-checks and cannot independently satisfy 30 minutes",
        "scan_limit": "Only initial, event-adjacent, and terminal windows were scanned; no full TS/raw scan was performed."}

def japanese_report(result):
    return "\n".join([
        "DT295 live PID 追加オフライン証拠レビュー", "",
        f"解析入力の有効性: {result['analysis_valid']}",
        f"live-add 証拠: {result['live_add_verdict']}",
        f"判定理由: {result['live_add_verdict_reason']}",
        f"録画継続30分: {result['runtime_after_add_30m']}",
        f"fix-path 判定: {result['fix_path_verdict']}",
        f"live-add 累積件数: {result['live_add_count']}",
        f"追加後に観測できた最小秒数: {result['minimum_post_add_seconds_observed']}",
        f"品質判定: {result['quality_verdict']}",
        f"追加イベント数: {result['diagnostics']['add_event_count']}; summary件数: {result['diagnostics']['summary_count']}",
        f"TS packet数: {result['inputs']['ts_packets']}; 端数bytes: {result['inputs']['trailing_bytes']}", "",
        "証拠の限界:",
        f"- {result['route_ecm_limit']}",
        f"- {result['startup_slot_limit']}",
        f"- {result['runtime_basis']}",
        f"- {result['scan_limit']}",
        "- イベントなし、status不足、PMT未完成、30分未満は unobserved/inconclusive とし、quality pass に置き換えません。",
        "- この解析器は保存ファイル専用です。USB、controller、payload、key、profileにはアクセスしません。", ""])

def write_new(path: Path, text: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8") as f:
        f.write(text)

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("stage", nargs="?", type=Path,
                    help="stage directory; writes the fixed review JSON and Japanese text here")
    ap.add_argument("--stage", dest="stage_option", type=Path,
                    help="compatibility alias for positional STAGE")
    ap.add_argument("--ts", type=Path, help="defaults to STAGE/capture.ts")
    ap.add_argument("--diagnostics", type=Path, help="defaults to STAGE/diagnostic/status.jsonl")
    ap.add_argument("--window-packets", type=int, default=DEFAULT_WINDOW)
    args = ap.parse_args(argv)
    stage = args.stage_option or args.stage
    if stage is None:
        ap.error("STAGE directory is required (positional or --stage)")
    if not 1 <= args.window_packets <= MAX_WINDOW:
        ap.error(f"--window-packets must be 1..{MAX_WINDOW}")
    ts_path = args.ts or stage / "capture.ts"
    diag_path = args.diagnostics or stage / "diagnostic/status.jsonl"
    json_path = stage / "live-add-evidence-review.json"
    report_path = stage / "live-add-evidence-review.txt"
    if json_path.exists() or report_path.exists():
        print("live-add evidence review: refusing to overwrite existing stage findings", file=sys.stderr)
        return 1
    try:
        result = analyze(ts_path, diag_path, args.window_packets)
    except (OSError, ValueError) as exc:
        result = {
            "schema": "dt295-live-add-evidence-review-v1", "analysis_valid": False,
            "analysis_error": str(exc)[:300], "fix_path_verdict": "invalid",
            "live_add_count": None, "minimum_post_add_seconds_observed": None,
            "live_add_verdict": "inconclusive",
            "live_add_verdict_reason": "required evidence input could not be read or parsed",
            "runtime_after_add_30m": "unobserved_or_inconclusive",
            "five_hour_fix_path_verdict": "inconclusive",
            "quality_verdict": "not_assessed"}
    try:
        write_new(json_path, json.dumps(result, ensure_ascii=False, indent=2) + "\n")
        write_new(report_path, japanese_report(result))
    except OSError as exc:
        print(f"live-add evidence review: {exc}", file=sys.stderr)
        return 1
    print(json.dumps({"live_add_verdict": result["live_add_verdict"],
                      "runtime_after_add_30m": result["runtime_after_add_30m"],
                      "fix_path_verdict": result["fix_path_verdict"],
                      "analysis_valid": result["analysis_valid"],
                      "live_add_count": result["live_add_count"],
                      "minimum_post_add_seconds_observed": result["minimum_post_add_seconds_observed"],
                      "quality_verdict": result["quality_verdict"]}, ensure_ascii=False))
    return 0 if result["analysis_valid"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
