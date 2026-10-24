/* SPDX-License-Identifier: MIT */
#ifndef PIX_DT181_CAPTURE_BRIDGE_H
#define PIX_DT181_CAPTURE_BRIDGE_H

#include "diagnostic.h"
#include "os.h"
#include "transport.h"

#define DT181_CAPTURE_QUEUE_SLOTS 1024

typedef struct {
    uint8_t bytes[DT181_CAPTURE_QUEUE_SLOTS][32768];
    size_t lengths[DT181_CAPTURE_QUEUE_SLOTS];
    unsigned head, count, high_water;
    uint64_t received_ms[DT181_CAPTURE_QUEUE_SLOTS];
    diagnostic *diag;
    int done, error, output, failure_exit;
    uint64_t write_started;
    uint64_t capture_elapsed_ms;
    uint64_t usb_bytes;
    uint8_t pid_mask[8192];
    int filter_pids;
    os_mutex mutex;
    os_cond cond;
    ts_decoder decoder;
    ts_check check;
} dt181_capture_bridge;

/* The caller owns synchronization and initializes the queue/check before use.
 * Keep the decoder sink and its user context immutable and alive for the run.
 * Drain USB callbacks and join the worker before reinitializing or freeing the
 * bridge or sink context. */
void dt181_capture_bridge_init_decoder(dt181_capture_bridge *,
                                       const uint8_t context[32], ts_sink,
                                       void *, ts_check *);
int stream_fail(dt181_capture_bridge *, int);
int enqueue(void *, const uint8_t *, size_t);
void *worker(void *);

#endif
