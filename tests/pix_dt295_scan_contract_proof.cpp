/* SPDX-License-Identifier: MIT */
/* Offline DT295 independent SI scan contract proof. */
#include "pix-dt295-receiver-adapter.h"

#include <libusb.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

extern "C" {
int dt295_actual_fixture_open(const char *);
void dt295_actual_fixture_close(void);
int dt295_actual_fixture_control(uint8_t, uint8_t, uint16_t, uint16_t, uint8_t *, uint16_t);
int dt295_actual_fixture_bulk(uint8_t, unsigned char *, int, int *);
int dt295_actual_fixture_interrupt(uint8_t, unsigned char *, int, int *);
unsigned dt295_actual_fixture_acquisition_bootstrap(uint8_t packets[4][188], int mode);
void dt295_actual_fixture_encrypt(uint8_t packet[188]);
}

namespace {

using dtv::userland::detail::ReceiverSystem;
using Adapter = pix::capture::dt295::ReceiverAdapter;
using Lease = Adapter::ScanLease;
using SiRead = Adapter::SiReadResult;
using SiEndReason = Adapter::SiEndReason;

std::mutex g_stream_mutex;
std::atomic<int> g_mode{0};
std::atomic<int> g_si_per_read{4};
std::atomic<int> g_read_fail{0};
std::atomic<unsigned> g_stream_starts{0};
bool g_first = true;
unsigned g_cc_si[3] = {0U, 0U, 0U};
unsigned g_cc_video = 0U;
unsigned g_tag = 0U;

struct Clock final : dtv::userland::detail::ReceiverClock {
    std::atomic<std::uint64_t> offset_ms{0U};
    std::uint64_t monotonic_ms() noexcept override
    {
        const auto real = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        return real + offset_ms.load(std::memory_order_acquire);
    }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }
    void advance(std::uint64_t milliseconds) noexcept
    {
        offset_ms.fetch_add(milliseconds, std::memory_order_acq_rel);
    }
};

class Tokens final : public dtv::userland::detail::ReceiverTokenSource {
public:
    dtv::userland::Result<dtv::userland::detail::ReceiverToken> generate() noexcept override
    {
        dtv::userland::detail::ReceiverToken value{};
        value[0] = ++next_;
        return dtv::userland::Result<dtv::userland::detail::ReceiverToken>::success(value);
    }
    std::uint8_t next_ = 0U;
};

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, \
                         #condition);                                             \
            return 1;                                                             \
        }                                                                         \
    } while (0)

#define CHECK_RESULT(result, what)                                             \
    do {                                                                       \
        if (!(result)) {                                                       \
            std::fprintf(stderr, "FAILED %s error=%d\n", what,                 \
                         static_cast<int>((result).error()));                  \
            return 1;                                                          \
        }                                                                      \
    } while (0)

void reset_stream(int mode, int si_per_read)
{
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    g_mode.store(mode, std::memory_order_release);
    g_si_per_read.store(si_per_read, std::memory_order_release);
    g_read_fail.store(0, std::memory_order_release);
    g_first = true;
    g_cc_si[0] = g_cc_si[1] = g_cc_si[2] = 0U;
    g_cc_video = 0U;
    g_tag = 0U;
}

void make_packet(std::uint8_t packet[188], unsigned pid, unsigned cc, unsigned tag)
{
    std::memset(packet, 0x5a, 188U);
    packet[0] = 0x47U;
    packet[1] = static_cast<std::uint8_t>(0x40U | ((pid >> 8) & 0x1fU));
    packet[2] = static_cast<std::uint8_t>(pid & 0xffU);
    packet[3] = static_cast<std::uint8_t>(0x10U | (cc & 15U));
    packet[4] = static_cast<std::uint8_t>(tag);
    dt295_actual_fixture_encrypt(packet);
}

bool read_si(Adapter &adapter, const Lease &lease, std::vector<std::uint8_t> &out,
             SiRead *result, std::chrono::milliseconds timeout)
{
    std::uint8_t buffer[188U * 16U];
    const auto read = adapter.read_scan(lease,
        dtv::userland::MutableByteView{buffer, sizeof(buffer)}, timeout, result);
    if (!read) return false;
    if (result->bytes != 0U)
        out.insert(out.end(), buffer, buffer + result->bytes);
    return true;
}

bool si_pid_only(const std::vector<std::uint8_t> &out)
{
    for (std::size_t offset = 0U; offset + 188U <= out.size(); offset += 188U) {
        const std::uint8_t *packet = out.data() + offset;
        const unsigned pid = (static_cast<unsigned>(packet[1] & 0x1fU) << 8) | packet[2];
        if (pid != 16U && pid != 17U && pid != 18U) return false;
    }
    return true;
}

struct Rig final {
    Rig(const std::vector<std::uint8_t> &assets, std::size_t si_capacity) noexcept
        : adapter(assets.data(), assets.size(), selector, clock, 0U, 2048U, -1,
                  si_capacity) {}
    Clock clock;
    usb_selector selector{};
    Adapter adapter;
};

int case_exclusion_and_core_compensation(const std::vector<std::uint8_t> &assets)
{
    reset_stream(3, 4);
    Rig rig(assets, 8U);
    CHECK(rig.adapter.ready());
    const dtv::userland::detail::ReceiverId receivers[] = {0U};
    Tokens tokens;
    dtv::userland::detail::ReceiverEngine engine(
        rig.adapter, receivers, 1U, tokens, rig.clock, nullptr, nullptr, &rig.adapter);
    CHECK(Adapter::configure(engine, 0U, 7U));
    auto lease = engine.acquire(11U, 0U);
    CHECK(lease);
    dtv::userland::detail::HzTuneRequest tune{};
    tune.lease_id = lease.value().lease_id;
    tune.system = ReceiverSystem::terrestrial;
    tune.requested_hz = 473142857U;
    tune.timeout_ms = 1000U;
    auto admitted = engine.try_admit_tune(11U, tune, rig.clock.monotonic_ms());
    CHECK(admitted);
    CHECK(engine.execute_admitted_tune(admitted.value()));
    const auto status = engine.status();
    CHECK(status);
    const std::uint64_t revision =
        status.value().tune_metadata[0].current_revision;
    Lease scan{};
    CHECK_RESULT(rig.adapter.begin_scan(&scan), "scan begin");
    const unsigned starts_before = g_stream_starts.load();
    CHECK(!rig.adapter.start_capture(0U, ReceiverSystem::terrestrial));
    dtv::userland::detail::Attachment direct{};
    direct.owner_client_id = 11U;
    direct.lease_id = direct.attachment_id = lease.value().lease_id;
    direct.receiver = 0U;
    direct.system = ReceiverSystem::terrestrial;
    CHECK(!rig.adapter.attach(direct));
    const auto started = engine.start_stream_v1(11U, lease.value().lease_id, revision,
                                                 rig.clock.monotonic_ms(), 5000U);
    CHECK(started);
    const auto ticket = engine.reserve_attach_v1(
        11U, lease.value().lease_id, started.value().epoch, started.value().stream_id,
        started.value().token, rig.clock.monotonic_ms());
    CHECK(ticket);
    const auto prepared = engine.prepare_attach_v1(ticket.value());
    CHECK(!prepared && prepared.error() == dtv::userland::Error::BUSY);
    CHECK_RESULT(engine.run_stream_cleanup_v1(ticket.value()), "core compensation cleanup");
    CHECK(g_stream_starts.load() == starts_before);
    std::vector<std::uint8_t> si;
    SiRead read{};
    CHECK(read_si(rig.adapter, scan, si, &read, std::chrono::milliseconds(500)));
    CHECK(read.bytes != 0U && si_pid_only(si));
    CHECK_RESULT(rig.adapter.end_scan(scan), "scan end");
    CHECK_RESULT(rig.adapter.close(0U), "scan core close");
    return 0;
}

int case_stop_terminates_si(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0, 2);
    Rig rig(assets, 8U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "stop scan open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "stop scan tune");
    Lease scan{};
    CHECK_RESULT(rig.adapter.begin_scan(&scan), "stop scan begin");
    std::vector<std::uint8_t> si;
    SiRead read{};
    CHECK(read_si(rig.adapter, scan, si, &read, std::chrono::milliseconds(500)));
    CHECK(read.bytes != 0U);
    CHECK_RESULT(rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial), "stop scan stop");
    CHECK(read_si(rig.adapter, scan, si, &read, std::chrono::milliseconds(100)));
    CHECK(read.terminal && read.reason == SiEndReason::stop);
    CHECK_RESULT(rig.adapter.end_scan(scan), "stop scan end");
    CHECK_RESULT(rig.adapter.close(0U), "stop scan close");
    return 0;
}

int case_semantic_failure_si_continues(const std::vector<std::uint8_t> &assets)
{
    reset_stream(3, 4);
    Rig rig(assets, 8U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "semantic scan open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "semantic scan tune");
    Lease scan{};
    CHECK_RESULT(rig.adapter.begin_scan(&scan), "semantic scan begin");
    for (unsigned n = 0U; n < 3000U; ++n) {
        if (rig.adapter.acquisition().phase == Adapter::AcquisitionPhase::terminal) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto state = rig.adapter.acquisition();
    CHECK(state.phase == Adapter::AcquisitionPhase::terminal);
    CHECK(state.failure == Adapter::AcquisitionFailure::unsupported);
    std::vector<std::uint8_t> si;
    SiRead read{};
    CHECK(read_si(rig.adapter, scan, si, &read, std::chrono::milliseconds(500)));
    CHECK(read.bytes != 0U && si_pid_only(si));
    CHECK(!read.terminal);
    CHECK_RESULT(rig.adapter.end_scan(scan), "semantic scan end");
    CHECK_RESULT(rig.adapter.close(0U), "semantic scan close");
    return 0;
}

int case_deadline_si_continues(const std::vector<std::uint8_t> &assets)
{
    reset_stream(5, 4);
    Rig rig(assets, 8U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "deadline scan open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "deadline scan tune");
    Lease scan{};
    CHECK_RESULT(rig.adapter.begin_scan(&scan), "deadline scan begin");
    std::vector<std::uint8_t> si;
    SiRead read{};
    CHECK(read_si(rig.adapter, scan, si, &read, std::chrono::milliseconds(500)));
    CHECK(read.bytes != 0U);
    rig.clock.advance(30001U);
    for (unsigned n = 0U; n < 3000U; ++n) {
        if (rig.adapter.acquisition().phase == Adapter::AcquisitionPhase::terminal) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(rig.adapter.acquisition().failure == Adapter::AcquisitionFailure::deadline);
    CHECK(read_si(rig.adapter, scan, si, &read, std::chrono::milliseconds(500)));
    CHECK(read.bytes != 0U);
    CHECK_RESULT(rig.adapter.end_scan(scan), "deadline scan end");
    CHECK_RESULT(rig.adapter.close(0U), "deadline scan close");
    return 0;
}

int case_none_and_machine_failure(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0, 0);
    Rig rig(assets, 8U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "none scan open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "none scan tune");
    Lease scan{};
    CHECK_RESULT(rig.adapter.begin_scan(&scan), "none scan begin");
    SiRead read{};
    std::vector<std::uint8_t> si;
    CHECK(read_si(rig.adapter, scan, si, &read, std::chrono::milliseconds(100)));
    CHECK(read.bytes == 0U && read.timed_out && !read.terminal);
    g_read_fail.store(1, std::memory_order_release);
    bool finished = false;
    for (unsigned n = 0U; n < 3000U && !finished; ++n) {
        CHECK(read_si(rig.adapter, scan, si, &read, std::chrono::milliseconds(100)));
        if (read.terminal) finished = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(finished);
    CHECK(read.reason == SiEndReason::usb);
    CHECK_RESULT(rig.adapter.end_scan(scan), "none scan end");
    CHECK_RESULT(rig.adapter.close(0U), "none scan close");
    return 0;
}

int case_tsid_terminates_si(const std::vector<std::uint8_t> &assets)
{
    reset_stream(4, 4);
    Rig rig(assets, 8U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "tsid scan open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::satellite, 1049480000ULL, 1000U),
                 "tsid scan tune");
    CHECK_RESULT(rig.adapter.select_satellite_tsid(0U, 16400U, 1000U),
                 "tsid scan select");
    Lease scan{};
    CHECK_RESULT(rig.adapter.begin_scan(&scan), "tsid scan begin");
    std::vector<std::uint8_t> si;
    SiRead read{};
    bool terminal = false;
    for (unsigned n = 0U; n < 3000U && !terminal; ++n) {
        CHECK(read_si(rig.adapter, scan, si, &read, std::chrono::milliseconds(100)));
        terminal = read.terminal;
    }
    CHECK(terminal);
    CHECK(read.reason == SiEndReason::transport);
    CHECK(rig.adapter.acquisition().failure == Adapter::AcquisitionFailure::satellite_tsid);
    CHECK_RESULT(rig.adapter.end_scan(scan), "tsid scan end");
    CHECK_RESULT(rig.adapter.close(0U), "tsid scan close");
    return 0;
}

int case_overflow_loss_epoch(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0, 8);
    Rig rig(assets, 2U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "overflow scan open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "overflow scan tune");
    Lease scan{};
    CHECK_RESULT(rig.adapter.begin_scan(&scan), "overflow scan begin");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    std::vector<std::uint8_t> si;
    SiRead read{};
    CHECK(read_si(rig.adapter, scan, si, &read, std::chrono::milliseconds(200)));
    CHECK(read.bytes != 0U && read.bytes % 188U == 0U);
    CHECK(read.bytes <= 2U * 188U);
    CHECK(si_pid_only(si));
    CHECK(read.dropped_packets > 0U);
    CHECK(read.continuity_epoch >= 1U);
    CHECK_RESULT(rig.adapter.end_scan(scan), "overflow scan end");
    CHECK_RESULT(rig.adapter.close(0U), "overflow scan close");
    return 0;
}

int case_lease_generation_and_stop(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0, 2);
    Rig rig(assets, 8U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "lease scan open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "lease scan tune");
    CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::terrestrial),
                 "lease normal start");
    Lease rejected{};
    CHECK(!rig.adapter.begin_scan(&rejected));
    CHECK_RESULT(rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial),
                 "lease normal stop");
    Lease first{};
    CHECK_RESULT(rig.adapter.begin_scan(&first), "lease first begin");
    std::vector<std::uint8_t> si;
    SiRead read{};
    CHECK(read_si(rig.adapter, first, si, &read, std::chrono::milliseconds(500)));
    CHECK(read.bytes != 0U);
    CHECK_RESULT(rig.adapter.end_scan(first), "lease first end");
    CHECK(read_si(rig.adapter, first, si, &read, std::chrono::milliseconds(100)));
    CHECK(read.terminal && read.reason == SiEndReason::scan_end);
    Lease second{};
    CHECK_RESULT(rig.adapter.begin_scan(&second), "lease second begin");
    CHECK(second.scan_id != first.scan_id);
    CHECK(read_si(rig.adapter, first, si, &read, std::chrono::milliseconds(100)));
    CHECK(read.terminal && read.stale_generation);
    CHECK(!rig.adapter.end_scan(first));
    CHECK(read_si(rig.adapter, second, si, &read, std::chrono::milliseconds(500)));
    CHECK(read.bytes != 0U);
    CHECK_RESULT(rig.adapter.end_scan(second), "lease second end");
    Lease retuned{};
    CHECK_RESULT(rig.adapter.begin_scan(&retuned), "lease retune begin");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 479142857ULL, 1000U),
                 "lease retune");
    CHECK(read_si(rig.adapter, retuned, si, &read, std::chrono::milliseconds(100)));
    CHECK(read.terminal && read.reason == SiEndReason::retune);
    Lease after{};
    CHECK_RESULT(rig.adapter.begin_scan(&after), "lease after begin");
    CHECK_RESULT(rig.adapter.end_scan(after), "lease after end");
    CHECK_RESULT(rig.adapter.close(0U), "lease close");
    return 0;
}

int case_close_shutdown_terminal(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0, 2);
    {
        Rig rig(assets, 8U);
        CHECK(rig.adapter.ready());
        CHECK_RESULT(rig.adapter.open(0U), "close scan open");
        CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                     "close scan tune");
        Lease scan{};
        CHECK_RESULT(rig.adapter.begin_scan(&scan), "close scan begin");
        CHECK_RESULT(rig.adapter.close(0U), "close scan");
        SiRead read{};
        std::vector<std::uint8_t> si;
        CHECK(read_si(rig.adapter, scan, si, &read, std::chrono::milliseconds(100)));
        CHECK(read.terminal && read.reason == SiEndReason::stop);
    }
    {
        Rig rig(assets, 8U);
        CHECK(rig.adapter.ready());
        CHECK_RESULT(rig.adapter.open(0U), "shutdown scan open");
        CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                     "shutdown scan tune");
        Lease scan{};
        CHECK_RESULT(rig.adapter.begin_scan(&scan), "shutdown scan begin");
        CHECK_RESULT(rig.adapter.shutdown(), "shutdown scan");
        SiRead read{};
        std::vector<std::uint8_t> si;
        CHECK(read_si(rig.adapter, scan, si, &read, std::chrono::milliseconds(100)));
        CHECK(read.terminal && read.reason == SiEndReason::shutdown);
    }
    return 0;
}

}  // namespace

extern "C" int __wrap_usb_open_model(usb_backend **output, const usb_selector *, int, int, int)
{
    if (output == nullptr) return -1;
    *output = reinterpret_cast<usb_backend *>(static_cast<std::uintptr_t>(0x295));
    return 0;
}
extern "C" void __wrap_usb_close(usb_backend *) {}
extern "C" void *__wrap_usb_native_context(usb_backend *)
{
    return reinterpret_cast<void *>(static_cast<std::uintptr_t>(0x29501));
}
extern "C" void *__wrap_usb_native_handle(usb_backend *)
{
    return reinterpret_cast<void *>(static_cast<std::uintptr_t>(0x29502));
}
extern "C" int __wrap_libusb_control_transfer(libusb_device_handle *, std::uint8_t type,
                                               std::uint8_t request, std::uint16_t value,
                                               std::uint16_t index, unsigned char *data,
                                               std::uint16_t length, unsigned)
{
    return dt295_actual_fixture_control(type, request, value, index, data, length);
}
extern "C" int __wrap_libusb_bulk_transfer(libusb_device_handle *, unsigned char endpoint,
                                            unsigned char *data, int length, int *actual,
                                            unsigned)
{
    return dt295_actual_fixture_bulk(endpoint, data, length, actual);
}
extern "C" int __wrap_libusb_interrupt_transfer(libusb_device_handle *, unsigned char endpoint,
                                                 unsigned char *data, int length, int *actual,
                                                 unsigned)
{
    return dt295_actual_fixture_interrupt(endpoint, data, length, actual);
}
extern "C" int __wrap_random_bytes(void *output, std::size_t length)
{
    if (output != nullptr) std::memset(output, 0, length);
    return 0;
}

extern "C" int __wrap_dt295_stream_start(libusb_context *, libusb_device_handle *,
                                          dt295_stream **stream)
{
    if (stream == nullptr) return LIBUSB_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    g_first = true;
    g_cc_si[0] = g_cc_si[1] = g_cc_si[2] = 0U;
    g_cc_video = 0U;
    g_stream_starts.fetch_add(1U, std::memory_order_acq_rel);
    *stream = reinterpret_cast<dt295_stream *>(static_cast<std::uintptr_t>(0x296));
    return 0;
}

extern "C" int __wrap_dt295_stream_read(dt295_stream *, void *buffer, std::size_t capacity,
                                         std::uint64_t *sequence, std::uint64_t *completed_ns,
                                         unsigned)
{
    if (buffer == nullptr || capacity < 188U) return 0;
    if (g_read_fail.load(std::memory_order_acquire) > 0) {
        g_read_fail.fetch_sub(1, std::memory_order_acq_rel);
        return LIBUSB_ERROR_IO;
    }
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    auto *output = static_cast<std::uint8_t *>(buffer);
    std::size_t length = 0U;
    if (g_first) {
        uint8_t packets[4][188];
        const unsigned count = dt295_actual_fixture_acquisition_bootstrap(
            packets, g_mode.load(std::memory_order_acquire));
        for (unsigned i = 0U; i < count && length + 188U <= capacity; ++i) {
            std::memcpy(output + length, packets[i], 188U);
            dt295_actual_fixture_encrypt(output + length);
            length += 188U;
        }
        g_first = false;
    }
    const int si_count = g_si_per_read.load(std::memory_order_acquire);
    if (si_count > 0) {
        for (int i = 0; i < si_count && length + 188U <= capacity; ++i) {
            const unsigned which = static_cast<unsigned>(i) % 3U;
            const unsigned pid = 16U + which;
            std::uint8_t packet[188];
            make_packet(packet, pid, g_cc_si[which]++, g_tag++);
            std::memcpy(output + length, packet, 188U);
            length += 188U;
        }
        std::uint8_t video[188];
        if (length + 188U <= capacity) {
            make_packet(video, 0x101U, g_cc_video++, g_tag++);
            std::memcpy(output + length, video, 188U);
            length += 188U;
        }
    }
    if (length == 0U) {
        if (sequence != nullptr) *sequence = 0U;
        if (completed_ns != nullptr) *completed_ns = 1U;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return 0;
    }
    if (sequence != nullptr) *sequence += length / 188U;
    if (completed_ns != nullptr) *completed_ns = 1U;
    return static_cast<int>(length);
}

extern "C" int __wrap_dt295_stream_stop(dt295_stream *, unsigned) { return 0; }
extern "C" int __wrap_dt295_stream_destroy(dt295_stream *) { return 0; }

int main()
{
    char directory[] = "/tmp/pix-dt295-scan-XXXXXX";
    if (mkdtemp(directory) == nullptr) {
        std::fprintf(stderr, "FAILED mkdtemp\n");
        return 1;
    }
    const std::string assets_path = std::string(directory) + "/dt295.assets";
    if (dt295_actual_fixture_open(assets_path.c_str()) != 0) return 1;
    std::ifstream assets_file(assets_path, std::ios::binary);
    if (!assets_file.good()) return 1;
    const std::vector<std::uint8_t> assets{
        std::istreambuf_iterator<char>(assets_file), std::istreambuf_iterator<char>()};
    assets_file.close();
    if (assets.empty()) return 1;

    CHECK(case_exclusion_and_core_compensation(assets) == 0);
    CHECK(case_stop_terminates_si(assets) == 0);
    CHECK(case_semantic_failure_si_continues(assets) == 0);
    CHECK(case_deadline_si_continues(assets) == 0);
    CHECK(case_none_and_machine_failure(assets) == 0);
    CHECK(case_tsid_terminates_si(assets) == 0);
    CHECK(case_overflow_loss_epoch(assets) == 0);
    CHECK(case_lease_generation_and_stop(assets) == 0);
    CHECK(case_close_shutdown_terminal(assets) == 0);

    dt295_actual_fixture_close();
    (void)unlink(assets_path.c_str());
    (void)rmdir(directory);
    std::fprintf(stderr, "DT295 scan contract: exclusion/si/lifetime OK\n");
    return 0;
}
