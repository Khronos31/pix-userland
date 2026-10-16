#ifndef PIX_DT295_STREAM_H
#define PIX_DT295_STREAM_H

#include <stddef.h>
#include <stdint.h>

#include <libusb.h>

typedef struct dt295_stream dt295_stream;

enum {
    DT295_STREAM_TRANSFERS = 8,
    DT295_STREAM_TRANSFER_BYTES = 65536,
    DT295_STREAM_RING_BYTES = 32 * 1024 * 1024,
    DT295_STREAM_RING_BLOCKS = DT295_STREAM_RING_BYTES / DT295_STREAM_TRANSFER_BYTES,
    DT295_STREAM_ABANDONED = -20001,
    DT295_STREAM_CLOSED = -20002
};

typedef struct {
    uint64_t submitted, completed, completed_bytes;
    uint64_t delivered, delivered_bytes;
    uint64_t pending_blocks, active_transfers, peak_pending_blocks;
    uint64_t timeouts, cancellations, late_callbacks, late_actual_bytes;
    uint64_t last_completion_ns, max_completion_gap_ns;
    uint64_t first_error_ns, last_error_status, last_error_actual;
    int first_error;
    int stopping, thread_done;
} dt295_stream_stats;

/* POSIX/macOS implementation only. Other platforms return LIBUSB_ERROR_NOT_SUPPORTED. */
int dt295_stream_start(libusb_context *context, libusb_device_handle *handle, dt295_stream **out);

/* Returns bytes read, including valid partial timeout transfers; 0 means timeout.
 * A negative libusb error or DT295_STREAM_* value signals failure/closure. */
int dt295_stream_read(dt295_stream *stream, void *buffer, size_t capacity, uint64_t *sequence,
                      uint64_t *completed_ns, unsigned timeout_ms);
void dt295_stream_snapshot(dt295_stream *stream, dt295_stream_stats *out);

/* Stop has a deadline. If it returns DT295_STREAM_ABANDONED, keep the stream,
 * libusb handle and context alive and call stop again; callbacks still own them. */
int dt295_stream_stop(dt295_stream *stream, unsigned timeout_ms);
/* Destroy succeeds only after stop has joined the event thread. */
int dt295_stream_destroy(dt295_stream *stream);

#ifdef DT295_STREAM_TESTING
dt295_stream *dt295_stream_test_create(unsigned capacity_blocks);
int dt295_stream_test_complete(dt295_stream *stream, unsigned transfer_index, int status,
                               int actual_length, unsigned char fill);
void dt295_stream_test_mark_stopped(dt295_stream *stream);
#endif

#endif
