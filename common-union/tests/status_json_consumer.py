#!/usr/bin/env python3
import copy
import json
import subprocess
import sys
from pathlib import Path


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise AssertionError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def expected_primary():
    return {
        "schema": {"name": "userland.status", "major": 1, "minor": 0},
        "producer": {"product": "p", "version": "v", "core_version": None},
        "context": {"epoch": "000102030405060708090a0b0c0d0e0f", "instance": None, "state": "ready", "last_error": {"code": "UNKNOWN", "numeric_code": 4294967295, "operation": "read", "detail": "q\"\\\nあ"}},
        "snapshot": {"sequence": "18446744073709551615", "observed_at_ns": "7"},
        "scope": {"kind": "enclosure", "id": None},
        "enclosure": None,
        "topology": {"state": "complete", "expected_function_count": 1},
        "usb_functions": [{
            "id": "0", "vid": "0b06", "pid": "0005", "mode": "runtime",
            "location": {"usb_path": None, "bus": None, "address": None},
            "serial": {"state": "absent", "value": None},
            "identification": {"model": None, "reason": "descriptor_unreadable"},
            "present": True, "last_error": None,
        }],
        "receivers": [{
            "descriptor": {
                "id": "0", "systems": ["isdb-t"], "binding": None,
                "capabilities": {"ts": {"scope": "multiplex", "pid_filter": None}, "controls": {}, "measurements": {"lock": {"unit": "boolean"}}},
            },
            "present": True, "state": "streaming", "operation": "idle", "leased": True,
            "tune_revision": "18446744073709551615", "current_tune": None,
            "last_successful_tune": None, "stream": None, "last_stream": None,
            "measurements": {"lock": {"value": False, "sampled_at_ns": "0", "reason": None}},
            "statistics": {"receiver": {"captured_packets": {"value": "18446744073709551615", "sampled_at_ns": "7", "reason": None, "overflow": True}}, "stream": None}, "last_error": None,
        }],
        "readers": [],
    }


def common_root():
    return expected_primary()


def expected_reader(sampled):
    expected = common_root()
    expected["context"]["last_error"] = {"code": "UNKNOWN", "numeric_code": 4294967295, "operation": "read", "detail": "q\"\\\nあ"}
    expected["scope"] = {"kind": "reader", "id": "0"}
    expected.pop("usb_functions")
    expected.pop("receivers")
    expected["readers"] = [{
        "descriptor": {"id": "0", "binding": None},
        "present": sampled,
        "card": {"state": "absent" if sampled else "unknown", "sampled_at_ns": "0" if sampled else None, "reason": None if sampled else "not_sampled"},
        "operation": "idle", "handles_open": 0, "transaction_active": False,
        "last_error": {"code": "DISCONNECTED", "operation": "read", "detail": "lost"},
    }]
    return expected


def expected_reader_unavailable():
    expected = expected_reader(False)
    expected["readers"][0]["card"] = {"state": "unavailable", "sampled_at_ns": None, "reason": "device_lost"}
    return expected


def expected_active_stream():
    expected = common_root()
    expected["scope"] = {"kind": "stream", "id": "1"}
    expected.pop("usb_functions")
    expected.pop("readers")
    receiver = expected["receivers"][0]
    receiver["descriptor"] = {
        "id": "0", "systems": ["isdb-t", "isdb-s"], "binding": None,
        "capabilities": {
            "ts": {"scope": "multiplex", "pid_filter": None},
            "controls": {"lna": {"values": [False, True], "default_by_system": {"isdb-t": False, "isdb-s": True}}, "lnb_voltage": {"values": [0, 15]}},
            "measurements": {"lock": {"unit": "boolean"}},
        },
    }
    receiver["tune_revision"] = "1"
    receiver["current_tune"] = {"revision": "1", "system": "isdb-s", "frequency_hz": 473142857, "programmed_hz": 473143000, "selection": {"kind": "tsid", "value": 4660}, "verification": "lock"}
    receiver["stream"] = {"id": "1", "tune_revision": "1", "phase": "active"}
    receiver["statistics"]["stream"] = {
        "transmitted_packets": {"value": "100", "sampled_at_ns": "7", "reason": None, "overflow": False},
        "queue_dropped_packets": {"value": "17", "sampled_at_ns": "7", "reason": None, "overflow": False},
    }
    return expected


def expected_retained_stream():
    expected = common_root()
    expected["scope"] = {"kind": "stream", "id": "9"}
    expected.pop("usb_functions")
    expected.pop("readers")
    receiver = expected["receivers"][0]
    receiver["state"] = "leased"
    receiver["tune_revision"] = "2"
    receiver["last_successful_tune"] = {"revision": "1", "system": "isdb-s", "frequency_hz": 473142857, "programmed_hz": 473143000, "selection": {"kind": "slot", "value": 7}, "verification": "lock"}
    receiver["stream"] = None
    receiver["last_stream"] = {
        "id": "9", "tune_revision": "1", "reason": "stopped", "error": None,
        "statistics": {
            "transmitted_packets": {"value": "100", "sampled_at_ns": "0", "reason": None, "overflow": False},
            "queue_dropped_packets": {"value": "17", "sampled_at_ns": "0", "reason": None, "overflow": False},
        },
    }
    return expected


def expected_empty_receiver():
    expected = common_root()
    expected["scope"] = {"kind": "receiver", "id": "0"}
    expected.pop("usb_functions")
    expected.pop("readers")
    receiver = expected["receivers"][0]
    receiver["descriptor"]["systems"] = ["isdb-t", "isdb-s"]
    receiver["descriptor"]["capabilities"]["controls"] = {"lna": {"values": [False, True], "default_by_system": {"isdb-t": False, "isdb-s": None}}}
    receiver["descriptor"]["capabilities"]["measurements"] = {}
    receiver["measurements"] = {}
    receiver["statistics"] = {"receiver": {}, "stream": None}
    return expected


def expected_receiver_scope():
    expected = common_root()
    expected["scope"] = {"kind": "receiver", "id": "0"}
    expected.pop("usb_functions")
    expected.pop("readers")
    return expected


def expected_unknown_reason(reason):
    expected = expected_receiver_scope()
    expected["receivers"][0]["measurements"]["lock"] = {"value": None, "sampled_at_ns": None, "reason": reason}
    return expected


def expected_counter_variants():
    expected = expected_receiver_scope()
    expected["receivers"][0]["statistics"]["receiver"] = {
        "captured_packets": {"value": "0", "sampled_at_ns": "0", "reason": None, "overflow": False},
        "usb_errors": {"value": None, "sampled_at_ns": None, "reason": "read_failed", "overflow": None},
        "framing_errors": {"value": "18446744073709551615", "sampled_at_ns": "7", "reason": None, "overflow": True},
    }
    return expected


def expected_lnb_only():
    expected = expected_receiver_scope()
    expected["receivers"][0]["descriptor"]["capabilities"]["controls"] = {"lnb_voltage": {"values": [0, 15]}}
    return expected


def expected_starting_unknown():
    expected = common_root()
    expected["context"]["state"] = "starting"
    expected["scope"] = {"kind": "enclosure", "id": None}
    expected["topology"] = {"state": "unknown", "expected_function_count": None}
    expected["usb_functions"] = []
    expected.pop("receivers")
    expected.pop("readers")
    return expected


def expected_error():
    expected = common_root()
    expected["context"]["last_error"] = {"code": "INVALID_STATE", "operation": "read", "detail": "q\"\\\nあ"}
    return expected


def max_error(code, operation, detail, numeric_code=None):
    result = {"code": code, "operation": operation, "detail": detail}
    if numeric_code is not None:
        result["numeric_code"] = numeric_code
    return result


def expected_max_snapshot():
    control = "\x01"
    error_unknown = max_error("UNKNOWN", control * 32, control * 256, 4294967295)
    receiver_descriptor = {
        "id": None, "systems": ["isdb-t", "isdb-s"], "binding": None,
        "capabilities": {
            "ts": {"scope": "pid_subset", "pid_filter": {"max_pids": 8192}},
            "controls": {"lna": {"values": [False, True], "default_by_system": {"isdb-t": False, "isdb-s": True}}, "lnb_voltage": {"values": [0, 15]}},
            "measurements": {"lock": {"unit": "boolean"}, "cnr_mdb": {"unit": "milli_dB"}, "signal_mdbm": {"unit": "milli_dBm"}},
        },
    }
    receivers = []
    for index in range(32):
        descriptor = copy.deepcopy(receiver_descriptor)
        descriptor["id"] = str(index)
        receivers.append({
            "descriptor": descriptor, "present": True, "state": "streaming", "operation": "idle", "leased": True,
            "tune_revision": "2",
            "current_tune": {"revision": "2", "system": "isdb-s", "frequency_hz": 473142857, "programmed_hz": 473143000, "selection": {"kind": "tsid", "value": 4660}, "verification": "lock"},
            "last_successful_tune": {"revision": "1", "system": "isdb-s", "frequency_hz": 473142857, "programmed_hz": 473143000, "selection": {"kind": "slot", "value": 7}, "verification": "lock"},
            "stream": {"id": str(index + 1), "tune_revision": "2", "phase": "active"},
            "last_stream": {"id": str(index + 100), "tune_revision": "1", "reason": "slow_consumer", "error": max_error("UNKNOWN", control * 32, control * 256, 42), "statistics": {
                "transmitted_packets": {"value": "100", "sampled_at_ns": "7", "reason": None, "overflow": False},
                "queue_dropped_packets": {"value": "17", "sampled_at_ns": "7", "reason": None, "overflow": False},
            }},
            "measurements": {
                "lock": {"value": index % 2 == 0, "sampled_at_ns": "7", "reason": None},
                "cnr_mdb": {"value": -2147483648, "sampled_at_ns": "7", "reason": None},
                "signal_mdbm": {"value": 2147483647, "sampled_at_ns": "7", "reason": None},
            },
            "statistics": {
                "receiver": {
                    "captured_packets": {"value": "18446744073709551615", "sampled_at_ns": "7", "reason": None, "overflow": True},
                    "usb_errors": {"value": str(index + 1), "sampled_at_ns": "7", "reason": None, "overflow": False},
                    "framing_errors": {"value": str(index + 2), "sampled_at_ns": "7", "reason": None, "overflow": False},
                    "upstream_lost_packets": {"value": str(index + 3), "sampled_at_ns": "7", "reason": None, "overflow": False},
                },
                "stream": {
                    "transmitted_packets": {"value": "100", "sampled_at_ns": "7", "reason": None, "overflow": False},
                    "queue_dropped_packets": {"value": "17", "sampled_at_ns": "7", "reason": None, "overflow": False},
                },
            },
            "last_error": copy.deepcopy(error_unknown),
        })
    readers = [{
        "descriptor": {"id": str(index), "binding": None}, "present": True,
        "card": {"state": "present", "sampled_at_ns": "7", "reason": None},
        "operation": "idle", "handles_open": 1, "transaction_active": False,
        "last_error": {"code": "TIMEOUT", "operation": control * 32, "detail": control * 256},
    } for index in range(8)]
    functions = [{
        "id": str(index), "vid": "1234", "pid": "5678", "mode": "runtime",
        "location": {"usb_path": control * 64, "bus": 1, "address": 2},
        "serial": {"state": "present", "value": control * 512},
        "identification": {"model": {"id": control * 64, "name": control * 128}, "reason": None},
        "present": True,
        "last_error": {"code": "TIMEOUT", "operation": control * 32, "detail": control * 256},
    } for index in range(8)]
    return {
        "schema": {"name": "userland.status", "major": 1, "minor": 0},
        "producer": {"product": control * 32, "version": control * 64, "core_version": control * 64},
        "context": {"epoch": "00000000000000000000000000000000", "instance": control * 64, "state": "ready", "last_error": copy.deepcopy(error_unknown)},
        "snapshot": {"sequence": "18446744073709551615", "observed_at_ns": "7"},
        "scope": {"kind": "enclosure", "id": None},
        "enclosure": {"id": "0", "model": {"id": control * 64, "name": control * 128}, "identity": {"serial": control * 512, "serial_state": "present", "uniqueness": "unknown"}, "limits": {"max_concurrent_streams": 32}},
        "topology": {"state": "complete", "expected_function_count": 8},
        "usb_functions": functions, "receivers": receivers, "readers": readers,
    }


EXPECTED = {
    "primary": expected_primary(),
    "receiver_scope": expected_receiver_scope(),
    "reader_unknown": expected_reader(False),
    "reader_sampled": expected_reader(True),
    "reader_unavailable": expected_reader_unavailable(),
    "active_stream": expected_active_stream(),
    "retained_stream": expected_retained_stream(),
    "known_error": expected_error(),
    "empty_receiver": expected_empty_receiver(),
    "starting_unknown": expected_starting_unknown(),
    "max_snapshot": expected_max_snapshot(),
    "lnb_only": expected_lnb_only(),
    "unsupported_omitted": expected_receiver_scope(),
    "counter_variants": expected_counter_variants(),
}
for reason in ("not_sampled", "stale", "tune_changed", "device_lost", "read_failed"):
    EXPECTED[f"unknown_reason_{reason}"] = expected_unknown_reason(reason)
for code in (42, 256, 257):
    expected = common_root()
    expected["context"]["last_error"] = {"code": "UNKNOWN", "numeric_code": code, "operation": "read", "detail": "q\"\\\nあ"}
    EXPECTED[f"unknown_error_{code}"] = expected
examples_path = Path(__file__).parent / "fixtures" / "status_json_examples.json"
examples = json.loads(examples_path.read_text(encoding="utf-8"), object_pairs_hook=unique_object)
assert examples["source"] == {"path": "shared/examples/json-records.json", "sha256": "6faa56eb8c0d6ac45873ced1fa5c94ad30868e1edf9cc0f332fccb872034c575"}
example_status = examples["documents"]
EXPECTED["example_cardless_status"] = example_status["cardless_status_supported_but_unknown_cnr"]
EXPECTED["example_receiver_scope"] = example_status["receiver_scope_not_empty_readers"]
EXPECTED["starting_unknown"] = example_status["starting_with_unknown_topology"]


for name, expected in EXPECTED.items():
    raw = subprocess.run([sys.argv[1], "--json", name], check=True, capture_output=True).stdout
    assert raw.endswith(b"}\n") and raw.count(b"\n") == 1, name
    actual = json.loads(raw, object_pairs_hook=unique_object)
    assert actual == expected, name
    if name == "primary":
        golden = json.dumps(expected, separators=(",", ":"), ensure_ascii=False).encode("utf-8") + b"\n"
        assert raw == golden
    if name == "max_snapshot":
        assert len(raw) > 65536
