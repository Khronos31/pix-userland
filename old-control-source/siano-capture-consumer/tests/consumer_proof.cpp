/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "siano-capture-consumer.h"
extern "C" {
#include "output-writer.h"
#include "siano-device.h"
}

extern "C" {
#include "protocol.h"
}

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>
#include <atomic>
#include <ctime>
#include <new>
#include <cstdlib>
#include <pthread.h>
#include <cstdio>

namespace siano::capture {
struct ConsumerTestAccess {
    static void sink_gate(Consumer& consumer, void (*gate)(void*) noexcept, void* context)
    {
        consumer.run_->test_sink_gate = gate;
        consumer.run_->test_sink_context = context;
    }
    static pthread_mutex_t* mutex(Consumer& consumer)
    {
        return consumer.session_.mutex.native_handle();
    }
    static pthread_cond_t* condition(Consumer& consumer)
    {
        return consumer.session_.changed.native_handle();
    }
};
}

extern "C" int test_libusb_deliver(struct libusb_transfer*, const std::uint8_t*,
                                    std::size_t);
extern "C" int test_libusb_pending(struct libusb_transfer*);
extern "C" unsigned test_libusb_control_requests(void);
extern "C" std::uint16_t test_libusb_last_control_type(void);
static void wait_transfer_pending(struct libusb_transfer*);
static bool fail_next_join;
static bool fail_next_create;
static bool fail_next_calloc;
static int fail_nothrow_after = -1;
static pthread_cond_t* capture_wait_condition;
static std::atomic<bool> capture_wait_entered{false};
static pthread_cond_t* old_read_condition_target;
static pthread_t proof_main_thread;
static std::atomic<bool> sink_gate_entered{false};
static std::atomic<bool> old_read_wait_entered{false};
static std::atomic<bool> old_read_woke{false};
static pthread_mutex_t proof_gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t proof_gate_cond = PTHREAD_COND_INITIALIZER;
static bool sink_gate_released;
static bool old_read_gate_released;
extern "C" int __real_pthread_join(pthread_t, void**);
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*,
                                      void* (*)(void*), void*);
extern "C" void* __real_calloc(std::size_t, std::size_t);
extern "C" void* __wrap_calloc(std::size_t count, std::size_t size)
{
    if (fail_next_calloc) { fail_next_calloc = false; return nullptr; }
    return __real_calloc(count, size);
}
extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attributes,
                                       void* (*entry)(void*), void* argument)
{
    if (fail_next_create) { fail_next_create = false; return EAGAIN; }
    return __real_pthread_create(thread, attributes, entry, argument);
}
extern "C" int __real_pthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*,
                                               const struct timespec*);
extern "C" int __real_pthread_cond_clockwait(pthread_cond_t*, pthread_mutex_t*,
                                               clockid_t, const struct timespec*);
extern "C" int __wrap_pthread_cond_timedwait(pthread_cond_t* condition,
                                                pthread_mutex_t* mutex,
                                                const struct timespec* deadline)
{
    if (condition == capture_wait_condition) capture_wait_entered.store(true);
    return __real_pthread_cond_timedwait(condition, mutex, deadline);
}
static void hold_test_sink(void*) noexcept
{
    sink_gate_entered.store(true);
    pthread_mutex_lock(&proof_gate_mutex);
    if (!sink_gate_released) {
        timespec deadline{};
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 3;
        while (!sink_gate_released) {
            const int rc = pthread_cond_timedwait(
                &proof_gate_cond, &proof_gate_mutex, &deadline);
            if (rc != 0) { sink_gate_released = true; break; }
        }
    }
    pthread_mutex_unlock(&proof_gate_mutex);
}
extern "C" int __wrap_pthread_cond_clockwait(pthread_cond_t* condition,
                                                pthread_mutex_t* mutex,
                                                clockid_t clock_id,
                                                const struct timespec* deadline)
{
    if (condition != old_read_condition_target)
        return __real_pthread_cond_clockwait(condition, mutex, clock_id, deadline);
    old_read_wait_entered.store(true);
    const int rc = __real_pthread_cond_clockwait(condition, mutex, clock_id, deadline);
    if (rc == 0) {
        pthread_mutex_unlock(mutex);
        old_read_woke.store(true);
        pthread_mutex_lock(&proof_gate_mutex);
        if (!old_read_gate_released) {
            timespec gate_deadline{};
            clock_gettime(CLOCK_REALTIME, &gate_deadline);
            gate_deadline.tv_sec += 3;
            while (!old_read_gate_released) {
                const int gate_rc = pthread_cond_timedwait(
                    &proof_gate_cond, &proof_gate_mutex, &gate_deadline);
                if (gate_rc != 0) { old_read_gate_released = true; break; }
            }
        }
        pthread_mutex_unlock(&proof_gate_mutex);
        pthread_mutex_lock(mutex);
    }
    return rc;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    if (fail_nothrow_after == 0) { fail_nothrow_after = -1; return nullptr; }
    if (fail_nothrow_after > 0) --fail_nothrow_after;
    try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (fail_nothrow_after == 0) { fail_nothrow_after = -1; return nullptr; }
    if (fail_nothrow_after > 0) --fail_nothrow_after;
    try { return ::operator new[](size); } catch (...) { return nullptr; }
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }
extern "C" int __wrap_pthread_join(pthread_t thread, void** result)
{
    if (fail_next_join) { fail_next_join = false; return EINVAL; }
    return __real_pthread_join(thread, result);
}

static void enqueue_framed_bda(siano_device& device,
                               const std::vector<std::uint8_t>& payload,
                               struct libusb_transfer* transfer)
{
    std::vector<std::uint8_t> message(SMS_HEADER_SIZE + payload.size());
    sms_pack_header(message.data(), MSG_SMS_DVBT_BDA_DATA, 0U, 0U,
                    static_cast<std::uint16_t>(message.size()), 0U);
    std::memcpy(message.data() + SMS_HEADER_SIZE, payload.data(), payload.size());
    sms_frame frame{};
    assert(sms_frame_message(message.data(), message.size(), 0U, &frame) == 0);
    assert(frame.type == MSG_SMS_DVBT_BDA_DATA);
    assert(test_libusb_deliver(transfer, message.data(), message.size()) == 0);
}

static void deliver_version_response(siano_device& device,
                                     struct libusb_transfer* transfer,
                                     unsigned expected_count)
{
    std::vector<std::uint8_t> message(SMS_HEADER_SIZE + 12U);
    sms_pack_header(message.data(), MSG_SMS_GET_VERSION_EX_RES, 0U, 0U,
                    static_cast<std::uint16_t>(message.size()), 0U);
    assert(test_libusb_deliver(transfer, message.data(), message.size()) == 0);
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        pthread_mutex_lock(&device.response_mutex);
        const unsigned count = device.response_count[MSG_SMS_GET_VERSION_EX_RES];
        pthread_mutex_unlock(&device.response_mutex);
        if (count >= expected_count && test_libusb_pending(transfer)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false);
}

static void wait_transfer_pending(struct libusb_transfer* transfer)
{
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        if (test_libusb_pending(transfer)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false);
}

static bool wait_transfer_pending_bounded(struct libusb_transfer* transfer)
{
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        if (test_libusb_pending(transfer)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

static bool enqueue_framed_bda_bounded(const std::vector<std::uint8_t>& payload,
                                       struct libusb_transfer* transfer)
{
    std::vector<std::uint8_t> message(SMS_HEADER_SIZE + payload.size());
    sms_pack_header(message.data(), MSG_SMS_DVBT_BDA_DATA, 0U, 0U,
                    static_cast<std::uint16_t>(message.size()), 0U);
    std::memcpy(message.data() + SMS_HEADER_SIZE, payload.data(), payload.size());
    sms_frame frame{};
    if (sms_frame_message(message.data(), message.size(), 0U, &frame) != 0 ||
        frame.type != MSG_SMS_DVBT_BDA_DATA)
        return false;
    return test_libusb_deliver(transfer, message.data(), message.size()) == 0;
}

int main()
{
    using namespace std::chrono_literals;
    using namespace siano::capture;
    proof_main_thread = pthread_self();

    siano_device device{};
    assert(init_device_state_capture(&device, nullptr, false, false) == 0);
    capture_wait_condition = &device.ts.available;

    Consumer consumer;
    Consumer::Attachment identity{};
    identity.owner_client_id = 21U;
    identity.lease_id = 22U;
    identity.attachment_id = 23U;
    identity.receiver = 2U;
    identity.token[0] = 24U;
    Consumer allocation_failure;
    fail_nothrow_after = 1;
    assert(!allocation_failure.start(&device, identity, 8U));
    fail_nothrow_after = 2;
    assert(!allocation_failure.start(&device, identity, 8U));
    fail_next_create = true;
    assert(!allocation_failure.start(&device, identity, 8U));
    fail_next_calloc = true;
    assert(!allocation_failure.start(&device, identity, 8U));
    assert(allocation_failure.start(&device, identity, 8U));
    assert(allocation_failure.stop());
    assert(consumer.start(&device, identity, 8U));
    assert(start_streaming(&device) == 0);

    std::vector<std::uint8_t> input(4U * SIANO_OUTPUT_PACKET);
    for (std::size_t packet = 0U; packet < 4U; ++packet) {
        auto* p = input.data() + packet * SIANO_OUTPUT_PACKET;
        std::memset(p, static_cast<int>(0x31U + packet), SIANO_OUTPUT_PACKET);
        p[0] = 0x47U;
        p[1] = 0x01U;
        p[2] = static_cast<std::uint8_t>(packet);
        p[3] = 0x10U;
    }
    std::vector<std::uint8_t> first(input.begin(), input.begin() + 263);
    std::vector<std::uint8_t> second(input.begin() + 263, input.end());
    enqueue_framed_bda(device, first, device.transfers[0]);
    wait_transfer_pending(device.transfers[1]);
    enqueue_framed_bda(device, second, device.transfers[1]);
    bool four_packets = false;
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        auto counters = consumer.stats(identity);
        assert(counters);
        if (counters.value().packets == 4U) { four_packets = true; break; }
        std::this_thread::sleep_for(1ms);
    }
    assert(four_packets);
    std::uint8_t output[4U * SIANO_OUTPUT_PACKET]{};
    auto read = consumer.read(identity,
                              dtv::userland::MutableByteView{output, sizeof(output)}, 1500ms);
    assert(read && read.value().bytes == sizeof(output));
    assert(std::memcmp(output, input.data(), sizeof(output)) == 0);
    assert(consumer.stop());
    const unsigned controls_before = test_libusb_control_requests();
    const std::uint16_t pid = 0U;
    assert(configure_pid_filters(&device, &pid, 1U) == 0);
    assert(test_libusb_control_requests() == controls_before + 1U);
    assert(test_libusb_last_control_type() == MSG_SMS_ADD_PID_FILTER_REQ);
    pthread_mutex_lock(&device.response_mutex);
    assert(device.response_count[MSG_SMS_ADD_PID_FILTER_RES] == 0U);
    pthread_mutex_unlock(&device.response_mutex);
    auto final = consumer.final_snapshot(identity);
    assert(final && final.value().counters.packets == 4U);
    assert(final.value().counters.bytes == sizeof(output));
    assert(consumer.release_final(identity));
    assert(consumer.start(&device, identity, 8U));
    bool pop_wait_observed = false;
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        if (capture_wait_entered.load()) {
            pop_wait_observed = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    std::fprintf(stderr, "S_PRODUCER_EMPTY_QUEUE_WAIT cond=device.ts.available reached=%d\n",
                 pop_wait_observed);
    assert(pop_wait_observed);
    const auto stop_started = std::chrono::steady_clock::now();
    assert(consumer.stop());
    assert(std::chrono::steady_clock::now() - stop_started < 1s);
    assert(device.event_thread_started);
    auto stale_input = input;
    stale_input[10] ^= 0x5aU;
    enqueue_framed_bda(device, input, device.transfers[1]);
    wait_transfer_pending(device.transfers[1]);
    deliver_version_response(device, device.transfers[1], 1U);
    auto replacement = consumer.final_snapshot(identity);
    assert(replacement && replacement.value().counters.packets == 0U);
    assert(consumer.release_final(identity));

    assert(consumer.start(&device, identity, 8U));
    enqueue_framed_bda(device, stale_input, device.transfers[1]);
    wait_transfer_pending(device.transfers[1]);
    enqueue_framed_bda(device, input, device.transfers[1]);
    bool replacement_packets = false;
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        auto counters = consumer.stats(identity);
        assert(counters);
        if (counters.value().packets == 4U) { replacement_packets = true; break; }
        std::this_thread::sleep_for(1ms);
    }
    assert(replacement_packets);
    std::uint8_t replacement_bytes[4U * SIANO_OUTPUT_PACKET]{};
    auto replacement_read = consumer.read(
        identity, dtv::userland::MutableByteView{replacement_bytes, sizeof(replacement_bytes)}, 1500ms);
    assert(replacement_read && replacement_read.value().bytes == sizeof(replacement_bytes));
    assert(std::memcmp(replacement_bytes, input.data(), sizeof(replacement_bytes)) == 0);
    assert(consumer.stop());
    replacement = consumer.final_snapshot(identity);
    assert(replacement && replacement.value().counters.packets == 4U);
    assert(replacement.value().counters.bytes == sizeof(replacement_bytes));
    assert(consumer.release_final(identity));

    Consumer held_sink;
    assert(held_sink.start(&device, identity, 8U));
    sink_gate_released = false;
    sink_gate_entered.store(false);
    ConsumerTestAccess::sink_gate(held_sink, hold_test_sink, nullptr);
    const bool held_input_queued = wait_transfer_pending_bounded(device.transfers[1]) &&
        enqueue_framed_bda_bounded(stale_input, device.transfers[1]) &&
        wait_transfer_pending_bounded(device.transfers[1]) &&
        enqueue_framed_bda_bounded(input, device.transfers[1]);
    bool sink_reached = false;
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        if (sink_gate_entered.load()) { sink_reached = true; break; }
        std::this_thread::sleep_for(1ms);
    }
    auto held_counters = held_sink.stats(identity);
    std::fprintf(stderr, "SINK before-stop reached=%d packets=%llu\n", sink_reached,
                 held_counters ? static_cast<unsigned long long>(held_counters.value().packets) : 0ULL);
    std::atomic<bool> held_stop_done{false};
    bool held_stop_result = false;
    std::thread held_stop([&] {
        held_stop_result = static_cast<bool>(held_sink.stop());
        held_stop_done.store(true);
    });
    for (unsigned attempt = 0U; attempt < 50U && !held_stop_done.load(); ++attempt)
        std::this_thread::sleep_for(1ms);
    const bool stop_waited = !held_stop_done.load();
    const bool replacement_rejected = !held_sink.start(&device, identity, 8U);
    pthread_mutex_lock(&proof_gate_mutex);
    sink_gate_released = true;
    pthread_cond_broadcast(&proof_gate_cond);
    pthread_mutex_unlock(&proof_gate_mutex);
    held_stop.join();
    std::fprintf(stderr, "SINK gate reached=%d stop_waited=%d replacement_rejected=%d stop_ok=%d\n",
                 sink_reached, stop_waited, replacement_rejected, held_stop_result);
    assert(held_input_queued && sink_reached && stop_waited && replacement_rejected && held_stop_result);
    std::uint8_t held_bytes[4U * SIANO_OUTPUT_PACKET]{};
    auto held_read = held_sink.read(
        identity, dtv::userland::MutableByteView{held_bytes, sizeof(held_bytes)}, 1500ms);
    assert(held_read && held_read.value().bytes == sizeof(held_bytes));
    assert(std::memcmp(held_bytes, input.data(), sizeof(held_bytes)) == 0);
    auto held_final = held_sink.final_snapshot(identity);
    assert(held_final && held_final.value().counters.packets == 4U &&
           held_final.value().counters.bytes == sizeof(held_bytes));
    assert(held_sink.release_final(identity));
    assert(held_sink.start(&device, identity, 8U));
    wait_transfer_pending(device.transfers[1]);
    enqueue_framed_bda(device, stale_input, device.transfers[1]);
    wait_transfer_pending(device.transfers[1]);
    enqueue_framed_bda(device, input, device.transfers[1]);
    bool held_replacement_ready = false;
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        auto counters = held_sink.stats(identity);
        assert(counters);
        if (counters.value().packets == 4U) { held_replacement_ready = true; break; }
        std::this_thread::sleep_for(1ms);
    }
    assert(held_replacement_ready);
    std::memset(held_bytes, 0, sizeof(held_bytes));
    held_read = held_sink.read(
        identity, dtv::userland::MutableByteView{held_bytes, sizeof(held_bytes)}, 1500ms);
    assert(held_read && held_read.value().bytes == sizeof(held_bytes));
    assert(std::memcmp(held_bytes, input.data(), sizeof(held_bytes)) == 0);
    assert(held_sink.stop());
    held_final = held_sink.final_snapshot(identity);
    assert(held_final && held_final.value().counters.packets == 4U &&
           held_final.value().counters.bytes == sizeof(held_bytes));
    assert(held_sink.release_final(identity));

    Consumer old_read_consumer;
    assert(old_read_consumer.start(&device, identity, 8U));
    old_read_condition_target = ConsumerTestAccess::condition(old_read_consumer);
    old_read_wait_entered.store(false);
    old_read_woke.store(false);
    old_read_gate_released = false;
    bool old_read_not_found = false;
    std::thread old_reader([&] {
        std::uint8_t stale[SIANO_OUTPUT_PACKET]{};
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
        old_read_consumer.start(&device, identity, 8U));
    bool old_b_complete = false;
    std::uint8_t old_b_bytes[4U * SIANO_OUTPUT_PACKET]{};
    dtv::userland::Result<Consumer::ReadResult> old_b_read =
        dtv::userland::Result<Consumer::ReadResult>::failure(dtv::userland::Error::INTERNAL);
    if (same_identity_b_started) {
        const bool old_b_input_queued = wait_transfer_pending_bounded(device.transfers[1]) &&
            enqueue_framed_bda_bounded(stale_input, device.transfers[1]) &&
            wait_transfer_pending_bounded(device.transfers[1]) &&
            enqueue_framed_bda_bounded(input, device.transfers[1]);
        old_b_complete = false;
        for (unsigned attempt = 0U; old_b_input_queued && attempt < 1500U; ++attempt) {
            auto counters = old_read_consumer.stats(identity);
            if (counters && counters.value().packets == 4U) { old_b_complete = true; break; }
            std::this_thread::sleep_for(1ms);
        }
        if (old_b_complete)
            old_b_read = old_read_consumer.read(
                identity, dtv::userland::MutableByteView{old_b_bytes, sizeof(old_b_bytes)}, 1500ms);
    }
    pthread_mutex_lock(&proof_gate_mutex);
    old_read_gate_released = true;
    pthread_cond_broadcast(&proof_gate_cond);
    pthread_mutex_unlock(&proof_gate_mutex);
    old_read_condition_target = nullptr;
    old_reader.join();
    const bool b_bytes_ok = old_b_read && old_b_read.value().bytes == sizeof(old_b_bytes) &&
        std::memcmp(old_b_bytes, input.data(), sizeof(old_b_bytes)) == 0;
    auto old_b_stats = same_identity_b_started ? old_read_consumer.stats(identity)
                                               : dtv::userland::Result<dtv::userland::detail::StreamCounters>::failure(dtv::userland::Error::NOT_FOUND);
    const bool b_counters_ok = old_b_stats && old_b_stats.value().packets == 4U &&
        old_b_stats.value().bytes == sizeof(old_b_bytes);
    const bool old_b_stopped = same_identity_b_started
        ? static_cast<bool>(old_read_consumer.stop()) : false;
    std::fprintf(stderr, "OLD_READ wait=%d wake_held=%d A_stopped=%d B_started=%d B_bytes=%d stale_not_found=%d B_counters=%d B_stopped=%d\n",
                 old_wait_observed, old_read_paused_after_wake, old_a_stopped,
                 same_identity_b_started, b_bytes_ok, old_read_not_found,
                 b_counters_ok, old_b_stopped);
    assert(old_wait_observed && old_a_stopped && old_read_paused_after_wake &&
           same_identity_b_started && old_b_complete && b_bytes_ok && old_read_not_found &&
           b_counters_ok && old_b_stopped);
    auto old_b_final = old_read_consumer.final_snapshot(identity);
    assert(old_b_final && old_b_final.value().counters.packets == 4U &&
           old_b_final.value().counters.bytes == sizeof(old_b_bytes));
    assert(old_read_consumer.release_final(identity));

    Consumer join_failure;
    assert(join_failure.start(&device, identity, 8U));
    capture_wait_entered.store(false);
    pop_wait_observed = false;
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        if (capture_wait_entered.load()) {
            pop_wait_observed = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    assert(pop_wait_observed);
    fail_next_join = true;
    assert(!join_failure.stop());
    assert(!join_failure.start(&device, identity, 8U));
    assert(join_failure.stop());

    Consumer overflow_consumer;
    assert(overflow_consumer.start(&device, identity, 1U));
    enqueue_framed_bda(device, stale_input, device.transfers[1]);
    wait_transfer_pending(device.transfers[1]);
    deliver_version_response(device, device.transfers[2], 2U);
    enqueue_framed_bda(device, input, device.transfers[2]);
    bool overflow_observed = false;
    for (unsigned attempt = 0U; attempt < 1500U; ++attempt) {
        auto counters = overflow_consumer.stats(identity);
        assert(counters);
        if (counters.value().packets >= 2U) {
            overflow_observed = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    assert(overflow_observed);
    assert(overflow_consumer.stop());
    std::uint8_t retained[SIANO_OUTPUT_PACKET]{};
    auto retained_read = overflow_consumer.read(
        identity, dtv::userland::MutableByteView{retained, sizeof(retained)}, 100ms);
    assert(retained_read && retained_read.value().bytes == sizeof(retained));
    assert(std::memcmp(retained, input.data(), sizeof(retained)) == 0);
    auto overflow_final = overflow_consumer.final_snapshot(identity);
    assert(overflow_final && overflow_final.value().counters.packets == 2U);
    assert(overflow_final.value().counters.bytes == 2U * SIANO_OUTPUT_PACKET);
    assert(overflow_final.value().counters.queue_drops == 1U);
    assert(overflow_final.value().terminal == 1U);
    assert(overflow_consumer.release_final(identity));
    std::uint64_t drops = 0U;
    close_device(&device, &drops);
    assert(!device.state_initialized && !device.ts_initialized);
    for (std::size_t i = 0U; i < MAX_URBS; ++i)
        assert(device.transfers[i] == nullptr);
}
