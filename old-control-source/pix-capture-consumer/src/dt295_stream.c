#define _POSIX_C_SOURCE 200809L
#include "dt295_stream.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
int dt295_stream_start(libusb_context *c, libusb_device_handle *h, dt295_stream **out) {
    (void)c;
    (void)h;
    if (out)
        *out = NULL;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}
int dt295_stream_read(dt295_stream *s, void *b, size_t c, uint64_t *q, uint64_t *n, unsigned t) {
    (void)s;
    (void)b;
    (void)c;
    (void)q;
    (void)n;
    (void)t;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}
void dt295_stream_snapshot(dt295_stream *s, dt295_stream_stats *o) {
    (void)s;
    if (o)
        memset(o, 0, sizeof(*o));
}
int dt295_stream_stop(dt295_stream *s, unsigned t) {
    (void)s;
    (void)t;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}
int dt295_stream_destroy(dt295_stream *s) {
    (void)s;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}
#else
#include <pthread.h>
#include <sys/time.h>

#define EVENT_WAIT_US 20000
struct dt295_block {
    uint64_t sequence, completed_ns;
    int length, ready;
};
struct dt295_slot {
    struct dt295_stream *owner;
    struct libusb_transfer *transfer;
    unsigned char *buffer;
    uint64_t sequence;
    int active;
};
struct dt295_stream {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t thread;
    libusb_context *context;
    libusb_device_handle *handle;
    struct dt295_slot slots[DT295_STREAM_TRANSFERS];
    struct dt295_block *blocks;
    unsigned char *ring;
    unsigned ring_blocks, transfer_count, active;
    uint64_t next_submit, next_read;
    int stopping, thread_started, thread_done, joined, destroying;
    dt295_stream_stats stats;
    int (*submit_fn)(struct libusb_transfer *);
};

static uint64_t monotonic_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return 0;
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}
static void first_error_locked(dt295_stream *s, int error, int status, int actual) {
    if (!s->stats.first_error) {
        s->stats.first_error = error;
        s->stats.first_error_ns = monotonic_ns();
        s->stats.last_error_status = (uint64_t)(int64_t)status;
        s->stats.last_error_actual = actual > 0 ? (uint64_t)actual : 0;
    }
    s->stopping = 1;
    s->stats.stopping = 1;
    pthread_cond_broadcast(&s->changed);
}
static int status_to_error(enum libusb_transfer_status status) {
    switch (status) {
    case LIBUSB_TRANSFER_TIMED_OUT:
        return LIBUSB_ERROR_TIMEOUT;
    case LIBUSB_TRANSFER_NO_DEVICE:
        return LIBUSB_ERROR_NO_DEVICE;
    case LIBUSB_TRANSFER_STALL:
        return LIBUSB_ERROR_PIPE;
    case LIBUSB_TRANSFER_OVERFLOW:
        return LIBUSB_ERROR_OVERFLOW;
    case LIBUSB_TRANSFER_CANCELLED:
        return LIBUSB_ERROR_INTERRUPTED;
    default:
        return LIBUSB_ERROR_IO;
    }
}
static int submit_locked(struct dt295_slot *slot) {
    dt295_stream *s = slot->owner;
    if (s->stopping)
        return s->stats.first_error ? s->stats.first_error : DT295_STREAM_CLOSED;
    if (s->next_submit - s->next_read >= s->ring_blocks) {
        first_error_locked(s, LIBUSB_ERROR_NO_MEM, -1, 0);
        return LIBUSB_ERROR_NO_MEM;
    }
    slot->sequence = s->next_submit++;
    slot->active = 1;
    ++s->active;
    s->stats.active_transfers = s->active;
    uint64_t pending = s->next_submit - s->next_read;
    if (pending > s->stats.peak_pending_blocks)
        s->stats.peak_pending_blocks = pending;
    int rc = s->submit_fn(slot->transfer);
    if (rc) {
        slot->active = 0;
        --s->active;
        s->stats.active_transfers = s->active;
        --s->next_submit;
        first_error_locked(s, rc, -1, 0);
        return rc;
    }
    ++s->stats.submitted;
    s->stats.pending_blocks = pending;
    return 0;
}
static void LIBUSB_CALL transfer_complete(struct libusb_transfer *transfer) {
    struct dt295_slot *slot = transfer->user_data;
    dt295_stream *s = slot->owner;
    pthread_mutex_lock(&s->mutex);
    if (!slot->active) {
        ++s->stats.late_callbacks;
        if (transfer->actual_length > 0 && transfer->actual_length <= DT295_STREAM_TRANSFER_BYTES)
            s->stats.late_actual_bytes += (uint64_t)transfer->actual_length;
        first_error_locked(s, LIBUSB_ERROR_OTHER, transfer->status, transfer->actual_length);
        pthread_mutex_unlock(&s->mutex);
        return;
    }
    slot->active = 0;
    if (s->active)
        --s->active;
    s->stats.active_transfers = s->active;
    if (s->stopping) {
        if (transfer->status == LIBUSB_TRANSFER_CANCELLED)
            ++s->stats.cancellations;
        else
            ++s->stats.late_callbacks;
        if (transfer->actual_length > 0 && transfer->actual_length <= DT295_STREAM_TRANSFER_BYTES)
            s->stats.late_actual_bytes += (uint64_t)transfer->actual_length;
        pthread_cond_broadcast(&s->changed);
        pthread_mutex_unlock(&s->mutex);
        return;
    }
    int length = transfer->actual_length;
    if (length < 0 || length > DT295_STREAM_TRANSFER_BYTES) {
        first_error_locked(s, LIBUSB_ERROR_OVERFLOW, transfer->status, length);
        pthread_mutex_unlock(&s->mutex);
        return;
    }
    if (transfer->status != LIBUSB_TRANSFER_COMPLETED &&
        transfer->status != LIBUSB_TRANSFER_TIMED_OUT) {
        first_error_locked(s, status_to_error(transfer->status), transfer->status, length);
        pthread_mutex_unlock(&s->mutex);
        return;
    }
    if (transfer->status == LIBUSB_TRANSFER_TIMED_OUT)
        ++s->stats.timeouts;
    uint64_t seq = slot->sequence;
    struct dt295_block *block = &s->blocks[seq % s->ring_blocks];
    if (block->ready) {
        first_error_locked(s, LIBUSB_ERROR_NO_MEM, transfer->status, length);
        pthread_mutex_unlock(&s->mutex);
        return;
    }
    block->sequence = seq;
    block->length = length;
    block->completed_ns = monotonic_ns();
    if (length)
        memcpy(s->ring + (size_t)(seq % s->ring_blocks) * DT295_STREAM_TRANSFER_BYTES,
               transfer->buffer, (size_t)length);
    block->ready = 1;
    ++s->stats.completed;
    s->stats.completed_bytes += (uint64_t)length;
    if (length) {
        uint64_t previous = s->stats.last_completion_ns;
        if (previous && block->completed_ns > previous &&
            block->completed_ns - previous > s->stats.max_completion_gap_ns)
            s->stats.max_completion_gap_ns = block->completed_ns - previous;
        s->stats.last_completion_ns = block->completed_ns;
    }
    pthread_cond_broadcast(&s->changed);
    (void)submit_locked(slot);
    pthread_mutex_unlock(&s->mutex);
}
static void *event_main(void *argument) {
    dt295_stream *s = argument;
    int cancel_sent = 0;
    for (;;) {
        pthread_mutex_lock(&s->mutex);
        if (s->stopping && !cancel_sent) {
            cancel_sent = 1;
            for (unsigned i = 0; i < s->transfer_count; ++i) {
                if (s->slots[i].active) {
                    int rc = libusb_cancel_transfer(s->slots[i].transfer);
                    if (rc && rc != LIBUSB_ERROR_NOT_FOUND && !s->stats.first_error)
                        first_error_locked(s, rc, -1, 0);
                }
            }
        }
        int done = s->stopping && s->active == 0;
        pthread_mutex_unlock(&s->mutex);
        if (done)
            break;
        struct timeval timeout = {0, EVENT_WAIT_US};
        int rc = libusb_handle_events_timeout_completed(s->context, &timeout, NULL);
        if (rc < 0 && rc != LIBUSB_ERROR_INTERRUPTED) {
            pthread_mutex_lock(&s->mutex);
            first_error_locked(s, rc, -1, 0);
            pthread_mutex_unlock(&s->mutex);
        }
    }
    pthread_mutex_lock(&s->mutex);
    s->thread_done = 1;
    s->stats.thread_done = 1;
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->mutex);
    return NULL;
}
static dt295_stream *allocate_stream(unsigned ring_blocks, unsigned transfer_count) {
    if (!ring_blocks || ring_blocks > DT295_STREAM_RING_BLOCKS || !transfer_count ||
        transfer_count > DT295_STREAM_TRANSFERS)
        return NULL;
    dt295_stream *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->ring_blocks = ring_blocks;
    s->transfer_count = transfer_count;
    s->blocks = calloc(ring_blocks, sizeof(*s->blocks));
    s->ring = malloc((size_t)ring_blocks * DT295_STREAM_TRANSFER_BYTES);
    if (!s->blocks || !s->ring || pthread_mutex_init(&s->mutex, NULL)) {
        free(s->ring);
        free(s->blocks);
        free(s);
        return NULL;
    }
    if (pthread_cond_init(&s->changed, NULL)) {
        pthread_mutex_destroy(&s->mutex);
        free(s->ring);
        free(s->blocks);
        free(s);
        return NULL;
    }
    s->submit_fn = libusb_submit_transfer;
    return s;
}
static void release_stream(dt295_stream *s) {
    for (unsigned i = 0; i < s->transfer_count; ++i) {
        if (s->slots[i].transfer)
            libusb_free_transfer(s->slots[i].transfer);
        free(s->slots[i].buffer);
    }
    pthread_cond_destroy(&s->changed);
    pthread_mutex_destroy(&s->mutex);
    free(s->ring);
    free(s->blocks);
    free(s);
}
int dt295_stream_start(libusb_context *context, libusb_device_handle *handle, dt295_stream **out) {
    if (!out || !context || !handle)
        return LIBUSB_ERROR_INVALID_PARAM;
    *out = NULL;
    dt295_stream *s = allocate_stream(DT295_STREAM_RING_BLOCKS, DT295_STREAM_TRANSFERS);
    if (!s)
        return LIBUSB_ERROR_NO_MEM;
    s->context = context;
    s->handle = handle;
    for (unsigned i = 0; i < s->transfer_count; ++i) {
        struct dt295_slot *slot = &s->slots[i];
        slot->owner = s;
        slot->transfer = libusb_alloc_transfer(0);
        slot->buffer = malloc(DT295_STREAM_TRANSFER_BYTES);
        if (!slot->transfer || !slot->buffer) {
            release_stream(s);
            return LIBUSB_ERROR_NO_MEM;
        }
        libusb_fill_bulk_transfer(slot->transfer, handle, 0x81, slot->buffer,
                                  DT295_STREAM_TRANSFER_BYTES, transfer_complete, slot, 0);
    }
    int rc = pthread_create(&s->thread, NULL, event_main, s);
    if (rc) {
        release_stream(s);
        return LIBUSB_ERROR_OTHER;
    }
    s->thread_started = 1;
    pthread_mutex_lock(&s->mutex);
    for (unsigned i = 0; i < s->transfer_count && !s->stopping; ++i)
        (void)submit_locked(&s->slots[i]);
    pthread_mutex_unlock(&s->mutex);
    *out = s;
    return 0;
}
static struct timespec realtime_after_ms(unsigned ms) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        ++t.tv_sec;
        t.tv_nsec -= 1000000000L;
    }
    return t;
}
int dt295_stream_read(dt295_stream *s, void *buffer, size_t capacity, uint64_t *sequence,
                      uint64_t *completed_ns, unsigned timeout_ms) {
    if (!s || !buffer || capacity < DT295_STREAM_TRANSFER_BYTES || !sequence || !completed_ns)
        return LIBUSB_ERROR_INVALID_PARAM;
    struct timespec deadline = realtime_after_ms(timeout_ms);
    pthread_mutex_lock(&s->mutex);
    for (;;) {
        struct dt295_block *block = &s->blocks[s->next_read % s->ring_blocks];
        if (block->ready && block->sequence == s->next_read) {
            int length = block->length;
            if (length)
                memcpy(buffer,
                       s->ring +
                           (size_t)(s->next_read % s->ring_blocks) * DT295_STREAM_TRANSFER_BYTES,
                       (size_t)length);
            *sequence = block->sequence;
            *completed_ns = block->completed_ns;
            block->ready = 0;
            ++s->next_read;
            ++s->stats.delivered;
            s->stats.delivered_bytes += (uint64_t)length;
            s->stats.pending_blocks = s->next_submit - s->next_read;
            pthread_mutex_unlock(&s->mutex);
            return length;
        }
        if (s->stats.first_error) {
            int error = s->stats.first_error;
            pthread_mutex_unlock(&s->mutex);
            return error;
        }
        if (s->stopping) {
            pthread_mutex_unlock(&s->mutex);
            return DT295_STREAM_CLOSED;
        }
        if (!timeout_ms) {
            pthread_mutex_unlock(&s->mutex);
            return 0;
        }
        int rc = pthread_cond_timedwait(&s->changed, &s->mutex, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&s->mutex);
            return 0;
        }
        if (rc) {
            pthread_mutex_unlock(&s->mutex);
            return LIBUSB_ERROR_OTHER;
        }
    }
}
void dt295_stream_snapshot(dt295_stream *s, dt295_stream_stats *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!s)
        return;
    pthread_mutex_lock(&s->mutex);
    *out = s->stats;
    out->pending_blocks = s->next_submit - s->next_read;
    out->active_transfers = s->active;
    out->stopping = s->stopping;
    out->thread_done = s->thread_done;
    pthread_mutex_unlock(&s->mutex);
}
int dt295_stream_stop(dt295_stream *s, unsigned timeout_ms) {
    if (!s)
        return LIBUSB_ERROR_INVALID_PARAM;
    uint64_t end = monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL;
    pthread_mutex_lock(&s->mutex);
    s->stopping = 1;
    s->stats.stopping = 1;
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->mutex);
    for (;;) {
        pthread_mutex_lock(&s->mutex);
        int done = s->thread_done;
        pthread_mutex_unlock(&s->mutex);
        if (done)
            break;
        if (monotonic_ns() >= end)
            return DT295_STREAM_ABANDONED;
        struct timespec pause = {0, 1000000L};
        nanosleep(&pause, NULL);
    }
    if (!s->joined && s->thread_started) {
        int rc = pthread_join(s->thread, NULL);
        if (rc)
            return LIBUSB_ERROR_OTHER;
        s->joined = 1;
        s->thread_started = 0;
    }
    return 0;
}
int dt295_stream_destroy(dt295_stream *s) {
    if (!s)
        return 0;
    pthread_mutex_lock(&s->mutex);
    int safe = s->thread_done && s->joined && s->active == 0;
    pthread_mutex_unlock(&s->mutex);
    if (!safe)
        return DT295_STREAM_ABANDONED;
    s->destroying = 1;
    release_stream(s);
    return 0;
}

#ifdef DT295_STREAM_TESTING
static int test_submit(struct libusb_transfer *transfer) {
    (void)transfer;
    return 0;
}
dt295_stream *dt295_stream_test_create(unsigned capacity_blocks) {
    dt295_stream *s = allocate_stream(capacity_blocks, 2);
    if (!s)
        return NULL;
    s->submit_fn = test_submit;
    for (unsigned i = 0; i < s->transfer_count; ++i) {
        struct dt295_slot *slot = &s->slots[i];
        slot->owner = s;
        slot->transfer = libusb_alloc_transfer(0);
        slot->buffer = malloc(DT295_STREAM_TRANSFER_BYTES);
        if (!slot->transfer || !slot->buffer) {
            release_stream(s);
            return NULL;
        }
        slot->transfer->buffer = slot->buffer;
        slot->transfer->user_data = slot;
        slot->transfer->callback = transfer_complete;
    }
    /* Synthetic transfers are queued but do not enter libusb. */
    for (unsigned i = 0; i < s->transfer_count; ++i) {
        struct dt295_slot *slot = &s->slots[i];
        slot->sequence = s->next_submit++;
        slot->active = 1;
        ++s->active;
        ++s->stats.submitted;
    }
    s->stats.active_transfers = s->active;
    return s;
}
void dt295_stream_test_mark_stopped(dt295_stream *s) {
    if (!s)
        return;
    pthread_mutex_lock(&s->mutex);
    s->stopping = 1;
    s->thread_done = 1;
    s->joined = 1;
    s->stats.stopping = 1;
    s->stats.thread_done = 1;
    for (unsigned i = 0; i < s->transfer_count; ++i) {
        if (s->slots[i].active) {
            s->slots[i].active = 0;
            if (s->active)
                --s->active;
        }
    }
    s->stats.active_transfers = s->active;
    pthread_mutex_unlock(&s->mutex);
}
int dt295_stream_test_complete(dt295_stream *s, unsigned transfer_index, int status,
                               int actual_length, unsigned char fill) {
    if (!s || transfer_index >= s->transfer_count || actual_length < 0 ||
        actual_length > DT295_STREAM_TRANSFER_BYTES)
        return LIBUSB_ERROR_INVALID_PARAM;
    struct dt295_slot *slot = &s->slots[transfer_index];
    memset(slot->buffer, fill, (size_t)actual_length);
    slot->transfer->status = (enum libusb_transfer_status)status;
    slot->transfer->actual_length = actual_length;
    transfer_complete(slot->transfer);
    return 0;
}
#endif
#endif
