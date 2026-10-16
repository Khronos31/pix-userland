#!/usr/bin/env python3
"""Hardware-free regression tests for live-add-evidence-review.py."""
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).with_name("live-add-evidence-review.py")
SPEC = importlib.util.spec_from_file_location("live_add_evidence_review", MODULE_PATH)
review = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(review)


def section(prefix):
    b = bytearray(prefix)
    size = len(b) + 4 - 3
    b[1] = (b[1] & 0xF0) | (size >> 8)
    b[2] = size & 0xFF
    crc = review.crc32_mpeg(bytes(b))
    b.extend(crc.to_bytes(4, "big"))
    assert review.crc32_mpeg(bytes(b)) == 0
    return bytes(b)


def pat(entries=((1, 100),)):
    body = bytearray([0x00, 0xB0, 0, 0x12, 0x34, 0xC1, 0, 0])
    for program, pid in entries:
        body.extend([program >> 8, program & 255, 0xE0 | (pid >> 8), pid & 255])
    return section(bytes(body))


def pmt(extra, version, program=1, ecm_pid=2305):
    ca = bytes([0x09, 4, 0, 5, 0xE0 | (ecm_pid >> 8), ecm_pid & 255])
    info = bytes([0xF0, len(ca)]) + ca
    streams = bytes([0x0F, 0xE1, 0x10, 0xF0, 0])
    if extra:
        streams += bytes([0x0F, 0xE1, 0x11, 0xF0, 0])
    return section(bytes([2, 0xB0, 0, program >> 8, program & 255,
                          0xC1 | (version << 1), 0, 0, 0xE1, 0x2C]) + info + streams)


class Builder:
    def __init__(self):
        self.cc = {}
        self.packets = []

    def packet(self, pid, data, pusi=True):
        cc = self.cc.get(pid, 0)
        self.cc[pid] = (cc + 1) & 15
        payload = (bytes([0]) + data if pusi else data)[:184]
        self.packets.append(bytes([0x47, (0x40 if pusi else 0) | (pid >> 8), pid & 255,
                                   0x10 | cc]) + payload.ljust(184, b"\xff"))

    def pcr_packet(self, ticks, pid=300):
        base, ext = divmod(ticks, 300)
        pcr = bytes([(base >> 25) & 255, (base >> 17) & 255, (base >> 9) & 255,
                     (base >> 1) & 255, ((base & 1) << 7) | 0x7E | (ext >> 8), ext & 255])
        cc = self.cc.get(pid, 0)
        self.cc[pid] = (cc + 1) & 15
        self.packets.append(bytes([0x47, pid >> 8, pid & 255, 0x30 | cc, 7, 0x10]) +
                            pcr + bytes([0xFF]) * 176)


def make_capture(path, count=1000, event_boundary=240, extra=True,
                 new_ecm_pid=2305, unrelated_incomplete=False, pcr_reset=False):
    b = Builder()
    old, new = pmt(False, 1), pmt(extra, 2, ecm_pid=new_ecm_pid)
    for i in range(count):
        if i % 40 == 0:
            b.packet(0, pat(((1, 100), (2, 101)) if unrelated_incomplete else ((1, 100),)))
        elif i % 40 == 1:
            b.packet(100, old if i < 190 else new)
        elif i >= event_boundary and i % 19 == 0:
            b.packet(273, b"\x00\x00\x01\xc0" + bytes(16))
        elif i % 5 == 0:
            ticks = (i * 540_000 if not pcr_reset or i >= 500
                     else 3_000_000_000 + i * 10_000)
            b.pcr_packet(ticks)
        else:
            b.packet(0x20, bytes([i & 255]) * 16)
    path.write_bytes(b"".join(b.packets))


def make_shared_capture(path, count=1000, event_boundary=240):
    b = Builder()
    tables = {
        100: (pmt(False, 1, 1), pmt(True, 2, 1)),
        101: (pmt(False, 1, 2), pmt(True, 2, 2)),
    }
    for i in range(count):
        if i % 40 == 0:
            b.packet(0, pat(((1, 100), (2, 101))))
        elif i % 40 == 1:
            b.packet(100, tables[100][0 if i < 190 else 1])
        elif i % 40 == 2:
            b.packet(101, tables[101][0 if i < 190 else 1])
        elif i >= event_boundary and i % 19 == 0:
            b.packet(273, b"\x00\x00\x01\xc0" + bytes(16))
        elif i % 5 == 0:
            b.pcr_packet(i * 540_000)
        else:
            b.packet(0x20, bytes([i & 255]) * 16)
    path.write_bytes(b"".join(b.packets))


def diagnostic_rows(event=True, summary=1, end_exit=0, malformed=False, duration=18_010_000):
    rows = [
        {"model": "DT295", "phase": 2, "packets": 150, "elapsed_ms": 5000,
         "confirmed_bytes": 150 * 188},
        {"model": "DT295", "phase": 2, "packets": 240, "elapsed_ms": 6000,
         "confirmed_bytes": 240 * 188},
        {"model": "DT295", "phase": 2, "packets": 1000, "elapsed_ms": duration - 1000,
         "confirmed_bytes": 1000 * 188},
    ]
    if event:
        rows.append({"event": "dt295_live_pid_add", "phase": 2, "after_packets": 240,
                     "pid": 273, "slot": 7, "ecm_slot": 63, "ecm_pid": 2305,
                     "previously_present": 0, "existing_routes_and_ecms_preserved": 1})
    if summary is not None:
        rows.append({"event": "dt295_live_add_summary", "phase": 3, "live_add_count": summary})
    rows.extend([
        {"model": "DT295", "phase": 3 if end_exit == 0 else 4, "packets": 1000,
         "elapsed_ms": duration, "confirmed_bytes": 1000 * 188},
        {"event": "end", "exit": end_exit, "packets": 1000, "state": 16},
    ])
    text = "\n".join(json.dumps(row) for row in rows) + "\n"
    return text + ("not json\n" if malformed else "")


class LiveAddEvidenceReviewTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.ts = self.root / "capture.ts"
        self.diag = self.root / "status.jsonl"
        make_capture(self.ts)

    def tearDown(self):
        self.temp.cleanup()

    def analyze(self, **kwargs):
        self.diag.write_text(diagnostic_rows(**kwargs), encoding="utf-8")
        return review.analyze(self.ts, self.diag, 128)

    def test_commit_event_matches_complete_pat_pmt_and_pid_packets(self):
        result = self.analyze()
        self.assertTrue(result["analysis_valid"])
        self.assertEqual(result["live_add_count"], 1)
        self.assertTrue(result["live_add_events"][0]["event_packet_to_saved_ts_mapping_exact"])
        self.assertTrue(result["live_add_events"][0]["table_and_packet_evidence_complete"])
        self.assertTrue(result["live_add_events"][0]["added_pid_absent_in_old_pmt"])
        self.assertTrue(result["live_add_events"][0]["added_pid_present_in_new_pmt"])
        self.assertEqual(result["live_add_events"][0]["program_ecms_before"], [2305])
        self.assertEqual(result["live_add_events"][0]["program_ecms_after"], [2305])
        self.assertGreater(result["live_add_events"][0]["added_pid_packet_evidence"]["pid_valid_headers"], 0)
        self.assertEqual(result["minimum_post_add_seconds_observed"], 18004)
        self.assertEqual(result["fix_path_verdict"], "observed_pass")
        self.assertEqual(result["quality_verdict"].split(";")[0], "not_assessed")

    def test_no_event_is_unobserved_not_quality_pass(self):
        result = self.analyze(event=False, summary=0, duration=60_000)
        self.assertTrue(result["analysis_valid"])
        self.assertEqual(result["fix_path_verdict"], "unobserved")
        self.assertEqual(result["live_add_count"], 0)
        self.assertEqual(result["runtime_after_add_30m"], "unobserved_or_inconclusive")
        self.assertEqual(result["quality_verdict"].split(";")[0], "not_assessed")

    def test_1799_seconds_after_add_stays_inconclusive(self):
        result = self.analyze(duration=1_805_000)
        self.assertEqual(result["fix_path_verdict"], "inconclusive")

    def test_1800_seconds_after_add_passes_fix_path_gate(self):
        result = self.analyze(duration=1_806_000)
        self.assertTrue(result["analysis_valid"])
        self.assertEqual(result["minimum_post_add_seconds_observed"], 1800)
        self.assertEqual(result["fix_path_verdict"], "observed_pass")

    def test_shared_pid_add_is_checked_against_both_programs_once(self):
        make_shared_capture(self.ts)
        self.diag.write_text(diagnostic_rows(), encoding="utf-8")
        result = review.analyze(self.ts, self.diag, 128)
        self.assertTrue(result["analysis_valid"])
        event = result["live_add_events"][0]
        self.assertTrue(event["table_and_packet_evidence_complete"])
        self.assertEqual(event["programs_advertising_added_pid"], [1, 2])
        self.assertEqual([x["program"] for x in event["program_transitions"]], [1, 2])
        self.assertTrue(all(x["ecms_unchanged"] for x in event["program_transitions"]))
        self.assertEqual(result["live_add_count"], 1)

    def test_unrelated_program_without_complete_pmt_does_not_block_target(self):
        make_capture(self.ts, unrelated_incomplete=True)
        self.diag.write_text(diagnostic_rows(), encoding="utf-8")
        result = review.analyze(self.ts, self.diag, 128)
        self.assertTrue(result["live_add_events"][0]["table_and_packet_evidence_complete"])
        self.assertEqual(result["live_add_events"][0]["programs_advertising_added_pid"], [1])
        self.assertEqual(result["fix_path_verdict"], "observed_pass")

    def test_ecm_binding_change_is_a_contradiction(self):
        make_capture(self.ts, new_ecm_pid=2306)
        self.diag.write_text(diagnostic_rows(), encoding="utf-8")
        result = review.analyze(self.ts, self.diag, 128)
        self.assertFalse(result["analysis_valid"])
        self.assertTrue(result["live_add_events"][0]["contradiction_observed"])
        self.assertFalse(result["live_add_events"][0]["event_ecm_sets_unchanged_for_each_program"])
        self.assertEqual(result["fix_path_verdict"], "invalid")

    def test_pcr_reset_cannot_replace_insufficient_status_duration(self):
        make_capture(self.ts, pcr_reset=True)
        self.diag.write_text(diagnostic_rows(duration=1_805_000), encoding="utf-8")
        result = review.analyze(self.ts, self.diag, 128)
        event = result["live_add_events"][0]
        self.assertGreater(event["pcr_elapsed_between_limited_windows_seconds"], 1800)
        self.assertEqual(result["minimum_post_add_seconds_observed"], 1799)
        self.assertEqual(result["runtime_after_add_30m"], "unobserved_or_inconclusive")
        self.assertEqual(result["fix_path_verdict"], "inconclusive")

    def test_summary_mismatch_is_invalid(self):
        result = self.analyze(summary=0)
        self.assertFalse(result["analysis_valid"])
        self.assertEqual(result["fix_path_verdict"], "invalid")

    def test_duplicate_event_pid_or_slot_is_invalid(self):
        rows = diagnostic_rows().splitlines()
        event_line = next(row for row in rows if json.loads(row).get("event") == "dt295_live_pid_add")
        self.diag.write_text("\n".join(rows + [event_line]) + "\n", encoding="utf-8")
        result = review.analyze(self.ts, self.diag, 64)
        self.assertFalse(result["analysis_valid"])
        self.assertEqual(result["diagnostics"]["duplicate_pids"], [273])

    def test_malformed_diagnostics_is_invalid(self):
        result = self.analyze(malformed=True)
        self.assertFalse(result["analysis_valid"])
        self.assertEqual(result["diagnostic_malformed_line_count"], 1)

    def test_stage_cli_writes_fixed_metadata_outputs(self):
        stage = self.root / "stage"
        stage.mkdir()
        (stage / "capture.ts").write_bytes(self.ts.read_bytes())
        (stage / "diagnostic").mkdir()
        (stage / "diagnostic/status.jsonl").write_text(diagnostic_rows(event=False, summary=0),
                                                       encoding="utf-8")
        code = review.main([str(stage), "--window-packets", "64"])
        self.assertEqual(code, 0)
        data = json.loads((stage / "live-add-evidence-review.json").read_text())
        self.assertIn("minimum_post_add_seconds_observed", data)
        self.assertTrue((stage / "live-add-evidence-review.txt").read_text().startswith("DT295"))
        self.assertEqual(review.main(["--stage", str(stage)]), 1)


if __name__ == "__main__":
    unittest.main()
