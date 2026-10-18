/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dt295_stream.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static struct libusb_transfer *transfers[DT295_STREAM_TRANSFERS];
static int pending[DT295_STREAM_TRANSFERS];
static int delivered[DT295_STREAM_TRANSFERS];
static unsigned transfer_count;
static unsigned registered_count;
static unsigned callback_count;
static unsigned cancellation_count;
static unsigned freed_count;
static int stopping;
static int fail_first_join = 1;
static unsigned event_cursor;

int __real_pthread_join(pthread_t, void **);
int __wrap_pthread_join(pthread_t thread, void **result)
{
    if (fail_first_join) {
        fail_first_join = 0;
        return EINVAL;
    }
    return __real_pthread_join(thread, result);
}

int __real_libusb_submit_transfer(struct libusb_transfer *transfer);
int __real_libusb_handle_events_timeout_completed(libusb_context *, struct timeval *, int *);
void __real_libusb_free_transfer(struct libusb_transfer *transfer);

int __wrap_libusb_submit_transfer(struct libusb_transfer *transfer)
{
    unsigned index = 0U;
    while (index < transfer_count && transfers[index] != transfer)
        ++index;
    if (index == transfer_count) {
        assert(transfer_count < DT295_STREAM_TRANSFERS);
        transfers[transfer_count++] = transfer;
    }
    pending[index] = 1;
    ++registered_count;
    return 0;
}

int __wrap_libusb_cancel_transfer(struct libusb_transfer *transfer)
{
    (void)transfer;
    stopping = 1;
    ++cancellation_count;
    return 0;
}

int __wrap_libusb_handle_events_timeout_completed(libusb_context *context,
                                                  struct timeval *timeout,
                                                  int *completed)
{
    (void)context;
    (void)timeout;
    (void)completed;
    for (unsigned step = 0U; step < transfer_count; ++step) {
        unsigned index = (event_cursor + step) % transfer_count;
        if (!pending[index])
            continue;
        event_cursor = (index + 1U) % transfer_count;
        struct libusb_transfer *transfer = transfers[index];
        pending[index] = 0;
        if (stopping) {
            transfer->status = LIBUSB_TRANSFER_CANCELLED;
            transfer->actual_length = index == 0U ? 188 : 0;
        } else if (!delivered[index]) {
            delivered[index] = 1;
            memset(transfer->buffer, (int)(0x40U + index), 188U);
            transfer->buffer[0] = 0x47U;
            transfer->status = LIBUSB_TRANSFER_COMPLETED;
            transfer->actual_length = 188;
        } else {
            transfer->status = LIBUSB_TRANSFER_TIMED_OUT;
            transfer->actual_length = 0;
        }
        ++callback_count;
        transfer->callback(transfer);
        return 0;
    }
    struct timespec pause = {0, 1000000L};
    nanosleep(&pause, NULL);
    return 0;
}

void __wrap_libusb_free_transfer(struct libusb_transfer *transfer)
{
    ++freed_count;
    __real_libusb_free_transfer(transfer);
}

int main(void)
{
    dt295_stream *stream = NULL;
    assert(dt295_stream_start((libusb_context *)1, (libusb_device_handle *)2, &stream) == 0);
    assert(stream && transfer_count == DT295_STREAM_TRANSFERS);

    unsigned char bytes[DT295_STREAM_TRANSFER_BYTES];
    uint64_t sequence = 0U, completed_ns = 0U;
    for (unsigned packet = 0U; packet < DT295_STREAM_TRANSFERS; ++packet) {
        int length = dt295_stream_read(stream, bytes, sizeof(bytes), &sequence,
                                       &completed_ns, 1000U);
        assert(length == 188 && sequence == packet && completed_ns != 0U);
        assert(bytes[0] == 0x47U && bytes[1] == 0x40U + packet);
    }

    const int first_stop = dt295_stream_stop(stream, 3000U);
    assert(first_stop == LIBUSB_ERROR_OTHER);
    dt295_stream_stats stats;
    dt295_stream_snapshot(stream, &stats);
    assert(stats.thread_done && !stats.active_transfers);
    assert(dt295_stream_destroy(stream) == DT295_STREAM_ABANDONED);
    assert(dt295_stream_stop(stream, 3000U) == 0);
    assert(stats.cancellations == cancellation_count && cancellation_count > 0U);
    assert(stats.late_actual_bytes == 188U);
    assert(callback_count >= DT295_STREAM_TRANSFERS * 2U);
    assert(dt295_stream_destroy(stream) == 0);
    assert(freed_count == DT295_STREAM_TRANSFERS);
    printf("DT295 native: registered=%u callbacks=%u cancellations=%u late_bytes=%llu "
           "join_retry=1 event_thread_joined=%d transfer_free=%u\n",
           registered_count, callback_count, cancellation_count,
           (unsigned long long)stats.late_actual_bytes, stats.thread_done, freed_count);
}
