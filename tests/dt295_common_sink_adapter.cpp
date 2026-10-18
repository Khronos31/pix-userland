/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pix-capture-consumer.h"
#include "pix_capture_consumer_internal.h"

extern "C" {
#include "dt295_capture.h"
#include "dt295_capture_host.h"
#include "dt295_stream.h"
}

#include <cassert>
#include <chrono>
#include <cstdint>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <pthread.h>
#include <ctime>
#include <thread>

namespace pix::capture {
struct ConsumerTestAccess {
    static void prepare(Consumer& value)
    {
        if (!value.owner_)
            value.owner_ = std::make_shared<ConsumerOwner>();
    }
    static std::mutex& mutex(Consumer& value) { return value.owner_->session.mutex; }
    static pthread_cond_t* condition(Consumer& value)
    {
        return value.owner_->session.changed.native_handle();
    }
    static std::weak_ptr<ConsumerOwner> weak_owner(Consumer& value) { return value.owner_; }
    static std::shared_ptr<ConsumerOwner> hold_owner(Consumer& value) { return value.owner_; }
    static bool idle(Consumer& value)
    {
        if (!value.owner_ || value.owner_->host || value.owner_->run)
            return false;
        std::lock_guard<std::mutex> lock(value.owner_->session.mutex);
        return !value.owner_->session.attached;
    }
    static bool empty_or_idle(Consumer& value)
    {
        return !value.owner_ || idle(value);
    }
};
}

namespace {
pix::capture::Consumer consumer;
pix::capture::Consumer overflow_consumer;
std::unique_ptr<pix::capture::Consumer> lifetime_consumer;
std::optional<pix::capture::RetainedOwner> retained_owner;
std::weak_ptr<pix::capture::ConsumerOwner> lifetime_owner_weak;
std::unique_ptr<pix::capture::Consumer> destructor_consumer;
std::shared_ptr<pix::capture::ConsumerOwner> destructor_owner_hold;
std::weak_ptr<pix::capture::ConsumerOwner> destructor_owner_weak;
pix::capture::Consumer *active_consumer = &consumer;
pix::capture::Consumer::Attachment identity{};
pthread_mutex_t *held_mutex;
pthread_t main_thread;
std::atomic<bool> gate_armed{false};
std::atomic<bool> gate_entered{false};
pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t gate_changed = PTHREAD_COND_INITIALIZER;
bool gate_released;
std::atomic<bool> stop_done{false};
bool stop_result;
std::atomic<bool> fail_host_join{false};
std::atomic<unsigned> injected_join_attempts{0U};
std::atomic<unsigned> host_destroy_calls{0U};
std::atomic<bool> fail_native_stop{false};
std::atomic<unsigned> native_stop_attempts{0U};
std::atomic<unsigned> fail_native_stop_on_attempt{0U};
std::atomic<bool> fail_host_create{false};
std::atomic<bool> fail_host_allocation{false};
pthread_cond_t *old_read_condition;
std::atomic<bool> old_read_wait_entered{false};
std::atomic<bool> old_read_woke{false};
bool old_read_gate_released;
std::thread old_reader;
bool old_read_not_found;
std::uint64_t retained_packets;
std::uint64_t retained_bytes;
std::atomic<bool> fixture_finished{false};
std::atomic<bool> pid_filter_gate_armed{false};
std::atomic<bool> pid_filter_gate_entered{false};
pthread_mutex_t pid_filter_gate_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t pid_filter_gate_changed = PTHREAD_COND_INITIALIZER;
bool pid_filter_gate_released;
}

extern "C" int __real_pthread_mutex_lock(pthread_mutex_t *);
extern "C" int __real_pthread_join(pthread_t, void **);
extern "C" int __real_pthread_create(pthread_t *, const pthread_attr_t *,
                                      void *(*)(void *), void *);
extern "C" void *__real_calloc(std::size_t, std::size_t);
extern "C" int __real_pthread_cond_clockwait(pthread_cond_t *, pthread_mutex_t *,
                                               clockid_t, const timespec *);
extern "C" void __real_dt295_capture_host_destroy(dt295_capture_host *);
extern "C" void dt295_capture_test_set_stream_stop_hook(
    int (*)(dt295_stream *, unsigned));

extern "C" int dt295_capture_test_stream_stop(dt295_stream *stream, unsigned timeout_ms)
{
    const unsigned attempt = native_stop_attempts.fetch_add(1U) + 1U;
    const bool targeted_failure = fail_native_stop_on_attempt.load() == attempt;
    if (targeted_failure)
        fail_native_stop_on_attempt.store(0U);
    if (fail_native_stop.exchange(false) || targeted_failure) {
        std::fprintf(stderr, "DT295 native_stop injected_failure\n");
        return EIO;
    }
    return dt295_stream_stop(stream, timeout_ms);
}

extern "C" void dt295_common_test_fixture_finished(void)
{
    fixture_finished.store(true, std::memory_order_release);
}

extern "C" void dt295_common_test_pid_filter_read_gate(void)
{
    if (!pid_filter_gate_armed.load(std::memory_order_acquire))
        return;
    pid_filter_gate_entered.store(true, std::memory_order_release);
    pthread_mutex_lock(&pid_filter_gate_mutex);
    if (!pid_filter_gate_released) {
        timespec deadline{};
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 3;
        while (!pid_filter_gate_released) {
            const int rc = pthread_cond_timedwait(&pid_filter_gate_changed,
                                                  &pid_filter_gate_mutex, &deadline);
            if (rc != 0) { pid_filter_gate_released = true; break; }
        }
    }
    pthread_mutex_unlock(&pid_filter_gate_mutex);
}

extern "C" void __wrap_dt295_capture_host_destroy(dt295_capture_host *host)
{
    if (host)
        host_destroy_calls.fetch_add(1U);
    __real_dt295_capture_host_destroy(host);
}

extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
    if (mutex == held_mutex)
        std::fprintf(stderr, "DT295 sink_mutex_target armed=%d\n", gate_armed.load());
    if (mutex == held_mutex && gate_armed.load() &&
        !pthread_equal(pthread_self(), main_thread)) {
        gate_entered.store(true);
        pthread_mutex_lock(&gate_mutex);
        if (!gate_released) {
            timespec deadline{};
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += 3;
            while (!gate_released) {
                const int rc = pthread_cond_timedwait(&gate_changed, &gate_mutex, &deadline);
                if (rc != 0) { gate_released = true; break; }
            }
        }
        pthread_mutex_unlock(&gate_mutex);
    }
    return __real_pthread_mutex_lock(mutex);
}

extern "C" int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                                       void *(*entry)(void *), void *argument)
{
    if (fail_host_create.exchange(false))
        return EAGAIN;
    return __real_pthread_create(thread, attributes, entry, argument);
}

extern "C" void *__wrap_calloc(std::size_t count, std::size_t size)
{
    if (fail_host_allocation.exchange(false))
        return nullptr;
    return __real_calloc(count, size);
}

extern "C" int __wrap_pthread_join(pthread_t thread, void **result)
{
    if (fail_host_join.exchange(false)) {
        injected_join_attempts.fetch_add(1U);
        std::fprintf(stderr, "DT295 worker_join injected_failure\n");
        return EINVAL;
    }
    const int rc = __real_pthread_join(thread, result);
    std::fprintf(stderr, "DT295 worker_join result=%d\n", rc);
    return rc;
}

extern "C" int __wrap_pthread_cond_clockwait(pthread_cond_t *condition,
                                               pthread_mutex_t *mutex,
                                               clockid_t clock_id,
                                               const timespec *deadline)
{
    if (condition != old_read_condition)
        return __real_pthread_cond_clockwait(condition, mutex, clock_id, deadline);
    old_read_wait_entered.store(true);
    const int rc = __real_pthread_cond_clockwait(condition, mutex, clock_id, deadline);
    std::fprintf(stderr, "DT295 old_read condition_return=%d target=%d\n", rc,
                 condition == old_read_condition);
    if (rc == 0) {
        pthread_mutex_unlock(mutex);
        old_read_woke.store(true);
        pthread_mutex_lock(&gate_mutex);
        if (!old_read_gate_released) {
            timespec gate_deadline{};
            clock_gettime(CLOCK_REALTIME, &gate_deadline);
            gate_deadline.tv_sec += 3;
            while (!old_read_gate_released) {
                const int gate_rc = pthread_cond_timedwait(
                    &gate_changed, &gate_mutex, &gate_deadline);
                if (gate_rc != 0) { old_read_gate_released = true; break; }
            }
        }
        pthread_mutex_unlock(&gate_mutex);
        __real_pthread_mutex_lock(mutex);
    }
    return rc;
}

extern "C" int dt295_common_test_begin(const void *configuration, int hold_sink,
                                        std::size_t queue_packets)
{
    main_thread = pthread_self();
    identity = {};
    identity.owner_client_id = 11U;
    identity.lease_id = 12U;
    identity.attachment_id = 13U;
    identity.receiver = 0U;
    identity.token[0] = 14U;
    active_consumer = queue_packets == 1U ? &overflow_consumer : &consumer;
    pix::capture::ConsumerTestAccess::prepare(*active_consumer);
    held_mutex = pix::capture::ConsumerTestAccess::mutex(*active_consumer).native_handle();
    gate_released = !hold_sink;
    gate_entered.store(false);
    stop_done.store(false);
    gate_armed.store(hold_sink != 0);
    const bool started = static_cast<bool>(
        active_consumer->start(configuration, "/tmp", identity, queue_packets));
    std::fprintf(stderr, "DT295 test_begin started=%d held_mutex=%p\n", started,
                 static_cast<void *>(held_mutex));
    return started ? 0 : -1;
}

extern "C" int dt295_common_test_wait_packets(std::uint64_t packets)
{
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        auto counters = active_consumer->stats(identity);
        if (counters && counters.value().packets >= packets)
            return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return -1;
}

extern "C" int dt295_common_test_stop_held(const void *configuration)
{
    bool entered = false;
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        if (gate_entered.load()) { entered = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::thread stopper([&] {
        stop_result = static_cast<bool>(active_consumer->stop());
        stop_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const bool waited = !stop_done.load();
    const bool replacement_rejected = !active_consumer->start(configuration, "/tmp", identity, 32U);
    held_mutex = nullptr;
    gate_armed.store(false);
    pthread_mutex_lock(&gate_mutex);
    gate_released = true;
    pthread_cond_broadcast(&gate_changed);
    pthread_mutex_unlock(&gate_mutex);
    stopper.join();
    std::fprintf(stderr, "DT295 held_sink entered=%d stop_waited=%d replacement_rejected=%d stop_ok=%d\n",
                 entered, waited, replacement_rejected, stop_result);
    return entered && waited && replacement_rejected && stop_result ? 0 : -1;
}

extern "C" int dt295_common_test_stop(void)
{
    return active_consumer->stop() ? 0 : -1;
}

extern "C" int dt295_common_test_stop_with_join_retry(void)
{
    injected_join_attempts.store(0U);
    fail_host_join.store(true);
    const bool first_stop_ok = static_cast<bool>(active_consumer->stop());
    const bool replacement_rejected = !active_consumer->start(nullptr, "/tmp", identity, 1U);
    const bool second_stop_ok = static_cast<bool>(active_consumer->stop());
    auto final = active_consumer->final_snapshot(identity);
    const bool retained_slow_final = final &&
        final.value().terminal == static_cast<std::uint8_t>(
            dtv::userland::detail::StreamTerminal::slow_consumer) &&
        final.value().counters.queue_drops > 0U;
    std::fprintf(stderr,
                 "DT295 join_retry first_stop=%d replacement_rejected=%d second_cleanup=%d "
                 "retained_slow_final=%d attempts=%u\n",
                 first_stop_ok, replacement_rejected, second_stop_ok, retained_slow_final,
                 injected_join_attempts.load());
    return !first_stop_ok && replacement_rejected && !second_stop_ok && retained_slow_final &&
                   injected_join_attempts.load() == 1U
        ? 0
        : -1;
}

extern "C" int dt295_common_test_start_compensation(const void *configuration)
{
    active_consumer = &consumer;
    fail_host_allocation.store(true);
    const bool allocation_failed = !active_consumer->start(configuration, "/tmp", identity, 32U);
    fail_host_create.store(true);
    const bool create_failed = !active_consumer->start(configuration, "/tmp", identity, 32U);
    const bool retry_started = static_cast<bool>(active_consumer->start(configuration, "/tmp", identity, 32U));
    std::fprintf(stderr, "DT295 start_compensation allocation=%d worker_create=%d retry=%d\n",
                 allocation_failed, create_failed, retry_started);
    return allocation_failed && create_failed && retry_started ? 0 : -1;
}

extern "C" int dt295_common_test_null_mask_rejected(const void *configuration)
{
    auto invalid = *static_cast<const dt295_capture_config *>(configuration);
    invalid.filter_pids = 1;
    invalid.pid_mask = nullptr;
    const bool rejected = !consumer.start(&invalid, "/tmp", identity, 32U);
    std::fprintf(stderr, "DT295 case=null_pid_mask rejected=%d\n", rejected);
    return rejected ? 0 : -1;
}

extern "C" int dt295_common_test_native_stop_failure(const void *configuration)
{
    active_consumer = &consumer;
    native_stop_attempts.store(0U);
    fail_native_stop.store(true);
    dt295_capture_test_set_stream_stop_hook(dt295_capture_test_stream_stop);
    const bool started = static_cast<bool>(
        active_consumer->start(configuration, "/tmp", identity, 32U));
    const bool stop_failed = !active_consumer->stop();
    dt295_capture_test_set_stream_stop_hook(nullptr);
    auto final = active_consumer->final_snapshot(identity);
    const bool fatal_final = final &&
        final.value().terminal == static_cast<std::uint8_t>(
            dtv::userland::detail::StreamTerminal::bridge_fatal);
    const bool retried = native_stop_attempts.load() >= 2U;
    const bool released = static_cast<bool>(active_consumer->release_final(identity));
    std::fprintf(stderr,
                 "DT295 case=native_stop_first_error started=%d stop_failed=%d "
                 "retry_quiesced=%d bridge_fatal=%d attempts=%u\n",
                 started, stop_failed, retried, fatal_final, native_stop_attempts.load());
    return started && stop_failed && retried && fatal_final && released ? 0 : -1;
}

extern "C" int dt295_common_test_lifetime_begin(const void *configuration)
{
    retained_owner.reset();
    lifetime_consumer = std::make_unique<pix::capture::Consumer>();
    active_consumer = lifetime_consumer.get();
    main_thread = pthread_self();
    identity = {};
    identity.owner_client_id = 11U;
    identity.lease_id = 12U;
    identity.attachment_id = 13U;
    identity.receiver = 0U;
    identity.token[0] = 14U;
    pix::capture::ConsumerTestAccess::prepare(*active_consumer);
    held_mutex = pix::capture::ConsumerTestAccess::mutex(*active_consumer).native_handle();
    gate_released = false;
    gate_entered.store(false);
    gate_armed.store(true);
    stop_done.store(false);
    if (!active_consumer->start(configuration, "/tmp", identity, 32U)) {
        gate_armed.store(false);
        pthread_mutex_lock(&gate_mutex);
        gate_released = true;
        pthread_cond_broadcast(&gate_changed);
        pthread_mutex_unlock(&gate_mutex);
        active_consumer->stop();
        lifetime_consumer.reset();
        active_consumer = &consumer;
        return -1;
    }
    bool entered = false;
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        if (gate_entered.load()) { entered = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!entered) {
        gate_armed.store(false);
        pthread_mutex_lock(&gate_mutex);
        gate_released = true;
        pthread_cond_broadcast(&gate_changed);
        pthread_mutex_unlock(&gate_mutex);
        (void)active_consumer->stop();
        lifetime_consumer.reset();
        active_consumer = &consumer;
        return -1;
    }
    return 0;
}

extern "C" int dt295_common_test_lifetime_retain(const void *configuration,
                                                   int force_failure_cleanup)
{
    if (!lifetime_consumer || !gate_entered.load())
        return -1;
    const unsigned destroy_before = host_destroy_calls.load();
    fail_host_join.store(true);
    const bool first_stop_failed = !lifetime_consumer->stop();
    const auto replacement = lifetime_consumer->start(
        configuration, "/tmp", identity, 32U);
    const bool replacement_rejected = !replacement &&
        replacement.error() == dtv::userland::Error::BUSY;
    lifetime_owner_weak = pix::capture::ConsumerTestAccess::weak_owner(*lifetime_consumer);
    auto retained = lifetime_consumer->take_retained_owner();
    if (retained)
        retained_owner.emplace(std::move(retained.value()));
    fail_host_create.store(true);
    const auto take_after_identity = lifetime_consumer->start(
        configuration, "/tmp", identity, 32U);
    const bool take_after_identity_rejected = !take_after_identity &&
        take_after_identity.error() == dtv::userland::Error::BUSY;
    const bool host_start_not_reached = fail_host_create.load();
    fail_host_create.store(false);
    const bool checks_passed = first_stop_failed && replacement_rejected &&
        take_after_identity_rejected && host_start_not_reached && retained_owner &&
        !force_failure_cleanup;
    if (!checks_passed) {
        gate_armed.store(false);
        pthread_mutex_lock(&gate_mutex);
        gate_released = true;
        pthread_cond_broadcast(&gate_changed);
        pthread_mutex_unlock(&gate_mutex);
        bool owner_cleaned = !retained_owner;
        if (retained_owner) {
            for (unsigned attempt = 0U; attempt < 3U && !retained_owner->quiesced(); ++attempt)
                (void)retained_owner->stop();
            const bool quiesced = retained_owner->quiesced();
            const bool released = quiesced && static_cast<bool>(retained_owner->release());
            if (released) {
                retained_owner.reset();
                owner_cleaned = lifetime_owner_weak.expired();
            } else {
                owner_cleaned = false;
            }
        }
        if (lifetime_consumer) {
            bool consumer_cleaned =
                pix::capture::ConsumerTestAccess::empty_or_idle(*lifetime_consumer);
            for (unsigned attempt = 0U;
                 attempt < 3U && !consumer_cleaned;
                 ++attempt)
                (void)lifetime_consumer->stop();
            consumer_cleaned =
                pix::capture::ConsumerTestAccess::empty_or_idle(*lifetime_consumer);
            owner_cleaned = owner_cleaned && consumer_cleaned;
        }
        lifetime_consumer.reset();
        active_consumer = &consumer;
        held_mutex = nullptr;
        std::fprintf(stderr,
                     "DT295 take_identity_failure_cleanup stop_failure=%d before_busy=%d "
                     "after_busy=%d host_unreached=%d forced_failure=%d owner_cleaned=%d\n",
                     first_stop_failed, replacement_rejected, take_after_identity_rejected,
                     host_start_not_reached, force_failure_cleanup, owner_cleaned);
        return owner_cleaned ? -1 : -2;
    }
    lifetime_consumer.reset();
    active_consumer = nullptr;
    const bool retained_context_alive = !lifetime_owner_weak.expired() &&
                                        !retained_owner->quiesced();
    const bool no_early_host_free = host_destroy_calls.load() == destroy_before;
    std::fprintf(stderr,
                 "DT295 case=destructor_retention wrapper_destroyed=1 owner_alive=%d "
                 "host_not_freed=%d identity_rejected=%d take_after_identity_busy=%d "
                 "host_start_unreached=%d\n",
                 retained_context_alive, no_early_host_free, replacement_rejected,
                 take_after_identity_rejected, host_start_not_reached);
    if (!retained_context_alive || !no_early_host_free) {
        gate_armed.store(false);
        pthread_mutex_lock(&gate_mutex);
        gate_released = true;
        pthread_cond_broadcast(&gate_changed);
        pthread_mutex_unlock(&gate_mutex);
        if (retained_owner) {
            (void)retained_owner->stop();
            if (retained_owner->quiesced())
                (void)retained_owner->release();
        }
        retained_owner.reset();
        active_consumer = &consumer;
        held_mutex = nullptr;
        return -1;
    }
    return 0;
}

extern "C" int dt295_common_test_lifetime_release(std::uint64_t expected_bytes)
{
    gate_armed.store(false);
    pthread_mutex_lock(&gate_mutex);
    gate_released = true;
    pthread_cond_broadcast(&gate_changed);
    pthread_mutex_unlock(&gate_mutex);
    const unsigned destroy_before = host_destroy_calls.load();
    const auto stop = retained_owner ? retained_owner->stop()
                                     : dtv::userland::Result<void>::failure(
                                           dtv::userland::Error::NOT_FOUND);
    bool final_ok = false;
    unsigned final_terminal = 255U;
    std::uint64_t final_bytes = 0U;
    std::uint64_t final_packets = 0U;
    if (retained_owner && retained_owner->quiesced()) {
        auto final = retained_owner->final_snapshot(identity);
        if (final) {
            final_terminal = final.value().terminal;
            final_bytes = final.value().counters.bytes;
            final_packets = final.value().counters.packets;
            final_ok = final_terminal == static_cast<std::uint8_t>(
                    dtv::userland::detail::StreamTerminal::bridge_fatal) &&
                final_packets == expected_bytes / 188U && final_bytes == expected_bytes;
        }
    }
    const bool host_freed_once = host_destroy_calls.load() == destroy_before + 1U;
    const bool released = retained_owner && retained_owner->release();
    const bool owner_freed = lifetime_owner_weak.expired();
    retained_owner.reset();
    active_consumer = &consumer;
    held_mutex = nullptr;
    std::fprintf(stderr,
                 "DT295 case=destructor_retention_release join_retry=%d bridge_fatal=%d "
                 "host_destroy_once=%d owner_released=%d terminal=%u bytes=%llu packets=%llu\n",
                 !stop, final_ok, host_freed_once, owner_freed, final_terminal,
                 static_cast<unsigned long long>(final_bytes),
                 static_cast<unsigned long long>(final_packets));
    return !stop && final_ok && host_freed_once && released && owner_freed ? 0 : -1;
}

extern "C" int dt295_common_test_host_case(const void *configuration, int expected_started,
                                             int expected_success,
                                             unsigned expected_terminal,
                                             std::uint64_t expected_bytes,
                                             unsigned expected_native_stops,
                                             std::uint8_t *output, std::size_t capacity,
                                             std::size_t *output_bytes)
{
    active_consumer = &consumer;
    fixture_finished.store(false, std::memory_order_release);
    native_stop_attempts.store(0U);
    fail_native_stop_on_attempt.store(expected_native_stops);
    dt295_capture_test_set_stream_stop_hook(dt295_capture_test_stream_stop);
    const bool started = static_cast<bool>(
        consumer.start(configuration, "/tmp", identity, 32U));
    if (!started) {
        dt295_capture_test_set_stream_stop_hook(nullptr);
        if (!expected_started) {
            if (!pix::capture::ConsumerTestAccess::idle(consumer)) {
                (void)consumer.stop();
                if (!pix::capture::ConsumerTestAccess::idle(consumer))
                    (void)consumer.stop();
            }
            const bool idle = pix::capture::ConsumerTestAccess::idle(consumer);
            std::fprintf(stderr, "DT295 host_case start_failed=1 cleanup_idle=%d\n", idle);
            return idle ? 0 : -1;
        }
        return -1;
    }
    bool completed = false;
    for (unsigned attempt = 0U; attempt < 3000U; ++attempt) {
        if (fixture_finished.load(std::memory_order_acquire)) {
            completed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto stop = consumer.stop();
    dt295_capture_test_set_stream_stop_hook(nullptr);
    auto final = consumer.final_snapshot(identity);
    bool final_matches = false;
    std::size_t bytes = 0U;
    if (final) {
        final_matches = final.value().terminal == expected_terminal &&
                        final.value().counters.bytes == expected_bytes &&
                        final.value().counters.packets == expected_bytes / 188U;
        if (expected_bytes && output && capacity) {
            auto read = consumer.read(identity,
                dtv::userland::MutableByteView{output, capacity},
                std::chrono::milliseconds(100));
            if (read)
                bytes = read.value().bytes;
            else
                final_matches = false;
        }
    }
    const bool success_matches = static_cast<bool>(stop) == (expected_success != 0);
    const bool native_matches = expected_native_stops == 0U ||
                                native_stop_attempts.load() == expected_native_stops + 1U;
    const bool released = static_cast<bool>(consumer.release_final(identity));
    const bool idle = pix::capture::ConsumerTestAccess::idle(consumer);
    if (output_bytes)
        *output_bytes = bytes;
    std::fprintf(stderr,
                 "DT295 host_case started=1 completed=%d stop_success=%d terminal=%u "
                 "bytes=%llu packets=%llu expected_bytes=%llu native_stops=%u "
                 "final_match=%d idle=%d\n",
                 completed, static_cast<bool>(stop), final ? final.value().terminal : 255U,
                 static_cast<unsigned long long>(final ? final.value().counters.bytes : 0U),
                 static_cast<unsigned long long>(final ? final.value().counters.packets : 0U),
                 static_cast<unsigned long long>(expected_bytes),
                 native_stop_attempts.load(), final_matches, idle);
    return completed && success_matches && final_matches && native_matches && released && idle
        ? 0
        : -1;
}

extern "C" void dt295_common_test_arm_native_stop_failure(unsigned attempt)
{
    native_stop_attempts.store(0U);
    fail_native_stop.store(false);
    fail_native_stop_on_attempt.store(attempt);
    dt295_capture_test_set_stream_stop_hook(dt295_capture_test_stream_stop);
}

extern "C" unsigned dt295_common_test_disarm_native_stop_failure(void)
{
    dt295_capture_test_set_stream_stop_hook(nullptr);
    fail_native_stop_on_attempt.store(0U);
    return native_stop_attempts.load();
}

extern "C" int dt295_common_test_overflow_native_stop_secondary(
    const void *configuration)
{
    active_consumer = &overflow_consumer;
    fixture_finished.store(false, std::memory_order_release);
    native_stop_attempts.store(0U);
    fail_native_stop_on_attempt.store(2U);
    dt295_capture_test_set_stream_stop_hook(dt295_capture_test_stream_stop);
    const unsigned destroy_before = host_destroy_calls.load();
    const bool started = static_cast<bool>(
        overflow_consumer.start(configuration, "/tmp", identity, 1U));
    bool completed = false;
    if (started) {
        for (unsigned attempt = 0U; attempt < 3000U; ++attempt) {
            if (fixture_finished.load(std::memory_order_acquire)) {
                completed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const auto stop = overflow_consumer.stop();
    dt295_capture_test_set_stream_stop_hook(nullptr);
    auto final = overflow_consumer.final_snapshot(identity);
    std::uint8_t packet[188U]{};
    auto read = overflow_consumer.read(identity,
        dtv::userland::MutableByteView{packet, sizeof(packet)},
        std::chrono::milliseconds(100));
    const bool first_packet_kept = read && read.value().bytes == sizeof(packet) &&
                                   packet[0] == 0x47U;
    const bool terminal_kept = final &&
        final.value().terminal == static_cast<std::uint8_t>(
            dtv::userland::detail::StreamTerminal::slow_consumer) &&
        final.value().counters.queue_drops > 0U;
    const bool native_retried = native_stop_attempts.load() == 3U;
    const bool host_destroyed_once = host_destroy_calls.load() == destroy_before + 1U;
    const bool released = static_cast<bool>(overflow_consumer.release_final(identity));
    const bool idle = pix::capture::ConsumerTestAccess::idle(overflow_consumer);
    std::fprintf(stderr,
                 "DT295 overflow_native_secondary started=%d completed=%d "
                 "slow_consumer_kept=%d terminal=%u bytes=%llu packets=%llu drops=%llu "
                 "first_packet_kept=%d native_attempts=%u host_destroy_once=%d idle=%d\n",
                 started, completed, terminal_kept, final ? final.value().terminal : 255U,
                 static_cast<unsigned long long>(final ? final.value().counters.bytes : 0U),
                 static_cast<unsigned long long>(final ? final.value().counters.packets : 0U),
                 static_cast<unsigned long long>(final ? final.value().counters.queue_drops : 0U),
                 first_packet_kept,
                 native_stop_attempts.load(),
                 host_destroyed_once, idle);
    active_consumer = &consumer;
    return started && completed && !stop && terminal_kept && first_packet_kept && native_retried &&
                   host_destroyed_once && released && idle
        ? 0
        : -1;
}

extern "C" int dt295_common_test_pid_mask_prepare(const void *configuration)
{
    active_consumer = &consumer;
    fixture_finished.store(false, std::memory_order_release);
    pid_filter_gate_entered.store(false, std::memory_order_release);
    pid_filter_gate_armed.store(true, std::memory_order_release);
    pthread_mutex_lock(&pid_filter_gate_mutex);
    pid_filter_gate_released = false;
    pthread_mutex_unlock(&pid_filter_gate_mutex);
    auto local = *static_cast<const dt295_capture_config *>(configuration);
    unsigned char caller_mask[8192];
    std::memset(caller_mask, 0, sizeof(caller_mask));
    caller_mask[0x111] = 1U;
    local.filter_pids = 1;
    local.pid_mask = caller_mask;
    const bool started = static_cast<bool>(
        consumer.start(&local, "/tmp", identity, 32U));
    bool entered = false;
    if (started) {
        for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
            if (pid_filter_gate_entered.load(std::memory_order_acquire)) {
                entered = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (!started || !entered) {
        pid_filter_gate_armed.store(false, std::memory_order_release);
        pthread_mutex_lock(&pid_filter_gate_mutex);
        pid_filter_gate_released = true;
        pthread_cond_broadcast(&pid_filter_gate_changed);
        pthread_mutex_unlock(&pid_filter_gate_mutex);
        if (!pix::capture::ConsumerTestAccess::idle(consumer)) {
            (void)consumer.stop();
            if (!pix::capture::ConsumerTestAccess::idle(consumer))
                (void)consumer.stop();
        }
        if (pix::capture::ConsumerTestAccess::idle(consumer))
            (void)consumer.release_final(identity);
        return -1;
    }
    std::memset(caller_mask, 0, sizeof(caller_mask));
    std::fprintf(stderr,
                 "DT295 pid_mask gate_entered=1 caller_changed=1 before_scope_exit=1\n");
    return 0;
}

extern "C" int dt295_common_test_pid_mask_release(std::uint8_t *output,
                                                    std::size_t capacity)
{
    pid_filter_gate_armed.store(false, std::memory_order_release);
    pthread_mutex_lock(&pid_filter_gate_mutex);
    pid_filter_gate_released = true;
    pthread_cond_broadcast(&pid_filter_gate_changed);
    pthread_mutex_unlock(&pid_filter_gate_mutex);
    bool completed = false;
    for (unsigned attempt = 0U; attempt < 3000U; ++attempt) {
        if (fixture_finished.load(std::memory_order_acquire)) {
            completed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto stop = consumer.stop();
    auto final = consumer.final_snapshot(identity);
    std::size_t bytes = 0U;
    bool policy_kept = false;
    if (final && output && capacity) {
        auto read = consumer.read(identity,
            dtv::userland::MutableByteView{output, capacity},
            std::chrono::milliseconds(100));
        if (read) {
            bytes = read.value().bytes;
            if (bytes == 188U) {
                const unsigned pid = ((static_cast<unsigned>(output[1]) & 31U) << 8) |
                                     output[2];
                policy_kept = output[0] == 0x47U && pid == 0x111U;
            }
        }
    }
    const bool final_matches = final &&
        final.value().terminal == static_cast<std::uint8_t>(
            dtv::userland::detail::StreamTerminal::stopped) &&
        final.value().counters.packets == 1U && final.value().counters.bytes == 188U;
    const bool released = static_cast<bool>(consumer.release_final(identity));
    const bool idle = pix::capture::ConsumerTestAccess::idle(consumer);
    std::fprintf(stderr,
                 "DT295 pid_mask completed=%d stop_success=%d caller_scope_ended=1 "
                 "bytes=%zu pid_allowed=%d final_match=%d idle=%d\n",
                 completed, static_cast<bool>(stop), bytes, policy_kept, final_matches, idle);
    return completed && stop && policy_kept && final_matches && released && idle ? 0 : -1;
}

extern "C" int dt295_common_test_destructor_begin(const void *configuration)
{
    destructor_consumer = std::make_unique<pix::capture::Consumer>();
    active_consumer = destructor_consumer.get();
    main_thread = pthread_self();
    identity = {};
    identity.owner_client_id = 11U;
    identity.lease_id = 12U;
    identity.attachment_id = 13U;
    identity.receiver = 0U;
    identity.token[0] = 14U;
    pix::capture::ConsumerTestAccess::prepare(*active_consumer);
    held_mutex = pix::capture::ConsumerTestAccess::mutex(*active_consumer).native_handle();
    fixture_finished.store(false, std::memory_order_release);
    gate_entered.store(false, std::memory_order_release);
    gate_armed.store(true, std::memory_order_release);
    injected_join_attempts.store(0U);
    fail_host_join.store(false);
    pthread_mutex_lock(&gate_mutex);
    gate_released = false;
    pthread_mutex_unlock(&gate_mutex);
    const unsigned destroy_before = host_destroy_calls.load();
    if (!active_consumer->start(configuration, "/tmp", identity, 32U)) {
        gate_armed.store(false, std::memory_order_release);
        pthread_mutex_lock(&gate_mutex);
        gate_released = true;
        pthread_cond_broadcast(&gate_changed);
        pthread_mutex_unlock(&gate_mutex);
        if (!pix::capture::ConsumerTestAccess::idle(*active_consumer))
            (void)active_consumer->stop();
        destructor_consumer.reset();
        active_consumer = &consumer;
        held_mutex = nullptr;
        return -1;
    }
    bool entered = false;
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        if (gate_entered.load(std::memory_order_acquire)) {
            entered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    destructor_owner_weak = pix::capture::ConsumerTestAccess::weak_owner(*active_consumer);
    destructor_owner_hold = pix::capture::ConsumerTestAccess::hold_owner(*active_consumer);
    fail_host_join.store(true);
    destructor_consumer.reset();
    active_consumer = nullptr;
    const bool callback_held = entered && !gate_released;
    const bool owner_alive = !destructor_owner_weak.expired() && destructor_owner_hold;
    const bool host_retained = host_destroy_calls.load() == destroy_before;
    std::fprintf(stderr,
                 "DT295 destructor_unretained wrapper_destroyed=1 callback_held=%d "
                 "owner_alive=%d host_retained=%d join_failure=%d\n",
                 callback_held, owner_alive, host_retained, injected_join_attempts.load() == 1U);
    return callback_held && owner_alive && host_retained &&
                   injected_join_attempts.load() == 1U
        ? 0
        : -1;
}

extern "C" int dt295_common_test_destructor_finish(std::uint64_t expected_bytes)
{
    gate_armed.store(false, std::memory_order_release);
    pthread_mutex_lock(&gate_mutex);
    gate_released = true;
    pthread_cond_broadcast(&gate_changed);
    pthread_mutex_unlock(&gate_mutex);
    bool completed = false;
    for (unsigned attempt = 0U; attempt < 3000U; ++attempt) {
        if (fixture_finished.load(std::memory_order_acquire)) {
            completed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const unsigned destroy_before = host_destroy_calls.load();
    const auto stop = destructor_owner_hold
        ? destructor_owner_hold->stop()
        : dtv::userland::Result<void>::failure(dtv::userland::Error::NOT_FOUND);
    bool final_matches = false;
    std::uint64_t bytes = 0U;
    std::uint64_t packets = 0U;
    unsigned terminal = 255U;
    if (destructor_owner_hold && destructor_owner_hold->quiesced()) {
        auto final = destructor_owner_hold->session.final_snapshot_query(identity);
        if (final) {
            terminal = final.value().terminal;
            bytes = final.value().counters.bytes;
            packets = final.value().counters.packets;
            final_matches = terminal == static_cast<std::uint8_t>(
                    dtv::userland::detail::StreamTerminal::bridge_fatal) &&
                bytes == expected_bytes && packets == expected_bytes / 188U && bytes != 0U;
        }
    }
    const bool host_destroyed_once = host_destroy_calls.load() == destroy_before + 1U;
    const bool released = destructor_owner_hold &&
                          destructor_owner_hold->session.release_final(identity);
    destructor_owner_hold.reset();
    const bool owner_released = destructor_owner_weak.expired();
    active_consumer = &consumer;
    held_mutex = nullptr;
    std::fprintf(stderr,
                 "DT295 destructor_unretained_finish callback_after_destroy=%d "
                 "join_retry=%d terminal=%u bytes=%llu packets=%llu host_destroy_once=%d "
                 "owner_released=%d final_released=%d\n",
                 completed && bytes != 0U, !stop, terminal,
                 static_cast<unsigned long long>(bytes),
                 static_cast<unsigned long long>(packets), host_destroyed_once, owner_released,
                 static_cast<bool>(released));
    return completed && !stop && final_matches && host_destroyed_once && released &&
                   owner_released
        ? 0
        : -1;
}

extern "C" int dt295_common_test_drain_active(std::uint8_t *output,
                                                std::size_t capacity)
{
    auto result = active_consumer->read(identity,
                                dtv::userland::MutableByteView{output, capacity},
                                std::chrono::milliseconds(100));
    return result ? static_cast<int>(result.value().bytes) : -1;
}

extern "C" int dt295_common_test_old_read_start(void)
{
    old_read_condition = pix::capture::ConsumerTestAccess::condition(*active_consumer);
    old_read_wait_entered.store(false);
    old_read_woke.store(false);
    old_read_gate_released = false;
    old_read_not_found = false;
    old_reader = std::thread([&] {
        std::uint8_t output[188U]{};
        auto result = active_consumer->read(identity,
                                    dtv::userland::MutableByteView{output, sizeof(output)},
                                    std::chrono::seconds(5));
        old_read_not_found = !result &&
            result.error() == dtv::userland::Error::NOT_FOUND;
    });
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        if (old_read_wait_entered.load())
            return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    (void)active_consumer->stop();
    pthread_mutex_lock(&gate_mutex);
    old_read_gate_released = true;
    pthread_cond_broadcast(&gate_changed);
    pthread_mutex_unlock(&gate_mutex);
    if (old_reader.joinable())
        old_reader.join();
    return -1;
}

extern "C" int dt295_common_test_old_read_woke(void)
{
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        if (old_read_woke.load())
            return 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
}

extern "C" int dt295_common_test_old_read_resume(void)
{
    pthread_mutex_lock(&gate_mutex);
    old_read_gate_released = true;
    pthread_cond_broadcast(&gate_changed);
    pthread_mutex_unlock(&gate_mutex);
    if (old_reader.joinable())
        old_reader.join();
    auto final = active_consumer->final_snapshot(identity);
    if (!final)
        return -1;
    const bool unchanged = final.value().counters.packets == retained_packets &&
                           final.value().counters.bytes == retained_bytes;
    std::fprintf(stderr, "DT295 old_read wait=%d woke=%d not_found=%d b_unchanged=%d\n",
                 old_read_wait_entered.load(), old_read_woke.load(), old_read_not_found,
                 unchanged);
    return old_read_not_found && unchanged ? 0 : -1;
}

extern "C" int dt295_common_test_save_final(void)
{
    auto final = active_consumer->final_snapshot(identity);
    if (!final)
        return -1;
    retained_packets = final.value().counters.packets;
    retained_bytes = final.value().counters.bytes;
    return 0;
}

extern "C" int dt295_common_test_release_final(void)
{
    return active_consumer->release_final(identity) ? 0 : -1;
}

extern "C" int dt295_common_test_finish_slow(std::uint8_t *output,
                                                std::size_t capacity)
{
    auto final = active_consumer->final_snapshot(identity);
    if (!final || final.value().terminal != static_cast<std::uint8_t>(
                      dtv::userland::detail::StreamTerminal::slow_consumer) ||
        final.value().counters.queue_drops == 0U)
        return 0U;
    auto read = active_consumer->read(identity, dtv::userland::MutableByteView{output, capacity},
                              std::chrono::milliseconds(100));
    if (!read || read.value().bytes != 188U)
        return 0U;
    const std::size_t bytes = read.value().bytes;
    assert(active_consumer->release_final(identity));
    return bytes;
}

extern "C" std::size_t dt295_common_test_finish(std::uint8_t *output,
                                                  std::size_t capacity)
{
    auto final = active_consumer->final_snapshot(identity);
    if (!final)
        return 0U;
    auto result = active_consumer->read(identity,
                                dtv::userland::MutableByteView{output, capacity},
                                std::chrono::milliseconds(100));
    if (!result)
        return 0U;
    assert(final && final.value().counters.packets * 188U == result.value().bytes);
    assert(final.value().counters.bytes == result.value().bytes);
    assert(active_consumer->release_final(identity));
    return result.value().bytes;
}
