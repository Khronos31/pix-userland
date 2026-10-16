#ifdef _WIN32
#include <stdio.h>
int main(void) {
    puts("DT295 async stream: skipped (POSIX implementation only)");
    return 0;
}
#else
#include "dt295_stream.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static dt295_stream *make(unsigned blocks) {
    dt295_stream *s = dt295_stream_test_create(blocks);
    assert(s);
    return s;
}
static void dispose(dt295_stream *s) {
    dt295_stream_test_mark_stopped(s);
    assert(dt295_stream_destroy(s) == 0);
}
static int complete(dt295_stream *s, unsigned slot, enum libusb_transfer_status status, int length,
                    unsigned char value) {
    return dt295_stream_test_complete(s, slot, status, length, value);
}
int main(void) {
    unsigned char buffer[DT295_STREAM_TRANSFER_BYTES];
    uint64_t seq = 0, when = 0;
    dt295_stream_stats stats;

    dt295_stream *s = make(8);
    assert(!complete(s, 1, LIBUSB_TRANSFER_COMPLETED, 188, 0x22));
    assert(dt295_stream_read(s, buffer, sizeof(buffer), &seq, &when, 0) == 0);
    assert(!complete(s, 0, LIBUSB_TRANSFER_COMPLETED, 188, 0x11));
    assert(dt295_stream_read(s, buffer, sizeof(buffer), &seq, &when, 0) == 188);
    assert(seq == 0 && buffer[0] == 0x11 && when != 0);
    assert(dt295_stream_read(s, buffer, sizeof(buffer), &seq, &when, 0) == 188);
    assert(seq == 1 && buffer[0] == 0x22);
    dt295_stream_snapshot(s, &stats);
    assert(stats.delivered == 2 && stats.completed == 2 && stats.pending_blocks == 2);
    dispose(s);

    s = make(8);
    assert(!complete(s, 0, LIBUSB_TRANSFER_TIMED_OUT, 17, 0x31));
    assert(dt295_stream_read(s, buffer, sizeof(buffer), &seq, &when, 0) == 17);
    assert(buffer[16] == 0x31);
    dt295_stream_snapshot(s, &stats);
    assert(stats.timeouts == 1 && stats.delivered_bytes == 17);
    dispose(s);

    s = make(8);
    assert(!complete(s, 0, LIBUSB_TRANSFER_COMPLETED, 188, 0x44));
    assert(!complete(s, 1, LIBUSB_TRANSFER_ERROR, 0, 0));
    dt295_stream_snapshot(s, &stats);
    int first = stats.first_error;
    assert(first == LIBUSB_ERROR_IO);
    assert(dt295_stream_read(s, buffer, sizeof(buffer), &seq, &when, 0) == 188);
    assert(buffer[0] == 0x44);
    assert(dt295_stream_read(s, buffer, sizeof(buffer), &seq, &when, 0) == first);
    assert(!complete(s, 0, LIBUSB_TRANSFER_COMPLETED, 19, 0x55));
    dt295_stream_snapshot(s, &stats);
    assert(stats.first_error == first && stats.late_actual_bytes == 19 &&
           stats.late_callbacks == 1);
    dispose(s);

    s = make(4);
    assert(!complete(s, 0, LIBUSB_TRANSFER_COMPLETED, 1, 0x60));
    assert(!complete(s, 1, LIBUSB_TRANSFER_COMPLETED, 1, 0x61));
    assert(!complete(s, 0, LIBUSB_TRANSFER_COMPLETED, 1, 0x62));
    dt295_stream_snapshot(s, &stats);
    assert(stats.first_error == LIBUSB_ERROR_NO_MEM && stats.peak_pending_blocks == 4);
    for (unsigned i = 0; i < 3; ++i) {
        assert(dt295_stream_read(s, buffer, sizeof(buffer), &seq, &when, 0) == 1);
        assert(seq == i && buffer[0] == (unsigned char)(0x60 + i));
    }
    dispose(s);

    /* An unjoined event owner keeps transfer storage alive after stop deadlines. */
    s = make(8);
    assert(dt295_stream_destroy(s) == DT295_STREAM_ABANDONED);
    assert(dt295_stream_stop(s, 0) == DT295_STREAM_ABANDONED);
    assert(dt295_stream_stop(s, 2) == DT295_STREAM_ABANDONED);
    assert(!complete(s, 0, LIBUSB_TRANSFER_COMPLETED, 37, 0x71));
    dt295_stream_snapshot(s, &stats);
    assert(stats.stopping && !stats.thread_done && stats.late_callbacks == 1 &&
           stats.late_actual_bytes == 37);
    assert(dt295_stream_destroy(s) == DT295_STREAM_ABANDONED);
    dt295_stream_test_mark_stopped(s);
    assert(dt295_stream_destroy(s) == 0);

    puts("DT295 synthetic stream cases passed: order, partial timeout, error prefix, bounded ring, "
         "first error, late-byte accounting, stop deadline ownership");
    return 0;
}
#endif
