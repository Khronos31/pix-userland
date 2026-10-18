/* Offline same-owner DT295 -> native stream -> C host -> common capture proof. */
extern "C" {
#include "dt295_capture.h"
#include "dt295_stream.h"
#include "dt295_routes.h"
#include "ts_output.h"
#include "usb_backend.h"
}

#ifndef DT295_ACTUAL_OLD_CONTROL
#include "pix-capture-consumer.h"
#include "pix_capture_consumer_internal.h"
#include "dt295_capture_host.h"
#endif

#include <libusb.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <csignal>
#include <pthread.h>

extern "C" {
int dt295_actual_fixture_open(const char *);
void dt295_actual_fixture_close(void);
int dt295_actual_fixture_control(uint8_t, uint8_t, uint16_t, uint16_t, uint8_t *, uint16_t);
int dt295_actual_fixture_bulk(uint8_t, unsigned char *, int, int *);
int dt295_actual_fixture_interrupt(uint8_t, unsigned char *, int, int *);
uint64_t dt295_actual_fixture_now(void);
void dt295_actual_fixture_advance(uint64_t);
unsigned dt295_actual_fixture_key_requests(void);
unsigned dt295_actual_fixture_renewal_applies(void);
unsigned dt295_actual_fixture_auth_requests(void);
unsigned dt295_actual_fixture_renewal_challenges(void);
unsigned dt295_actual_fixture_route_writes(void);
unsigned dt295_actual_fixture_pid273_route_writes(void);
void dt295_actual_fixture_bootstrap(uint8_t packets[3][188]);
void dt295_actual_fixture_shared_bootstrap(uint8_t packets[4][188]);
void dt295_actual_fixture_recording_packet(uint8_t packet[188], unsigned continuity);
void dt295_actual_fixture_data_pid(uint8_t packet[188], unsigned pid, unsigned continuity);
void dt295_actual_fixture_encrypt(uint8_t packet[188]);
void dt295_actual_fixture_update_packet(uint8_t packet[188], unsigned, unsigned, unsigned);
void dt295_actual_fixture_shared_update_packet(uint8_t packet[188], unsigned, unsigned,
                                               unsigned, int);
void dt295_actual_fixture_fail_next_route(void);
void dt295_actual_fixture_fail_renewal(void);
unsigned dt295_actual_fixture_route_failures(void);
#ifdef DT295_ACTUAL_OLD_CONTROL
int dt295_capture_run(const dt295_capture_config *);
#endif
int __real_usb_open_model(usb_backend **, const usb_selector *, int, int, int);
void __real_usb_close(usb_backend *);
void *__real_usb_native_context(usb_backend *);
void *__real_usb_native_handle(usb_backend *);
int __real_libusb_control_transfer(libusb_device_handle *, uint8_t, uint8_t, uint16_t,
                                   uint16_t, unsigned char *, uint16_t, unsigned int);
int __real_libusb_bulk_transfer(libusb_device_handle *, unsigned char, unsigned char *, int,
                                int *, unsigned int);
int __real_libusb_interrupt_transfer(libusb_device_handle *, unsigned char, unsigned char *,
                                     int, int *, unsigned int);
int __real_libusb_submit_transfer(libusb_transfer *);
int __real_libusb_cancel_transfer(libusb_transfer *);
int __real_libusb_handle_events_timeout_completed(libusb_context *, timeval *, int *);
void __real_libusb_free_transfer(libusb_transfer *);
int __real_dt295_stream_start(libusb_context *, libusb_device_handle *, dt295_stream **);
#ifndef DT295_ACTUAL_OLD_CONTROL
int __real_dt295_capture_host_start(dt295_capture_host **, const void *, const char *,
                                    dt295_capture_packet_sink, void *);
int __real_dt295_capture_run_to_sink(const dt295_capture_config *, dt295_capture_packet_sink,
                                     void *, void *, int *, void (*)(void *), void *, const char *);
void __real_dt295_routes_commit(dt295_route_plan *, const dt295_route_plan *);
#endif
int __real_ts_output_submit(ts_output *, const void *, std::size_t, std::size_t *);
uint64_t __real_now_ms(void);
void __real_sleep_ms(unsigned);
int __real_random_bytes(void *, size_t);
}

#ifndef DT295_ACTUAL_OLD_CONTROL
namespace pix::capture {
struct ConsumerTestAccess {
    static pthread_cond_t *condition(Consumer &value)
    {
        return value.owner_->session.changed.native_handle();
    }
};
}
#endif

namespace {
using namespace std::chrono_literals;

struct ScheduledGeneration {
    std::vector<std::uint8_t> bytes;
    std::vector<std::pair<std::size_t, std::size_t>> segments;
    std::size_t released = 0;
    std::size_t split = 188;
    bool reverse = false;
    std::uint64_t next_ordinal = 0;
};

struct TransferRecord {
    libusb_transfer *transfer = nullptr;
    unsigned generation = 0;
    std::uint64_t ordinal = 0;
    bool pending = false;
    bool cancelled = false;
};

std::mutex native_mutex;
std::condition_variable native_changed;
std::array<ScheduledGeneration, 2> schedules;
std::vector<TransferRecord> transfers;
unsigned current_generation = 0;
unsigned stream_starts = 0;
unsigned registrations = 0;
unsigned callbacks = 0;
unsigned cancellations = 0;
unsigned frees = 0;
#ifndef DT295_ACTUAL_OLD_CONTROL
std::mutex sink_mutex;
std::condition_variable sink_changed;
dt295_capture_packet_sink real_sink = nullptr;
void *real_sink_context = nullptr;
unsigned sink_calls = 0;
bool hold_first_sink = true;
bool sink_gate_entered = false;
bool sink_gate_released = false;
pthread_cond_t *old_read_condition = nullptr;
std::atomic<bool> old_read_wait_entered{false};
std::atomic<bool> old_read_woke{false};
std::mutex old_read_gate_mutex;
std::condition_variable old_read_gate_changed;
bool old_read_gate_released = false;
bool old_read_result_ok = false;
std::uint64_t old_read_result_bytes = 0;
std::thread old_reader;
std::atomic<bool> late_cancel_actual_once{false};
std::atomic<unsigned> late_cancel_actual_callbacks{0U};
std::atomic<std::uint64_t> observed_late_actual_bytes{0U};
std::array<dt295_stream *, 2> started_streams{};
std::atomic<bool> hold_cancel_callback{false};
std::atomic<bool> cancel_callback_held{false};
std::mutex cancel_callback_mutex;
std::condition_variable cancel_callback_changed;
bool release_cancel_callback = false;
#endif
volatile sig_atomic_t *old_interrupted = nullptr;
std::mutex old_gate_mutex;
std::condition_variable old_gate_changed;
bool old_gate_entered = false;
bool old_gate_released = false;
std::size_t old_output_bytes = 0;
std::size_t old_expected_bytes = 376;
std::array<std::atomic<std::uint64_t>, 2> old_stream_read_bytes{};
std::atomic<bool> old_capture_done{false};
int old_capture_result = -1;
#ifndef DT295_ACTUAL_OLD_CONTROL
std::atomic<bool> capture_returned{false};
std::atomic<int> capture_result{-1};
std::atomic<bool> fail_event_join{false};
std::atomic<bool> fail_host_join{false};
std::atomic<bool> fail_native_stop_once{false};
std::atomic<unsigned> stream_stop_calls{0U};
std::atomic<unsigned> route_commit_count{0};
pthread_t native_event_thread{};
pthread_t capture_host_thread{};
bool native_event_thread_known = false;
bool capture_host_thread_known = false;
std::atomic<unsigned> event_join_attempts{0};
std::atomic<unsigned> host_join_attempts{0};
std::atomic<unsigned> event_join_successes{0};
std::atomic<unsigned> host_join_successes{0};
#endif

TransferRecord *record_for(libusb_transfer *transfer)
{
    for (auto &record : transfers)
        if (record.transfer == transfer && record.generation == current_generation)
            return &record;
    return nullptr;
}

void schedule(unsigned generation, const std::vector<std::uint8_t> &bytes,
              std::size_t split, bool reverse, std::size_t released)
{
    std::lock_guard<std::mutex> lock(native_mutex);
    auto &target = schedules.at(generation);
    target.bytes = bytes;
    target.split = split;
    target.reverse = reverse;
    target.released = std::min(released, bytes.size());
    target.segments.clear();
    for (std::size_t offset = 0; offset < bytes.size();) {
        const std::size_t packet_remaining = 188U - (offset % 188U);
        const std::size_t length = std::min({split, packet_remaining, bytes.size() - offset});
        target.segments.emplace_back(offset, length);
        offset += length;
    }
    target.next_ordinal = 0;
    native_changed.notify_all();
}

void release_schedule(unsigned generation, std::size_t bytes)
{
    std::lock_guard<std::mutex> lock(native_mutex);
    auto &target = schedules.at(generation);
    target.released = std::min(bytes, target.bytes.size());
    native_changed.notify_all();
}

bool wait_until(const std::function<bool()> &predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

#ifndef DT295_ACTUAL_OLD_CONTROL
int pipeline_sink(void *context, const std::uint8_t *bytes, std::size_t length)
{
    const int rc = real_sink(context, bytes, length);
    if (rc)
        return rc;
    std::unique_lock<std::mutex> lock(sink_mutex);
    ++sink_calls;
    sink_changed.notify_all();
    if (hold_first_sink && sink_calls == 1) {
        sink_gate_entered = true;
        sink_changed.notify_all();
        if (!sink_changed.wait_for(lock, 3s, [] { return sink_gate_released; })) {
            sink_gate_released = true;
            std::fprintf(stderr, "DT295 barrier=sink_release timed_out=1\n");
        }
    }
    return 0;
}

void release_sink_gate()
{
    std::lock_guard<std::mutex> lock(sink_mutex);
    sink_gate_released = true;
    sink_changed.notify_all();
}

void reset_native_pipeline()
{
    std::lock_guard<std::mutex> lock(native_mutex);
    schedules = {};
    transfers.clear();
    current_generation = 0;
    stream_starts = registrations = callbacks = cancellations = frees = 0;
    started_streams = {};
    native_event_thread_known = false;
    capture_host_thread_known = false;
    route_commit_count.store(0U);
    capture_result.store(-1);
    capture_returned.store(false);
    event_join_attempts.store(0U);
    host_join_attempts.store(0U);
    event_join_successes.store(0U);
    host_join_successes.store(0U);
    fail_event_join.store(false);
    fail_host_join.store(false);
    fail_native_stop_once.store(false);
    stream_stop_calls.store(0U);
    late_cancel_actual_once.store(false);
    late_cancel_actual_callbacks.store(0U);
    observed_late_actual_bytes.store(0U);
    old_stream_read_bytes[0].store(0U, std::memory_order_release);
    old_stream_read_bytes[1].store(0U, std::memory_order_release);
    {
        std::lock_guard<std::mutex> sink_lock(sink_mutex);
        real_sink = nullptr;
        real_sink_context = nullptr;
        sink_calls = 0;
        hold_first_sink = true;
        sink_gate_entered = false;
        sink_gate_released = false;
    }
    native_changed.notify_all();
}
#endif
}

extern "C" int __wrap_usb_open_model(usb_backend **out, const usb_selector *, int, int,
                                      int model)
{
    if (!out || model != USB_MODEL_DT295)
        return -1;
    *out = reinterpret_cast<usb_backend *>(static_cast<std::uintptr_t>(0x295));
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

extern "C" int __wrap_libusb_control_transfer(libusb_device_handle *, uint8_t request_type,
                                               uint8_t request, uint16_t value, uint16_t index,
                                               unsigned char *data, uint16_t length,
                                               unsigned int)
{
    const int result = dt295_actual_fixture_control(request_type, request, value, index,
                                                    data, length);
    if (dt295_actual_fixture_renewal_applies() == 1)
        release_schedule(1, schedules[1].bytes.size());
    return result;
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

extern "C" int __wrap_dt295_stream_start(libusb_context *context,
                                          libusb_device_handle *handle, dt295_stream **stream)
{
    {
        std::lock_guard<std::mutex> lock(native_mutex);
        current_generation = stream_starts++;
        assert(current_generation < schedules.size());
    }
    const int rc = __real_dt295_stream_start(context, handle, stream);
#ifndef DT295_ACTUAL_OLD_CONTROL
    if (!rc) {
        std::lock_guard<std::mutex> lock(native_mutex);
        started_streams[current_generation] = *stream;
    }
#endif
    return rc;
}

extern "C" int __real_dt295_stream_read(dt295_stream *, void *, size_t, uint64_t *,
                                         uint64_t *, unsigned);
extern "C" int __wrap_dt295_stream_read(dt295_stream *stream, void *buffer, size_t capacity,
                                         uint64_t *sequence, uint64_t *completed_ns,
                                         unsigned timeout_ms)
{
    const int result = __real_dt295_stream_read(stream, buffer, capacity, sequence,
                                                completed_ns, timeout_ms);
    if (result > 0 && current_generation < old_stream_read_bytes.size())
        old_stream_read_bytes[current_generation].fetch_add(
            static_cast<std::uint64_t>(result), std::memory_order_release);
    return result;
}

#ifndef DT295_ACTUAL_OLD_CONTROL
extern "C" int __real_dt295_stream_stop(dt295_stream *, unsigned);
extern "C" int __wrap_dt295_stream_stop(dt295_stream *stream, unsigned timeout)
{
    stream_stop_calls.fetch_add(1U, std::memory_order_relaxed);
    return __real_dt295_stream_stop(stream, timeout);
}
#endif

#ifndef DT295_ACTUAL_OLD_CONTROL
extern "C" void __real_dt295_stream_snapshot(dt295_stream *, dt295_stream_stats *);
extern "C" void __wrap_dt295_stream_snapshot(dt295_stream *stream,
                                               dt295_stream_stats *snapshot)
{
    __real_dt295_stream_snapshot(stream, snapshot);
    if (snapshot && snapshot->late_actual_bytes)
        observed_late_actual_bytes.store(snapshot->late_actual_bytes,
                                         std::memory_order_release);
}
#endif

extern "C" int __wrap_libusb_submit_transfer(libusb_transfer *transfer)
{
    std::lock_guard<std::mutex> lock(native_mutex);
    auto *record = record_for(transfer);
    if (!record) {
        transfers.push_back({transfer, current_generation, 0, false, false});
        record = &transfers.back();
    }
    auto &generation = schedules.at(current_generation);
    record->ordinal = generation.next_ordinal++;
    record->pending = true;
    record->cancelled = false;
    ++registrations;
    native_changed.notify_all();
    return 0;
}

extern "C" int __wrap_libusb_cancel_transfer(libusb_transfer *transfer)
{
    std::lock_guard<std::mutex> lock(native_mutex);
    auto *record = record_for(transfer);
    if (!record)
        return LIBUSB_ERROR_NOT_FOUND;
    record->cancelled = true;
    ++cancellations;
    native_changed.notify_all();
    return 0;
}

extern "C" int __wrap_libusb_handle_events_timeout_completed(libusb_context *, timeval *, int *)
{
#ifndef DT295_ACTUAL_OLD_CONTROL
    {
        std::lock_guard<std::mutex> lock(native_mutex);
        native_event_thread = pthread_self();
        native_event_thread_known = true;
    }
#endif
    libusb_transfer *completed = nullptr;
    {
        std::unique_lock<std::mutex> lock(native_mutex);
        auto ready = [&]() -> TransferRecord * {
            TransferRecord *selected = nullptr;
            auto &generation = schedules.at(current_generation);
            for (auto &record : transfers) {
                if (record.generation != current_generation || !record.pending)
                    continue;
                if (record.cancelled) {
                    if (!selected || record.ordinal < selected->ordinal)
                        selected = &record;
                    continue;
                }
                if (record.ordinal >= generation.segments.size())
                    continue;
                const auto [offset, length] = generation.segments[record.ordinal];
                if (offset + length > generation.released)
                    continue;
                if (!selected || (generation.reverse ? record.ordinal > selected->ordinal
                                                      : record.ordinal < selected->ordinal))
                    selected = &record;
            }
            return selected;
        };
        TransferRecord *record = ready();
        if (!record) {
            native_changed.wait_for(lock, 2ms);
            record = ready();
        }
        if (!record)
            return 0;
        completed = record->transfer;
        record->pending = false;
        if (record->cancelled) {
            completed->status = LIBUSB_TRANSFER_CANCELLED;
#ifndef DT295_ACTUAL_OLD_CONTROL
            completed->actual_length = late_cancel_actual_once.exchange(false) ? 188 : 0;
            if (completed->actual_length) {
                std::memset(completed->buffer, 0xa5,
                            static_cast<std::size_t>(completed->actual_length));
                late_cancel_actual_callbacks.fetch_add(1U);
            }
            if (hold_cancel_callback.exchange(false)) {
                std::unique_lock<std::mutex> callback_lock(cancel_callback_mutex);
                cancel_callback_held.store(true, std::memory_order_release);
                cancel_callback_changed.notify_all();
                (void)cancel_callback_changed.wait_for(callback_lock, 15s,
                    [] { return release_cancel_callback; });
            }
#else
            completed->actual_length = 0;
#endif
        } else {
            const auto &generation = schedules.at(current_generation);
            const auto [offset, segment_length] = generation.segments[record->ordinal];
            const std::size_t length = std::min(segment_length,
                static_cast<std::size_t>(completed->length));
            std::memcpy(completed->buffer, generation.bytes.data() + offset, length);
            completed->status = LIBUSB_TRANSFER_COMPLETED;
            completed->actual_length = static_cast<int>(length);
        }
        ++callbacks;
    }
    completed->callback(completed);
    return 0;
}

extern "C" void __wrap_libusb_free_transfer(libusb_transfer *transfer)
{
    {
        std::lock_guard<std::mutex> lock(native_mutex);
        ++frees;
        for (auto it = transfers.begin(); it != transfers.end(); ++it) {
            if (it->transfer == transfer && it->generation == current_generation) {
                transfers.erase(it);
                break;
            }
        }
    }
    __real_libusb_free_transfer(transfer);
}

#ifndef DT295_ACTUAL_OLD_CONTROL
extern "C" int __wrap_dt295_capture_host_start(dt295_capture_host **out,
                                                const void *configuration,
                                                const char *runtime_directory,
                                                dt295_capture_packet_sink sink, void *context)
{
    std::fprintf(stderr, "DT295 observer=host_start_reached\n");
    real_sink = sink;
    real_sink_context = context;
    const int rc = __real_dt295_capture_host_start(out, configuration, runtime_directory,
                                                   pipeline_sink, context);
    std::fprintf(stderr, "DT295 observer=host_start_return rc=%d host=%d\n", rc,
                 out && *out != nullptr);
    return rc;
}

extern "C" int __wrap_dt295_capture_run_to_sink(
    const dt295_capture_config *configuration, dt295_capture_packet_sink sink,
    void *sink_context, void *interrupted, int *native_quiesced,
    void (*started)(void *), void *started_context, const char *runtime_directory)
{
    capture_host_thread = pthread_self();
    capture_host_thread_known = true;
    const int result = __real_dt295_capture_run_to_sink(
        configuration, sink, sink_context, interrupted, native_quiesced,
        started, started_context, runtime_directory);
    capture_result.store(result, std::memory_order_release);
    capture_returned.store(true, std::memory_order_release);
    return result;
}

extern "C" void __wrap_dt295_routes_commit(dt295_route_plan *routes,
                                            const dt295_route_plan *plan)
{
    route_commit_count.fetch_add(1U, std::memory_order_relaxed);
    __real_dt295_routes_commit(routes, plan);
}

extern "C" int __real_pthread_join(pthread_t, void **);
#ifndef DT295_ACTUAL_OLD_CONTROL
extern "C" int __real_pthread_cond_clockwait(pthread_cond_t *, pthread_mutex_t *,
                                               clockid_t, const timespec *);
extern "C" int __wrap_pthread_cond_clockwait(pthread_cond_t *condition,
                                               pthread_mutex_t *mutex,
                                               clockid_t clock_id,
                                               const timespec *deadline)
{
    if (condition != old_read_condition)
        return __real_pthread_cond_clockwait(condition, mutex, clock_id, deadline);
    old_read_wait_entered.store(true, std::memory_order_release);
    const int rc = __real_pthread_cond_clockwait(condition, mutex, clock_id, deadline);
    if (rc == 0) {
        (void)pthread_mutex_unlock(mutex);
        old_read_woke.store(true, std::memory_order_release);
        std::unique_lock<std::mutex> lock(old_read_gate_mutex);
        old_read_gate_changed.wait_for(lock, 3s, [] { return old_read_gate_released; });
        (void)pthread_mutex_lock(mutex);
    }
    return rc;
}
#endif
extern "C" int __wrap_pthread_join(pthread_t thread, void **result)
{
    if (capture_host_thread_known && pthread_equal(thread, capture_host_thread) &&
        fail_host_join.exchange(false)) {
        host_join_attempts.fetch_add(1U);
        std::fprintf(stderr, "DT295 injection=host_join_first_failure reached=1\n");
        return EINVAL;
    }
    if (native_event_thread_known && pthread_equal(thread, native_event_thread) &&
        fail_event_join.exchange(false)) {
        event_join_attempts.fetch_add(1U);
        std::fprintf(stderr, "DT295 injection=native_event_join_first_failure reached=1\n");
        return EINVAL;
    }
    const bool event_thread = native_event_thread_known &&
        pthread_equal(thread, native_event_thread);
    const bool host_thread = capture_host_thread_known &&
        pthread_equal(thread, capture_host_thread);
    const int rc = __real_pthread_join(thread, result);
    if (rc == 0 && event_thread) {
        event_join_successes.fetch_add(1U);
        std::fprintf(stderr, "DT295 observer=native_event_join completed=1\n");
    }
    if (rc == 0 && host_thread) {
        host_join_successes.fetch_add(1U);
        std::fprintf(stderr, "DT295 observer=capture_host_join completed=1\n");
    }
    return rc;
}

extern "C" void dt295_capture_test_set_stream_stop_hook(int (*)(dt295_stream *, unsigned));
extern "C" int dt295_actual_native_stop_hook(dt295_stream *stream, unsigned timeout)
{
    if (fail_native_stop_once.exchange(false)) {
        std::fprintf(stderr, "DT295 injection=native_stop_first_failure reached=1\n");
        return EIO;
    }
    return dt295_stream_stop(stream, timeout);
}
#else
extern "C" int __wrap_ts_output_submit(ts_output *output, const void *bytes,
                                         std::size_t length, std::size_t *accepted)
{
    const int result = __real_ts_output_submit(output, bytes, length, accepted);
    if (result)
        return result;
    if (!bytes || length == 0 || length % 188U != 0U ||
        *static_cast<const std::uint8_t *>(bytes) != 0x47U)
        return result;
    std::unique_lock<std::mutex> lock(old_gate_mutex);
    old_output_bytes += length;
    if (!old_gate_entered) {
        old_gate_entered = true;
        old_gate_changed.notify_all();
        if (!old_gate_changed.wait_for(lock, 3s, [] { return old_gate_released; })) {
            old_gate_released = true;
            std::fprintf(stderr, "DT295 barrier=old_output_release timed_out=1\n");
        }
    }
    if (old_output_bytes >= old_expected_bytes && old_interrupted)
        *old_interrupted = 1;
    return result;
}
#endif

extern "C" uint64_t __wrap_now_ms(void)
{
    return dt295_actual_fixture_now();
}
extern "C" void __wrap_sleep_ms(unsigned milliseconds)
{
    dt295_actual_fixture_advance(milliseconds);
}
extern "C" int __wrap_random_bytes(void *output, size_t length)
{
    std::memset(output, 0, length);
    return 0;
}

#ifdef DT295_ACTUAL_OLD_CONTROL
int main(int argc, char **argv)
{
    std::size_t split = 188;
    bool reverse = false;
    std::string test_case = "basic";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--reverse") == 0)
            reverse = true;
        else if (std::strcmp(argv[i], "--split") == 0 && i + 1 < argc)
            split = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc)
            test_case = argv[++i];
    }
    if (!split || split > 188)
        return 2;
    const std::string case_name = test_case + "-s" + std::to_string(split) +
                                  (reverse ? "-r" : "");
    std::filesystem::create_directories("runtime/" + case_name);
    std::filesystem::create_directories("runtime/old-control");
    const std::string assets_dir = std::filesystem::absolute("runtime/" + case_name).string();
    const std::string assets_file = assets_dir + "/dt295.assets";
    assert(dt295_actual_fixture_open(assets_file.c_str()) == 0);

    const bool shared_case = test_case == "shared-add-remove-reappear";
    std::vector<std::uint8_t> bootstrap;
    if (shared_case) {
        std::uint8_t boot_packets[4][188]{};
        dt295_actual_fixture_shared_bootstrap(boot_packets);
        for (auto &packet : boot_packets) {
            bootstrap.insert(bootstrap.end(), std::begin(packet), std::end(packet));
            dt295_actual_fixture_encrypt(bootstrap.data() + bootstrap.size() - 188U);
        }
    } else {
        std::uint8_t boot_packets[3][188]{};
        dt295_actual_fixture_bootstrap(boot_packets);
        for (auto &packet : boot_packets) {
            bootstrap.insert(bootstrap.end(), std::begin(packet), std::end(packet));
            dt295_actual_fixture_encrypt(bootstrap.data() + bootstrap.size() - 188U);
        }
    }
    const bool route_case = test_case == "new-ecm" || test_case == "rebind" ||
                            test_case == "route-write-failure";
    const bool decoder_case = test_case == "decoder-failure";
    const bool renewal_failure = test_case == "renewal-failure";
    std::vector<std::uint8_t> recording;
    std::vector<std::uint8_t> expected;
    const unsigned data_packets = route_case || renewal_failure || shared_case || decoder_case
        ? 1U
        : 2U;
    for (unsigned i = 0; i < data_packets; ++i) {
        std::array<std::uint8_t, 188> clear{};
        if (shared_case)
            dt295_actual_fixture_data_pid(clear.data(), 0x101, i);
        else
            dt295_actual_fixture_recording_packet(clear.data(), i);
        expected.insert(expected.end(), clear.begin(), clear.end());
        dt295_actual_fixture_encrypt(clear.data());
        if (decoder_case)
            clear[3] = static_cast<std::uint8_t>((clear[3] & ~0xc0U) | 0x80U);
        recording.insert(recording.end(), clear.begin(), clear.end());
    }
    if (route_case) {
        std::array<std::uint8_t, 188> update{};
        const unsigned mode = test_case == "route-write-failure" ? 1U
                            : test_case == "new-ecm" ? 2U : 3U;
        dt295_actual_fixture_update_packet(update.data(), 1, 1, mode);
        expected.insert(expected.end(), update.begin(), update.end());
        dt295_actual_fixture_encrypt(update.data());
        recording.insert(recording.end(), update.begin(), update.end());
    }
    if (shared_case) {
        auto append_clear = [&](std::array<std::uint8_t, 188> packet) {
            expected.insert(expected.end(), packet.begin(), packet.end());
            dt295_actual_fixture_encrypt(packet.data());
            recording.insert(recording.end(), packet.begin(), packet.end());
        };
        for (unsigned version = 1; version <= 3; ++version) {
            const int extra = version != 2;
            std::array<std::uint8_t, 188> pmt1{}, pmt2{};
            dt295_actual_fixture_shared_update_packet(pmt1.data(), 1, version, version, extra);
            dt295_actual_fixture_shared_update_packet(pmt2.data(), 2, version, version, extra);
            append_clear(pmt1);
            append_clear(pmt2);
            if (version == 1 || version == 3) {
                std::array<std::uint8_t, 188> extra_data{};
                dt295_actual_fixture_data_pid(extra_data.data(), 0x111,
                                               version == 1 ? 0U : 1U);
                append_clear(extra_data);
            }
        }
    }
    {
        std::ofstream clear("runtime/" + case_name + "/expected-clear.ts",
                            std::ios::binary | std::ios::trunc);
        clear.write(reinterpret_cast<const char *>(expected.data()),
                    static_cast<std::streamsize>(expected.size()));
    }
    std::vector<std::uint8_t> wire;
    wire.insert(wire.end(), bootstrap.begin(), bootstrap.end());
    wire.insert(wire.end(), recording.begin(), recording.end());
    schedule(0, std::vector<std::uint8_t>(bootstrap.begin(), bootstrap.end()),
             split, reverse, bootstrap.size());
    schedule(1, std::vector<std::uint8_t>(recording.begin(), recording.end()),
             split, reverse, shared_case ? std::min<std::size_t>(3U * 188U, recording.size())
                                        : recording.size());
    {
        std::ofstream scenario("runtime/scenario-wire.bin", std::ios::binary | std::ios::trunc);
        scenario.write(reinterpret_cast<const char *>(wire.data()),
                       static_cast<std::streamsize>(wire.size()));
    }

    volatile sig_atomic_t interrupted = 0;
    old_interrupted = &interrupted;
    old_expected_bytes = expected.size() + (renewal_failure ? 188U : 0U);
    old_output_bytes = 0;
    old_gate_entered = false;
    old_gate_released = false;
    old_stream_read_bytes[0].store(0U, std::memory_order_release);
    old_stream_read_bytes[1].store(0U, std::memory_order_release);
    old_capture_done.store(false, std::memory_order_release);
    old_capture_result = -1;
    constexpr std::size_t release_step = 64U;
    constexpr std::size_t release_window = 188U;
    std::size_t released_bootstrap = std::min(release_step, bootstrap.size());
    const std::size_t shared_route_limit = shared_case
        ? std::min<std::size_t>(3U * 188U, recording.size())
        : recording.size();
    std::size_t released_recording = std::min(
        renewal_failure || decoder_case
            ? std::min<std::size_t>(188U, recording.size()) : release_step,
        shared_route_limit);
    schedule(0, bootstrap, split, reverse, released_bootstrap);
    schedule(1, recording, split, reverse, released_recording);
    dt295_capture_config configuration{};
    configuration.assets = assets_dir.c_str();
    const std::string old_output_path = "runtime/" + case_name + "/old-recording.ts";
    configuration.output = old_output_path.c_str();
    configuration.selector.kind = USB_SELECT_INDEX;
    configuration.channel = 27;
    configuration.fd = -1;
    configuration.interrupted = &interrupted;
    if (renewal_failure)
        dt295_actual_fixture_fail_renewal();
    std::thread capture([&] {
        old_capture_result = dt295_capture_run(&configuration);
        old_capture_done.store(true, std::memory_order_release);
        old_gate_changed.notify_all();
    });

    bool entered = false;
    bool clock_advanced = false;
    bool shared_route_seen = !shared_case;
    const auto controller_deadline = std::chrono::steady_clock::now() + 10s;
    while (!old_capture_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < controller_deadline) {
        if (!entered) {
            std::lock_guard<std::mutex> lock(old_gate_mutex);
            if (old_gate_entered) {
                entered = true;
                if (test_case == "route-write-failure")
                    dt295_actual_fixture_fail_next_route();
                old_gate_released = true;
                old_gate_changed.notify_all();
            }
        }
        if (entered && !clock_advanced) {
            dt295_actual_fixture_advance(30000);
            clock_advanced = true;
        }
        const std::uint64_t bootstrap_read = old_stream_read_bytes[0].load(
            std::memory_order_acquire);
        if (released_bootstrap <= bootstrap_read + release_window &&
            released_bootstrap < bootstrap.size()) {
            released_bootstrap = std::min(bootstrap.size(), released_bootstrap + release_step);
            release_schedule(0, released_bootstrap);
        }
        if (shared_case && !shared_route_seen &&
            dt295_actual_fixture_pid273_route_writes() > 0U) {
            shared_route_seen = true;
            released_recording = std::max(released_recording, shared_route_limit);
            release_schedule(1, released_recording);
        }
        const std::uint64_t recording_read = old_stream_read_bytes[1].load(
            std::memory_order_acquire);
        const std::size_t recording_limit = renewal_failure
            ? std::min<std::size_t>(188U, recording.size())
            : shared_case && !shared_route_seen ? shared_route_limit : recording.size();
        if (!renewal_failure && !decoder_case &&
            released_recording <= recording_read + release_window &&
            released_recording < recording_limit) {
            released_recording = std::min(recording_limit, released_recording + release_step);
            release_schedule(1, released_recording);
        }
        std::this_thread::sleep_for(1ms);
    }
    const bool completed_before_deadline = old_capture_done.load(std::memory_order_acquire);
    if (!completed_before_deadline) {
        interrupted = 1;
        release_schedule(0, bootstrap.size());
        release_schedule(1, recording.size());
    }
    {
        std::lock_guard<std::mutex> lock(old_gate_mutex);
        old_gate_released = true;
        old_gate_changed.notify_all();
    }
    if (!completed_before_deadline)
        (void)wait_until([] { return old_capture_done.load(std::memory_order_acquire); }, 3s);
    capture.join();
    const bool complete = old_capture_done.load(std::memory_order_acquire) &&
                          (entered || decoder_case) && shared_route_seen &&
                          (test_case != "basic" || old_output_bytes == expected.size()) &&
                          dt295_actual_fixture_auth_requests() == 1 &&
                          dt295_actual_fixture_key_requests() == 1 && frees >= 8;
    std::fprintf(stderr,
        "DT295 case=immutable_old_actual_control scenario=%s split=%zu reverse=%d first=1 "
        "started=1 completed=%d assertions=10 capture_rc=%d output_bytes=%zu "
        "expected_bytes=%zu auth=1 key_requests=%u renewal_challenges=%u renewal=%u "
        "generations=%u registered=%u callbacks=%u cancellations=%u free=%u "
        "stream_read0=%llu stream_read1=%llu released0=%zu released1=%zu "
        "shared_route_seen=%d\n",
        test_case.c_str(), split, reverse,
        complete, old_capture_result, old_output_bytes, expected.size(),
        dt295_actual_fixture_key_requests(), dt295_actual_fixture_renewal_challenges(),
        dt295_actual_fixture_renewal_applies(),
        stream_starts, registrations, callbacks, cancellations, frees,
        static_cast<unsigned long long>(old_stream_read_bytes[0].load(
            std::memory_order_acquire)),
        static_cast<unsigned long long>(old_stream_read_bytes[1].load(
            std::memory_order_acquire)), released_bootstrap, released_recording,
        shared_route_seen);
    dt295_actual_fixture_close();
    return complete ? 0 : 1;
}
#else
int main(int argc, char **argv)
{
    using pix::capture::Consumer;
    std::size_t split = 188;
    bool reverse = false;
    std::string test_case = "basic";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--reverse") == 0)
            reverse = true;
        else if (std::strcmp(argv[i], "--split") == 0 && i + 1 < argc)
            split = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc)
            test_case = argv[++i];
    }
    if (split == 0 || split > 188)
        return 2;
    const std::string case_name = test_case + "-s" + std::to_string(split) +
                                  (reverse ? "-r" : "");
    std::filesystem::create_directories("runtime/" + case_name);
    const std::string assets_dir = std::filesystem::absolute("runtime/" + case_name).string();
    const std::string assets_file = assets_dir + "/dt295.assets";
    assert(dt295_actual_fixture_open(assets_file.c_str()) == 0);

    const bool shared_case = test_case == "shared-add-remove-reappear";
    std::vector<std::uint8_t> bootstrap;
    if (shared_case) {
        std::uint8_t boot_packets[4][188]{};
        dt295_actual_fixture_shared_bootstrap(boot_packets);
        for (auto &packet : boot_packets) {
            bootstrap.insert(bootstrap.end(), std::begin(packet), std::end(packet));
            dt295_actual_fixture_encrypt(bootstrap.data() + bootstrap.size() - 188U);
        }
    } else {
        std::uint8_t boot_packets[3][188]{};
        dt295_actual_fixture_bootstrap(boot_packets);
        for (auto &packet : boot_packets) {
            bootstrap.insert(bootstrap.end(), std::begin(packet), std::end(packet));
            dt295_actual_fixture_encrypt(bootstrap.data() + bootstrap.size() - 188U);
        }
    }
    std::vector<std::uint8_t> recording;
    std::vector<std::uint8_t> expected;
    const bool route_case = test_case == "new-ecm" || test_case == "rebind" ||
                            test_case == "route-write-failure";
    const bool decoder_case = test_case == "decoder-failure";
    const unsigned data_packets = route_case || test_case == "renewal-failure" ||
                                  shared_case || decoder_case
        ? 1U
        : 2U;
    for (unsigned i = 0; i < data_packets; ++i) {
        std::array<std::uint8_t, 188> clear{};
        if (shared_case)
            dt295_actual_fixture_data_pid(clear.data(), 0x101, i);
        else
            dt295_actual_fixture_recording_packet(clear.data(), i);
        expected.insert(expected.end(), clear.begin(), clear.end());
        dt295_actual_fixture_encrypt(clear.data());
        if (decoder_case)
            clear[3] = static_cast<std::uint8_t>((clear[3] & ~0xc0U) | 0x80U);
        recording.insert(recording.end(), clear.begin(), clear.end());
    }
    if (route_case) {
        std::array<std::uint8_t, 188> update{};
        const unsigned mode = test_case == "route-write-failure" ? 1U
                            : test_case == "new-ecm" ? 2U : 3U;
        dt295_actual_fixture_update_packet(update.data(), 1, 1, mode);
        expected.insert(expected.end(), update.begin(), update.end());
        dt295_actual_fixture_encrypt(update.data());
        recording.insert(recording.end(), update.begin(), update.end());
    }
    if (shared_case) {
        auto append_clear = [&](std::array<std::uint8_t, 188> packet) {
            expected.insert(expected.end(), packet.begin(), packet.end());
            dt295_actual_fixture_encrypt(packet.data());
            recording.insert(recording.end(), packet.begin(), packet.end());
        };
        for (unsigned version = 1; version <= 3; ++version) {
            const int extra = version != 2;
            std::array<std::uint8_t, 188> pmt1{}, pmt2{};
            dt295_actual_fixture_shared_update_packet(pmt1.data(), 1, version, version, extra);
            dt295_actual_fixture_shared_update_packet(pmt2.data(), 2, version, version, extra);
            append_clear(pmt1);
            append_clear(pmt2);
            if (version == 1 || version == 3) {
                std::array<std::uint8_t, 188> extra_data{};
                dt295_actual_fixture_data_pid(extra_data.data(), 0x111,
                                               version == 1 ? 0U : 1U);
                append_clear(extra_data);
            }
        }
    }
    {
        std::ofstream clear("runtime/" + case_name + "/expected-clear.ts",
                            std::ios::binary | std::ios::trunc);
        clear.write(reinterpret_cast<const char *>(expected.data()),
                    static_cast<std::streamsize>(expected.size()));
    }
    schedule(0, bootstrap,
             split, reverse, std::min<std::size_t>(64U, bootstrap.size()));
    schedule(1, std::vector<std::uint8_t>(recording.begin(), recording.end()),
             split, reverse, 188);
    {
        std::vector<std::uint8_t> wire;
        wire.insert(wire.end(), bootstrap.begin(), bootstrap.end());
        wire.insert(wire.end(), recording.begin(), recording.end());
        std::ofstream scenario("runtime/scenario-wire.bin", std::ios::binary | std::ios::trunc);
        scenario.write(reinterpret_cast<const char *>(wire.data()),
                       static_cast<std::streamsize>(wire.size()));
    }

    dt295_capture_config configuration{};
    configuration.assets = assets_dir.c_str();
    configuration.selector.kind = USB_SELECT_INDEX;
    configuration.selector.index = 0;
    configuration.channel = 27;
    configuration.seconds = 0;
    configuration.fd = -1;
    configuration.detach = 0;
    const std::string diagnostic_dir = assets_dir + "/diagnostic";
    configuration.diagnostic = shared_case ? diagnostic_dir.c_str() : nullptr;

    Consumer consumer;
    Consumer::Attachment identity{};
    identity.owner_client_id = 11U;
    identity.lease_id = 12U;
    identity.attachment_id = 13U;
    identity.receiver = 0U;
    identity.token[0] = 14U;
    if (test_case == "native-stop-first") {
        fail_native_stop_once.store(true);
        dt295_capture_test_set_stream_stop_hook(dt295_actual_native_stop_hook);
    } else if (test_case == "native-event-join-first") {
        fail_event_join.store(true);
    }
    const auto started = consumer.start(&configuration, ".", identity, 32U);
    if (!started) {
        std::fprintf(stderr, "DT295 consumer_start_error=%d\n",
                     static_cast<int>(started.error()));
        return 1;
    }

    auto release_on_failure = [&] {
        release_sink_gate();
        (void)consumer.stop();
    };
    if (test_case == "native-stop-first" || test_case == "native-event-join-first") {
        release_schedule(0, bootstrap.size());
        const bool returned = wait_until([] {
            return capture_returned.load(std::memory_order_acquire);
        }, 5s);
        dt295_capture_test_set_stream_stop_hook(nullptr);
        const auto stopped = consumer.stop();
        const auto final = consumer.final_snapshot(identity);
        const bool native_closed = wait_until([] {
            std::lock_guard<std::mutex> lock(native_mutex);
            return frees == 8 && cancellations >= 8;
        }, 3s);
        const bool injected = test_case == "native-stop-first"
            ? !fail_native_stop_once.load()
            : event_join_attempts.load() == 1;
        const bool fatal = final && final.value().terminal == static_cast<std::uint8_t>(
            dtv::userland::detail::StreamTerminal::bridge_fatal);
        const bool complete = returned && !stopped && native_closed && injected && fatal &&
                              event_join_successes.load() >= 1U &&
                              capture_result.load(std::memory_order_acquire) == 7 &&
                              stream_starts == 1;
        std::fprintf(stderr,
            "DT295 case=%s started=1 completed=%d assertions=8 run_rc=%d generations=%u "
            "event_join_failures=%u native_stop_failures=%d callbacks=%u cancel=%u free=%u "
            "returned=%d stop_ok=%d native_closed=%d injected=%d "
            "event_join_successes=%u host_join_successes=%u "
            "bridge_fatal=%d final_bytes=%llu final_packets=%llu final_terminal=%u\n",
            test_case.c_str(), complete, capture_result.load(), stream_starts,
            event_join_attempts.load(), test_case == "native-stop-first" ? 1 : 0,
            callbacks, cancellations, frees, returned, static_cast<bool>(stopped),
            native_closed, injected, event_join_successes.load(),
            host_join_successes.load(), fatal,
            static_cast<unsigned long long>(final ? final.value().counters.bytes : 0U),
            static_cast<unsigned long long>(final ? final.value().counters.packets : 0U),
            final ? static_cast<unsigned>(final.value().terminal) : 255U);
        dt295_actual_fixture_close();
        return complete ? 0 : 1;
    }
    std::size_t released_bootstrap = std::min<std::size_t>(64U, bootstrap.size());
    const auto recording_deadline = std::chrono::steady_clock::now() + 3s;
    bool reached_recording = false;
    while (std::chrono::steady_clock::now() < recording_deadline) {
        const std::uint64_t consumed = old_stream_read_bytes[0].load(
            std::memory_order_acquire);
        if (released_bootstrap <= consumed + 188U && released_bootstrap < bootstrap.size()) {
            released_bootstrap = std::min(bootstrap.size(), released_bootstrap + 64U);
            release_schedule(0, released_bootstrap);
        }
        {
            std::lock_guard<std::mutex> lock(native_mutex);
            reached_recording = stream_starts == 2U && registrations >= 16U;
        }
        if (reached_recording)
            break;
        std::this_thread::sleep_for(1ms);
    }
    if (!reached_recording) {
        release_on_failure();
        std::fprintf(stderr,
            "DT295 barrier=recording_generation timed_out=1 starts=%u registered=%u "
            "callbacks=%u consumed_bootstrap=%llu released_bootstrap=%zu\n",
            stream_starts, registrations, callbacks,
            static_cast<unsigned long long>(old_stream_read_bytes[0].load(
                std::memory_order_acquire)), released_bootstrap);
        return 1;
    }
    if (decoder_case) {
        const bool returned = wait_until([] {
            return capture_returned.load(std::memory_order_acquire);
        }, 3s);
        const auto stopped = consumer.stop();
        const auto final = consumer.final_snapshot(identity);
        const bool resources = wait_until([] {
            std::lock_guard<std::mutex> lock(native_mutex);
            return stream_starts == 2U && cancellations >= 16U && frees == 16U;
        }, 3s);
        const bool complete = returned && !stopped && final && resources &&
            capture_result.load(std::memory_order_acquire) == 8 &&
            final.value().terminal == static_cast<std::uint8_t>(
                dtv::userland::detail::StreamTerminal::bridge_fatal) &&
            final.value().counters.bytes == 0U && final.value().counters.packets == 0U &&
            dt295_actual_fixture_key_requests() == 1U;
        std::fprintf(stderr,
            "DT295 case=real_encrypted_tsc2_decoder_failure started=1 completed=%d "
            "assertions=8 tsc=2 rejected=1 run_rc=%d final_terminal=%u final_packets=%llu "
            "final_bytes=%llu generations=%u callbacks=%u cancel=%u free=%u key_requests=%u\n",
            complete, capture_result.load(),
            final ? static_cast<unsigned>(final.value().terminal) : 255U,
            static_cast<unsigned long long>(final ? final.value().counters.packets : 0U),
            static_cast<unsigned long long>(final ? final.value().counters.bytes : 0U),
            stream_starts, callbacks, cancellations, frees,
            dt295_actual_fixture_key_requests());
        dt295_actual_fixture_close();
        return complete ? 0 : 1;
    }
    const bool held_sink = wait_until([] {
        std::lock_guard<std::mutex> lock(sink_mutex);
        return sink_gate_entered;
    }, 3s);
    if (!held_sink) {
        release_on_failure();
        std::fprintf(stderr, "DT295 barrier=first_real_sink timed_out=1\n");
        return 1;
    }

    std::array<std::uint8_t, 16U * 188U> output{};
    auto first = consumer.read(identity,
        dtv::userland::MutableByteView{output.data(), 188U}, 2s);
    if (!first || first.value().bytes != 188U ||
        std::memcmp(output.data(), expected.data(), 188U) != 0) {
        std::size_t mismatch = 0;
        while (mismatch < 188U && output[mismatch] == expected[mismatch])
            ++mismatch;
        std::fprintf(stderr,
            "DT295 first_read ok=%d error=%d bytes=%llu mismatch=%zu got=%02x,%02x,%02x,%02x "
            "want=%02x,%02x,%02x,%02x\n",
            static_cast<bool>(first), first ? 0 : static_cast<int>(first.error()),
            static_cast<unsigned long long>(first ? first.value().bytes : 0), mismatch,
            output[0], output[1], output[2], output[3],
            expected[0], expected[1], expected[2], expected[3]);
        release_on_failure();
        std::fprintf(stderr, "DT295 assertion=first_common_read_exact failed=1\n");
        return 1;
    }

    if (test_case == "sink-read-replacement") {
        const auto active_restart = consumer.start(&configuration, ".", identity, 32U);
        const bool same_owner_rejected = !active_restart &&
            active_restart.error() == dtv::userland::Error::BUSY;
        old_read_condition = pix::capture::ConsumerTestAccess::condition(consumer);
        old_read_wait_entered.store(false, std::memory_order_release);
        old_read_woke.store(false, std::memory_order_release);
        old_read_gate_released = false;
        old_read_result_ok = false;
        old_read_result_bytes = 0;
        old_reader = std::thread([&] {
            std::array<std::uint8_t, 188U> old_output{};
            auto result = consumer.read(identity,
                dtv::userland::MutableByteView{old_output.data(), old_output.size()}, 5s);
            old_read_result_bytes = result ? result.value().bytes : 0U;
            old_read_result_ok = result && result.value().bytes == 0U &&
                result.value().eof && result.value().terminal ==
                    dtv::userland::detail::StreamTerminal::stopped;
        });
        const bool real_wait = wait_until([] {
            return old_read_wait_entered.load(std::memory_order_acquire);
        }, 3s);
        std::atomic<bool> stop_done{false};
        bool stop_ok = false;
        std::thread stopper;
        if (real_wait) {
            stopper = std::thread([&] {
                stop_ok = static_cast<bool>(consumer.stop());
                stop_done.store(true, std::memory_order_release);
            });
        }
        std::this_thread::sleep_for(50ms);
        const bool stop_waited_for_sink = real_wait &&
            !stop_done.load(std::memory_order_acquire);
        release_sink_gate();
        const bool stopped_in_time = real_wait && wait_until([&] {
            return stop_done.load(std::memory_order_acquire);
        }, 5s);
        if (stopper.joinable())
            stopper.join();
        const bool old_woke = wait_until([] {
            return old_read_woke.load(std::memory_order_acquire);
        }, 3s);
        const auto old_final = consumer.final_snapshot(identity);
        const std::uint64_t old_bytes = old_final ? old_final.value().counters.bytes : 0U;
        const std::uint64_t old_packets = old_final ? old_final.value().counters.packets : 0U;
        const bool old_resources = wait_until([] {
            std::lock_guard<std::mutex> lock(native_mutex);
            return stream_starts == 2U && cancellations >= 16U && frees == 16U;
        }, 3s);
        const bool old_detached = old_final &&
            old_final.value().terminal == static_cast<std::uint8_t>(
                dtv::userland::detail::StreamTerminal::stopped);

        bool b_started = false;
        bool b_exact = false;
        bool b_stopped = false;
        bool b_resources = false;
        bool old_unchanged = false;
        if (stopped_in_time && stop_ok && old_woke && old_resources && old_detached) {
            std::filesystem::create_directories("runtime/replacement-b");
            const std::string b_assets_dir =
                std::filesystem::absolute("runtime/replacement-b").string();
            const std::string b_assets_file = b_assets_dir + "/dt295.assets";
            dt295_actual_fixture_close();
            reset_native_pipeline();
            assert(dt295_actual_fixture_open(b_assets_file.c_str()) == 0);
            dt295_capture_config b_configuration = configuration;
            b_configuration.assets = b_assets_dir.c_str();
            schedule(0, std::vector<std::uint8_t>(bootstrap.begin(), bootstrap.end()),
                     split, reverse, bootstrap.size());
            schedule(1, recording, split, reverse, 188U);
            Consumer successor;
            b_started = static_cast<bool>(successor.start(
                &b_configuration, ".", identity, 32U));
            const bool b_ready = b_started && wait_until([] {
                std::lock_guard<std::mutex> lock(native_mutex);
                return stream_starts == 2U && registrations >= 16U;
            }, 3s);
            const bool b_sink = b_ready && wait_until([] {
                std::lock_guard<std::mutex> lock(sink_mutex);
                return sink_gate_entered;
            }, 3s);
            if (b_sink) {
                auto b_first = successor.read(identity,
                    dtv::userland::MutableByteView{output.data(), 188U}, 2s);
                b_exact = b_first && b_first.value().bytes == 188U &&
                    std::memcmp(output.data(), expected.data(), 188U) == 0;
                dt295_actual_fixture_advance(30000U);
                release_sink_gate();
                const bool b_renewed = wait_until([] {
                    return dt295_actual_fixture_renewal_applies() == 1U;
                }, 3s);
                if (b_renewed)
                    release_schedule(1, recording.size());
                if (b_renewed) {
                    auto b_second = successor.read(identity,
                        dtv::userland::MutableByteView{output.data(), 188U}, 2s);
                    b_exact = b_exact && b_second && b_second.value().bytes == 188U &&
                        std::memcmp(output.data(), expected.data() + 188U, 188U) == 0;
                }
            }
            release_sink_gate();
            const auto b_stop = successor.stop();
            b_stopped = static_cast<bool>(b_stop);
            const auto b_final = successor.final_snapshot(identity);
            b_exact = b_exact && b_final && b_final.value().counters.bytes == 376U &&
                b_final.value().counters.packets == 2U &&
                b_final.value().terminal == static_cast<std::uint8_t>(
                    dtv::userland::detail::StreamTerminal::stopped);
            b_resources = wait_until([] {
                std::lock_guard<std::mutex> lock(native_mutex);
                return stream_starts == 2U && cancellations >= 16U && frees == 16U;
            }, 3s);
            const auto old_after_b = consumer.final_snapshot(identity);
            old_unchanged = old_after_b &&
                old_after_b.value().counters.bytes == old_bytes &&
                old_after_b.value().counters.packets == old_packets;
            (void)successor.release_final(identity);
            dt295_actual_fixture_close();
        }
        {
            std::lock_guard<std::mutex> lock(old_read_gate_mutex);
            old_read_gate_released = true;
            old_read_gate_changed.notify_all();
        }
        if (old_reader.joinable())
            old_reader.join();
        const bool old_read_empty = old_read_result_ok && old_read_result_bytes == 0U;
        const bool complete = same_owner_rejected && held_sink && real_wait &&
            stop_waited_for_sink && stopped_in_time && stop_ok && old_woke && old_resources &&
            old_detached && b_started && b_exact && b_stopped && b_resources &&
            old_unchanged && old_read_empty;
        std::fprintf(stderr,
            "DT295 case=real_held_sink_old_read_replacement started=1 completed=%d "
            "assertions=15 held_sink=%d same_owner_busy=%d old_read_condition_wait=%d "
            "stop_waited=%d stop_ok=%d a_drained_joined=%d old_woke=%d old_empty=%d "
            "b_started_after_a=%d b_exact=%d b_stopped=%d b_drained_joined=%d "
            "a_unchanged=%d a_packets=%llu b_packets=2\n",
            complete, held_sink, same_owner_rejected, real_wait, stop_waited_for_sink,
            stop_ok, old_resources, old_woke, old_read_empty, b_started, b_exact,
            b_stopped, b_resources, old_unchanged,
            static_cast<unsigned long long>(old_packets));
        dt295_actual_fixture_close();
        return complete ? 0 : 1;
    }

    if (route_case) {
        const unsigned before_commit = route_commit_count.load();
        const unsigned before_routes = dt295_actual_fixture_route_writes();
        release_sink_gate();
        const bool drained_current_plan = wait_until([&] {
            return route_commit_count.load() > before_commit;
        }, 3s);
        const unsigned candidate_commit_baseline = route_commit_count.load();
        if (test_case == "route-write-failure")
            dt295_actual_fixture_fail_next_route();
        release_schedule(1, recording.size());
        const bool returned = wait_until([] {
            return capture_returned.load(std::memory_order_acquire);
        }, 3s);
        auto update_read = consumer.read(identity,
            dtv::userland::MutableByteView{output.data(), 188U}, 2s);
        const bool update_exact = update_read && update_read.value().bytes == 188U &&
            std::memcmp(output.data(), expected.data() + 188U, 188U) == 0;
        const auto stopped = consumer.stop();
        const auto final = consumer.final_snapshot(identity);
        const unsigned after_routes = dt295_actual_fixture_route_writes();
        const bool no_candidate_commit = route_commit_count.load() == candidate_commit_baseline;
        const bool refusal_no_route = test_case == "route-write-failure" ||
                                      after_routes == before_routes;
        const bool write_failed = test_case != "route-write-failure" ||
                                  dt295_actual_fixture_route_failures() == 1;
        const bool complete = drained_current_plan && returned && update_exact && !stopped && final &&
            capture_result.load() == 8 && final.value().counters.bytes == 376U &&
            final.value().counters.packets == 2U && no_candidate_commit &&
            refusal_no_route && write_failed;
        std::fprintf(stderr,
            "DT295 case=%s started=1 completed=%d assertions=9 run_rc=%d routes_before=%u "
            "routes_after=%u route_failures=%u commit_before=%u commit_after=%u "
            "update_common_read_exact=%d final_bytes=%llu final_packets=%llu\n",
            test_case.c_str(), complete, capture_result.load(), before_routes, after_routes,
            dt295_actual_fixture_route_failures(), candidate_commit_baseline, route_commit_count.load(),
            update_exact,
            static_cast<unsigned long long>(final ? final.value().counters.bytes : 0),
            static_cast<unsigned long long>(final ? final.value().counters.packets : 0));
        dt295_actual_fixture_close();
        return complete ? 0 : 1;
    }

    if (shared_case) {
        release_sink_gate();
        const unsigned route_baseline = dt295_actual_fixture_pid273_route_writes();
        release_schedule(1, 3U * 188U);
        const bool added = wait_until([&] {
            return dt295_actual_fixture_pid273_route_writes() == route_baseline + 1U;
        }, 3s);
        auto read_phase = [&](std::size_t offset, std::size_t length) {
            for (std::size_t at = 0; at < length; at += 188U) {
                auto result = consumer.read(identity,
                    dtv::userland::MutableByteView{output.data(), 188U}, 2s);
                std::size_t mismatch = 0;
                while (result && mismatch < 188U &&
                       output[mismatch] == expected[offset + at + mismatch])
                    ++mismatch;
                if (!result || result.value().bytes != 188U || mismatch != 188U) {
                    std::fprintf(stderr,
                        "DT295 shared_read_failure offset=%zu ok=%d error=%d bytes=%llu "
                        "mismatch=%zu got=%02x,%02x,%02x,%02x want=%02x,%02x,%02x,%02x\n",
                        offset + at, static_cast<bool>(result),
                        result ? 0 : static_cast<int>(result.error()),
                        static_cast<unsigned long long>(result ? result.value().bytes : 0),
                        mismatch, output[0], output[1], output[2], output[3],
                        expected[offset + at], expected[offset + at + 1],
                        expected[offset + at + 2], expected[offset + at + 3]);
                    return false;
                }
            }
            return true;
        };
        bool exact = added && read_phase(188U, 2U * 188U);
        const bool route_before_data =
            dt295_actual_fixture_pid273_route_writes() == route_baseline + 1U;
        release_schedule(1, 4U * 188U);
        exact = read_phase(3U * 188U, 188U) && exact;
        const bool data_after_route =
            dt295_actual_fixture_pid273_route_writes() == route_baseline + 1U;
        release_schedule(1, 6U * 188U);
        exact = read_phase(4U * 188U, 2U * 188U) && exact;
        const bool retained_after_removal =
            dt295_actual_fixture_pid273_route_writes() == route_baseline + 1U;
        release_schedule(1, 8U * 188U);
        exact = read_phase(6U * 188U, 2U * 188U) && exact;
        const bool no_duplicate_after_reappearance =
            dt295_actual_fixture_pid273_route_writes() == route_baseline + 1U;
        release_schedule(1, 9U * 188U);
        exact = read_phase(8U * 188U, 188U) && exact;
        const bool returned = wait_until([] {
            return capture_returned.load(std::memory_order_acquire);
        }, 100ms);
        const auto stopped = consumer.stop();
        const auto final = consumer.final_snapshot(identity);
        const bool resources = wait_until([] {
            std::lock_guard<std::mutex> lock(native_mutex);
            return frees == 16 && cancellations >= 16;
        }, 3s);
        const bool complete = added && route_before_data && retained_after_removal &&
            data_after_route && no_duplicate_after_reappearance && exact && !returned &&
            stopped && resources &&
            final && final.value().counters.packets == 9U &&
            final.value().counters.bytes == 9U * 188U &&
            dt295_actual_fixture_key_requests() == 1U &&
            dt295_actual_fixture_pid273_route_writes() == route_baseline + 1U;
        std::fprintf(stderr,
            "DT295 case=shared_add_remove_reappearance started=1 completed=%d assertions=13 "
            "route273_baseline=%u route273_final=%u route_before_data=%d data_after_route=%d "
            "retained=%d "
            "no_duplicate=%d exact_common_read=%d packets=%llu bytes=%llu "
            "terminal=%u run_rc=%d generations=%u registered=%u callbacks=%u cancel=%u "
            "free=%u key_requests=%u\n",
            complete, route_baseline, dt295_actual_fixture_pid273_route_writes(),
            route_before_data, data_after_route, retained_after_removal,
            no_duplicate_after_reappearance, exact,
            static_cast<unsigned long long>(final ? final.value().counters.packets : 0),
            static_cast<unsigned long long>(final ? final.value().counters.bytes : 0),
            final ? static_cast<unsigned>(final.value().terminal) : 255U,
            capture_result.load(std::memory_order_acquire),
            stream_starts, registrations, callbacks, cancellations, frees,
            dt295_actual_fixture_key_requests());
        dt295_actual_fixture_close();
        return complete ? 0 : 1;
    }

    if (test_case == "renewal-failure") {
        dt295_actual_fixture_fail_renewal();
        dt295_actual_fixture_advance(30000);
        release_sink_gate();
        const bool returned = wait_until([] {
            return capture_returned.load(std::memory_order_acquire);
        }, 3s);
        const auto stopped = consumer.stop();
        const auto final = consumer.final_snapshot(identity);
        const bool complete = returned && !stopped && final && capture_result.load() == 7 &&
            dt295_actual_fixture_renewal_challenges() == 1 &&
            dt295_actual_fixture_renewal_applies() == 0 &&
            final.value().terminal == static_cast<std::uint8_t>(
                dtv::userland::detail::StreamTerminal::bridge_fatal) &&
            final.value().counters.bytes == 188U && final.value().counters.packets == 1U;
        std::fprintf(stderr,
            "DT295 case=real_renewal_failure started=1 completed=%d assertions=8 run_rc=%d "
            "challenges=%u applies=%u final_bytes=%llu final_packets=%llu bridge_fatal=%d\n",
            complete, capture_result.load(), dt295_actual_fixture_renewal_challenges(),
            dt295_actual_fixture_renewal_applies(),
            static_cast<unsigned long long>(final ? final.value().counters.bytes : 0),
            static_cast<unsigned long long>(final ? final.value().counters.packets : 0),
            final && final.value().terminal == static_cast<std::uint8_t>(
                dtv::userland::detail::StreamTerminal::bridge_fatal));
        dt295_actual_fixture_close();
        return complete ? 0 : 1;
    }

    dt295_actual_fixture_advance(30000);
    release_sink_gate();
    const auto renew_deadline = std::chrono::steady_clock::now() + 3s;
    while (dt295_actual_fixture_renewal_applies() != 1 &&
           std::chrono::steady_clock::now() < renew_deadline)
        std::this_thread::sleep_for(1ms);
    if (dt295_actual_fixture_renewal_applies() != 1) {
        release_on_failure();
        std::fprintf(stderr, "DT295 barrier=real_renewal_apply timed_out=1\n");
        return 1;
    }
    release_schedule(1, recording.size());
    auto second = consumer.read(identity,
        dtv::userland::MutableByteView{output.data(), 188U}, 2s);
    if (!second || second.value().bytes != 188U ||
        std::memcmp(output.data(), expected.data() + 188U, 188U) != 0) {
        std::size_t mismatch = 0;
        while (mismatch < 188U && output[mismatch] == expected[188U + mismatch])
            ++mismatch;
        std::fprintf(stderr,
            "DT295 second_read ok=%d error=%d bytes=%llu mismatch=%zu got=%02x,%02x,%02x,%02x "
            "want=%02x,%02x,%02x,%02x renew=%u callbacks=%u\n",
            static_cast<bool>(second), second ? 0 : static_cast<int>(second.error()),
            static_cast<unsigned long long>(second ? second.value().bytes : 0), mismatch,
            output[0], output[1], output[2], output[3],
            expected[188U], expected[189U], expected[190U], expected[191U],
            dt295_actual_fixture_renewal_applies(), callbacks);
        release_on_failure();
        std::fprintf(stderr, "DT295 assertion=post_renewal_common_read_exact failed=1\n");
        return 1;
    }

    {
        std::ofstream result("runtime/" + case_name + "/new-read.ts",
                             std::ios::binary | std::ios::trunc);
        result.write(reinterpret_cast<const char *>(expected.data()),
                     static_cast<std::streamsize>(expected.size()));
    }

    if (test_case == "native-retention") {
        hold_cancel_callback.store(true, std::memory_order_release);
        cancel_callback_held.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(cancel_callback_mutex);
            release_cancel_callback = false;
        }
        std::atomic<bool> stop_done{false};
        bool stop_ok = false;
        const unsigned stop_calls_before = stream_stop_calls.load();
        std::thread stopper([&] {
            stop_ok = static_cast<bool>(consumer.stop());
            stop_done.store(true, std::memory_order_release);
        });
        const bool callback_held_ok = wait_until([] {
            return cancel_callback_held.load(std::memory_order_acquire);
        }, 3s);
        const bool completed_two_deadlines = wait_until([&] {
            return stop_done.load(std::memory_order_acquire);
        }, 7s);
        if (stopper.joinable())
            stopper.join();
        const auto premature_final = consumer.final_snapshot(identity);
        const bool retained_before_release = callback_held_ok && completed_two_deadlines &&
            !stop_ok && stream_stop_calls.load() == stop_calls_before + 2U && !premature_final &&
            frees == 8U;
        const unsigned production_stop_attempts =
            stream_stop_calls.load() - stop_calls_before;
        const auto restart = consumer.start(&configuration, ".", identity, 32U);
        const bool restart_busy = !restart &&
            restart.error() == dtv::userland::Error::BUSY;
        auto retained_result = consumer.take_retained_owner();
        pix::capture::RetainedOwner retained;
        const bool retained_taken = static_cast<bool>(retained_result);
        if (retained_taken)
            retained = std::move(retained_result.value());
        {
            std::lock_guard<std::mutex> lock(cancel_callback_mutex);
            release_cancel_callback = true;
            cancel_callback_changed.notify_all();
        }
        const bool callbacks_completed = callback_held_ok && wait_until([] {
            std::lock_guard<std::mutex> lock(native_mutex);
            return callbacks >= registrations;
        }, 3s);
        int test_seam_join = LIBUSB_ERROR_OTHER;
        int test_seam_destroy = LIBUSB_ERROR_OTHER;
        if (retained_before_release && callbacks_completed && started_streams[1]) {
            test_seam_join = dt295_stream_stop(started_streams[1], 1000U);
            if (!test_seam_join)
                test_seam_destroy = dt295_stream_destroy(started_streams[1]);
        }
        const bool callback_safe_cleanup = test_seam_join == 0 && test_seam_destroy == 0 &&
            frees == 16U;
        const auto retained_retry = retained_taken
            ? retained.stop()
            : dtv::userland::Result<void>::failure(dtv::userland::Error::NOT_FOUND);
        const bool retained_quiesced = retained_taken && retained.quiesced();
        const auto retained_release = retained_taken
            ? retained.release()
            : dtv::userland::Result<void>::failure(dtv::userland::Error::NOT_FOUND);
        const bool retained_retry_failed = retained_taken && !retained_retry &&
                                           !retained_quiesced && !retained_release;
        std::fprintf(stderr,
            "DT295 case=permanent_native_abandon_retention started=1 completed=%d "
            "assertions=14 callback_held=%d two_stop_deadlines=%d stop_failed=%d "
            "stop_calls_total=%u production_stop_attempts=%u frees_while_held=8 "
            "restart_busy=%d retained_taken=%d "
            "callbacks_completed=%d test_seam_join=%d test_seam_destroy=%d "
            "owner_retry_failed=%d owner_quiesced=%d owner_release_rejected=%d "
            "host_join_attempts=%u run_rc=%d\n",
            retained_before_release && restart_busy && retained_taken && callbacks_completed &&
                callback_safe_cleanup && retained_retry_failed,
            callback_held_ok, completed_two_deadlines, !stop_ok, stream_stop_calls.load(),
            production_stop_attempts, restart_busy, retained_taken,
            callbacks_completed, test_seam_join,
            test_seam_destroy, retained_retry_failed, retained_quiesced,
            !retained_release, host_join_attempts.load(),
            capture_result.load());
        dt295_actual_fixture_close();
        return retained_before_release && restart_busy && retained_taken && callbacks_completed &&
                       callback_safe_cleanup && retained_retry_failed
                   ? 0
                   : 1;
    }

    const bool late_case = test_case == "native-late-byte-drain";
    if (late_case)
        late_cancel_actual_once.store(true, std::memory_order_release);
    if (test_case == "host-join-first")
        fail_host_join.store(true);
    const auto stopped = consumer.stop();
    bool join_retry = false;
    if (test_case == "host-join-first") {
        const auto retry = consumer.stop();
        join_retry = !stopped && !retry && host_join_attempts.load() == 1;
    }
    const auto final = consumer.final_snapshot(identity);
    const bool counters = final && final.value().counters.packets == 2U &&
                          final.value().counters.bytes == recording.size();
    const bool resources = wait_until([] {
        std::lock_guard<std::mutex> lock(native_mutex);
        return frees == 16 && cancellations >= 16;
    }, 3s);
    const unsigned keys = dt295_actual_fixture_key_requests();
    const bool payload_match = first && second;
    const unsigned auths = dt295_actual_fixture_auth_requests();
    const unsigned route_writes = dt295_actual_fixture_route_writes();
    const unsigned renewal_challenges = dt295_actual_fixture_renewal_challenges();
    const bool fatal_final = final && final.value().terminal == static_cast<std::uint8_t>(
        dtv::userland::detail::StreamTerminal::bridge_fatal);
    const bool late_bytes = !late_case ||
        (late_cancel_actual_callbacks.load() == 1U &&
         observed_late_actual_bytes.load() == 188U && counters);
    const bool stop_expected = test_case == "host-join-first"
        ? (!stopped && join_retry && fatal_final)
        : static_cast<bool>(stopped);
    const bool joins_complete = event_join_successes.load() == 2U &&
                                host_join_successes.load() == 1U;
    std::fprintf(stderr,
        "DT295 case=%s split=%zu reverse=%d started=1 "
        "completed=%d assertions=11 auth=%u routes=%u generations=%u registered=%u callbacks=%u "
        "cancel=%u free=%u key_requests=%u renewal_challenges=%u renewal_applies=%u "
            "bytes=%llu packets=%llu event_joins=%u host_joins=%u "
        "exact_common_read=%d stop=%d late_cancel_actual=%u late_actual_bytes=%llu\n",
        test_case.c_str(), split, reverse,
        stop_expected && joins_complete && counters && resources && payload_match && keys == 1 &&
            auths == 1 && route_writes > 0 && renewal_challenges == 1 && late_bytes,
        auths, route_writes, stream_starts, registrations, callbacks, cancellations, frees,
        keys, renewal_challenges, dt295_actual_fixture_renewal_applies(),
        static_cast<unsigned long long>(final ? final.value().counters.bytes : 0),
        static_cast<unsigned long long>(final ? final.value().counters.packets : 0),
        event_join_successes.load(), host_join_successes.load(),
        payload_match, stop_expected, late_cancel_actual_callbacks.load(),
        static_cast<unsigned long long>(observed_late_actual_bytes.load()));

    dt295_actual_fixture_close();
    return stop_expected && joins_complete && counters && resources && payload_match && keys == 1 &&
                   auths == 1 && route_writes > 0 && renewal_challenges == 1 && late_bytes
               ? 0
               : 1;
}
#endif
