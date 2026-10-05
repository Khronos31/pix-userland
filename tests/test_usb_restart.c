/* SPDX-License-Identifier: MIT */
/* Exercise the real backend lifetime logic with deterministic USB completions.
 * Device discovery/claiming are never invoked. */
#include <assert.h>
#include <libusb.h>
#include "usb_log.h"
static struct libusb_transfer *LIBUSB_CALL test_alloc(int);
static void LIBUSB_CALL test_free(struct libusb_transfer *);
static int LIBUSB_CALL test_submit(struct libusb_transfer *);
static int LIBUSB_CALL test_cancel(struct libusb_transfer *);
static int LIBUSB_CALL test_events(libusb_context *, struct timeval *);
static usb_log *test_log_open(usb_log_writer, void *);
static int test_log_submit(usb_log *, const usb_log_record *);
static int test_log_close(usb_log *, uint64_t *);
#define libusb_alloc_transfer test_alloc
#define libusb_free_transfer test_free
#define libusb_submit_transfer test_submit
#define libusb_cancel_transfer test_cancel
#define libusb_handle_events_timeout test_events
#define usb_log_open test_log_open
#define usb_log_submit test_log_submit
#define usb_log_close test_log_close
#include "../src/usb_libusb.c"
#undef libusb_alloc_transfer
#undef libusb_free_transfer
#undef libusb_submit_transfer
#undef libusb_cancel_transfer
#undef libusb_handle_events_timeout
#undef usb_log_open
#undef usb_log_submit
#undef usb_log_close
static struct {
    struct libusb_transfer *t;
    int pending, cancelled;
} transfers[16];
static unsigned allocations, frees, submits, log_opens, log_closes, log_records;
static unsigned fail_submit_at;
static int event_failure, logger_failure;
static int logger_marker;
static unsigned slot(struct libusb_transfer *t) {
    for (unsigned i = 0; i < 16; i++)
        if (transfers[i].t == t)
            return i;
    assert(0);
    return 0;
}
static struct libusb_transfer *LIBUSB_CALL test_alloc(int iso) {
    assert(!iso);
    for (unsigned i = 0; i < 16; i++) {
        if (!transfers[i].t) {
            transfers[i].t = calloc(1, sizeof(struct libusb_transfer));
            assert(transfers[i].t);
            allocations++;
            return transfers[i].t;
        }
    }
    assert(0);
    return NULL;
}
static void LIBUSB_CALL test_free(struct libusb_transfer *t) {
    unsigned i = slot(t);
    assert(!transfers[i].pending);
    free(t);
    memset(&transfers[i], 0, sizeof(transfers[i]));
    frees++;
}
static int LIBUSB_CALL test_submit(struct libusb_transfer *t) {
    unsigned i = slot(t);
    assert(!transfers[i].pending);
    if (++submits == fail_submit_at)
        return LIBUSB_ERROR_IO;
    transfers[i].pending = 1;
    transfers[i].cancelled = 0;
    return 0;
}
static int LIBUSB_CALL test_cancel(struct libusb_transfer *t) {
    unsigned i = slot(t);
    if (!transfers[i].pending)
        return LIBUSB_ERROR_NOT_FOUND;
    transfers[i].cancelled = 1;
    return 0;
}
static int LIBUSB_CALL test_events(libusb_context *ctx, struct timeval *timeout) {
    (void)ctx;
    (void)timeout;
    if (event_failure) {
        event_failure = 0;
        return LIBUSB_ERROR_IO;
    }
    for (unsigned i = 0; i < 16; i++) {
        if (!transfers[i].pending)
            continue;
        struct libusb_transfer *t = transfers[i].t;
        t->status = transfers[i].cancelled ? LIBUSB_TRANSFER_CANCELLED : LIBUSB_TRANSFER_COMPLETED;
        t->actual_length = transfers[i].cancelled ? 0 : t->length;
        transfers[i].pending = 0;
        t->callback(t);
        return 0;
    }
    return 0;
}
static usb_log *test_log_open(usb_log_writer writer, void *user) {
    (void)writer;
    (void)user;
    assert(log_opens == log_closes);
    log_opens++;
    return (usb_log *)&logger_marker;
}
static int test_log_submit(usb_log *log, const usb_log_record *record) {
    assert(log == (usb_log *)&logger_marker && log_opens == log_closes + 1);
    (void)record;
    log_records++;
    return 0;
}
static int test_log_close(usb_log *log, uint64_t *dropped) {
    assert(log == (usb_log *)&logger_marker && log_opens == log_closes + 1);
    log_closes++;
    *dropped = 0;
    return logger_failure ? -1 : 0;
}
static int receive(void *user, const uint8_t *bytes, size_t size) {
    (void)bytes;
    assert(size == 32768);
    (*(unsigned *)user)++;
    return 0;
}
static void stopped(usb_backend *b) {
    assert(!b->pending && !b->log && !b->sink && !b->user);
    assert(allocations == frees && log_opens == log_closes);
    for (unsigned i = 0; i < 16; i++)
        assert(!b->transfers[i]);
}
int main(void) {
    usb_backend *b = calloc(1, sizeof(*b));
    assert(b);
    b->diagnostic = 1;
    unsigned received = 0;
    for (unsigned run = 0; run < 3; run++) {
        assert(!usb_stream_start(b, receive, &received));
        assert(b->pending == 16 && !b->stopping && !b->last_ms);
        unsigned allocated = allocations;
        assert(usb_stream_start(b, receive, &received) < 0);
        assert(allocated == allocations && !b->error);
        for (unsigned i = 0; i < 20; i++)
            assert(!usb_poll(b, 1));
        assert(received == (run + 1) * 20 && b->pending == 16);
        assert(!usb_stream_stop(b));
        stopped(b);
        assert(!usb_stream_stop(b));
        stopped(b);
    }
    assert(log_records);
    fail_submit_at = submits + 4;
    assert(usb_stream_start(b, receive, &received) < 0);
    assert(b->pending == 3 && b->error);
    assert(usb_stream_stop(b) < 0);
    stopped(b);
    assert(usb_stream_start(b, receive, &received) < 0);
    usb_close(b);
    b = calloc(1, sizeof(*b));
    assert(b);
    assert(!usb_stream_start(b, receive, &received));
    event_failure = 1;
    assert(usb_poll(b, 1) < 0 && b->error);
    assert(usb_stream_stop(b) < 0);
    stopped(b);
    assert(usb_stream_start(b, receive, &received) < 0);
    usb_close(b);
    b = calloc(1, sizeof(*b));
    assert(b);
    b->diagnostic = 1;
    assert(!usb_stream_start(b, receive, &received));
    logger_failure = 1;
    assert(usb_stream_stop(b) < 0 && b->error);
    stopped(b);
    assert(usb_stream_start(b, receive, &received) < 0);
    usb_close(b);
    puts("USB backend restart, transfer lifetime and fail-closed tests passed");
    return 0;
}
