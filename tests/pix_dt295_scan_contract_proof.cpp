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
#include <pthread.h>
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
std::atomic<bool> g_suppress_input{false};
std::atomic<unsigned> g_stream_starts{0};
std::atomic<unsigned> g_stream_stops{0};
std::atomic<unsigned> g_join_calls{0};
std::atomic<unsigned> g_creates{0};
std::atomic<unsigned> g_device_stops{0};
std::atomic<unsigned> g_stream_destroys{0U};
std::atomic<unsigned> g_usb_closes{0U};
std::atomic<unsigned> g_device_freed{0U};
std::atomic<unsigned> g_routes_freed{0U};
std::atomic<unsigned> g_assets_freed{0U};
std::atomic<bool> g_fail_device_stop{false};
std::atomic<bool> g_persistent_device_stop{false};
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
    g_suppress_input.store(false, std::memory_order_release);
    g_fail_device_stop.store(false, std::memory_order_release);
    g_persistent_device_stop.store(false, std::memory_order_release);
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

int case_stale_end_normal(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0, 4);
    Rig rig(assets, 8U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "stale open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "stale tune");
    Lease lease{};
    CHECK_RESULT(rig.adapter.begin_scan(&lease), "stale begin");
    CHECK_RESULT(rig.adapter.end_scan(lease), "stale end");
    reset_stream(0, 4);
    CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::terrestrial), "stale normal start");
    dtv::userland::detail::Attachment attachment{};
    attachment.owner_client_id = 11U;
    attachment.lease_id = attachment.attachment_id = 101U;
    attachment.receiver = 0U;
    attachment.system = ReceiverSystem::terrestrial;
    CHECK_RESULT(rig.adapter.attach(attachment), "stale normal attach");
    for (unsigned n = 0U; n < 3000U; ++n) {
        if (rig.adapter.acquisition().phase == Adapter::AcquisitionPhase::ready) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(rig.adapter.acquisition().phase == Adapter::AcquisitionPhase::ready);
    const unsigned stops = g_stream_stops.load();
    const unsigned joins = g_join_calls.load();
    CHECK_RESULT(rig.adapter.end_scan(lease), "stale old end");
    std::uint8_t buffer[188U * 16U];
    const auto read = rig.adapter.read(attachment,
        dtv::userland::MutableByteView{buffer, sizeof(buffer)},
        std::chrono::milliseconds(100));
    CHECK(read && !read.value().eof);
    CHECK(g_stream_stops.load() == stops);
    CHECK(g_join_calls.load() == joins);
    CHECK(rig.adapter.acquisition().phase == Adapter::AcquisitionPhase::ready);
    CHECK_RESULT(rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial), "stale normal stop");
    CHECK_RESULT(rig.adapter.detach(attachment), "stale detach");
    CHECK_RESULT(rig.adapter.close(0U), "stale close");
    return 0;
}

int case_late_core_cleanup(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0, 4);
    Rig rig(assets, 8U);
    const dtv::userland::detail::ReceiverId receivers[] = {0U};
    Tokens tokens;
    dtv::userland::detail::ReceiverEngine engine(
        rig.adapter, receivers, 1U, tokens, rig.clock, nullptr, nullptr, &rig.adapter);
    CHECK(Adapter::configure(engine, 0U, 7U));
    auto owner = engine.acquire(12U, 0U);
    CHECK(owner);
    dtv::userland::detail::HzTuneRequest tune{};
    tune.lease_id = owner.value().lease_id;
    tune.system = ReceiverSystem::terrestrial;
    tune.requested_hz = 473142857U;
    tune.timeout_ms = 1000U;
    auto admitted = engine.try_admit_tune(12U, tune, rig.clock.monotonic_ms());
    CHECK(admitted);
    CHECK(engine.execute_admitted_tune(admitted.value()));
    const auto status = engine.status();
    CHECK(status);
    Lease first{}, second{};
    CHECK_RESULT(rig.adapter.begin_scan(&first), "late core first");
    const auto started = engine.start_stream_v1(12U, owner.value().lease_id,
        status.value().tune_metadata[0].current_revision, rig.clock.monotonic_ms(), 5000U);
    CHECK(started);
    const auto ticket = engine.reserve_attach_v1(12U, owner.value().lease_id,
        started.value().epoch, started.value().stream_id, started.value().token,
        rig.clock.monotonic_ms());
    CHECK(ticket);
    const auto prepared = engine.prepare_attach_v1(ticket.value());
    CHECK(!prepared && prepared.error() == dtv::userland::Error::BUSY);
    CHECK_RESULT(rig.adapter.end_scan(first), "late core end first");
    CHECK_RESULT(rig.adapter.begin_scan(&second), "late core second");
    const unsigned stops = g_stream_stops.load();
    CHECK_RESULT(engine.run_stream_cleanup_v1(ticket.value()), "late core cleanup");
    std::vector<std::uint8_t> si;
    SiRead read{};
    CHECK(read_si(rig.adapter, second, si, &read, std::chrono::milliseconds(500)));
    CHECK(!read.terminal);
    CHECK(g_stream_stops.load() == stops);
    CHECK_RESULT(rig.adapter.end_scan(second), "late core end second");
    CHECK_RESULT(rig.adapter.close(0U), "late core close");
    return 0;
}

int case_terminal_handoff(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0, 0);
    Rig rig(assets, 8U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "handoff open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "handoff tune");
    Lease lease{};
    CHECK_RESULT(rig.adapter.begin_scan(&lease), "handoff begin");
    g_read_fail.store(1, std::memory_order_release);
    SiRead si_read{};
    std::vector<std::uint8_t> si;
    for (unsigned n = 0U; n < 3000U && !si_read.terminal; ++n)
        CHECK(read_si(rig.adapter, lease, si, &si_read, std::chrono::milliseconds(10)));
    CHECK(si_read.terminal && si_read.reason == SiEndReason::usb);
    const unsigned joins = g_join_calls.load();
    CHECK_RESULT(rig.adapter.end_scan(lease), "handoff end");
    CHECK(g_join_calls.load() == joins + 1U);
    reset_stream(0, 4);
    CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::terrestrial), "handoff normal start");
    dtv::userland::detail::Attachment attachment{};
    attachment.owner_client_id = 11U;
    attachment.lease_id = attachment.attachment_id = 102U;
    attachment.receiver = 0U;
    attachment.system = ReceiverSystem::terrestrial;
    CHECK_RESULT(rig.adapter.attach(attachment), "handoff normal attach");
    for (unsigned n = 0U; n < 3000U; ++n) {
        if (rig.adapter.acquisition().phase == Adapter::AcquisitionPhase::ready) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(rig.adapter.acquisition().phase == Adapter::AcquisitionPhase::ready);
    std::uint8_t buffer[188U * 16U];
    const auto read = rig.adapter.read(attachment,
        dtv::userland::MutableByteView{buffer, sizeof(buffer)},
        std::chrono::milliseconds(100));
    CHECK(read && read.value().bytes > 0U);
    CHECK_RESULT(rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial), "handoff stop");
    CHECK_RESULT(rig.adapter.detach(attachment), "handoff detach");
    CHECK_RESULT(rig.adapter.close(0U), "handoff close");
    return 0;
}

int case_late_tsid(const std::vector<std::uint8_t> &assets)
{
    reset_stream(4, 4);
    g_suppress_input.store(true, std::memory_order_release);
    Rig rig(assets, 8U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "late tsid open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::satellite, 1049480000ULL, 1000U),
                 "late tsid tune");
    CHECK_RESULT(rig.adapter.select_satellite_tsid(0U, 16400U, 1000U), "late tsid select");
    Lease lease{};
    CHECK_RESULT(rig.adapter.begin_scan(&lease), "late tsid begin");
    rig.clock.advance(30001U);
    for (unsigned n = 0U; n < 3000U; ++n) {
        if (rig.adapter.acquisition().phase == Adapter::AcquisitionPhase::terminal) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(rig.adapter.acquisition().failure == Adapter::AcquisitionFailure::deadline);
    g_suppress_input.store(false, std::memory_order_release);
    SiRead read{};
    std::vector<std::uint8_t> si;
    bool rejected = false;
    for (unsigned n = 0U; n < 3000U && !rejected; ++n) {
        CHECK(read_si(rig.adapter, lease, si, &read, std::chrono::milliseconds(10)));
        rejected = read.terminal && read.reason == SiEndReason::transport;
    }
    CHECK(rejected);
    CHECK(rig.adapter.acquisition().phase == Adapter::AcquisitionPhase::terminal);
    CHECK_RESULT(rig.adapter.end_scan(lease), "late tsid end");
    CHECK_RESULT(rig.adapter.close(0U), "late tsid close");
    return 0;
}

int case_waiting_readers(const std::vector<std::uint8_t> &assets)
{
    for (unsigned action = 0U; action < 6U; ++action) {
        reset_stream(0, 0);
        Rig rig(assets, 8U);
        CHECK(rig.adapter.ready());
        CHECK_RESULT(rig.adapter.open(0U), "waiter open");
        CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial,
                                           473142857ULL, 1000U),
                     "waiter tune");
        Lease lease{};
        CHECK_RESULT(rig.adapter.begin_scan(&lease), "waiter begin");
        std::atomic<bool> entered{false};
        SiRead result{};
        bool read_ok = false;
        std::thread reader([&] {
            std::vector<std::uint8_t> si;
            entered.store(true, std::memory_order_release);
            read_ok = read_si(rig.adapter, lease, si, &result,
                              std::chrono::milliseconds(1500));
        });
        while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto started = std::chrono::steady_clock::now();
        bool ok = true;
        switch (action) {
        case 0U: ok = static_cast<bool>(rig.adapter.end_scan(lease)); break;
        case 1U: ok = static_cast<bool>(
            rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial)); break;
        case 2U: ok = static_cast<bool>(rig.adapter.close(0U)); break;
        case 3U: ok = static_cast<bool>(rig.adapter.shutdown()); break;
        case 4U: rig.adapter.mark_disconnected(0U); break;
        case 5U: ok = static_cast<bool>(rig.adapter.program_hz(
            0U, ReceiverSystem::terrestrial, 479142857ULL, 1000U)); break;
        default: break;
        }
        reader.join();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        std::fprintf(stderr,
                     "waiter action=%u terminal=%d timeout=%d bytes=%zu reason=%u elapsed_ms=%lld\n",
                     action, static_cast<int>(result.terminal),
                     static_cast<int>(result.timed_out), result.bytes,
                     static_cast<unsigned>(result.reason),
                     static_cast<long long>(elapsed));
        CHECK(ok && read_ok && result.terminal && !result.timed_out &&
              result.bytes == 0U && elapsed < 1000);
    }
    return 0;
}

int case_terminal_attach(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0, 0);
    Rig rig(assets, 8U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "terminal attach open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "terminal attach tune");
    Lease scan{}, other{};
    CHECK_RESULT(rig.adapter.begin_scan(&scan), "terminal attach begin");
    g_read_fail.store(1, std::memory_order_release);
    SiRead result{};
    std::vector<std::uint8_t> si;
    for (unsigned n = 0U; n < 3000U && !result.terminal; ++n)
        CHECK(read_si(rig.adapter, scan, si, &result, std::chrono::milliseconds(10)));
    CHECK(result.terminal && result.reason == SiEndReason::usb);
    const auto started = rig.adapter.start_capture(0U, ReceiverSystem::terrestrial);
    const auto begun = rig.adapter.begin_scan(&other);
    const unsigned creates = g_creates.load();
    const unsigned joins = g_join_calls.load();
    dtv::userland::detail::Attachment attachment{};
    attachment.owner_client_id = 11U;
    attachment.lease_id = attachment.attachment_id = 103U;
    attachment.receiver = 0U;
    attachment.system = ReceiverSystem::terrestrial;
    const auto attached = rig.adapter.attach(attachment);
    std::fprintf(stderr,
                 "u1-terminal-attach: start_ok=%d begin_ok=%d attach_ok=%d create_delta=%u join_delta=%u\n",
                 static_cast<int>(static_cast<bool>(started)),
                 static_cast<int>(static_cast<bool>(begun)),
                 static_cast<int>(static_cast<bool>(attached)),
                 g_creates.load() - creates, g_join_calls.load() - joins);
    CHECK(!started && !begun && !attached);
    CHECK(attached.error() == dtv::userland::Error::BUSY);
    CHECK(g_creates.load() == creates);
    CHECK(g_join_calls.load() == joins);
    CHECK_RESULT(rig.adapter.end_scan(scan), "terminal attach end");
    CHECK_RESULT(rig.adapter.close(0U), "terminal attach close");
    return 0;
}

int case_physical_stop_retry(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0, 0);
    Rig rig(assets, 8U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "physical stop open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "physical stop tune");
    Lease scan{}, other{};
    CHECK_RESULT(rig.adapter.begin_scan(&scan), "physical stop begin");
    g_fail_device_stop.store(true, std::memory_order_release);
    const unsigned before = g_device_stops.load();
    const auto first = rig.adapter.end_scan(scan);
    const auto second = rig.adapter.end_scan(scan);
    const unsigned retries = g_device_stops.load() - before;
    const auto next = rig.adapter.begin_scan(&other);
    std::fprintf(stderr,
                 "u1-physical-stop: first_ok=%d first_error=%u second_ok=%d stop_calls=%u next_ok=%d\n",
                 static_cast<int>(static_cast<bool>(first)),
                 static_cast<unsigned>(first ? 0U : static_cast<unsigned>(first.error())),
                 static_cast<int>(static_cast<bool>(second)), retries,
                 static_cast<int>(static_cast<bool>(next)));
    CHECK(!first && first.error() == dtv::userland::Error::USB_IO);
    CHECK(second);
    CHECK(retries == 2U);
    CHECK(next);
    CHECK_RESULT(rig.adapter.end_scan(other), "physical stop next end");
    CHECK_RESULT(rig.adapter.close(0U), "physical stop close");
    return 0;
}

int case_refusal_count(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0, 4);
    Rig rig(assets, 8U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "count open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "count tune");
    Lease first{}, second{}, third{};
    CHECK_RESULT(rig.adapter.begin_scan(&first), "count first begin");
    CHECK(!rig.adapter.start_capture(0U, ReceiverSystem::terrestrial));
    CHECK(!rig.adapter.start_capture(0U, ReceiverSystem::terrestrial));
    CHECK_RESULT(rig.adapter.end_scan(first), "count first end");
    CHECK_RESULT(rig.adapter.begin_scan(&second), "count second begin");
    std::vector<std::uint8_t> si;
    SiRead result{};
    for (unsigned n = 0U; n < 2U; ++n) {
        CHECK_RESULT(rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial),
                     "count compensation");
        CHECK(read_si(rig.adapter, second, si, &result, std::chrono::milliseconds(100)));
        CHECK(!result.terminal);
    }
    CHECK_RESULT(rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial), "count direct stop");
    CHECK(read_si(rig.adapter, second, si, &result, std::chrono::milliseconds(100)));
    CHECK(result.terminal && result.reason == SiEndReason::stop);
    CHECK_RESULT(rig.adapter.end_scan(second), "count second end");
    CHECK_RESULT(rig.adapter.begin_scan(&third), "count third begin");
    CHECK(!rig.adapter.start_capture(0U, ReceiverSystem::terrestrial));
    CHECK_RESULT(rig.adapter.end_scan(third), "count third end");
    CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::terrestrial),
                 "count normal start");
    const unsigned before = g_device_stops.load();
    CHECK_RESULT(rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial),
                 "count normal stop");
    CHECK(g_device_stops.load() == before + 1U);
    CHECK_RESULT(rig.adapter.close(0U), "count close");
    return 0;
}

int case_close_retry(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0, 0);
    Rig rig(assets, 8U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "close retry open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "close retry tune");
    Lease scan{};
    CHECK_RESULT(rig.adapter.begin_scan(&scan), "close retry begin");
    g_fail_device_stop.store(true, std::memory_order_release);
    CHECK(!rig.adapter.close(0U));
    CHECK_RESULT(rig.adapter.close(0U), "close retry second");
    return 0;
}

int case_persistent_close(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0, 0);
    Rig rig(assets, 8U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "persistent close open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "persistent close tune");
    Lease scan{};
    CHECK_RESULT(rig.adapter.begin_scan(&scan), "persistent close begin");
    g_persistent_device_stop.store(true, std::memory_order_release);
    const unsigned destroys = g_stream_destroys.load();
    const unsigned joins = g_join_calls.load();
    const unsigned closes = g_usb_closes.load();
    const unsigned device_free = g_device_freed.load();
    const unsigned routes_free = g_routes_freed.load();
    const unsigned assets_free = g_assets_freed.load();
    const auto first = rig.adapter.close(0U);
    const auto second = rig.adapter.close(0U);
    std::fprintf(stderr,
                 "u1-persistent-close: first_ok=%d second_ok=%d destroys=%u joins=%u\n",
                 static_cast<int>(static_cast<bool>(first)),
                 static_cast<int>(static_cast<bool>(second)),
                 g_stream_destroys.load() - destroys, g_join_calls.load() - joins);
    CHECK(!first && first.error() == dtv::userland::Error::USB_IO);
    CHECK(second);
    CHECK(g_stream_destroys.load() == destroys + 1U);
    CHECK(g_join_calls.load() == joins + 1U);
    const auto shutdown = rig.adapter.shutdown();
    std::fprintf(stderr,
                 "u1-shutdown-persistent: shutdown_ok=%d usb=%u device=%u routes=%u assets=%u destroys=%u joins=%u\n",
                 static_cast<int>(static_cast<bool>(shutdown)),
                 g_usb_closes.load() - closes, g_device_freed.load() - device_free,
                 g_routes_freed.load() - routes_free, g_assets_freed.load() - assets_free,
                 g_stream_destroys.load() - destroys, g_join_calls.load() - joins);
    CHECK(!shutdown);
    CHECK(g_usb_closes.load() == closes + 1U);
    CHECK(g_device_freed.load() == device_free + 1U);
    CHECK(g_routes_freed.load() == routes_free + 1U);
    CHECK(g_assets_freed.load() == assets_free + 1U);
    CHECK(g_stream_destroys.load() == destroys + 1U);
    CHECK(g_join_calls.load() == joins + 1U);
    g_persistent_device_stop.store(false, std::memory_order_release);
    return 0;
}

int case_shutdown_persistent(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0, 0);
    Rig rig(assets, 8U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "persistent shutdown open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "persistent shutdown tune");
    Lease scan{};
    CHECK_RESULT(rig.adapter.begin_scan(&scan), "persistent shutdown begin");
    g_persistent_device_stop.store(true, std::memory_order_release);
    const unsigned destroys = g_stream_destroys.load();
    const unsigned joins = g_join_calls.load();
    const unsigned closes = g_usb_closes.load();
    const unsigned device_free = g_device_freed.load();
    const unsigned routes_free = g_routes_freed.load();
    const unsigned assets_free = g_assets_freed.load();
    const auto shutdown = rig.adapter.shutdown();
    std::fprintf(stderr,
                 "u1-shutdown-persistent: shutdown_ok=%d usb=%u device=%u routes=%u assets=%u destroys=%u joins=%u\n",
                 static_cast<int>(static_cast<bool>(shutdown)),
                 g_usb_closes.load() - closes, g_device_freed.load() - device_free,
                 g_routes_freed.load() - routes_free, g_assets_freed.load() - assets_free,
                 g_stream_destroys.load() - destroys, g_join_calls.load() - joins);
    CHECK(!shutdown);
    CHECK(g_usb_closes.load() == closes + 1U);
    CHECK(g_device_freed.load() == device_free + 1U);
    CHECK(g_routes_freed.load() == routes_free + 1U);
    CHECK(g_assets_freed.load() == assets_free + 1U);
    CHECK(g_stream_destroys.load() == destroys + 1U);
    CHECK(g_join_calls.load() == joins + 1U);
    g_persistent_device_stop.store(false, std::memory_order_release);
    return 0;
}

int case_retry_stop(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0, 0);
    Rig rig(assets, 8U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "retry stop open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "retry stop tune");
    Lease scan{}, next{};
    CHECK_RESULT(rig.adapter.begin_scan(&scan), "retry stop begin");
    const unsigned stops = g_device_stops.load();
    const unsigned destroys = g_stream_destroys.load();
    const unsigned joins = g_join_calls.load();
    g_fail_device_stop.store(true, std::memory_order_release);
    const auto first = rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial);
    const auto rejected = rig.adapter.begin_scan(&next);
    const auto second = rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial);
    const auto begun = rig.adapter.begin_scan(&next);
    std::fprintf(stderr,
                 "u1-retry-stop: first_ok=%d rejected_busy=%d second_ok=%d stops=%u destroys=%u joins=%u next_ok=%d\n",
                 static_cast<int>(static_cast<bool>(first)),
                 static_cast<int>(!rejected),
                 static_cast<int>(static_cast<bool>(second)),
                 g_device_stops.load() - stops, g_stream_destroys.load() - destroys,
                 g_join_calls.load() - joins, static_cast<int>(static_cast<bool>(begun)));
    CHECK(!first && first.error() == dtv::userland::Error::USB_IO);
    CHECK(!rejected && rejected.error() == dtv::userland::Error::BUSY);
    CHECK(second);
    CHECK(g_device_stops.load() == stops + 2U);
    CHECK(g_stream_destroys.load() == destroys + 1U);
    CHECK(g_join_calls.load() == joins + 1U);
    CHECK(begun);
    CHECK_RESULT(rig.adapter.end_scan(next), "retry stop end");
    CHECK_RESULT(rig.adapter.close(0U), "retry stop close");
    return 0;
}

int case_retry_retune(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0, 0);
    Rig rig(assets, 8U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "retry retune open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "retry retune tune");
    Lease scan{}, next{};
    CHECK_RESULT(rig.adapter.begin_scan(&scan), "retry retune begin");
    const unsigned stops = g_device_stops.load();
    const unsigned destroys = g_stream_destroys.load();
    const unsigned joins = g_join_calls.load();
    g_fail_device_stop.store(true, std::memory_order_release);
    const auto first = rig.adapter.program_hz(0U, ReceiverSystem::terrestrial,
                                              479142857ULL, 1000U);
    const auto rejected = rig.adapter.begin_scan(&next);
    const auto second = rig.adapter.program_hz(0U, ReceiverSystem::terrestrial,
                                               479142857ULL, 1000U);
    const auto begun = rig.adapter.begin_scan(&next);
    std::fprintf(stderr,
                 "u1-retry-retune: first_ok=%d rejected_busy=%d second_ok=%d stops=%u destroys=%u joins=%u next_ok=%d\n",
                 static_cast<int>(static_cast<bool>(first)),
                 static_cast<int>(!rejected),
                 static_cast<int>(static_cast<bool>(second)),
                 g_device_stops.load() - stops, g_stream_destroys.load() - destroys,
                 g_join_calls.load() - joins, static_cast<int>(static_cast<bool>(begun)));
    CHECK(!first && first.error() == dtv::userland::Error::USB_IO);
    CHECK(!rejected && rejected.error() == dtv::userland::Error::BUSY);
    CHECK(second);
    CHECK(g_device_stops.load() == stops + 2U);
    CHECK(g_stream_destroys.load() == destroys + 1U);
    CHECK(g_join_calls.load() == joins + 1U);
    CHECK(begun);
    CHECK_RESULT(rig.adapter.end_scan(next), "retry retune end");
    CHECK_RESULT(rig.adapter.close(0U), "retry retune close");
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
extern "C" void __wrap_usb_close(usb_backend *)
{
    g_usb_closes.fetch_add(1U, std::memory_order_acq_rel);
}
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
    if (g_suppress_input.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return 0;
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

extern "C" int __wrap_dt295_stream_stop(dt295_stream *, unsigned)
{
    g_stream_stops.fetch_add(1U, std::memory_order_acq_rel);
    return 0;
}
extern "C" int __real_dt295_stop(dt295 *, size_t *, char *, size_t);
extern "C" int __wrap_dt295_stop(dt295 *device, size_t *drained, char *error,
                                  size_t error_size)
{
    g_device_stops.fetch_add(1U, std::memory_order_acq_rel);
    if (g_persistent_device_stop.load(std::memory_order_acquire)) return LIBUSB_ERROR_IO;
    if (g_fail_device_stop.exchange(false, std::memory_order_acq_rel))
        return LIBUSB_ERROR_IO;
    return __real_dt295_stop(device, drained, error, error_size);
}
extern "C" int __real_pthread_create(pthread_t *, const pthread_attr_t *,
                                     void *(*)(void *), void *);
extern "C" int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                                     void *(*entry)(void *), void *argument)
{
    g_creates.fetch_add(1U, std::memory_order_acq_rel);
    return __real_pthread_create(thread, attributes, entry, argument);
}
extern "C" int __wrap_dt295_stream_destroy(dt295_stream *)
{
    g_stream_destroys.fetch_add(1U, std::memory_order_acq_rel);
    return 0;
}
extern "C" void __real_dt295_destroy(dt295 *);
extern "C" void __wrap_dt295_destroy(dt295 *device)
{
    g_device_freed.fetch_add(1U, std::memory_order_acq_rel);
    __real_dt295_destroy(device);
}
extern "C" void __real_dt295_routes_destroy(dt295_routes *);
extern "C" void __wrap_dt295_routes_destroy(dt295_routes *routes)
{
    g_routes_freed.fetch_add(1U, std::memory_order_acq_rel);
    __real_dt295_routes_destroy(routes);
}
extern "C" void __real_dt295_assets_free(dt295_assets *);
extern "C" void __wrap_dt295_assets_free(dt295_assets *assets)
{
    g_assets_freed.fetch_add(1U, std::memory_order_acq_rel);
    __real_dt295_assets_free(assets);
}
extern "C" int __real_pthread_join(pthread_t, void **);
extern "C" int __wrap_pthread_join(pthread_t thread, void **value)
{
    g_join_calls.fetch_add(1U, std::memory_order_acq_rel);
    return __real_pthread_join(thread, value);
}

int main(int argc, char **argv)
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

    const std::string only = argc > 1 ? argv[1] : std::string();
    const auto run = [&](const char *name, const auto &body) {
        if (!only.empty() && only != name) return true;
        if (body() != 0) {
            std::fprintf(stderr, "CASE failed: %s\n", name);
            return false;
        }
        return true;
    };
    CHECK(run("exclusion-core", [&] { return case_exclusion_and_core_compensation(assets); }));
    CHECK(run("stop-terminates", [&] { return case_stop_terminates_si(assets); }));
    CHECK(run("semantic-failure", [&] { return case_semantic_failure_si_continues(assets); }));
    CHECK(run("deadline", [&] { return case_deadline_si_continues(assets); }));
    CHECK(run("none-machine", [&] { return case_none_and_machine_failure(assets); }));
    CHECK(run("tsid", [&] { return case_tsid_terminates_si(assets); }));
    CHECK(run("overflow", [&] { return case_overflow_loss_epoch(assets); }));
    CHECK(run("lease-generation", [&] { return case_lease_generation_and_stop(assets); }));
    CHECK(run("stale-end-normal", [&] { return case_stale_end_normal(assets); }));
    CHECK(run("late-core-cleanup", [&] { return case_late_core_cleanup(assets); }));
    CHECK(run("terminal-handoff", [&] { return case_terminal_handoff(assets); }));
    CHECK(run("late-tsid", [&] { return case_late_tsid(assets); }));
    CHECK(run("terminal-attach", [&] { return case_terminal_attach(assets); }));
    CHECK(run("physical-stop", [&] { return case_physical_stop_retry(assets); }));
    CHECK(run("refusal-count", [&] { return case_refusal_count(assets); }));
    CHECK(run("close-retry", [&] { return case_close_retry(assets); }));
    if (!only.empty()) {
        CHECK(run("persistent-close", [&] { return case_persistent_close(assets); }));
        CHECK(run("shutdown-persistent", [&] { return case_shutdown_persistent(assets); }));
    }
    CHECK(run("retry-stop", [&] { return case_retry_stop(assets); }));
    CHECK(run("retry-retune", [&] { return case_retry_retune(assets); }));
    CHECK(run("waiting-readers", [&] { return case_waiting_readers(assets); }));
    CHECK(run("close-shutdown", [&] { return case_close_shutdown_terminal(assets); }));

    dt295_actual_fixture_close();
    (void)unlink(assets_path.c_str());
    (void)rmdir(directory);
    std::fprintf(stderr, "DT295 scan contract: exclusion/si/lifetime OK\n");
    return 0;
}
