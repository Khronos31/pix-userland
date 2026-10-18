#!/usr/bin/env python3
"""Compare the extracted foundation's observable behavior with pinned PX4 code."""

import argparse
import hashlib
import json
import pathlib
import stat
import subprocess
import tempfile


DRIVER = r'''#include <cstdint>
#include <iostream>
#include <string_view>

#ifdef DTV_LEGACY
#include "px4/error.h"
#include "px4/logging.h"
#include "px4/mock_transport.h"
#include "px4/transport.h"
namespace api = px4::userland;
#else
#include "dtv/userland/error.h"
#include "dtv/userland/logging.h"
#include "dtv/userland/mock_transport.h"
#include "dtv/userland/transport.h"
namespace api = dtv::userland;
#endif

static int logs = 0;
static int log_sink_calls = 0;
static int replacement_logs = 0;
static int replacement_sink_calls = 0;
static void sink(const api::LogRecord& record, void*) noexcept
{
    ++log_sink_calls;
    logs += static_cast<int>(record.level) + static_cast<int>(record.message.size());
}
static void replacement_sink(const api::LogRecord& record, void*) noexcept
{
    ++replacement_sink_calls;
    replacement_logs += static_cast<int>(record.level) + static_cast<int>(record.message.size());
}

static void result(std::string_view label, api::Error error)
{
    std::cout << label << ':' << api::error_string(error) << '\n';
}

int main()
{
    for (unsigned value = 0; value <= 16; ++value) {
        result("error", static_cast<api::Error>(value));
    }
    result("internal", api::Error::INTERNAL);
    result("unknown", static_cast<api::Error>(42));
    auto failed = api::Result<int>::failure(api::Error::TIMEOUT);
    auto succeeded = api::Result<int>::success(7);
    auto void_failed = api::Result<void>::failure(api::Error::INTERNAL);
    auto void_succeeded = api::Result<void>::success();
    std::cout << "results:" << failed.has_value() << ',' << static_cast<int>(failed.error())
              << ',' << succeeded.has_value() << ',' << succeeded.value() << ','
              << void_failed.has_value() << ',' << static_cast<int>(void_failed.error()) << ','
              << void_succeeded.has_value() << ',' << static_cast<int>(void_succeeded.error()) << '\n';
    api::Logger logger(api::LogLevel::warn, sink, nullptr);
    logger.log(api::LogLevel::info, "hidden");
    logger.log(api::LogLevel::warn, "visible");
    logger.set_minimum_level(api::LogLevel::debug);
    logger.log(api::LogLevel::debug, "debug");
    std::cout << "logs:" << log_sink_calls << ',' << logs << ','
              << api::log_level_string(static_cast<api::LogLevel>(9)) << '\n';
    logger.set_sink(replacement_sink, nullptr);
    logger.log(api::LogLevel::info, "again");
    logger.set_sink(nullptr, nullptr);
    logger.log(api::LogLevel::error, "discard");
    std::cout << "sink_replacement:" << replacement_sink_calls << ',' << replacement_logs << ','
              << log_sink_calls << ',' << logs << '\n';

#ifdef DTV_LEGACY
    api::MockTransport mock;
#else
    api::MockTransport mock(api::MockTransportPolicy{65535U, 1048576U, 0x84U});
#endif
    std::uint8_t payload[] = {3, 5, 8, 13};
    std::uint8_t output[8] = {};
    api::BulkReadObservation observation{};
    mock.expect_bulk_read(0x81U, {payload, sizeof(payload)}, api::MockOutcome::timeout, 2U);
    auto partial = mock.bulk_read(0x81U, {output, sizeof(output)}, {125U}, &observation);
    std::cout << "partial:" << partial.has_value() << ',' << static_cast<int>(partial.error())
              << ',' << observation.transferred << ',' << static_cast<int>(observation.completion_error)
              << ',' << static_cast<int>(output[0]) << ',' << static_cast<int>(output[1]) << '\n';
    auto invalid_endpoint = mock.bulk_read(0x01U, {output, sizeof(output)}, {5U});
    auto invalid_stream = mock.start_stream({0x85U, 16U, 2U});
    auto oversized = mock.bulk_write(0x02U, {payload, 65536U}, {5U});
    std::cout << "invalid:" << static_cast<int>(invalid_endpoint.error()) << ','
              << static_cast<int>(invalid_stream.error()) << ','
              << static_cast<int>(oversized.error()) << '\n';

#ifdef DTV_LEGACY
    api::MockTransport fifo;
#else
    api::MockTransport fifo(api::MockTransportPolicy{65535U, 1048576U, 0x84U});
#endif
    std::uint8_t request[] = {1, 2};
    fifo.expect_bulk_write(0x02U, {request, sizeof(request)});
    auto wrong_first = fifo.bulk_read(0x81U, {output, sizeof(output)}, {10U});
    auto correct_next = fifo.bulk_write(0x02U, {request, sizeof(request)}, {20U});
    std::cout << "fifo:" << static_cast<int>(wrong_first.error()) << ','
              << static_cast<int>(correct_next.error()) << ',' << fifo.remaining_expectations()
              << ',' << fifo.operations().size() << '\n';

#ifdef DTV_LEGACY
    api::MockTransport stream;
#else
    api::MockTransport stream(api::MockTransportPolicy{65535U, 1048576U, 0x84U});
#endif
    const api::StreamConfig config{0x84U, 16U, 2U};
    stream.expect_stream_start(config);
    stream.expect_stream_event({payload, sizeof(payload)}, api::MockOutcome::short_transfer);
    stream.expect_stream_cancel();
    stream.expect_stream_start(config);
    stream.expect_stream_stop();
    auto started = stream.start_stream(config);
    auto event = stream.wait_stream({77U});
    auto canceled = stream.cancel_stream();
    auto restarted = stream.start_stream(config);
    auto stopped = stream.stop_stream();
    std::cout << "stream:" << started.has_value() << ',' << event.has_value() << ','
              << (event ? static_cast<int>(event.value().kind) : -1) << ','
              << (event ? event.value().size : 0U) << ',' << stream.stream_active() << ','
              << canceled.has_value() << ',' << restarted.has_value() << ','
              << stopped.has_value() << ',' << stream.stream_active()
              << ',' << stream.timeouts().size() << ',' << stream.operations().size() << '\n';
    std::cout << "levels:" << api::log_level_string(api::LogLevel::trace) << ','
              << api::log_level_string(api::LogLevel::error) << '\n';
}
'''

SOURCE_REVISION = "90f08a594487e2603bd5c09f778a0b3b64ac9a4d"
SOURCE_PATHS = {
    "userland/include/px4/error.h",
    "userland/include/px4/logging.h",
    "userland/include/px4/transport.h",
    "userland/include/px4/mock_transport.h",
    "userland/src/error.cpp",
    "userland/src/logging.cpp",
    "userland/src/mock_transport.cpp",
}


def verify_source_files(source, provenance_path):
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    records = provenance["foundation"]["source_files"]
    if {record["path"] for record in records} != SOURCE_PATHS or len(records) != len(SOURCE_PATHS):
        raise ValueError("PROVENANCE.json must list exactly the seven extracted foundation files")
    for record in records:
        path = source / record["path"]
        if path.is_symlink() or not stat.S_ISREG(path.stat().st_mode):
            raise ValueError(f"pinned source file is not a regular file: {record['path']}")
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != record["sha256"]:
            raise ValueError(f"pinned source file hash mismatch: {record['path']}")


def compile_and_run(cxx, include_dir, sources, macro, output, temp_dir):
    driver = temp_dir / ("driver_legacy.cpp" if macro else "driver_common.cpp")
    driver.write_text(DRIVER, encoding="utf-8")
    command = [cxx, "-std=c++17", "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra",
               "-Wpedantic", "-I", str(include_dir)]
    if macro:
        command.append("-DDTV_LEGACY=1")
    command.extend([str(driver), *(str(item) for item in sources), "-o", str(output)])
    subprocess.run(command, check=True)
    return subprocess.run([str(output)], check=True, capture_output=True, text=True).stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=pathlib.Path,
                        help="PX4 userland source tree pinned at 90f08a594487e2603bd5c09f778a0b3b64ac9a4d")
    parser.add_argument("--cxx", required=True)
    args = parser.parse_args()
    source = args.source.resolve()
    revision = subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"],
                              check=True, capture_output=True, text=True).stdout.strip()
    if revision != SOURCE_REVISION:
        parser.error(f"--source must be fixed at {SOURCE_REVISION}")
    repo = pathlib.Path(__file__).resolve().parents[1]
    try:
        verify_source_files(source, repo / "PROVENANCE.json")
    except (KeyError, OSError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))
    original_root = source / "userland"
    original_sources = [original_root / "src" / name for name in
                       ("error.cpp", "logging.cpp", "mock_transport.cpp")]
    if not all(path.is_file() for path in original_sources):
        parser.error("--source must name the pinned PX4 source root containing userland/src")

    with tempfile.TemporaryDirectory(prefix="dtv-legacy-equivalence-") as temporary:
        temp_dir = pathlib.Path(temporary)
        legacy = compile_and_run(args.cxx, original_root / "include", original_sources,
                                 True, temp_dir / "legacy", temp_dir)
        common = compile_and_run(args.cxx, repo / "include",
                                 [repo / "src" / name for name in
                                  ("error.cpp", "logging.cpp", "mock_transport.cpp")],
                                 False, temp_dir / "common", temp_dir)
    if legacy != common:
        print("Legacy and common outputs differ:")
        print("--- legacy ---")
        print(legacy, end="")
        print("--- common ---")
        print(common, end="")
        raise SystemExit(1)
    print("legacy and common foundation behavior matched")


if __name__ == "__main__":
    main()
