// SPDX-License-Identifier: GPL-2.0-only
#include <dtv/host_ts/px4_ts_core.h>
#include <dtv/host_ts/px4_ts_posix.h>
#include <dtv/userland/identity.h>
#include <dtv/userland/tuning.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {

using namespace px4::userland;
using namespace px4::userland::cli;

volatile std::sig_atomic_t stop_flag = 0;

void stop_handler(int) noexcept { stop_flag = 1; }

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
    bool stop_requested() const noexcept override { return stop_flag != 0; }
};

bool install_signals() noexcept
{
    struct sigaction ignore_pipe {};
    ignore_pipe.sa_handler = SIG_IGN;
    if (sigemptyset(&ignore_pipe.sa_mask) != 0 ||
        ::sigaction(SIGPIPE, &ignore_pipe, nullptr) != 0) return false;
    struct sigaction action {};
    action.sa_handler = stop_handler;
    if (sigemptyset(&action.sa_mask) != 0) return false;
    return ::sigaction(SIGINT, &action, nullptr) == 0 &&
           ::sigaction(SIGTERM, &action, nullptr) == 0;
}

void print_counters(const char* label, const ipc::CountersPayload& counters) noexcept
{
    std::fprintf(stderr, "%s packets=%llu bytes=%llu usb-errors=%llu\n", label,
        static_cast<unsigned long long>(counters.packets),
        static_cast<unsigned long long>(counters.bytes),
        static_cast<unsigned long long>(counters.usb_errors));
}

}  // namespace

int main(int argc, char** argv)
{
    static const TsClientProfile profile{
        "pix-ts", "pix-userland", dtv::userland::identity::Product::pix, true, false};
    const Px4TsArguments arguments = parse_px4_ts_product_arguments(
        argc, const_cast<const char* const*>(argv), profile);
    if (!arguments.valid) {
        std::fprintf(stderr, "argument error: %s\n", arguments.error.c_str());
        print_px4_ts_usage(profile, stderr);
        return 2;
    }
    if (arguments.help) { print_px4_ts_usage(profile, stdout); return 0; }
    if (arguments.version) { std::puts("pix-ts 0.0.0-dev"); return 0; }
    if (!install_signals()) return 70;
    const bool stdout_output = arguments.output == "-";
    const int fd = stdout_output ? STDOUT_FILENO :
        ::open(arguments.output.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        std::fprintf(stderr, "output: %s\n", std::strerror(errno));
        return 70;
    }
    Px4TsFdOutput output(fd, !stdout_output);
    if (!output.ready()) return 70;
    PosixClock clock;
    PosixSignal signal;
    Px4TsFailureKind failure_kind = Px4TsFailureKind::ipc;
    Px4TsRunDiagnostics diagnostics;
    const auto result = Px4TsRunner::run_wire(arguments, profile, output, clock, signal,
                                              &failure_kind, &diagnostics);
    if (!result) {
        std::fprintf(stderr, "pix-ts: %s\n", error_string(result.error()));
        return px4_ts_exit_status(result.error(), failure_kind);
    }
    print_counters("stream", result.value().counters);
    return 0;
}
