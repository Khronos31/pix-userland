#define _POSIX_C_SOURCE 200809L
#include "dt295_stream.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FAKE_EVENT_CAPACITY 32U

struct fake_transfer {
    struct libusb_transfer *transfer;
    unsigned submissions;
    int active;
    int queued;
    int cancel_requested;
};

struct fake_event {
    struct libusb_transfer *transfer;
    enum libusb_transfer_status status;
    int length;
    unsigned char fill;
};

static pthread_mutex_t fake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_changed = PTHREAD_COND_INITIALIZER;
static struct fake_transfer fake_transfers[DT295_STREAM_TRANSFERS];
static struct fake_event fake_events[FAKE_EVENT_CAPACITY];
static unsigned fake_transfer_count;
static unsigned fake_event_head;
static unsigned fake_event_count;
static unsigned fake_active_count;
static unsigned fake_cancel_requests;
static unsigned fake_frees;
static int fake_hold_cancel;

static void deadline_after_ms(struct timespec *deadline, unsigned timeout_ms)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += timeout_ms / 1000U;
    deadline->tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
}

static struct fake_transfer *find_transfer_locked(struct libusb_transfer *transfer)
{
    for (unsigned i = 0U; i < fake_transfer_count; ++i)
        if (fake_transfers[i].transfer == transfer)
            return &fake_transfers[i];
    return NULL;
}

static int enqueue_locked(struct libusb_transfer *transfer,
                          enum libusb_transfer_status status, int length,
                          unsigned char fill)
{
    struct fake_transfer *item = find_transfer_locked(transfer);
    if (item == NULL || !item->active || item->queued || fake_event_count == FAKE_EVENT_CAPACITY)
        return -1;
    const unsigned tail = (fake_event_head + fake_event_count) % FAKE_EVENT_CAPACITY;
    fake_events[tail] = (struct fake_event){transfer, status, length, fill};
    item->queued = 1;
    ++fake_event_count;
    pthread_cond_broadcast(&fake_changed);
    return 0;
}

static void fake_reset(int hold_cancel)
{
    pthread_mutex_lock(&fake_mutex);
    if (fake_active_count != 0U) {
        pthread_mutex_unlock(&fake_mutex);
        abort();
    }
    memset(fake_transfers, 0, sizeof(fake_transfers));
    memset(fake_events, 0, sizeof(fake_events));
    fake_transfer_count = 0U;
    fake_event_head = 0U;
    fake_event_count = 0U;
    fake_cancel_requests = 0U;
    fake_frees = 0U;
    fake_hold_cancel = hold_cancel;
    pthread_mutex_unlock(&fake_mutex);
}

static void pause_ms(unsigned timeout_ms)
{
    struct timespec pause = {(time_t)(timeout_ms / 1000U),
                             (long)(timeout_ms % 1000U) * 1000000L};
    nanosleep(&pause, NULL);
}

static int wait_for_active(unsigned expected)
{
    for (unsigned i = 0U; i < 5000U; ++i) {
        pthread_mutex_lock(&fake_mutex);
        const unsigned active = fake_active_count;
        pthread_mutex_unlock(&fake_mutex);
        if (active == expected)
            return 0;
        pause_ms(1U);
    }
    return -1;
}

static int wait_for_cancels(unsigned expected)
{
    for (unsigned i = 0U; i < 5000U; ++i) {
        pthread_mutex_lock(&fake_mutex);
        const unsigned requests = fake_cancel_requests;
        pthread_mutex_unlock(&fake_mutex);
        if (requests >= expected)
            return 0;
        pause_ms(1U);
    }
    return -1;
}

static int wait_for_snapshot(dt295_stream *stream, uint64_t completed,
                             int expected_error)
{
    for (unsigned i = 0U; i < 5000U; ++i) {
        dt295_stream_stats stats;
        dt295_stream_snapshot(stream, &stats);
        if (stats.completed >= completed &&
            (expected_error == 0 || stats.first_error == expected_error))
            return 0;
        pause_ms(1U);
    }
    return -1;
}

static int fake_complete(unsigned slot, enum libusb_transfer_status status,
                         int length, unsigned char fill)
{
    pthread_mutex_lock(&fake_mutex);
    if (slot >= fake_transfer_count) {
        pthread_mutex_unlock(&fake_mutex);
        return -1;
    }
    const int result = enqueue_locked(fake_transfers[slot].transfer, status, length, fill);
    pthread_mutex_unlock(&fake_mutex);
    return result;
}

static int fake_release_cancellations(void)
{
    pthread_mutex_lock(&fake_mutex);
    fake_hold_cancel = 0;
    for (unsigned i = 0U; i < fake_transfer_count; ++i) {
        struct fake_transfer *item = &fake_transfers[i];
        if (!item->active || !item->cancel_requested)
            continue;
        const enum libusb_transfer_status status =
            i == 0U ? LIBUSB_TRANSFER_COMPLETED : LIBUSB_TRANSFER_CANCELLED;
        const int length = i == 0U ? 37 : 0;
        if (enqueue_locked(item->transfer, status, length, 0x71U) != 0) {
            pthread_mutex_unlock(&fake_mutex);
            return -1;
        }
    }
    pthread_mutex_unlock(&fake_mutex);
    return 0;
}

static int fail(const char *name, int line)
{
    fprintf(stderr, "I2 failed at line %d: %s\n", line, name);
    return 1;
}

#define CHECK(expression) do { if (!(expression)) return fail(#expression, __LINE__); } while (0)

struct libusb_transfer *LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
    (void)iso_packets;
    pthread_mutex_lock(&fake_mutex);
    if (fake_transfer_count == DT295_STREAM_TRANSFERS) {
        pthread_mutex_unlock(&fake_mutex);
        return NULL;
    }
    struct libusb_transfer *transfer = calloc(1U, sizeof(*transfer));
    if (transfer != NULL)
        fake_transfers[fake_transfer_count++].transfer = transfer;
    pthread_mutex_unlock(&fake_mutex);
    return transfer;
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *transfer)
{
    pthread_mutex_lock(&fake_mutex);
    struct fake_transfer *item = find_transfer_locked(transfer);
    if (item == NULL || item->active || item->queued) {
        pthread_mutex_unlock(&fake_mutex);
        abort();
    }
    ++fake_frees;
    pthread_mutex_unlock(&fake_mutex);
    free(transfer);
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *transfer)
{
    pthread_mutex_lock(&fake_mutex);
    struct fake_transfer *item = find_transfer_locked(transfer);
    if (item == NULL || item->active || item->queued) {
        pthread_mutex_unlock(&fake_mutex);
        return LIBUSB_ERROR_OTHER;
    }
    item->active = 1;
    item->cancel_requested = 0;
    ++item->submissions;
    ++fake_active_count;
    pthread_cond_broadcast(&fake_changed);
    pthread_mutex_unlock(&fake_mutex);
    return 0;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *transfer)
{
    pthread_mutex_lock(&fake_mutex);
    struct fake_transfer *item = find_transfer_locked(transfer);
    if (item == NULL || !item->active || item->queued) {
        pthread_mutex_unlock(&fake_mutex);
        return LIBUSB_ERROR_NOT_FOUND;
    }
    if (!item->cancel_requested) {
        item->cancel_requested = 1;
        ++fake_cancel_requests;
    }
    const int held = fake_hold_cancel;
    const int result = held ? 0 : enqueue_locked(transfer, LIBUSB_TRANSFER_CANCELLED, 0, 0U);
    pthread_mutex_unlock(&fake_mutex);
    return result == 0 ? 0 : LIBUSB_ERROR_OTHER;
}

int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context *context,
                                                        struct timeval *timeout,
                                                        int *completed)
{
    (void)context;
    (void)completed;
    pthread_mutex_lock(&fake_mutex);
    struct timespec deadline;
    deadline_after_ms(&deadline, timeout == NULL ? 20U :
                      (unsigned)(timeout->tv_sec * 1000L + timeout->tv_usec / 1000L));
    while (fake_event_count == 0U) {
        const int rc = pthread_cond_timedwait(&fake_changed, &fake_mutex, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&fake_mutex);
            return 0;
        }
        if (rc != 0) {
            pthread_mutex_unlock(&fake_mutex);
            return LIBUSB_ERROR_OTHER;
        }
    }

    const struct fake_event event = fake_events[fake_event_head];
    fake_event_head = (fake_event_head + 1U) % FAKE_EVENT_CAPACITY;
    --fake_event_count;
    struct fake_transfer *item = find_transfer_locked(event.transfer);
    if (item == NULL || !item->active || !item->queued || event.length < 0 ||
        event.length > event.transfer->length) {
        pthread_mutex_unlock(&fake_mutex);
        return LIBUSB_ERROR_OTHER;
    }
    item->active = 0;
    item->queued = 0;
    --fake_active_count;
    pthread_mutex_unlock(&fake_mutex);

    if (event.length != 0)
        memset(event.transfer->buffer, event.fill, (size_t)event.length);
    event.transfer->status = event.status;
    event.transfer->actual_length = event.length;
    event.transfer->callback(event.transfer);
    return 0;
}

static int run_ordered_partial(void)
{
    fake_reset(0);
    dt295_stream *stream = NULL;
    CHECK(dt295_stream_start((libusb_context *)(uintptr_t)1U,
                             (libusb_device_handle *)(uintptr_t)1U, &stream) == 0);
    CHECK(wait_for_active(DT295_STREAM_TRANSFERS) == 0);
    CHECK(fake_complete(1U, LIBUSB_TRANSFER_COMPLETED, 188, 0x22U) == 0);
    CHECK(wait_for_snapshot(stream, 1U, 0) == 0);

    unsigned char buffer[DT295_STREAM_TRANSFER_BYTES];
    uint64_t sequence = 0U;
    uint64_t completed_ns = 0U;
    CHECK(dt295_stream_read(stream, buffer, sizeof(buffer), &sequence,
                            &completed_ns, 10U) == 0);
    CHECK(fake_complete(0U, LIBUSB_TRANSFER_COMPLETED, 188, 0x11U) == 0);
    CHECK(wait_for_snapshot(stream, 2U, 0) == 0);
    CHECK(dt295_stream_read(stream, buffer, sizeof(buffer), &sequence,
                            &completed_ns, 0U) == 188);
    CHECK(sequence == 0U && buffer[0] == 0x11U);
    CHECK(dt295_stream_read(stream, buffer, sizeof(buffer), &sequence,
                            &completed_ns, 0U) == 188);
    CHECK(sequence == 1U && buffer[0] == 0x22U);

    CHECK(fake_complete(2U, LIBUSB_TRANSFER_TIMED_OUT, 17, 0x31U) == 0);
    CHECK(wait_for_snapshot(stream, 3U, 0) == 0);
    CHECK(dt295_stream_read(stream, buffer, sizeof(buffer), &sequence,
                            &completed_ns, 0U) == 17);
    CHECK(sequence == 2U && buffer[16] == 0x31U);
    CHECK(dt295_stream_stop(stream, 5000U) == 0);
    dt295_stream_stats stats;
    dt295_stream_snapshot(stream, &stats);
    CHECK(stats.timeouts == 1U && stats.active_transfers == 0U && stats.thread_done);
    CHECK(dt295_stream_destroy(stream) == 0);
    pthread_mutex_lock(&fake_mutex);
    const unsigned frees = fake_frees;
    pthread_mutex_unlock(&fake_mutex);
    CHECK(frees == DT295_STREAM_TRANSFERS);
    puts("I2 case=ordered_partial passed");
    return 0;
}

static int run_first_error(void)
{
    fake_reset(0);
    dt295_stream *stream = NULL;
    CHECK(dt295_stream_start((libusb_context *)(uintptr_t)1U,
                             (libusb_device_handle *)(uintptr_t)1U, &stream) == 0);
    CHECK(wait_for_active(DT295_STREAM_TRANSFERS) == 0);
    CHECK(fake_complete(0U, LIBUSB_TRANSFER_ERROR, 0, 0U) == 0);
    CHECK(wait_for_snapshot(stream, 0U, LIBUSB_ERROR_IO) == 0);
    CHECK(dt295_stream_stop(stream, 5000U) == 0);

    dt295_stream_stats stats;
    dt295_stream_snapshot(stream, &stats);
    CHECK(stats.first_error == LIBUSB_ERROR_IO && stats.active_transfers == 0U &&
          stats.thread_done && stats.cancellations == DT295_STREAM_TRANSFERS - 1U);
    unsigned char buffer[DT295_STREAM_TRANSFER_BYTES];
    uint64_t sequence = 0U;
    uint64_t completed_ns = 0U;
    CHECK(dt295_stream_read(stream, buffer, sizeof(buffer), &sequence,
                            &completed_ns, 0U) == LIBUSB_ERROR_IO);
    CHECK(dt295_stream_destroy(stream) == 0);
    puts("I2 case=first_error_cancel passed");
    return 0;
}

static int run_queue_bound(void)
{
    fake_reset(0);
    dt295_stream *stream = NULL;
    CHECK(dt295_stream_start((libusb_context *)(uintptr_t)1U,
                             (libusb_device_handle *)(uintptr_t)1U, &stream) == 0);
    CHECK(wait_for_active(DT295_STREAM_TRANSFERS) == 0);

    uint64_t expected_completed = 0U;
    dt295_stream_stats stats;
    for (uint64_t i = 0U; i < DT295_STREAM_RING_BLOCKS + DT295_STREAM_TRANSFERS; ++i) {
        CHECK(fake_complete((unsigned)(i % DT295_STREAM_TRANSFERS),
                            LIBUSB_TRANSFER_COMPLETED, 188, (unsigned char)i) == 0);
        CHECK(wait_for_snapshot(stream, i + 1U, 0) == 0);
        dt295_stream_snapshot(stream, &stats);
        expected_completed = i + 1U;
        if (stats.first_error != 0)
            break;
    }
    CHECK(stats.first_error == LIBUSB_ERROR_NO_MEM);
    CHECK(stats.peak_pending_blocks == DT295_STREAM_RING_BLOCKS);
    CHECK(dt295_stream_stop(stream, 5000U) == 0);

    unsigned char buffer[DT295_STREAM_TRANSFER_BYTES];
    uint64_t sequence = 0U;
    uint64_t completed_ns = 0U;
    for (uint64_t i = 0U; i < expected_completed; ++i) {
        CHECK(dt295_stream_read(stream, buffer, sizeof(buffer), &sequence,
                                &completed_ns, 0U) == 188);
        CHECK(sequence == i && buffer[0] == (unsigned char)i);
    }
    CHECK(dt295_stream_read(stream, buffer, sizeof(buffer), &sequence,
                            &completed_ns, 0U) == LIBUSB_ERROR_NO_MEM);
    dt295_stream_snapshot(stream, &stats);
    CHECK(stats.delivered == expected_completed && stats.active_transfers == 0U &&
          stats.thread_done);
    CHECK(dt295_stream_destroy(stream) == 0);
    puts("I2 case=queue_bound passed");
    return 0;
}

static int run_deadline_late_callback(void)
{
    fake_reset(1);
    dt295_stream *stream = NULL;
    CHECK(dt295_stream_start((libusb_context *)(uintptr_t)1U,
                             (libusb_device_handle *)(uintptr_t)1U, &stream) == 0);
    CHECK(wait_for_active(DT295_STREAM_TRANSFERS) == 0);
    CHECK(dt295_stream_stop(stream, 1U) == DT295_STREAM_ABANDONED);
    CHECK(wait_for_cancels(DT295_STREAM_TRANSFERS) == 0);

    dt295_stream_stats stats;
    dt295_stream_snapshot(stream, &stats);
    CHECK(stats.stopping && !stats.thread_done &&
          stats.active_transfers == DT295_STREAM_TRANSFERS);
    CHECK(dt295_stream_destroy(stream) == DT295_STREAM_ABANDONED);
    CHECK(fake_release_cancellations() == 0);
    CHECK(dt295_stream_stop(stream, 5000U) == 0);

    dt295_stream_snapshot(stream, &stats);
    CHECK(stats.active_transfers == 0U && stats.thread_done &&
          stats.cancellations == DT295_STREAM_TRANSFERS - 1U &&
          stats.late_callbacks == 1U && stats.late_actual_bytes == 37U);
    CHECK(dt295_stream_destroy(stream) == 0);
    puts("I2 case=deadline_late_callback passed");
    return 0;
}

int main(void)
{
    if (run_ordered_partial() != 0 || run_first_error() != 0 ||
        run_queue_bound() != 0 || run_deadline_late_callback() != 0)
        return 1;
    puts("I2 cases=4 passed; skipped=0; production_state_machine=used");
    return 0;
}
