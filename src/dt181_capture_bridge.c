/* SPDX-License-Identifier: MIT */
#include "dt181_capture_bridge.h"

#include <stdio.h>
#include <string.h>

#include "pixy.h"

int stream_fail(dt181_capture_bridge *s, int code) {
    os_mutex_lock(&s->mutex);
    if (!s->failure_exit) s->failure_exit = code;
    s->error = 1;
    os_mutex_unlock(&s->mutex);
    return -1;
}

void dt181_capture_bridge_init_decoder(dt181_capture_bridge *s,
                                       const uint8_t context[32], ts_sink sink,
                                       void *user, ts_check *check) {
    ts_decoder_init(&s->decoder, context, sink, user, check);
}

int enqueue(void *u, const uint8_t *p, size_t n) {
    dt181_capture_bridge *s = u;
    os_mutex_lock(&s->mutex);
    if (n > 32768 || s->count == DT181_CAPTURE_QUEUE_SLOTS || s->error) {
        if (!s->error)
            fprintf(stderr, "USB receive queue rejected input: queued=%u/%u bytes=%zu\n", s->count,
                    DT181_CAPTURE_QUEUE_SLOTS, n);
        s->error = 1;
        os_cond_signal(&s->cond);
        os_mutex_unlock(&s->mutex);
        return -1;
    }
    unsigned at = (s->head + s->count) % DT181_CAPTURE_QUEUE_SLOTS;
    memcpy(s->bytes[at], p, n);
    s->lengths[at] = n;
    s->received_ms[at] = now_ms();
    s->count++;
    if (s->count > s->high_water)
        s->high_water = s->count;
    s->usb_bytes += n;
    os_cond_signal(&s->cond);
    os_mutex_unlock(&s->mutex);
    return 0;
}

void *worker(void *u) {
    dt181_capture_bridge *s = u;
    uint8_t data[32768];
    for (;;) {
        os_mutex_lock(&s->mutex);
        while (!s->count && !s->done && !s->error)
            os_cond_wait(&s->cond, &s->mutex);
        if (s->error || (!s->count && s->done)) {
            os_mutex_unlock(&s->mutex);
            break;
        }
        size_t n = s->lengths[s->head];
        uint64_t received = s->received_ms[s->head];
        unsigned queued = s->count, high_water = s->high_water;
        memcpy(data, s->bytes[s->head], n);
        s->head = (s->head + 1) % DT181_CAPTURE_QUEUE_SLOTS;
        s->count--;
        os_mutex_unlock(&s->mutex);
        int failed =
            diag_before(s->diag, &s->decoder, &s->check, data, n, received, queued, high_water);
        if (failed) stream_fail(s, 70);
        if (!failed) {
            failed = ts_decoder_feed(&s->decoder, data, n);
            if (failed) stream_fail(s, 8);
        }
        if (diag_after(s->diag, &s->decoder, &s->check, failed))
            failed = stream_fail(s, 70);
        if (failed) {
            if (s->decoder.error)
                fprintf(stderr, "TS decoder: %s\n", s->decoder.error);
            os_mutex_lock(&s->mutex);
            s->error = 1;
            os_mutex_unlock(&s->mutex);
            break;
        }
    }
    return NULL;
}
