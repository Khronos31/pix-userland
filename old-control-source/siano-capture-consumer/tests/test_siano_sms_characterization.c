/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "siano-device.h"
#include "protocol.h"
#include "output-writer.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static struct siano_device *active_device;
static uint8_t captured[3 * SIANO_OUTPUT_PACKET];
static size_t captured_length;
static unsigned completion_count;

struct libusb_transfer *LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
    (void)iso_packets;
    return calloc(1, sizeof(struct libusb_transfer));
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *transfer)
{
    free(transfer);
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *transfer)
{
    if (!active_device)
        active_device = ((struct siano_transfer_context *)transfer->user_data)->device;
    return 0;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *transfer)
{
    transfer->status = LIBUSB_TRANSFER_CANCELLED;
    return 0;
}

int LIBUSB_CALL libusb_handle_events_timeout(libusb_context *ctx,
                                             struct timeval *tv)
{
    (void)ctx;
    (void)tv;
    for (size_t i = 0; i < MAX_URBS; i++) {
        struct libusb_transfer *transfer = active_device->transfers[i];
        if (transfer && transfer->status == LIBUSB_TRANSFER_CANCELLED) {
            transfer->callback(transfer);
            return 0;
        }
    }
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
    nanosleep(&delay, NULL);
    return 0;
}

const char *LIBUSB_CALL libusb_error_name(int error_code)
{
    (void)error_code;
    return "fake libusb error";
}

const char *LIBUSB_CALL libusb_strerror(enum libusb_error error_code)
{
    (void)error_code;
    return "fake libusb error";
}

int LIBUSB_CALL libusb_release_interface(libusb_device_handle *handle,
                                         int interface_number)
{
    (void)handle;
    (void)interface_number;
    return 0;
}

void LIBUSB_CALL libusb_close(libusb_device_handle *handle)
{
    (void)handle;
}

static int64_t capture_write(const uint8_t *data, size_t length)
{
    assert(captured_length + length <= sizeof(captured));
    memcpy(captured + captured_length, data, length);
    captured_length += length;
    return (int)length;
}

static void deliver_bda(struct libusb_transfer *transfer,
                        const uint8_t *data, size_t length)
{
    sms_pack_header(transfer->buffer, MSG_SMS_DVBT_BDA_DATA, SMS_HIF_TASK, 0,
                    (uint16_t)(SMS_HEADER_SIZE + length), 0);
    memcpy(transfer->buffer + SMS_HEADER_SIZE, data, length);
    transfer->actual_length = (int)(SMS_HEADER_SIZE + length);
    transfer->status = LIBUSB_TRANSFER_COMPLETED;
    completion_count++;
    transfer->callback(transfer);
}

int main(void)
{
    struct siano_device *device = calloc(1, sizeof(*device));
    struct siano_output_pump pump;
    uint8_t packets[3 * SIANO_OUTPUT_PACKET];
    uint8_t chunk[USB_TRANSFER_SIZE];
    size_t length = 0;
    uint64_t drops = 0;
    int fds[2];

    assert(device != NULL);
    for (size_t i = 0; i < sizeof(packets); i++)
        packets[i] = (uint8_t)(i * 13U + 7U);
    for (size_t i = 0; i < sizeof(packets); i += SIANO_OUTPUT_PACKET) {
        packets[i] = 0x47;
        packets[i + 1] = (uint8_t)(i / SIANO_OUTPUT_PACKET);
    }
    assert(init_device_state(device, NULL, false) == 0);
    device->handle = (libusb_device_handle *)(uintptr_t)1;
    device->interface_number = 0;
    device->response_alignment = 0;
    active_device = device;
    assert(start_streaming(device) == 0);
    deliver_bda(device->transfers[0], packets, 187);
    deliver_bda(device->transfers[0], packets + 187,
                sizeof(packets) - 187);

    assert(pipe(fds) == 0);
    siano_output_test_set_write_hook(capture_write);
    assert(siano_output_pump_init(&pump, fds[1]) == 0);
    assert(ts_pop(&device->ts, chunk, &length) == 0);
    siano_output_pump_feed(&pump, chunk, length);
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    assert(ts_pop(&device->ts, chunk, &length) == 0);
    siano_output_pump_feed(&pump, chunk, length);
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    assert(captured_length == sizeof(packets));
    assert(memcmp(captured, packets, sizeof(packets)) == 0);
    siano_output_pump_destroy(&pump);
    close(fds[0]);
    close(fds[1]);
    close_device(device, &drops);
    printf("completion_count=%u;drops=%llu;bytes=", completion_count,
           (unsigned long long)drops);
    for (size_t i = 0; i < captured_length; i++)
        printf("%02x", captured[i]);
    puts("");
    free(device);
    return 0;
}
