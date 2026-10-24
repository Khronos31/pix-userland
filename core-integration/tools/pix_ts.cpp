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

bool decimal(std::string_view text, std::uint64_t& value) noexcept
{
    if (text.empty()) return false;
    std::uint64_t parsed = 0U;
    for (char ch : text) {
        if (ch < '0' || ch > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (parsed > (UINT64_MAX - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
    }
    value = parsed;
    return true;
}

void usage(FILE* file) noexcept
{
    std::fprintf(file,
        "usage: pix-ts (--device serial:SELECTOR|usb:SELECTOR | --instance TOKEN) "
        "(-f HZ | -c CHANNEL) [--time SECONDS|-] [-o PATH|-] [--runtime-dir PATH]\n");
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
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::puts("pix-ts 0.0.0-dev");
        return 0;
    }
    if (argc == 2 && (std::strcmp(argv[1], "-h") == 0 ||
                      std::strcmp(argv[1], "--help") == 0)) {
        usage(stdout);
        return 0;
    }

    const char* normalized[64]{};
    int normalized_count = 0;
    normalized[normalized_count++] = argv[0];
    normalized[normalized_count++] = "--receiver";
    normalized[normalized_count++] = "0";
    bool have_channel = false;
    bool have_frequency = false;
    bool have_device = false;
    bool have_instance = false;
    std::uint64_t frequency_hz = 0U;
    const char* channel = nullptr;
    std::string resolved_instance;
    for (int index = 1; index < argc; ++index) {
        if (normalized_count >= 58) {
            usage(stderr);
            return 2;
        }
        const std::string_view option(argv[index]);
        if (option == "-h" || option == "--help") {
            usage(stderr);
            return 2;
        }
        if (option == "-f" || option == "--freq") {
            if (have_frequency || have_channel || index + 1 >= argc ||
                !decimal(argv[index + 1], frequency_hz) || frequency_hz == 0U ||
                frequency_hz > UINT32_MAX) return 2;
            have_frequency = true;
            normalized[normalized_count++] = "--channel";
            normalized[normalized_count++] = "T13";
            ++index;
        } else if (option == "-c" || option == "--channel") {
            if (have_channel || have_frequency || index + 1 >= argc) return 2;
            have_channel = true;
            channel = argv[index + 1];
            normalized[normalized_count++] = "--channel";
            normalized[normalized_count++] = argv[++index];
        } else if (option == "-o" || option == "--output") {
            if (index + 1 >= argc) return 2;
            normalized[normalized_count++] = "--output";
            normalized[normalized_count++] = argv[++index];
        } else if (option == "--time") {
            if (index + 1 >= argc) return 2;
            const char* seconds = argv[++index];
            normalized[normalized_count++] = "--time";
            normalized[normalized_count++] = seconds;
        } else if (option == "--device") {
            if (have_device || have_instance || index + 1 >= argc) return 2;
            have_device = true;
            const auto selector = dtv::userland::identity::parse_device_selector(argv[++index]);
            if (!selector) return 2;
            if (selector.value().kind == dtv::userland::identity::SelectorKind::serial)
                return 3;
            const auto identity = dtv::userland::identity::resolve_selector(
                dtv::userland::identity::Product::pix, selector.value());
            if (!identity) return 3;
            const auto automatic = dtv::userland::identity::auto_instance(identity.value());
            if (!automatic) return 3;
            resolved_instance.assign(automatic.value().bytes.data(), automatic.value().length);
            normalized[normalized_count++] = "--instance";
            normalized[normalized_count++] = resolved_instance.c_str();
        } else if (option == "--instance") {
            if (have_device || have_instance || index + 1 >= argc) return 2;
            have_instance = true;
            normalized[normalized_count++] = argv[index];
            normalized[normalized_count++] = argv[++index];
        } else if (option == "--runtime-dir") {
            if (index + 1 >= argc) return 2;
            normalized[normalized_count++] = argv[index];
            normalized[normalized_count++] = argv[++index];
        } else {
            return 2;
        }
    }
    if (!have_channel && !have_frequency) return 2;
    const TsClientProfile profile{"pix-ts", "pix-userland",
        dtv::userland::identity::Product::pix, false, false, nullptr, true};
    const Px4TsArguments arguments = parse_px4_ts_product_arguments(
        normalized_count, normalized, profile);
    if (!arguments.valid) {
        std::fprintf(stderr, "argument error: %s\n", arguments.error.c_str());
        usage(stderr);
        return 2;
    }
    if (arguments.help) return 0;
    Px4TsArguments exact = arguments;
    dtv::tuning::Request tune_request{};
    if (have_frequency) {
        tune_request.system = dtv::userland::wire::System::isdb_t;
        tune_request.frequency_hz = frequency_hz;
    } else {
        tune_request.channel = std::string_view(channel);
    }
    const auto resolved = dtv::tuning::resolve(tune_request);
    if (!resolved) {
        std::fprintf(stderr, "frequency/channel rejected: %s\n",
                     error_string(resolved.error()));
        return 2;
    }
    exact.system = resolved.value().system == dtv::userland::wire::System::isdb_t
        ? ipc::System::ISDB_T : ipc::System::ISDB_S;
    exact.frequency_khz = 0U;
    exact.frequency_hz = resolved.value().frequency_hz;
    if (!install_signals()) return 70;
    const bool stdout_output = exact.output == "-";
    const int fd = stdout_output ? STDOUT_FILENO :
        ::open(exact.output.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
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
    const auto result = Px4TsRunner::run_wire(exact, profile, output, clock, signal,
                                              &failure_kind, &diagnostics);
    if (!result) {
        std::fprintf(stderr, "pix-ts: %s\n", error_string(result.error()));
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
