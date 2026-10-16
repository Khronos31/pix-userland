#!/usr/bin/env python3
"""Hardware-free tests using synthetic TS and synthetic FFmpeg AAC output."""
import importlib.util
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("audit-aac-window.py")
SPEC = importlib.util.spec_from_file_location("audit_aac_window", SCRIPT)
auditor = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(auditor)


def make_aac_frames():
    if not shutil.which("ffmpeg"):
        raise unittest.SkipTest("ffmpeg unavailable for synthetic AAC generation")
    result = subprocess.run([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
        "-i", "anullsrc=r=44100:cl=stereo", "-frames:a", "12", "-c:a", "aac",
        "-f", "adts", "pipe:1"], check=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, timeout=20)
    data = result.stdout
    frames = []
    pos = 0
    while pos + 7 <= len(data):
        header = auditor._AV.ADTSStream.parse_header(data[pos:pos + 7])
        if header is None:
            break
        frame_len = header[0]
        if pos + frame_len > len(data):
            break
        frames.append(data[pos:pos + frame_len])
        pos += frame_len
    if not frames:
        raise AssertionError("synthetic AAC encoder produced no ADTS frames")
    return b"".join(frames)


class TSBuilder:
    def __init__(self):
        self.cc = 0
        self.packets = []

    def packet(self, pid, payload, *, pusi=False):
        payload = payload[:184]
        cc = self.cc
        self.cc = (self.cc + 1) & 15
        if len(payload) == 184:
            header = bytes([0x47, ((0x40 if pusi else 0) | (pid >> 8)),
                            pid & 255, 0x10 | cc])
            self.packets.append(header + payload)
            return
        adaptation_total = 184 - len(payload)
        adaptation_length = adaptation_total - 1
        adaptation = bytes([adaptation_length])
        if adaptation_length:
            adaptation += b"\x00" + bytes([0xFF]) * (adaptation_length - 1)
        header = bytes([0x47, ((0x40 if pusi else 0) | (pid >> 8)),
                        pid & 255, 0x30 | cc])
        packet = header + adaptation + payload
        assert len(packet) == 188
        self.packets.append(packet)

    def stream(self, pid, data, *, pusi_first=True):
        first = True
        while data:
            part, data = data[:184], data[184:]
            self.packet(pid, part, pusi=(first and pusi_first))
            first = False

    def bytes(self):
        return b"".join(self.packets)


def pes(es, *, pid=273, declared_adjust=0):
    declared = len(es) + 3
    declared += declared_adjust
    header = bytes([0x00, 0x00, 0x01, 0xC0, (declared >> 8) & 255,
                    declared & 255, 0x80, 0x00, 0x00])
    return header + es


class AACWindowAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.aac = make_aac_frames()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.ts = self.root / "capture.ts"
        self.run_number = 0

    def tearDown(self):
        self.temp.cleanup()

    def run_window(self, builder, *, start=0, count=None, pid=273):
        self.ts.write_bytes(builder.bytes())
        if count is None:
            count = len(builder.packets) - start
        self.run_number += 1
        output_dir = self.root / f"result-{self.run_number}"
        self.last_output = output_dir
        return auditor.audit_window(self.ts, pid=pid, start_packet=start,
                                    packet_count=count,
                                    output_dir=output_dir)

    def test_offset_seek_ignores_unrequested_bad_prefix_and_decodes_complete_aac(self):
        b = TSBuilder()
        b.packets.extend([b"X" * 188, b"Y" * 188])
        b.stream(273, pes(self.aac))
        result = self.run_window(b, start=2)
        self.assertEqual(result["verdict"], "decoded_pass")
        self.assertEqual(result["input"]["start_packet_zero_based"], 2)
        self.assertEqual(result["input"]["packet_count_requested"], len(b.packets) - 2)
        self.assertEqual(result["window"]["complete_pes_count"], 1)
        self.assertGreater(result["audio"]["frames_emitted"], 0)
        self.assertFalse(result["audio"]["decoder_timed_out"])
        self.assertFalse(list(self.last_output.rglob("*.aac")))

    def test_absent_pid_is_absent_not_pass(self):
        b = TSBuilder()
        b.stream(100, pes(self.aac))
        result = self.run_window(b)
        self.assertEqual(result["verdict"], "absent")
        self.assertEqual(result["audio"]["frames_emitted"], 0)

    def test_leading_partial_pes_is_reported_and_following_complete_pes_decodes(self):
        b = TSBuilder()
        b.packet(273, b"partial continuation bytes", pusi=False)
        b.stream(273, pes(self.aac))
        result = self.run_window(b)
        self.assertEqual(result["verdict"], "decoded_pass")
        self.assertEqual(result["window"]["leading_partial_pes_packets_omitted"], 1)
        self.assertEqual(result["window"]["complete_pes_count"], 1)

    def test_interior_truncated_pes_is_preserved_as_failure(self):
        b = TSBuilder()
        b.stream(273, pes(self.aac, declared_adjust=100))
        b.stream(273, pes(self.aac))
        result = self.run_window(b)
        self.assertEqual(result["verdict"], "fail")
        self.assertTrue(result["window"]["interior_pes_errors"])

    def test_interior_adts_framing_error_fails_after_initial_sync(self):
        b = TSBuilder()
        b.stream(273, pes(self.aac + b"\x00" * 32))
        b.stream(273, pes(self.aac))
        result = self.run_window(b)
        self.assertEqual(result["verdict"], "fail")
        self.assertIsNotNone(result["audio"]["framing_error"])

    def test_selected_pid_tei_and_scrambling_are_failures(self):
        for header_index, bit, expected in ((1, 0x80, "TEI"),
                                            (3, 0x40, "transport scrambling")):
            with self.subTest(expected=expected):
                b = TSBuilder()
                b.stream(273, pes(self.aac))
                packet = bytearray(b.packets[0])
                packet[header_index] |= bit
                b.packets[0] = bytes(packet)
                result = self.run_window(b)
                self.assertEqual(result["verdict"], "fail")
                self.assertIn(expected, [x["error"] for x in result["window"]["ts_packet_errors"]])

    def test_selected_pid_continuity_gap_is_a_failure(self):
        b = TSBuilder()
        b.stream(273, pes(self.aac * 3))
        self.assertGreater(len(b.packets), 2)
        packet = bytearray(b.packets[1])
        packet[3] = (packet[3] & 0xF0) | ((packet[3] + 2) & 0x0F)
        b.packets[1] = bytes(packet)
        result = self.run_window(b)
        self.assertEqual(result["verdict"], "fail")
        self.assertIn("continuity counter",
                      [x["error"] for x in result["window"]["ts_packet_errors"]])

    def test_selected_pid_invalid_adaptation_control_is_a_failure(self):
        b = TSBuilder()
        b.stream(273, pes(self.aac))
        packet = bytearray(b.packets[0])
        packet[3] &= 0xCF
        b.packets[0] = bytes(packet)
        result = self.run_window(b)
        self.assertEqual(result["verdict"], "fail")
        self.assertIn("adaptation_field_control=0",
                      [x["error"] for x in result["window"]["ts_packet_errors"]])

    def test_malformed_interior_pes_prefix_is_a_failure(self):
        b = TSBuilder()
        b.stream(273, b"BAD" + bytes(32))
        b.stream(273, pes(self.aac))
        result = self.run_window(b)
        self.assertEqual(result["verdict"], "fail")
        self.assertTrue(result["window"]["interior_pes_errors"])

    def test_final_open_pes_is_counted_as_window_boundary(self):
        b = TSBuilder()
        b.stream(273, pes(self.aac))
        b.stream(273, pes(self.aac)[:14])
        result = self.run_window(b)
        self.assertEqual(result["verdict"], "decoded_pass")
        self.assertEqual(result["window"]["complete_pes_count"], 1)
        self.assertEqual(result["window"]["final_open_pes_count"], 1)
        self.assertEqual(result["window"]["final_partial_pes_count"], 1)

    def test_no_frames_never_passes(self):
        b = TSBuilder()
        b.stream(273, pes(b"not ADTS" * 3))
        result = self.run_window(b)
        self.assertIn(result["verdict"], ("fail", "inconclusive"))
        self.assertNotEqual(result["verdict"], "decoded_pass")

    def test_initial_adts_sync_bytes_are_counted_separately(self):
        b = TSBuilder()
        b.stream(273, pes(b"\x00\x11\x22" + self.aac))
        result = self.run_window(b)
        self.assertEqual(result["verdict"], "decoded_pass")
        self.assertEqual(result["audio"]["initial_sync_bytes_omitted"], 3)

    def test_pce_wait_is_reported_separately_and_cannot_pass(self):
        es = bytearray(self.aac)
        pos = 0
        while pos + 7 <= len(es):
            header = auditor._AV.ADTSStream.parse_header(es[pos:pos + 7])
            if header is None or pos + header[0] > len(es):
                break
            es[pos + 3] &= 0x3F
            pos += header[0]
        b = TSBuilder()
        b.stream(273, pes(bytes(es)))
        result = self.run_window(b)
        self.assertEqual(result["verdict"], "inconclusive")
        self.assertGreater(result["audio"]["pce_wait_pending_epoch_count"], 0)
        self.assertGreater(result["audio"]["frames_before_pce"], 0)
        self.assertEqual(result["audio"]["frames_emitted"], 0)

    def test_window_cap_is_enforced_before_input_read(self):
        with self.assertRaisesRegex(ValueError, "packet_count"):
            auditor.audit_window(self.root / "does-not-exist.ts", pid=273,
                                 start_packet=0, packet_count=65537,
                                 output_dir=self.root / "no-output")
        self.assertFalse((self.root / "no-output").exists())


if __name__ == "__main__":
    unittest.main()
