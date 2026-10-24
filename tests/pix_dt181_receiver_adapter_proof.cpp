/* SPDX-License-Identifier: MIT */
#include "pix-dt181-receiver-adapter.h"

extern "C" {
#include "dt181_session.h"
#include "usb_backend.h"
}
#include "vectors.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <pthread.h>
#include <thread>

extern "C" void pix_test_usb_queue(const std::uint8_t*, std::size_t);

namespace {

class Clock final : public dtv::userland::detail::ReceiverClock {
public:
    std::uint64_t monotonic_ms() noexcept override
    { return 1000U + static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count()); }
    void sleep_ms(std::uint32_t ms) noexcept override { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
};

class Tokens final : public dtv::userland::detail::ReceiverTokenSource {
public:
    dtv::userland::Result<dtv::userland::detail::ReceiverToken> generate() noexcept override
    {
        dtv::userland::detail::ReceiverToken value{};
        value[0] = ++next;
        return dtv::userland::Result<dtv::userland::detail::ReceiverToken>::success(value);
    }
    std::uint8_t next = 0U;
};

std::uint32_t last_hz;
pixy_system last_system;
int last_lna;
int last_tsid;
int last_slot;
unsigned tune_count;
bool fail_next_tune;
std::size_t last_status_capacity;
std::atomic<pthread_mutex_t*> held_sink_mutex{nullptr};
pthread_t proof_main_thread;
std::atomic<bool> held_sink_armed{false};
std::atomic<bool> held_sink_entered{false};
pthread_mutex_t held_sink_gate_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t held_sink_gate_cond = PTHREAD_COND_INITIALIZER;
bool held_sink_gate_released;

}  // namespace

namespace pix::capture::dt181 {
struct ConsumerTestAccess {
    static const Consumer::Attachment& identity(const Consumer& consumer)
    { return consumer.run_->identity; }
    static pthread_mutex_t* mutex(Consumer& consumer)
    { return consumer.session_.mutex.native_handle(); }
};
}

extern "C" {
int __real_pthread_mutex_lock(pthread_mutex_t*);
int __wrap_pixy_power(pixy*, const std::uint8_t*, std::size_t) { return 0; }
int __wrap_pixy_session(pixy*, const std::uint8_t[16], const std::uint8_t[64]) { return 0; }
int __wrap_pixy_tuner_init(pixy*) { return 0; }
int __wrap_pixy_tune_slot(pixy* device, pixy_system system, std::uint32_t hz,
                          int lna, int* tsid, int slot)
{
    ++tune_count;
    last_system = system;
    last_hz = hz;
    last_lna = lna;
    last_tsid = *tsid;
    last_slot = slot;
    if (fail_next_tune) {
        fail_next_tune = false;
        device->failure_exit = 5;
        return -1;
    }
    if (system != PIXY_TERRESTRIAL) {
        if (slot >= 0) {
            if (slot > 7 || *tsid >= 0) {
                device->failure_exit = 2;
                return -1;
            }
            *tsid = 0x1234;
        } else if (*tsid == -1) {
            *tsid = 0x1234;
        } else if (*tsid != 0x1234) {
            device->failure_exit = 5;
            return -1;
        }
    }
    return 0;
}
int __wrap_pixy_status(pixy*, std::uint8_t* status, std::size_t* length)
{
    last_status_capacity = *length;
    if (*length < 64U) return -1;
    std::memset(status, 0, *length);
    status[3] = 1U;
    *length = 64U;
    return 0;
}
int __wrap_pixy_fifo_clear(pixy*) { return 0; }
int __wrap_pixy_start(pixy*, std::uint8_t output[32])
{ std::memcpy(output, context, sizeof(context)); return 0; }
int __wrap_pixy_stop(pixy*) { return 0; }
int __wrap_pixy_certify(pixy*) { return 0; }
int __wrap_pixy_shutdown(pixy*) { return 0; }
int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex)
{
    if (mutex == held_sink_mutex.load() && held_sink_armed.load() &&
        !pthread_equal(pthread_self(), proof_main_thread)) {
        held_sink_entered.store(true);
        pthread_mutex_lock(&held_sink_gate_mutex);
        while (!held_sink_gate_released)
            pthread_cond_wait(&held_sink_gate_cond, &held_sink_gate_mutex);
        pthread_mutex_unlock(&held_sink_gate_mutex);
    }
    return __real_pthread_mutex_lock(mutex);
}
}

int main()
{
    using namespace std::chrono_literals;
    using namespace dtv::userland;
    using namespace dtv::userland::detail;
    using namespace pix::capture::dt181;
    constexpr ReceiverId receiver = 0U;
    proof_main_thread = pthread_self();
    constexpr std::uint64_t client = 11U;
    const ReceiverId receivers[] = {receiver};
    dt181_session session{};
    assert(dt181_session_open(&session, nullptr, 42, 0) == 0);
    const std::array<std::uint8_t, 1> firmware{{0U}};
    const std::array<std::uint8_t, 16> bootstrap{};
    const std::array<std::uint8_t, 64> validation{};
    assert(dt181_session_initialize(&session, firmware.data(), bootstrap.data(), validation.data()) == 0);

    Clock clock;
    Tokens tokens;
    Consumer consumer;
    ReceiverAdapter adapter(session, consumer, clock, receiver, 8U);
    ReceiverEngine engine(adapter, receivers, 1U, tokens, clock, nullptr, nullptr, &adapter);
    assert(ReceiverAdapter::configure(engine, receiver, 7U));
    auto lease = engine.acquire(client, receiver);
    assert(lease);

    HzTuneRequest tune{};
    tune.lease_id = lease.value().lease_id;
    tune.system = ReceiverSystem::terrestrial;
    tune.requested_hz = 473142857U;
    tune.timeout_ms = 1000U;
    auto admitted = engine.try_admit_tune(client, tune, clock.monotonic_ms());
    assert(admitted && engine.execute_admitted_tune(admitted.value()));
    assert(tune_count == 1U && last_hz == 473142857U && last_system == PIXY_TERRESTRIAL);
    const auto status = engine.status();
    assert(status && status.value().tune_metadata[0].current.lock.state == wire::MeasureState::valid);
    assert(!status.value().tune_metadata[0].current.programmed_hz.has_value());

    tune.requested_hz = 473142000U;
    admitted = engine.try_admit_tune(client, tune, clock.monotonic_ms());
    assert(admitted && engine.execute_admitted_tune(admitted.value()));
    assert(tune_count == 2U && last_hz == 473142000U);

    tune.system = ReceiverSystem::satellite;
    tune.requested_hz = 1049480000U;
    tune.stream_id = 0x1234U;
    admitted = engine.try_admit_tune(client, tune, clock.monotonic_ms());
    assert(admitted && engine.execute_admitted_tune(admitted.value()));
    assert(tune_count == 3U && last_system == PIXY_BS && last_lna == 1 &&
           last_tsid == 0x1234 && last_slot == -1);

    tune.stream_id = 0xffffU;
    tune.slot = 4U;
    admitted = engine.try_admit_tune(client, tune, clock.monotonic_ms());
    assert(admitted && engine.execute_admitted_tune(admitted.value()));
    assert(tune_count == 4U && last_system == PIXY_BS && last_lna == 1 &&
           last_tsid == -1 && last_slot == 4);

    tune.stream_id = 0xffffU;
    tune.slot = 0xffffU;
    admitted = engine.try_admit_tune(client, tune, clock.monotonic_ms());
    assert(admitted && engine.execute_admitted_tune(admitted.value()));
    assert(tune_count == 5U && last_system == PIXY_BS && last_lna == 1 &&
           last_tsid == -1 && last_slot == -1);
    assert(last_status_capacity == 64U);

    tune.system = ReceiverSystem::terrestrial;
    tune.stream_id = 0xffffU;
    tune.slot = 0xffffU;
    tune.requested_hz = 557142857U;
    admitted = engine.try_admit_tune(client, tune, clock.monotonic_ms());
    assert(admitted && engine.execute_admitted_tune(admitted.value()));
    assert(tune_count == 6U);
    assert(last_hz == 557142857U);
    const auto tuned = engine.status();
    assert(tuned);
    auto started = engine.start_stream_v1(client, lease.value().lease_id,
        tuned.value().tune_metadata[0].current_revision, clock.monotonic_ms(), 5000U);
    assert(started);
    auto ticket = engine.reserve_attach_v1(client, lease.value().lease_id,
        started.value().epoch, started.value().stream_id, started.value().token,
        clock.monotonic_ms());
    assert(ticket && engine.prepare_attach_v1(ticket.value()));
    assert(engine.commit_attach_v1(ticket.value()));
    const Attachment attachment = pix::capture::dt181::ConsumerTestAccess::identity(consumer);

    pix_test_usb_queue(::wire, sizeof(::wire));
    std::array<std::uint8_t, sizeof(plain)> output{};
    auto read = consumer.read(attachment,
        MutableByteView{output.data(), output.size()}, 1000ms);
    assert(read && read.value().bytes == sizeof(plain));
    assert(std::memcmp(output.data(), plain, sizeof(plain)) == 0);

    auto stopped = engine.stop_stream_v1(client, lease.value().lease_id, started.value().stream_id);
    assert(stopped && !stopped.value().quiesced);
    assert(engine.run_stream_cleanup_v1(ticket.value()));
    auto final = adapter.final_snapshot(attachment);
    assert(final && final.value().counters.bytes == sizeof(plain));
    assert(adapter.release_final(attachment));
    assert(engine.release(client, lease.value().lease_id));

    lease = engine.acquire(client, receiver);
    assert(lease);
    tune.lease_id = lease.value().lease_id;
    tune.system = ReceiverSystem::terrestrial;
    tune.requested_hz = 473142857U;
    admitted = engine.try_admit_tune(client, tune, clock.monotonic_ms());
    assert(admitted && engine.execute_admitted_tune(admitted.value()));
    const auto retuned = engine.status();
    assert(retuned);
    started = engine.start_stream_v1(client, lease.value().lease_id,
        retuned.value().tune_metadata[0].current_revision, clock.monotonic_ms(), 5000U);
    assert(started);
    ticket = engine.reserve_attach_v1(client, lease.value().lease_id,
        started.value().epoch, started.value().stream_id, started.value().token,
        clock.monotonic_ms());
    assert(ticket && engine.prepare_attach_v1(ticket.value()));
    assert(engine.commit_attach_v1(ticket.value()));
    const Attachment replay_attachment = ConsumerTestAccess::identity(consumer);
    pix_test_usb_queue(::wire, sizeof(::wire));
    output.fill(0U);
    read = consumer.read(replay_attachment,
        MutableByteView{output.data(), output.size()}, 1000ms);
    assert(read && read.value().bytes == sizeof(plain));
    assert(std::memcmp(output.data(), plain, sizeof(plain)) == 0);
    assert(adapter.detach(replay_attachment));
    stopped = engine.stop_stream_v1(client, lease.value().lease_id, started.value().stream_id);
    assert(stopped && !stopped.value().quiesced);
    assert(engine.run_stream_cleanup_v1(ticket.value()));
    final = adapter.final_snapshot(replay_attachment);
    assert(final && final.value().counters.bytes == sizeof(plain));
    assert(adapter.release_final(replay_attachment));
    assert(engine.release(client, lease.value().lease_id));

    lease = engine.acquire(client, receiver);
    assert(lease);
    tune.lease_id = lease.value().lease_id;
    tune.system = ReceiverSystem::terrestrial;
    tune.requested_hz = 473142857U;
    admitted = engine.try_admit_tune(client, tune, clock.monotonic_ms());
    assert(admitted && engine.execute_admitted_tune(admitted.value()));
    const auto held_tuned = engine.status();
    assert(held_tuned);
    started = engine.start_stream_v1(client, lease.value().lease_id,
        held_tuned.value().tune_metadata[0].current_revision, clock.monotonic_ms(), 5000U);
    assert(started);
    ticket = engine.reserve_attach_v1(client, lease.value().lease_id,
        started.value().epoch, started.value().stream_id, started.value().token,
        clock.monotonic_ms());
    assert(ticket && engine.prepare_attach_v1(ticket.value()));
    assert(engine.commit_attach_v1(ticket.value()));
    const Attachment held_attachment = ConsumerTestAccess::identity(consumer);
    held_sink_mutex.store(ConsumerTestAccess::mutex(consumer));
    held_sink_gate_released = false;
    held_sink_entered.store(false);
    held_sink_armed.store(true);
    pix_test_usb_queue(::wire, sizeof(::wire));
    for (unsigned attempt = 0U; attempt < 1500U && !held_sink_entered.load(); ++attempt)
        std::this_thread::sleep_for(1ms);
    assert(held_sink_entered.load());
    stopped = engine.stop_stream_v1(client, lease.value().lease_id, started.value().stream_id);
    assert(stopped && !stopped.value().quiesced);
    std::atomic<bool> detach_started{false};
    std::atomic<bool> detach_done{false};
    bool detach_ok = false;
    std::thread detach([&] {
        detach_started.store(true);
        detach_ok = static_cast<bool>(adapter.detach(held_attachment));
        detach_done.store(true);
    });
    for (unsigned attempt = 0U; attempt < 1500U && !detach_started.load(); ++attempt)
        std::this_thread::sleep_for(1ms);
    assert(detach_started.load());
    for (unsigned attempt = 0U; attempt < 50U && !detach_done.load(); ++attempt)
        std::this_thread::sleep_for(1ms);
    assert(!detach_done.load());
    assert(session.opened != 0 && session.usb != nullptr);
    held_sink_armed.store(false);
    held_sink_mutex.store(nullptr);
    pthread_mutex_lock(&held_sink_gate_mutex);
    held_sink_gate_released = true;
    pthread_cond_broadcast(&held_sink_gate_cond);
    pthread_mutex_unlock(&held_sink_gate_mutex);
    detach.join();
    assert(detach_ok && detach_done.load());
    assert(engine.run_stream_cleanup_v1(ticket.value()));
    assert(session.opened != 0 && session.usb != nullptr);
    final = adapter.final_snapshot(held_attachment);
    assert(final && final.value().counters.bytes == sizeof(plain));
    output.fill(0U);
    read = adapter.read(held_attachment,
        MutableByteView{output.data(), output.size()}, 1000ms);
    assert(read && read.value().bytes == sizeof(plain));
    assert(std::memcmp(output.data(), plain, sizeof(plain)) == 0);
    assert(adapter.release_final(held_attachment));
    assert(engine.release(client, lease.value().lease_id));
    assert(engine.shutdown());

    assert(adapter.open(receiver));
    assert(adapter.select_satellite_slot(receiver, 6U, 1000U));
    fail_next_tune = true;
    auto failed_selection = adapter.program_hz(receiver, ReceiverSystem::satellite,
        1049480000U, 1000U);
    assert(!failed_selection && failed_selection.error() == Error::TIMEOUT);
    auto terrestrial_after_failure = adapter.program_hz(receiver,
        ReceiverSystem::terrestrial, 473142000U, 1000U);
    assert(terrestrial_after_failure && last_system == PIXY_TERRESTRIAL &&
           last_slot == -1 && last_tsid == -1);
    assert(adapter.close(receiver));

    Consumer evict_consumer;
    Attachment evict_attachment{};
    evict_attachment.owner_client_id = 21U;
    evict_attachment.lease_id = 22U;
    evict_attachment.attachment_id = 23U;
    evict_attachment.receiver = receiver;
    evict_attachment.evict_oldest = true;
    assert(evict_consumer.start(&session, evict_attachment, 1U));
    pix_test_usb_queue(::wire, sizeof(::wire));
    assert(evict_consumer.poll(1U));
    assert(evict_consumer.stop());
    auto evict_final = evict_consumer.final_snapshot(evict_attachment);
    assert(evict_final && evict_final.value().counters.queue_drops == 0U &&
           evict_final.value().terminal == static_cast<std::uint8_t>(StreamTerminal::stopped));
    std::array<std::uint8_t, 188> newest{};
    auto evict_read = evict_consumer.read(evict_attachment,
        MutableByteView{newest.data(), newest.size()}, 100ms);
    assert(evict_read && evict_read.value().bytes == newest.size());
    assert(std::memcmp(newest.data(), plain + 188U, newest.size()) == 0);
    assert(evict_consumer.release_final(evict_attachment));

    assert(dt181_session_shutdown(&session) == 0);
    dt181_session_close(&session);
    return 0;
}
