/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../stream-state.h"

#include <assert.h>
#include <errno.h>
#include <libusb.h>
#include <stdio.h>

static void test_transfer_status_classification(void)
{
    assert(siano_transfer_classify_status(LIBUSB_TRANSFER_COMPLETED, false) ==
           SIANO_TRANSFER_COMPLETE);
    assert(siano_transfer_classify_status(LIBUSB_TRANSFER_TIMED_OUT, false) ==
           SIANO_TRANSFER_RETRY);
    assert(siano_transfer_classify_status(LIBUSB_TRANSFER_ERROR, false) ==
           SIANO_TRANSFER_FATAL);
    assert(siano_transfer_classify_status(LIBUSB_TRANSFER_NO_DEVICE, false) ==
           SIANO_TRANSFER_FATAL);
    assert(siano_transfer_classify_status(LIBUSB_TRANSFER_STALL, false) ==
           SIANO_TRANSFER_FATAL);
    assert(siano_transfer_classify_status(LIBUSB_TRANSFER_OVERFLOW, false) ==
           SIANO_TRANSFER_FATAL);
    assert(siano_transfer_classify_status(LIBUSB_TRANSFER_CANCELLED, true) ==
           SIANO_TRANSFER_DRAIN);
    assert(siano_transfer_classify_status(LIBUSB_TRANSFER_CANCELLED, false) ==
           SIANO_TRANSFER_FATAL);

    assert(siano_transfer_status_error(LIBUSB_TRANSFER_ERROR) ==
           LIBUSB_ERROR_IO);
    assert(siano_transfer_status_error(LIBUSB_TRANSFER_NO_DEVICE) ==
           LIBUSB_ERROR_NO_DEVICE);
    assert(siano_transfer_status_error(LIBUSB_TRANSFER_STALL) ==
           LIBUSB_ERROR_PIPE);
    assert(siano_transfer_status_error(LIBUSB_TRANSFER_OVERFLOW) ==
           LIBUSB_ERROR_OVERFLOW);
    assert(siano_transfer_status_error(LIBUSB_TRANSFER_CANCELLED) ==
           LIBUSB_ERROR_INTERRUPTED);
    assert(siano_transfer_status_error(999) == LIBUSB_ERROR_OTHER);
}

static void test_stream_disconnect_normalization(void)
{
    assert(siano_stream_disconnect_error(LIBUSB_ERROR_NO_DEVICE) == LIBUSB_ERROR_NO_DEVICE);
    assert(siano_stream_disconnect_error(LIBUSB_ERROR_IO) == LIBUSB_ERROR_NO_DEVICE);
    assert(siano_stream_disconnect_error(LIBUSB_ERROR_PIPE) == LIBUSB_ERROR_NO_DEVICE);
    assert(siano_stream_disconnect_error(LIBUSB_ERROR_TIMEOUT) == LIBUSB_ERROR_TIMEOUT);
}

static void test_pid_ack_policy(void)
{
    assert(!siano_pid_filter_requires_ack(0x2000));
    assert(siano_pid_filter_requires_ack(0x0000));
    assert(siano_pid_filter_requires_ack(0x1fff));
}

static void test_disconnect_propagates_once(void)
{
    struct siano_stream_state state;

    assert(siano_stream_state_init(&state) == 0);
    assert(!siano_stream_state_is_stopping(&state));
    assert(siano_stream_state_error(&state) == 0);

    assert(siano_stream_state_fail(&state, LIBUSB_ERROR_NO_DEVICE));
    assert(siano_stream_state_is_stopping(&state));
    assert(siano_stream_state_error(&state) == LIBUSB_ERROR_NO_DEVICE);
    assert(siano_stream_state_stream_result(&state, -EAGAIN) ==
           LIBUSB_ERROR_NO_DEVICE);

    /* Peer callbacks after disconnect cannot overwrite the original error. */
    assert(!siano_stream_state_fail(&state, LIBUSB_ERROR_IO));
    assert(siano_stream_state_error(&state) == LIBUSB_ERROR_NO_DEVICE);
    siano_stream_state_destroy(&state);
}

static void test_async_io_and_pipe_are_disconnects(void)
{
    const int errors[] = {LIBUSB_ERROR_IO, LIBUSB_ERROR_PIPE};

    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        struct siano_stream_state state;
        assert(siano_stream_state_init(&state) == 0);
        assert(siano_stream_state_fail(&state, errors[i]));
        assert(siano_stream_state_error(&state) == LIBUSB_ERROR_NO_DEVICE);
        siano_stream_state_destroy(&state);
    }
}

static void test_graceful_stop_has_no_failure(void)
{
    struct siano_stream_state state;

    assert(siano_stream_state_init(&state) == 0);
    siano_stream_state_stop(&state);
    assert(siano_stream_state_is_stopping(&state));
    assert(siano_stream_state_error(&state) == 0);
    assert(siano_stream_state_stream_result(&state, -EAGAIN) == -EAGAIN);
    assert(!siano_stream_state_fail(&state, LIBUSB_ERROR_NO_DEVICE));
    assert(siano_stream_state_error(&state) == 0);
    siano_stream_state_destroy(&state);
}

int main(void)
{
    test_transfer_status_classification();
    test_stream_disconnect_normalization();
    test_pid_ack_policy();
    test_disconnect_propagates_once();
    test_async_io_and_pipe_are_disconnects();
    test_graceful_stop_has_no_failure();
    puts("stream state tests: PASS");
    return 0;
}
