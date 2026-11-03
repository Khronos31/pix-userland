#include "dt295_stream.h"

#include <stdio.h>

static int fail(const char *name)
{
    fprintf(stderr, "I1 failed: %s\n", name);
    return 1;
}

int main(void)
{
    dt295_stream *stream = dt295_stream_test_create(8U);
    if (stream == NULL)
        return fail("create");

    unsigned char buffer[DT295_STREAM_TRANSFER_BYTES];
    uint64_t sequence = 0U;
    uint64_t completed_ns = 0U;

    if (dt295_stream_test_complete(stream, 1U, LIBUSB_TRANSFER_COMPLETED, 188, 0x22U) != 0 ||
        dt295_stream_read(stream, buffer, sizeof(buffer), &sequence, &completed_ns, 0U) != 0)
        return fail("wait for earlier sequence");

    if (dt295_stream_test_complete(stream, 0U, LIBUSB_TRANSFER_COMPLETED, 188, 0x11U) != 0 ||
        dt295_stream_read(stream, buffer, sizeof(buffer), &sequence, &completed_ns, 0U) != 188 ||
        sequence != 0U || buffer[0] != 0x11U || completed_ns == 0U)
        return fail("read sequence zero");

    if (dt295_stream_read(stream, buffer, sizeof(buffer), &sequence, &completed_ns, 0U) != 188 ||
        sequence != 1U || buffer[0] != 0x22U)
        return fail("read sequence one");

    if (dt295_stream_test_complete(stream, 1U, LIBUSB_TRANSFER_TIMED_OUT, 17, 0x31U) != 0 ||
        dt295_stream_read(stream, buffer, sizeof(buffer), &sequence, &completed_ns, 0U) != 17 ||
        sequence != 2U || buffer[16] != 0x31U)
        return fail("read partial timeout bytes");

    dt295_stream_test_mark_stopped(stream);
    if (dt295_stream_destroy(stream) != 0)
        return fail("synthetic cleanup");

    puts("I1 fake cases=3 passed; skipped=0; lifecycle=not-tested");
    return 0;
}
