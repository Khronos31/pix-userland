/* SPDX-License-Identifier: MIT */
/* Offline DT295 normal acquisition gate proof. */
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
void dt295_actual_fixture_recording_packet(uint8_t packet[188], unsigned continuity);
void dt295_actual_fixture_encrypt(uint8_t packet[188]);
int __real_dt295_reprepare_plan(dt295 *, const dt295_route_plan *, char *, std::size_t);
int __real_dt295_routes_feed(dt295_routes *, const uint8_t packet[188]);
}

namespace {

using dtv::userland::detail::ReceiverSystem;
using Attachment = dtv::userland::detail::Attachment;
using Phase = pix::capture::dt295::ReceiverAdapter::AcquisitionPhase;
using Failure = pix::capture::dt295::ReceiverAdapter::AcquisitionFailure;

std::mutex g_stream_mutex;
std::atomic<unsigned> g_generation{0U};
bool g_bootstrap_sent = false;
unsigned g_continuity = 0U;
int g_split_phase = 0;
std::atomic<int> g_mode{0};
std::atomic<bool> g_mode_pending{false};
std::atomic<bool> g_use_mode{false};
std::atomic<bool> g_release_split{false};
std::atomic<bool> g_split_pat_sent{false};
std::atomic<int> g_read_fail{0};
std::atomic<int> g_start_fail{0};
std::atomic<int> g_reprepare_fail{0};
std::atomic<bool> g_arm_restart_block{false};
std::atomic<int> g_race_remaining{0};
std::atomic<int> g_race_stage{0};
std::atomic<bool> g_release_first{false};

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

Attachment attachment_for(std::uint64_t id,
                          ReceiverSystem system = ReceiverSystem::terrestrial)
{
    Attachment attachment{};
    attachment.owner_client_id = 7U;
    attachment.lease_id = id;
    attachment.attachment_id = id;
    attachment.receiver = 0U;
    attachment.system = system;
    attachment.token[0] = static_cast<std::uint8_t>(id);
    return attachment;
}

void reset_stream(int mode)
{
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    g_mode.store(mode, std::memory_order_release);
    g_mode_pending.store(true, std::memory_order_release);
    g_use_mode.store(false, std::memory_order_release);
    g_release_split.store(false, std::memory_order_release);
    g_split_pat_sent.store(false, std::memory_order_release);
    g_read_fail.store(0, std::memory_order_release);
    g_start_fail.store(0, std::memory_order_release);
    g_reprepare_fail.store(0, std::memory_order_release);
    g_arm_restart_block.store(false, std::memory_order_release);
    g_race_remaining.store(0, std::memory_order_release);
    g_race_stage.store(0, std::memory_order_release);
    g_release_first.store(false, std::memory_order_release);
    g_bootstrap_sent = false;
    g_split_phase = 0;
    g_continuity = 0U;
}

bool wait_phase(pix::capture::dt295::ReceiverAdapter &adapter, Phase phase,
                unsigned timeout_ms = 3000U)
{
    for (unsigned attempt = 0U; attempt < timeout_ms; ++attempt) {
        if (adapter.acquisition().phase == phase) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool read_any(pix::capture::dt295::ReceiverAdapter &adapter, const Attachment &attachment,
              std::uint8_t *buffer, std::size_t capacity, std::size_t *bytes,
              bool *eof, bool *timed_out,
              dtv::userland::detail::StreamTerminal *terminal = nullptr)
{
    const auto result = adapter.read(attachment,
        dtv::userland::MutableByteView{buffer, capacity}, std::chrono::milliseconds(100));
    if (!result) return false;
    *bytes = result.value().bytes;
    *eof = result.value().eof;
    *timed_out = result.value().timed_out;
    if (terminal != nullptr) *terminal = result.value().terminal;
    return true;
}

bool collect_until_bytes(pix::capture::dt295::ReceiverAdapter &adapter,
                         const Attachment &attachment, std::vector<std::uint8_t> &out)
{
    std::uint8_t buffer[188U * 64U];
    for (unsigned attempt = 0U; attempt < 40U && out.size() < 8U * 188U; ++attempt) {
        std::size_t bytes = 0U;
        bool eof = false, timed_out = false;
        if (!read_any(adapter, attachment, buffer, sizeof(buffer), &bytes, &eof, &timed_out))
            return false;
        if (eof) break;
        if (bytes != 0U) out.insert(out.end(), buffer, buffer + bytes);
    }
    return !out.empty();
}

bool expect_terminal(pix::capture::dt295::ReceiverAdapter &adapter,
                     const Attachment &attachment, Failure failure,
                     std::uint8_t raw_terminal)
{
    if (!wait_phase(adapter, Phase::terminal)) return false;
    if (adapter.acquisition().failure != failure) return false;
    std::uint8_t buffer[188U]{};
    std::size_t bytes = 1U;
    bool eof = false, timed_out = false;
    dtv::userland::detail::StreamTerminal terminal =
        dtv::userland::detail::StreamTerminal::none;
    if (!read_any(adapter, attachment, buffer, sizeof(buffer), &bytes, &eof, &timed_out,
                  &terminal))
        return false;
    if (!eof || timed_out || bytes != 0U) return false;
    if (terminal != static_cast<dtv::userland::detail::StreamTerminal>(raw_terminal))
        return false;
    const auto final = adapter.final_snapshot(attachment);
    return final && final.value().terminal == raw_terminal;
}

struct Rig final {
    explicit Rig(const std::vector<std::uint8_t> &assets,
                 std::size_t queue_packets = 2048U) noexcept
        : adapter(assets.data(), assets.size(), selector, clock, 0U, queue_packets) {}
    Clock clock;
    usb_selector selector{};
    pix::capture::dt295::ReceiverAdapter adapter;
};

int case_ready(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0);
    Rig rig(assets);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "ready open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "ready tune");
    CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::terrestrial), "ready start");
    const unsigned initial_generation = g_generation.load();
    const Attachment attachment = attachment_for(0U);
    CHECK_RESULT(rig.adapter.attach(attachment), "ready attach");
    CHECK(wait_phase(rig.adapter, Phase::ready));
    const auto ready = rig.adapter.acquisition();
    CHECK(ready.failure == Failure::none);
    CHECK(ready.plan_generation == 1U);
    CHECK(ready.tune_generation == 1U);
    const unsigned ready_generation = g_generation.load();
    CHECK(ready_generation > initial_generation);
    std::vector<std::uint8_t> output;
    CHECK(collect_until_bytes(rig.adapter, attachment, output));
    bool saw_new = false;
    for (std::size_t offset = 0U; offset + 188U <= output.size(); offset += 188U) {
        const std::uint8_t *packet = output.data() + offset;
        const unsigned pid = static_cast<unsigned>(((packet[1] & 0x1fU) << 8) | packet[2]);
        if (pid != 0x101U) continue;
        CHECK((packet[4] & 0x3fU) == (ready_generation & 0x3fU));
        saw_new = true;
    }
    CHECK(saw_new);
    (void)rig.adapter.detach(attachment);
    CHECK_RESULT(rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial), "ready stop");
    CHECK_RESULT(rig.adapter.close(0U), "ready close");
    return 0;
}

int case_split_gate(const std::vector<std::uint8_t> &assets)
{
    reset_stream(6);
    Rig rig(assets);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "split open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "split tune");
    CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::terrestrial), "split start");
    const unsigned initial_generation = g_generation.load();
    const Attachment attachment = attachment_for(1U);
    CHECK_RESULT(rig.adapter.attach(attachment), "split attach");
    for (unsigned attempt = 0U; attempt < 2000U && !g_split_pat_sent.load(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(g_split_pat_sent.load());
    const auto preparing = rig.adapter.acquisition();
    CHECK(preparing.phase == Phase::preparing);
    CHECK(preparing.plan_generation == 0U);
    std::uint8_t buffer[188U]{};
    std::size_t bytes = 1U;
    bool eof = false, timed_out = false;
    CHECK(read_any(rig.adapter, attachment, buffer, sizeof(buffer), &bytes, &eof, &timed_out));
    CHECK(bytes == 0U && timed_out && !eof);
    g_release_split.store(true, std::memory_order_release);
    CHECK(wait_phase(rig.adapter, Phase::ready));
    const auto ready = rig.adapter.acquisition();
    CHECK(ready.failure == Failure::none);
    CHECK(ready.plan_generation >= 1U);
    CHECK(ready.acquisition_generation == preparing.acquisition_generation);
    const unsigned ready_generation = g_generation.load();
    std::vector<std::uint8_t> output;
    CHECK(collect_until_bytes(rig.adapter, attachment, output));
    bool saw_new = false;
    for (std::size_t offset = 0U; offset + 188U <= output.size(); offset += 188U) {
        const std::uint8_t *packet = output.data() + offset;
        const unsigned pid = static_cast<unsigned>(((packet[1] & 0x1fU) << 8) | packet[2]);
        if (pid != 0x101U) continue;
        const unsigned tag = packet[4] & 0x3fU;
        CHECK(tag != (initial_generation & 0x3fU));
        CHECK(tag == (ready_generation & 0x3fU));
        saw_new = true;
    }
    CHECK(saw_new);
    CHECK(g_generation.load() > initial_generation);
    (void)rig.adapter.detach(attachment);
    CHECK_RESULT(rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial), "split stop");
    CHECK_RESULT(rig.adapter.close(0U), "split close");
    return 0;
}

int case_deadline_and_retune(const std::vector<std::uint8_t> &assets)
{
    reset_stream(5);
    Rig rig(assets);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "deadline open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "deadline tune");
    CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::terrestrial), "deadline start");
    const Attachment attachment = attachment_for(2U);
    CHECK_RESULT(rig.adapter.attach(attachment), "deadline attach");
    CHECK(wait_phase(rig.adapter, Phase::preparing));
    rig.clock.advance(30001U);
    CHECK(expect_terminal(rig.adapter, attachment, Failure::deadline,
                          static_cast<std::uint8_t>(
                              dtv::userland::detail::StreamTerminal::sync_error)));
    const unsigned old_acquisition = rig.adapter.acquisition().acquisition_generation;
    (void)rig.adapter.detach(attachment);
    CHECK_RESULT(rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial), "deadline stop");
    const auto idle = rig.adapter.acquisition();
    CHECK(idle.phase == Phase::idle);
    CHECK(idle.failure == Failure::none);
    const unsigned old_tune = idle.tune_generation;
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 479142857ULL, 1000U),
                 "deadline retune");
    const auto retuned = rig.adapter.acquisition();
    CHECK(retuned.tune_generation == old_tune + 1U);
    CHECK(retuned.failure == Failure::none);
    reset_stream(0);
    CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::terrestrial), "retune start");
    const Attachment next = attachment_for(3U);
    CHECK_RESULT(rig.adapter.attach(next), "retune attach");
    CHECK(wait_phase(rig.adapter, Phase::ready));
    CHECK(rig.adapter.acquisition().acquisition_generation > old_acquisition);
    std::vector<std::uint8_t> output;
    CHECK(collect_until_bytes(rig.adapter, next, output));
    (void)rig.adapter.detach(next);
    CHECK_RESULT(rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial), "retune stop");
    CHECK_RESULT(rig.adapter.close(0U), "retune close");
    return 0;
}

int case_failure(const std::vector<std::uint8_t> &assets, int mode, Failure failure,
                 std::uint8_t raw_terminal, bool satellite)
{
    reset_stream(mode);
    Rig rig(assets);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "failure open");
    Attachment attachment = attachment_for(4U);
    if (satellite) {
        CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::satellite, 1049480000ULL,
                                            1000U),
                     "failure satellite tune");
        CHECK_RESULT(rig.adapter.select_satellite_tsid(0U, 16400U, 1000U),
                     "failure satellite tsid");
        attachment = attachment_for(4U, ReceiverSystem::satellite);
        CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::satellite),
                     "failure satellite start");
        CHECK_RESULT(rig.adapter.attach(attachment), "failure satellite attach");
    } else {
        CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL,
                                            1000U),
                     "failure tune");
        CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::terrestrial),
                     "failure start");
        CHECK_RESULT(rig.adapter.attach(attachment), "failure attach");
    }
    CHECK(expect_terminal(rig.adapter, attachment, failure, raw_terminal));
    CHECK(rig.adapter.acquisition().phase == Phase::terminal);
    (void)rig.adapter.detach(attachment);
    CHECK_RESULT(rig.adapter.stop_capture(0U,
                 satellite ? ReceiverSystem::satellite : ReceiverSystem::terrestrial),
                 "failure stop");
    CHECK_RESULT(rig.adapter.close(0U), "failure close");
    return 0;
}

int case_reprepare_failure(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0);
    Rig rig(assets);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "reprepare open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "reprepare tune");
    CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::terrestrial), "reprepare start");
    g_reprepare_fail.store(1, std::memory_order_release);
    const Attachment attachment = attachment_for(5U);
    CHECK_RESULT(rig.adapter.attach(attachment), "reprepare attach");
    CHECK(expect_terminal(rig.adapter, attachment, Failure::usb,
                          static_cast<std::uint8_t>(
                              dtv::userland::detail::StreamTerminal::usb_error)));
    (void)rig.adapter.detach(attachment);
    CHECK_RESULT(rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial), "reprepare stop");
    CHECK_RESULT(rig.adapter.close(0U), "reprepare close");
    return 0;
}

int case_read_failure(const std::vector<std::uint8_t> &assets)
{
    reset_stream(5);
    g_read_fail.store(1, std::memory_order_release);
    Rig rig(assets);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "read failure open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "read failure tune");
    CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::terrestrial),
                 "read failure start");
    const Attachment attachment = attachment_for(6U);
    CHECK_RESULT(rig.adapter.attach(attachment), "read failure attach");
    CHECK(expect_terminal(rig.adapter, attachment, Failure::usb,
                          static_cast<std::uint8_t>(
                              dtv::userland::detail::StreamTerminal::usb_error)));
    (void)rig.adapter.detach(attachment);
    CHECK_RESULT(rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial), "read failure stop");
    CHECK_RESULT(rig.adapter.close(0U), "read failure close");
    return 0;
}

int case_disconnect_preparing(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0);
    Rig rig(assets);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "preparing disconnect open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "preparing disconnect tune");
    CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::terrestrial),
                 "preparing disconnect start");
    g_race_remaining.store(1, std::memory_order_release);
    const Attachment attachment = attachment_for(9U);
    CHECK_RESULT(rig.adapter.attach(attachment), "preparing disconnect attach");
    for (unsigned attempt = 0U; attempt < 3000U && g_race_stage.load() != 1; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(g_race_stage.load() == 1);
    CHECK(rig.adapter.acquisition().phase == Phase::preparing);
    const unsigned generation_before = g_generation.load();
    rig.adapter.mark_disconnected(0U);
    CHECK(rig.adapter.acquisition().phase == Phase::terminal);
    CHECK(rig.adapter.acquisition().failure == Failure::disconnected);
    g_release_first.store(true, std::memory_order_release);
    CHECK(!wait_phase(rig.adapter, Phase::ready, 500U));
    const auto state = rig.adapter.acquisition();
    CHECK(state.phase == Phase::terminal);
    CHECK(state.failure == Failure::disconnected);
    CHECK(state.plan_generation == 0U);
    CHECK(g_generation.load() == generation_before);
    (void)rig.adapter.detach(attachment);
    (void)rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial);
    CHECK_RESULT(rig.adapter.close(0U), "preparing disconnect close");
    return 0;
}

int case_retune_inflight(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0);
    Rig rig(assets);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "retune open");
    CHECK_RESULT(rig.adapter.select_satellite_tsid(0U, 16400U, 1000U), "retune saved TSID");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "retune terrestrial tune");
    CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::terrestrial), "retune start");
    g_race_remaining.store(1, std::memory_order_release);
    const Attachment attachment = attachment_for(10U);
    CHECK_RESULT(rig.adapter.attach(attachment), "retune attach");
    for (unsigned attempt = 0U; attempt < 3000U && g_race_stage.load() != 1; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(g_race_stage.load() == 1);
    std::atomic<int> tune_error{-1};
    std::thread retune([&] {
        const auto result = rig.adapter.program_hz(
            0U, ReceiverSystem::satellite, 1049480000ULL, 1000U);
        tune_error.store(result ? 0 : static_cast<int>(result.error()));
    });
    for (unsigned attempt = 0U; attempt < 3000U &&
         rig.adapter.acquisition().tune_generation < 2U; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(rig.adapter.acquisition().tune_generation == 2U);
    g_release_first.store(true, std::memory_order_release);
    retune.join();
    CHECK(tune_error.load() == 0);
    CHECK(rig.adapter.acquisition().tune_generation == 2U);
    (void)rig.adapter.detach(attachment);
    (void)rig.adapter.stop_capture(0U, ReceiverSystem::satellite);
    CHECK_RESULT(rig.adapter.close(0U), "retune close");
    return 0;
}

int case_stop_reader_awake(const std::vector<std::uint8_t> &assets, bool shutdown_mode)
{
    reset_stream(6);
    Rig rig(assets);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "stop reader open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "stop reader tune");
    CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::terrestrial),
                 "stop reader start");
    const Attachment attachment = attachment_for(11U);
    CHECK_RESULT(rig.adapter.attach(attachment), "stop reader attach");
    for (unsigned attempt = 0U; attempt < 3000U && !g_split_pat_sent.load(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(g_split_pat_sent.load());
    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
    std::size_t bytes = 1U;
    bool eof = false, timed_out = true;
    dtv::userland::detail::StreamTerminal terminal =
        dtv::userland::detail::StreamTerminal::none;
    std::thread reader([&] {
        std::uint8_t packet[188U]{};
        entered.store(true, std::memory_order_release);
        const auto result = rig.adapter.read(attachment,
            dtv::userland::MutableByteView{packet, sizeof(packet)},
            std::chrono::milliseconds(5000));
        if (result) {
            bytes = result.value().bytes;
            eof = result.value().eof;
            timed_out = result.value().timed_out;
            terminal = result.value().terminal;
        }
        finished.store(true, std::memory_order_release);
    });
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (shutdown_mode) {
        CHECK_RESULT(rig.adapter.shutdown(), "stop reader shutdown");
    } else {
        CHECK_RESULT(rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial),
                     "stop reader stop");
    }
    bool woke = false;
    for (unsigned attempt = 0U; attempt < 3000U && !finished.load(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    woke = finished.load();
    if (!woke) (void)rig.adapter.detach(attachment);
    reader.join();
    CHECK(woke);
    CHECK(bytes == 0U && eof && !timed_out);
    CHECK(terminal == dtv::userland::detail::StreamTerminal::stopped);
    CHECK(rig.adapter.acquisition().phase == Phase::idle);
    (void)rig.adapter.detach(attachment);
    if (!shutdown_mode) {
        CHECK_RESULT(rig.adapter.close(0U), "stop reader close");
    }
    return 0;
}

int case_slow_consumer_terminal(const std::vector<std::uint8_t> &assets, bool shutdown_mode)
{
    reset_stream(0);
    Rig rig(assets, 1U);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "slow open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "slow tune");
    CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::terrestrial), "slow start");
    const Attachment attachment = attachment_for(12U);
    CHECK_RESULT(rig.adapter.attach(attachment), "slow attach");
    bool overflow = false;
    for (unsigned attempt = 0U; attempt < 3000U; ++attempt) {
        const auto stats = rig.adapter.stats(attachment);
        if (stats && stats.value().queue_drops > 0U) {
            overflow = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(overflow);
    if (shutdown_mode) {
        CHECK_RESULT(rig.adapter.shutdown(), "slow shutdown");
    } else {
        CHECK_RESULT(rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial), "slow stop");
        (void)rig.adapter.detach(attachment);
    }
    std::uint8_t packet[188U]{};
    dtv::userland::detail::StreamTerminal terminal =
        dtv::userland::detail::StreamTerminal::none;
    bool saw_eof = false;
    for (unsigned attempt = 0U; attempt < 16U; ++attempt) {
        std::size_t bytes = 0U;
        bool eof = false, timed_out = false;
        dtv::userland::detail::StreamTerminal value =
            dtv::userland::detail::StreamTerminal::none;
        CHECK(read_any(rig.adapter, attachment, packet, sizeof(packet), &bytes, &eof,
                       &timed_out, &value));
        if (bytes != 0U) continue;
        if (eof) {
            terminal = value;
            saw_eof = true;
            break;
        }
        CHECK(timed_out);
        break;
    }
    CHECK(saw_eof);
    CHECK(terminal == dtv::userland::detail::StreamTerminal::slow_consumer);
    const auto final = rig.adapter.final_snapshot(attachment);
    CHECK(final);
    CHECK(final.value().terminal == static_cast<std::uint8_t>(
              dtv::userland::detail::StreamTerminal::slow_consumer));
    CHECK(final.value().counters.queue_drops > 0U);
    if (!shutdown_mode) CHECK_RESULT(rig.adapter.close(0U), "slow close");
    return 0;
}

int case_terminal_detach_race(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0);
    g_arm_restart_block.store(true, std::memory_order_release);
    Rig rig(assets);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "race open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "race tune");
    CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::terrestrial), "race start");
    const Attachment attachment = attachment_for(8U);
    CHECK_RESULT(rig.adapter.attach(attachment), "race attach");
    for (unsigned attempt = 0U; attempt < 3000U && g_race_stage.load() != 1; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(g_race_stage.load() == 1);
    CHECK(rig.adapter.acquisition().phase == Phase::ready);
    rig.adapter.mark_disconnected(0U);
    CHECK(rig.adapter.acquisition().phase == Phase::terminal);
    CHECK(rig.adapter.acquisition().failure == Failure::disconnected);
    g_release_first.store(true, std::memory_order_release);
    (void)rig.adapter.detach(attachment);
    std::uint8_t buffer[188U]{};
    std::size_t bytes = 1U;
    bool eof = false, timed_out = false;
    CHECK(read_any(rig.adapter, attachment, buffer, sizeof(buffer), &bytes, &eof, &timed_out));
    CHECK(bytes == 0U && eof && !timed_out);
    const auto final = rig.adapter.final_snapshot(attachment);
    CHECK(final);
    CHECK(final.value().terminal == static_cast<std::uint8_t>(
              dtv::userland::detail::StreamTerminal::disconnected));
    (void)rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial);
    CHECK_RESULT(rig.adapter.close(0U), "race close");
    return 0;
}

int case_disconnected_and_reopen(const std::vector<std::uint8_t> &assets)
{
    reset_stream(0);
    Rig rig(assets);
    CHECK(rig.adapter.ready());
    CHECK_RESULT(rig.adapter.open(0U), "disconnect open");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "disconnect tune");
    CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::terrestrial), "disconnect start");
    const Attachment attachment = attachment_for(7U);
    CHECK_RESULT(rig.adapter.attach(attachment), "disconnect attach");
    CHECK(wait_phase(rig.adapter, Phase::ready));
    for (unsigned attempt = 0U; attempt < 3000U; ++attempt) {
        const auto stats = rig.adapter.stats(attachment);
        if (stats && stats.value().packets > 0U) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto queued = rig.adapter.stats(attachment);
    CHECK(queued && queued.value().packets > 0U);
    rig.adapter.mark_disconnected(0U);
    CHECK(rig.adapter.acquisition().phase == Phase::terminal);
    CHECK(rig.adapter.acquisition().failure == Failure::disconnected);
    (void)rig.adapter.detach(attachment);
    std::size_t drained_packets = 0U;
    bool saw_eof = false;
    dtv::userland::detail::StreamTerminal terminal =
        dtv::userland::detail::StreamTerminal::none;
    std::uint8_t buffer[188U * 64U];
    for (unsigned attempt = 0U; attempt < 200U; ++attempt) {
        std::size_t bytes = 0U;
        bool eof = false, timed_out = false;
        dtv::userland::detail::StreamTerminal value =
            dtv::userland::detail::StreamTerminal::none;
        CHECK(read_any(rig.adapter, attachment, buffer, sizeof(buffer), &bytes, &eof,
                       &timed_out, &value));
        if (bytes != 0U) {
            CHECK(bytes % 188U == 0U);
            drained_packets += bytes / 188U;
            CHECK(buffer[0] == 0x47U);
            continue;
        }
        if (eof) {
            saw_eof = true;
            terminal = value;
            break;
        }
        CHECK(timed_out);
        break;
    }
    CHECK(saw_eof);
    CHECK(drained_packets > 0U);
    CHECK(terminal == dtv::userland::detail::StreamTerminal::disconnected);
    const auto final = rig.adapter.final_snapshot(attachment);
    CHECK(final);
    CHECK(final.value().terminal == static_cast<std::uint8_t>(
              dtv::userland::detail::StreamTerminal::disconnected));
    (void)rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial);
    CHECK_RESULT(rig.adapter.close(0U), "disconnect close");
    CHECK(rig.adapter.acquisition().phase == Phase::idle);
    CHECK_RESULT(rig.adapter.open(0U), "reopen");
    CHECK_RESULT(rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "reopen tune");
    const unsigned generation = rig.adapter.acquisition().acquisition_generation;
    CHECK_RESULT(rig.adapter.start_capture(0U, ReceiverSystem::terrestrial), "reopen start");
    CHECK(rig.adapter.acquisition().acquisition_generation > generation);
    CHECK_RESULT(rig.adapter.close(0U), "reopen close");
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

extern "C" int __wrap_libusb_control_transfer(libusb_device_handle *, std::uint8_t request_type,
                                               std::uint8_t request, std::uint16_t value,
                                               std::uint16_t index, unsigned char *data,
                                               std::uint16_t length, unsigned int)
{
    return dt295_actual_fixture_control(request_type, request, value, index, data, length);
}

extern "C" int __wrap_libusb_bulk_transfer(libusb_device_handle *, unsigned char endpoint,
                                            unsigned char *data, int length, int *actual,
                                            unsigned int)
{
    return dt295_actual_fixture_bulk(endpoint, data, length, actual);
}

extern "C" int __wrap_libusb_interrupt_transfer(libusb_device_handle *, unsigned char endpoint,
                                                 unsigned char *data, int length, int *actual,
                                                 unsigned int)
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
    if (g_start_fail.load(std::memory_order_acquire) > 0) {
        g_start_fail.fetch_sub(1, std::memory_order_acq_rel);
        return LIBUSB_ERROR_IO;
    }
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    const bool initial = g_mode_pending.exchange(false, std::memory_order_acq_rel);
    g_use_mode.store(initial, std::memory_order_release);
    g_generation.fetch_add(1U, std::memory_order_acq_rel);
    g_bootstrap_sent = false;
    g_split_phase = 0;
    g_continuity = 0U;
    if (initial) g_split_pat_sent.store(false, std::memory_order_release);
    if (!initial && g_arm_restart_block.exchange(false, std::memory_order_acq_rel))
        g_race_remaining.store(1, std::memory_order_release);
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
    const unsigned generation = g_generation.load(std::memory_order_acquire);
    const int mode = g_use_mode.load(std::memory_order_acquire)
                         ? g_mode.load(std::memory_order_acquire)
                         : 0;
    if (!g_bootstrap_sent) {
        uint8_t packets[4][188];
        const unsigned count =
            dt295_actual_fixture_acquisition_bootstrap(packets, mode == 6 ? 0 : mode);
        auto append_bootstrap = [&](unsigned index) {
            std::memcpy(output + length, packets[index], 188U);
            dt295_actual_fixture_encrypt(output + length);
            length += 188U;
        };
        if (mode == 5) {
            append_bootstrap(0U);
            g_bootstrap_sent = true;
            *sequence += 1U;
            *completed_ns = 1U;
            return static_cast<int>(length);
        }
        if (mode == 6 && g_split_phase == 0) {
            append_bootstrap(0U);
            g_split_phase = 1;
            g_split_pat_sent.store(true, std::memory_order_release);
        } else if (mode == 6 && g_split_phase == 1) {
            if (!g_release_split.load(std::memory_order_acquire)) return 0;
            append_bootstrap(1U);
            append_bootstrap(2U);
            g_split_phase = 2;
            g_bootstrap_sent = true;
        } else {
            for (unsigned index = 0U; index < count; ++index) append_bootstrap(index);
            g_bootstrap_sent = true;
        }
    }
    if (g_bootstrap_sent) {
        for (unsigned index = 0U;
             index < 16U && length + 188U <= capacity; ++index) {
            uint8_t packet[188];
            dt295_actual_fixture_recording_packet(packet, g_continuity++);
            packet[4] = static_cast<std::uint8_t>(0x40U | (generation & 0x3fU));
            dt295_actual_fixture_encrypt(packet);
            std::memcpy(output + length, packet, 188U);
            length += 188U;
        }
    }
    if (length == 0U) return 0;
    if (sequence != nullptr) *sequence += length / 188U;
    if (completed_ns != nullptr) *completed_ns = 1U;
    return static_cast<int>(length);
}

extern "C" int __wrap_dt295_routes_feed(dt295_routes *routes, const uint8_t packet[188])
{
    const int remaining = g_race_remaining.load(std::memory_order_acquire);
    const int previous = remaining > 0
        ? g_race_remaining.fetch_sub(1, std::memory_order_acq_rel) : 0;
    if (previous == 1) {
        g_race_stage.store(1, std::memory_order_release);
        while (!g_release_first.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return __real_dt295_routes_feed(routes, packet);
}

extern "C" int __wrap_dt295_stream_stop(dt295_stream *, unsigned) { return 0; }
extern "C" int __wrap_dt295_stream_destroy(dt295_stream *) { return 0; }

extern "C" int __wrap_dt295_reprepare_plan(dt295 *device, const dt295_route_plan *plan,
                                            char *error, std::size_t error_size)
{
    if (g_reprepare_fail.load(std::memory_order_acquire) > 0) {
        g_reprepare_fail.fetch_sub(1, std::memory_order_acq_rel);
        if (error != nullptr && error_size != 0U)
            std::snprintf(error, error_size, "synthetic reprepare failure");
        return -1;
    }
    return __real_dt295_reprepare_plan(device, plan, error, error_size);
}

int main(int argc, char **argv)
{
    char directory[] = "/tmp/pix-dt295-acquisition-XXXXXX";
    if (mkdtemp(directory) == nullptr) {
        std::fprintf(stderr, "FAILED mkdtemp\n");
        return 1;
    }
    const std::string assets_path = std::string(directory) + "/dt295.assets";
    CHECK(dt295_actual_fixture_open(assets_path.c_str()) == 0);
    std::ifstream assets_file(assets_path, std::ios::binary);
    CHECK(assets_file.good());
    const std::vector<std::uint8_t> assets{
        std::istreambuf_iterator<char>(assets_file), std::istreambuf_iterator<char>()};
    assets_file.close();
    CHECK(!assets.empty());
    const std::string only = argc > 1 ? argv[1] : std::string();
    const auto run = [&](const char *name, const auto &body) {
        if (!only.empty() && only != name) return true;
        if (body() != 0) {
            std::fprintf(stderr, "CASE failed: %s\n", name);
            return false;
        }
        return true;
    };

    CHECK(run("ready", [&] { return case_ready(assets); }));
    CHECK(run("split", [&] { return case_split_gate(assets); }));
    CHECK(run("deadline", [&] { return case_deadline_and_retune(assets); }));
    CHECK(run("unsupported", [&] {
        return case_failure(assets, 3, Failure::unsupported,
                            static_cast<std::uint8_t>(
                                dtv::userland::detail::StreamTerminal::bridge_fatal), false);
    }));
    CHECK(run("plan", [&] {
        return case_failure(assets, 2, Failure::plan,
                            static_cast<std::uint8_t>(
                                dtv::userland::detail::StreamTerminal::bridge_fatal), false);
    }));
    CHECK(run("multiple-ecm", [&] {
        return case_failure(assets, 1, Failure::multiple_ecm,
                            static_cast<std::uint8_t>(
                                dtv::userland::detail::StreamTerminal::bridge_fatal), false);
    }));
    CHECK(run("satellite-tsid", [&] {
        return case_failure(assets, 4, Failure::satellite_tsid,
                            static_cast<std::uint8_t>(
                                dtv::userland::detail::StreamTerminal::bridge_fatal), true);
    }));
    CHECK(run("reprepare", [&] { return case_reprepare_failure(assets); }));
    CHECK(run("read-failure", [&] { return case_read_failure(assets); }));
    CHECK(run("terminal-race", [&] { return case_terminal_detach_race(assets); }));
    CHECK(run("slow-stop", [&] { return case_slow_consumer_terminal(assets, false); }));
    CHECK(run("slow-shutdown", [&] { return case_slow_consumer_terminal(assets, true); }));
    CHECK(run("disconnect-preparing", [&] { return case_disconnect_preparing(assets); }));
    CHECK(run("retune-inflight", [&] { return case_retune_inflight(assets); }));
    CHECK(run("stop-reader", [&] { return case_stop_reader_awake(assets, false); }));
    CHECK(run("shutdown-reader", [&] { return case_stop_reader_awake(assets, true); }));
    CHECK(run("disconnect-drain", [&] { return case_disconnected_and_reopen(assets); }));

    dt295_actual_fixture_close();
    (void)unlink(assets_path.c_str());
    (void)rmdir(directory);
    std::fprintf(stderr, "DT295 acquisition gate: preparing/ready/terminal/failure OK\n");
    return 0;
}
