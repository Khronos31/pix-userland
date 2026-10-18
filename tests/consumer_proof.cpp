/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pix-capture-consumer.h"
extern "C" {
#include "dt181_session.h"
#include "usb_backend.h"
}
#include "vectors.h"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <atomic>
#include <pthread.h>
#include <ctime>
#include <cstdio>

extern "C" void pix_test_usb_queue(const std::uint8_t*, std::size_t);
extern "C" void pix_test_usb_fail_cancel(void);
extern "C" unsigned pix_test_usb_submissions(void);
extern "C" unsigned pix_test_usb_completions(void);
extern "C" unsigned pix_test_usb_cancellations(void);
extern "C" unsigned pix_test_usb_frees(void);
static bool fail_fifo;
static bool fail_start;
static bool fail_worker;
static bool fail_join;
static bool fail_allocation;
static bool fail_device_stop;

namespace pix::capture {
struct ConsumerTestAccess {
    static std::mutex& mutex(Consumer& consumer) { return consumer.session_.mutex; }
    static pthread_cond_t* condition(Consumer& consumer)
    {
        return consumer.session_.changed.native_handle();
    }
};
}
static pthread_mutex_t* held_sink_mutex;
static pthread_t proof_main_thread;
static std::atomic<bool> held_sink_armed{false};
static std::atomic<bool> held_sink_entered{false};
static pthread_mutex_t held_sink_gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t held_sink_gate_cond = PTHREAD_COND_INITIALIZER;
static bool held_sink_gate_released;
static pthread_cond_t* old_read_condition_target;
static std::atomic<bool> old_read_wait_entered{false};
static std::atomic<bool> old_read_woke{false};
static bool old_read_gate_released;
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*,
                                      void* (*)(void*), void*);
extern "C" int __real_pthread_join(pthread_t, void**);
extern "C" void __real_dt181_capture_host_destroy(dt181_capture_host*);
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __real_pthread_cond_clockwait(pthread_cond_t*, pthread_mutex_t*,
                                               clockid_t, const timespec*);
extern "C" void* __real_calloc(std::size_t, std::size_t);

extern "C" {
int __wrap_pixy_fifo_clear(pixy*) { const bool fail = fail_fifo; fail_fifo = false; return fail ? -1 : 0; }
int __wrap_pixy_start(pixy*, uint8_t out[32]) { std::memcpy(out, context, 32U); const bool fail = fail_start; fail_start = false; return fail ? -1 : 0; }
int __wrap_pixy_stop(pixy*)
{
    const bool fail = fail_device_stop;
    fail_device_stop = false;
    if (fail) std::fprintf(stderr, "PIX first_error hardware_stop\n");
    return fail ? -1 : 0;
}
int __wrap_pixy_certify(pixy*) { return 0; }
int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*entry)(void*), void* argument)
{
    if (fail_worker) { fail_worker = false; return EAGAIN; }
    return __real_pthread_create(thread, attr, entry, argument);
}
int __wrap_pthread_join(pthread_t thread, void** result)
{
    if (fail_join) {
        fail_join = false;
        std::fprintf(stderr, "PIX worker_join injected_failure\n");
        return EINVAL;
    }
    const int rc = __real_pthread_join(thread, result);
    std::fprintf(stderr, "PIX worker_join result=%d\n", rc);
    return rc;
}
void __wrap_dt181_capture_host_destroy(dt181_capture_host* host)
{
    __real_dt181_capture_host_destroy(host);
    std::fprintf(stderr, "PIX host_bridge_context_free_complete\n");
}
int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex)
{
    if (mutex == held_sink_mutex && held_sink_armed.load() &&
        !pthread_equal(pthread_self(), proof_main_thread)) {
        held_sink_entered.store(true);
        std::fprintf(stderr, "PIX real_sink_callback_gate_entered\n");
        pthread_mutex_lock(&held_sink_gate_mutex);
        if (!held_sink_gate_released) {
            timespec deadline{};
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += 3;
            while (!held_sink_gate_released) {
                const int rc = pthread_cond_timedwait(
                    &held_sink_gate_cond, &held_sink_gate_mutex, &deadline);
                if (rc != 0) { held_sink_gate_released = true; break; }
            }
        }
        pthread_mutex_unlock(&held_sink_gate_mutex);
    }
    return __real_pthread_mutex_lock(mutex);
}
int __wrap_pthread_cond_clockwait(pthread_cond_t* condition,
                                  pthread_mutex_t* mutex, clockid_t clock_id,
                                  const timespec* deadline)
{
    if (condition != old_read_condition_target)
        return __real_pthread_cond_clockwait(condition, mutex, clock_id, deadline);
    old_read_wait_entered.store(true);
    const int rc = __real_pthread_cond_clockwait(condition, mutex, clock_id, deadline);
    if (rc == 0) {
        pthread_mutex_unlock(mutex);
        old_read_woke.store(true);
        pthread_mutex_lock(&held_sink_gate_mutex);
        if (!old_read_gate_released) {
            timespec gate_deadline{};
            clock_gettime(CLOCK_REALTIME, &gate_deadline);
            gate_deadline.tv_sec += 3;
            while (!old_read_gate_released) {
                const int gate_rc = pthread_cond_timedwait(
                    &held_sink_gate_cond, &held_sink_gate_mutex, &gate_deadline);
                if (gate_rc != 0) { old_read_gate_released = true; break; }
            }
        }
        pthread_mutex_unlock(&held_sink_gate_mutex);
        pthread_mutex_lock(mutex);
    }
    return rc;
}
void* __wrap_calloc(std::size_t count, std::size_t size)
{
    if (fail_allocation) { fail_allocation = false; return nullptr; }
    return __real_calloc(count, size);
}
}

int main()
{
    using namespace std::chrono_literals;
    using namespace pix::capture;
    proof_main_thread = pthread_self();
    dt181_session session{};
    assert(dt181_session_open(&session, nullptr, 42, 0) == 0);
    Consumer consumer;
    Consumer::Attachment identity{};
    identity.owner_client_id = 11U;
    identity.lease_id = 12U;
    identity.attachment_id = 13U;
    identity.receiver = 1U;
    identity.token[0] = 14U;
    assert(consumer.start(&session, identity, 8U));
    pix_test_usb_queue(wire, 100U);
    assert(consumer.poll(1U));
    assert(consumer.certify());
    pix_test_usb_queue(wire + 100U, sizeof(wire) - 100U);
    const unsigned registered_before_stop = pix_test_usb_submissions();
    const unsigned callbacks_before_stop = pix_test_usb_completions();
    assert(consumer.stop());
    assert(pix_test_usb_submissions() >= registered_before_stop);
    assert(pix_test_usb_completions() > callbacks_before_stop);
    assert(pix_test_usb_cancellations() >= 16U);
    assert(pix_test_usb_frees() >= 16U);

    std::uint8_t output[sizeof(plain)]{};
    auto read = consumer.read(identity,
                              dtv::userland::MutableByteView{output, sizeof(output)}, 250ms);
    assert(read && read.value().bytes == sizeof(plain));
    assert(std::memcmp(output, plain, sizeof(plain)) == 0);
    auto final = consumer.final_snapshot(identity);
    assert(final && final.value().counters.packets == 2U);
    assert(final.value().counters.bytes == sizeof(plain));
    assert(consumer.release_final(identity));

    Consumer held_sink;
    assert(held_sink.start(&session, identity, 8U));
    held_sink_mutex = ConsumerTestAccess::mutex(held_sink).native_handle();
    held_sink_gate_released = false;
    held_sink_armed.store(true);
    pix_test_usb_queue(wire, sizeof(wire));
    const bool held_poll_ok = static_cast<bool>(held_sink.poll(1U));
    for (unsigned attempt = 0U; attempt < 1500U && !held_sink_entered.load(); ++attempt)
        std::this_thread::sleep_for(1ms);
    const bool real_sink_entered = held_sink_entered.load();
    std::atomic<bool> held_stop_done{false};
    bool held_stop_result = false;
    std::thread held_stop([&] {
        held_stop_result = static_cast<bool>(held_sink.stop());
        held_stop_done.store(true);
    });
    for (unsigned attempt = 0U; attempt < 50U && !held_stop_done.load(); ++attempt)
        std::this_thread::sleep_for(1ms);
    const bool stop_waited_for_sink = !held_stop_done.load();
    const bool replacement_rejected_while_held = !held_sink.start(&session, identity, 8U);
    held_sink_mutex = nullptr;
    held_sink_armed.store(false);
    pthread_mutex_lock(&held_sink_gate_mutex);
    held_sink_gate_released = true;
    pthread_cond_broadcast(&held_sink_gate_cond);
    pthread_mutex_unlock(&held_sink_gate_mutex);
    held_stop.join();
    std::fprintf(stderr, "PIX held_sink stop_waited=%d replacement_rejected=%d stop_ok=%d\n",
                 stop_waited_for_sink, replacement_rejected_while_held, held_stop_result);
    assert(held_poll_ok && real_sink_entered && stop_waited_for_sink && replacement_rejected_while_held &&
           held_stop_result);
    std::memset(output, 0, sizeof(output));
    read = held_sink.read(identity,
                          dtv::userland::MutableByteView{output, sizeof(output)}, 250ms);
    assert(read && read.value().bytes == sizeof(plain));
    assert(std::memcmp(output, plain, sizeof(plain)) == 0);
    final = held_sink.final_snapshot(identity);
    assert(final && final.value().counters.packets == 2U &&
           final.value().counters.bytes == sizeof(plain));
    assert(held_sink.release_final(identity));

    Consumer old_read_consumer;
    assert(old_read_consumer.start(&session, identity, 8U));
    old_read_condition_target = ConsumerTestAccess::condition(old_read_consumer);
    old_read_wait_entered.store(false);
    old_read_woke.store(false);
    old_read_gate_released = false;
    bool old_read_not_found = false;
    std::thread old_reader([&] {
        std::uint8_t stale[188U]{};
        auto result = old_read_consumer.read(
            identity, dtv::userland::MutableByteView{stale, sizeof(stale)}, 5s);
        old_read_not_found = !result &&
            result.error() == dtv::userland::Error::NOT_FOUND;
    });
    bool old_wait_observed = false;
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        if (old_read_wait_entered.load()) { old_wait_observed = true; break; }
        std::this_thread::sleep_for(1ms);
    }
    const bool old_a_stopped = static_cast<bool>(old_read_consumer.stop());
    bool old_read_paused_after_wake = false;
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        if (old_read_woke.load()) { old_read_paused_after_wake = true; break; }
        std::this_thread::sleep_for(1ms);
    }
    const bool same_identity_b_started = static_cast<bool>(
        old_read_consumer.start(&session, identity, 8U));
    const bool b_poll_ok = same_identity_b_started &&
        (pix_test_usb_queue(wire, sizeof(wire)),
         static_cast<bool>(old_read_consumer.poll(1U)));
    bool old_b_complete = false;
    for (unsigned attempt = 0U; b_poll_ok && attempt < 1500U; ++attempt) {
        auto counters = old_read_consumer.stats(identity);
        if (counters && counters.value().packets == 2U) { old_b_complete = true; break; }
        std::this_thread::sleep_for(1ms);
    }
    std::uint8_t old_b_bytes[sizeof(plain)]{};
    auto old_b_read = old_b_complete ? old_read_consumer.read(
        identity, dtv::userland::MutableByteView{old_b_bytes, sizeof(old_b_bytes)}, 1500ms)
        : dtv::userland::Result<Consumer::ReadResult>::failure(dtv::userland::Error::INTERNAL);
    pthread_mutex_lock(&held_sink_gate_mutex);
    old_read_gate_released = true;
    pthread_cond_broadcast(&held_sink_gate_cond);
    pthread_mutex_unlock(&held_sink_gate_mutex);
    old_read_condition_target = nullptr;
    old_reader.join();
    const bool b_bytes_ok = old_b_read && old_b_read.value().bytes == sizeof(old_b_bytes) &&
        std::memcmp(old_b_bytes, plain, sizeof(old_b_bytes)) == 0;
    auto old_b_stats = same_identity_b_started ? old_read_consumer.stats(identity)
        : dtv::userland::Result<dtv::userland::detail::StreamCounters>::failure(
            dtv::userland::Error::NOT_FOUND);
    const bool b_counters_ok = old_b_stats && old_b_stats.value().packets == 2U &&
        old_b_stats.value().bytes == sizeof(old_b_bytes);
    const bool old_b_stopped = same_identity_b_started
        ? static_cast<bool>(old_read_consumer.stop()) : false;
    std::fprintf(stderr, "OLD_READ wait=%d wake_held=%d A_stopped=%d B_started=%d B_bytes=%d stale_not_found=%d B_counters=%d B_stopped=%d\n",
                 old_wait_observed, old_read_paused_after_wake, old_a_stopped,
                 same_identity_b_started, b_bytes_ok, old_read_not_found,
                 b_counters_ok, old_b_stopped);
    assert(old_wait_observed && old_a_stopped && old_read_paused_after_wake &&
           same_identity_b_started && b_poll_ok && old_b_complete && b_bytes_ok &&
           old_read_not_found && b_counters_ok && old_b_stopped);
    auto old_b_final = old_read_consumer.final_snapshot(identity);
    assert(old_b_final && old_b_final.value().counters.packets == 2U &&
           old_b_final.value().counters.bytes == sizeof(old_b_bytes));
    assert(old_read_consumer.release_final(identity));

    Consumer join_failure;
    assert(join_failure.start(&session, identity, 8U));
    fail_join = true;
    assert(!join_failure.stop());
    assert(!join_failure.start(&session, identity, 8U));
    assert(join_failure.stop());
    final = join_failure.final_snapshot(identity);
    assert(final && final.value().terminal == static_cast<std::uint8_t>(
        dtv::userland::detail::StreamTerminal::stopped));
    assert(join_failure.release_final(identity));
    assert(held_sink.start(&session, identity, 8U));
    pix_test_usb_queue(wire, sizeof(wire));
    assert(held_sink.poll(1U));
    assert(held_sink.stop());
    std::memset(output, 0, sizeof(output));
    read = held_sink.read(identity,
                          dtv::userland::MutableByteView{output, sizeof(output)}, 250ms);
    assert(read && read.value().bytes == sizeof(plain));
    assert(std::memcmp(output, plain, sizeof(plain)) == 0);
    final = held_sink.final_snapshot(identity);
    assert(final && final.value().counters.packets == 2U &&
           final.value().counters.bytes == sizeof(plain));
    assert(held_sink.release_final(identity));

    fail_allocation = true;
    assert(!consumer.start(&session, identity, 8U));
    fail_fifo = true;
    assert(!consumer.start(&session, identity, 8U));
    fail_start = true;
    assert(!consumer.start(&session, identity, 8U));
    fail_worker = true;
    assert(!consumer.start(&session, identity, 8U));

    assert(consumer.start(&session, identity, 8U));
    pix_test_usb_queue(wire, sizeof(wire));
    assert(consumer.poll(1U));
    assert(consumer.stop());
    std::memset(output, 0, sizeof(output));
    read = consumer.read(identity,
                         dtv::userland::MutableByteView{output, sizeof(output)}, 250ms);
    assert(read && read.value().bytes == sizeof(plain));
    assert(std::memcmp(output, plain, sizeof(plain)) == 0);
    final = consumer.final_snapshot(identity);
    assert(final && final.value().counters.packets == 2U);
    assert(final.value().counters.bytes == sizeof(plain));
    assert(consumer.release_final(identity));

    {
        dt181_session error_session{};
        assert(dt181_session_open(&error_session, nullptr, 43, 0) == 0);
        Consumer stop_error;
        assert(stop_error.start(&error_session, identity, 8U));
        const unsigned cancel_before_error = pix_test_usb_cancellations();
        const unsigned frees_before_error = pix_test_usb_frees();
        pix_test_usb_fail_cancel();
        fail_device_stop = true;
        fail_join = true;
        assert(!stop_error.stop());
        const bool host_retained_after_join_failure =
            !stop_error.start(&error_session, identity, 8U);
        assert(host_retained_after_join_failure);
        assert(!stop_error.stop());
        assert(pix_test_usb_cancellations() > cancel_before_error);
        assert(pix_test_usb_frees() > frees_before_error);
        final = stop_error.final_snapshot(identity);
        assert(final && final.value().terminal == static_cast<std::uint8_t>(
            dtv::userland::detail::StreamTerminal::bridge_fatal));
        std::fprintf(stderr, "PIX cleanup_retry retained=%d stop=INTERNAL terminal=bridge_fatal quiesced=1\n",
                     host_retained_after_join_failure);
        assert(stop_error.release_final(identity));
        dt181_session_close(&error_session);
    }

    Consumer overflow_consumer;
    assert(overflow_consumer.start(&session, identity, 1U));
    pix_test_usb_queue(wire, sizeof(wire));
    assert(overflow_consumer.poll(1U));
    bool sink_failed = false;
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        auto counters = overflow_consumer.stats(identity);
        assert(counters);
        sink_failed = counters.value().queue_drops != 0U;
        if (sink_failed) break;
        std::this_thread::sleep_for(1ms);
    }
    assert(sink_failed);
    assert(!overflow_consumer.stop());
    final = overflow_consumer.final_snapshot(identity);
    assert(final && final.value().counters.queue_drops == 1U &&
           final.value().terminal == static_cast<std::uint8_t>(
               dtv::userland::detail::StreamTerminal::slow_consumer));
    assert(overflow_consumer.release_final(identity));
    dt181_session_close(&session);
}
