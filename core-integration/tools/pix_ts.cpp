// SPDX-License-Identifier: MIT
#include <dtv/host_ts/px4_ts_core.h>
#ifdef _WIN32
#include <dtv/host_ts/px4_ts_windows_output.h>
#include "px4_windows_args.h"
#include "px4d_signals.h"
#else
#include <dtv/host_ts/px4_ts_posix.h>
#endif

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace px4::userland;
using namespace px4::userland::cli;

#ifndef _WIN32
volatile std::sig_atomic_t stop_flag = 0;

void stop_handler(int) noexcept { stop_flag = 1; }
#endif

class PosixClock final : public Px4TsClock {
public:
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }
};

class PosixSignal final : public Px4TsSignal {
public:
    bool stop_requested() const noexcept override
    {
#ifdef _WIN32
        return px4::userland::px4d::stop_requested();
#else
        return stop_flag != 0;
#endif
    }
};

bool install_signals() noexcept
{
#ifdef _WIN32
    return px4::userland::px4d::install_signal_handlers();
#else
    struct sigaction ignore_pipe {};
    ignore_pipe.sa_handler = SIG_IGN;
    if (sigemptyset(&ignore_pipe.sa_mask) != 0 ||
        ::sigaction(SIGPIPE, &ignore_pipe, nullptr) != 0) return false;
    struct sigaction action {};
    action.sa_handler = stop_handler;
    if (sigemptyset(&action.sa_mask) != 0) return false;
    return ::sigaction(SIGINT, &action, nullptr) == 0 &&
           ::sigaction(SIGTERM, &action, nullptr) == 0;
#endif
}

void usage(FILE* file) noexcept
{
    std::fprintf(file,
        "usage: pix-ts (--device serial:SELECTOR|usb:SELECTOR | --instance TOKEN) "
        "[--receiver ID] (-c|--channel CH | -f|--freq HZ) [--system isdb-t|isdb-s] "
        "[--slot N|--tsid N] [--time SECONDS|-] [-o PATH|-] "
        "[--runtime-dir PATH] [--group] [--packet-count N] "
        "[--tune-timeout-ms N]\n");
}

void print_counters(const char* label, const ipc::CountersPayload& counters) noexcept
{
    const auto print = [](const char* name, std::uint64_t value,
                          dtv::userland::wire::MeasureState state) noexcept {
        if (state == dtv::userland::wire::MeasureState::valid)
            std::fprintf(stderr, " %s=%llu", name, static_cast<unsigned long long>(value));
        else if (state == dtv::userland::wire::MeasureState::unknown)
            std::fprintf(stderr, " %s=unknown", name);
    };
    std::fprintf(stderr, "%s", label);
    print("packets", counters.packets, counters.availability[0U]);
    print("bytes", counters.bytes, counters.availability[1U]);
    print("usb-errors", counters.usb_errors, counters.availability[6U]);
    std::fputc('\n', stderr);
}

}  // namespace

int run(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::puts("pix-ts " PIX_RUNTIME_VERSION);
        return 0;
    }
    if (argc == 2 && (std::strcmp(argv[1], "-h") == 0 ||
                      std::strcmp(argv[1], "--help") == 0)) {
        usage(stdout);
        return 0;
    }

    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--lnb-voltage") == 0) {
            std::fprintf(stderr, "pix-ts: --lnb-voltage is not supported by PIX\n");
            return 2;
        }
    }
    const TsClientProfile profile{"pix-ts", "pix-userland",
        dtv::userland::identity::Product::pix, true, false, nullptr, true};
    const Px4TsArguments arguments = parse_px4_ts_product_arguments(
        argc, const_cast<const char* const*>(argv), profile);
    if (!arguments.valid) {
        std::fprintf(stderr, "argument error: %s\n", arguments.error.c_str());
        usage(stderr);
        return 2;
    }
    if (arguments.help) return 0;
    if (arguments.version) {
        std::puts("pix-ts " PIX_RUNTIME_VERSION);
        return 0;
    }
    if (!install_signals()) return 70;
    const bool stdout_output = arguments.output == "-";
#ifdef _WIN32
    HANDLE handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (!stdout_output) {
        const auto wide = utf8_to_wide_path(arguments.output);
        if (wide.empty()) return 70;
        handle = ::CreateFileW(wide.data(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return 70;
    auto sink = Px4TsWindowsOutput::create(handle, !stdout_output);
    if (!sink) return 70;
    Px4TsWindowsOutput& output = *sink.value();
#else
    const int fd = stdout_output ? STDOUT_FILENO :
        ::open(arguments.output.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        std::fprintf(stderr, "output: %s\n", std::strerror(errno));
        return 70;
    }
    Px4TsFdOutput output(fd, !stdout_output);
    if (!output.ready()) return 70;
#endif
    PosixClock clock;
    PosixSignal signal;
    Px4TsFailureKind failure_kind = Px4TsFailureKind::ipc;
    Px4TsRunDiagnostics diagnostics;
    const auto result = Px4TsRunner::run_wire(arguments, profile, output, clock, signal,
                                              &failure_kind, &diagnostics);
#ifdef _WIN32
    const auto finished = output.finish();
    if (!finished && result) return 70;
#endif
    if (!result) {
        std::fprintf(stderr, "pix-ts: %s\n", error_string(result.error()));
        print_px4_ts_run_diagnostic(stderr, "pix-ts", result.error(), diagnostics);
        std::fprintf(stderr,
            "pix-ts: diagnostics stream-end=%s error=%u packets-written=%llu packets-final=%llu\n",
            diagnostics.stream_end_valid ? "valid" : "missing",
            static_cast<unsigned>(diagnostics.stream_end.error_code),
            static_cast<unsigned long long>(diagnostics.packets_written),
            static_cast<unsigned long long>(diagnostics.stream_end.counters.packets));
        return px4_ts_exit_status(result.error(), failure_kind);
    }
    print_counters("stream", result.value().counters);
    return 0;
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    std::vector<std::string> owned = windows_argv_utf8(argc, argv);
    if (owned.empty()) return 2;
    std::vector<char*> views;
    views.reserve(owned.size());
    for (std::string& value : owned) views.push_back(value.data());
    const int result = run(static_cast<int>(views.size()), views.data());
    px4::userland::px4d::notify_cleanup_complete();
    return result;
#else
    return run(argc, argv);
#endif
}
