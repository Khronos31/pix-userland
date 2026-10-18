"""Synthetic profile normalization tests; never loads private files."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "prepare_dt295_assets", ROOT / "tools" / "prepare-dt295-assets.py")
ASSETS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ASSETS)


def field(name, address, value=0, shift=0, width=1):
    return {"name": name, "address": address, "shift": shift,
            "width": width, "value": value}


class TestDT295Assets(unittest.TestCase):
    def parse(self, profile):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "synthetic-profile.json"
            path.write_text(json.dumps(profile), encoding="utf-8")
            return ASSETS.parse_profile(path)

    def profile(self):
        return {
            "schema": 1,
            "target": 0,
            "ts_key_request_words": [1, 2, 3],
            "fields": [
                field("INIT_S_AUTO_PID_MODE", 0x1000, 1),
                field("INIT_S_PAYLOAD_UNIT_START_DISABLE_MODE", 0x1002, 0),
                field("INIT_S_PROGRAM_NUMBER", 0x1018, 0x1234, width=16),
                field("INIT_S_V_STREAM_TYPE", 0x101C, 2),
                field("INIT_S_A_STREAM_TYPE", 0x101C, 4),
                field("INIT_S_V_ECM", 0x10A2, 5, width=7),
                field("INIT_S_A_ECM", 0x10A2, 6, shift=8, width=7),
                field("INIT_S_PID_SLOT_63", 0x10E0, 63, width=7),
                field("INIT_S_ECM_SLOT_6", 0x10A8, 6, shift=8, width=7),
                field("INIT_S_OTHER_BASELINE", 0x1100, 1),
            ],
        }

    def test_route_fields_are_removed_and_required_modes_are_normalized(self):
        fields, key_request = self.parse(self.profile())
        records = [struct.unpack_from("<HBBHH", fields, offset)
                   for offset in range(0, len(fields), 8)]
        self.assertEqual(key_request, struct.pack(">3I", 1, 2, 3))
        self.assertEqual(records, [
            (0x1000, 0, 1, 0, 0),
            (0x1002, 0, 1, 1, 0),
            (0x1100, 0, 1, 1, 0),
        ])

    def test_missing_or_duplicate_normalized_fields_are_rejected(self):
        profile = self.profile()
        profile["fields"] = [row for row in profile["fields"]
                             if row["name"] != "INIT_S_AUTO_PID_MODE"]
        with self.assertRaisesRegex(ValueError, "missing required normalized field"):
            self.parse(profile)

        profile = self.profile()
        profile["fields"].append(field("INIT_S_AUTO_PID_MODE", 0x1004, 1))
        with self.assertRaisesRegex(ValueError, "duplicate INIT_S_AUTO_PID_MODE"):
            self.parse(profile)

    def test_bad_profile_shape_and_bit_ranges_are_rejected(self):
        profile = self.profile()
        profile["fields"][0] = "malformed row"
        with self.assertRaisesRegex(ValueError, "fields must be objects"):
            self.parse(profile)

        profile = self.profile()
        profile["fields"][-1]["width"] = 17
        with self.assertRaisesRegex(ValueError, "bit range"):
            self.parse(profile)

        profile = self.profile()
        profile["target"] = 1
        with self.assertRaisesRegex(ValueError, "target 0"):
            self.parse(profile)

    def test_boolean_schema_and_target_are_rejected(self):
        profile = self.profile()
        profile["schema"] = True
        with self.assertRaisesRegex(ValueError, "schema 1 and target 0"):
            self.parse(profile)

    def test_bundle_keeps_ts_iv_distinct_from_control_session_material(self):
        fields, key_request = self.parse(self.profile())
        material = bytes(ASSETS.MAT_END - ASSETS.MAT_BASE)
        control_iv = material[0x85218 - ASSETS.MAT_BASE:0x85218 - ASSETS.MAT_BASE + 16]
        ts_iv = bytes(range(16))
        self.assertNotEqual(ts_iv, control_iv)
        bundle = ASSETS.build_bundle(b"idle", b"trc", material, fields, key_request, ts_iv)
        lengths = [struct.unpack_from("<I", bundle, 8 + i * 36)[0] for i in range(6)]
        sections = []
        offset = 8 + 6 * 36
        for length in lengths:
            sections.append(bundle[offset:offset + length])
            offset += length
        self.assertEqual(sections[2], material)
        self.assertEqual(sections[5], ts_iv)
        self.assertNotEqual(sections[5], sections[2][0x85218 - ASSETS.MAT_BASE:
                                                     0x85218 - ASSETS.MAT_BASE + 16])

        profile = self.profile()
        profile["target"] = False
        with self.assertRaisesRegex(ValueError, "schema 1 and target 0"):
            self.parse(profile)


if __name__ == "__main__":
    unittest.main()
