/* SPDX-License-Identifier: MIT */
#include <libusb.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

int pix_test_libusb_handle_events_timeout(libusb_context *, struct timeval *);

static int fake_context_storage;
static int fake_device_storage[2];
static int fake_handle_storage;
static struct libusb_transfer *active[32];
static unsigned active_count;
static uint8_t queued[65536];
static size_t queued_length;
static size_t queued_offset;
static unsigned submissions;
static unsigned completions;
static unsigned cancellations;
static unsigned frees;
static int fail_cancel;
unsigned pix_test_usb_device_index(libusb_device *device);
unsigned pix_test_usb_opened_device(void);
void pix_test_usb_select_fd_model(unsigned);

void pix_test_usb_queue(const uint8_t *bytes, size_t length)
{
    if (length > sizeof(queued) - queued_length) return;
    memcpy(queued + queued_length, bytes, length);
    queued_length += length;
}

void pix_test_usb_fail_cancel(void) { fail_cancel = 1; }
unsigned pix_test_usb_submissions(void) { return submissions; }
unsigned pix_test_usb_completions(void) { return completions; }
unsigned pix_test_usb_cancellations(void) { return cancellations; }
unsigned pix_test_usb_frees(void) { return frees; }

int LIBUSB_CALL libusb_init(libusb_context **ctx)
{ *ctx = (libusb_context *)&fake_context_storage; return 0; }
void LIBUSB_CALL libusb_exit(libusb_context *ctx) { (void)ctx; }
int LIBUSB_CALL libusb_set_option(libusb_context *ctx, enum libusb_option option, ...)
{ (void)ctx; (void)option; return 0; }
int LIBUSB_CALL libusb_wrap_sys_device(libusb_context *ctx, intptr_t fd,
                                       libusb_device_handle **handle)
{
    (void)ctx;
    const char *model = getenv("PIX_TEST_USB_FD_MODEL");
    const unsigned index = model != NULL && strcmp(model, "dt295") == 0 ? 1U : 0U;
    pix_test_usb_select_fd_model(index);
    const char *path = getenv("PIX_TEST_USB_FD_LOG");
    if (path != NULL) {
        FILE *file = fopen(path, "a");
        if (file != NULL) {
            const int flags = fcntl((int)fd, F_GETFD);
            fprintf(file, "wrap fd=%ld cloexec=%d\n", (long)fd,
                    flags >= 0 && (flags & FD_CLOEXEC) != 0);
            fclose(file);
        }
    }
    *handle = (libusb_device_handle *)&fake_handle_storage;
    return 0;
}
libusb_device * LIBUSB_CALL libusb_get_device(libusb_device_handle *handle)
{ (void)handle; return (libusb_device *)&fake_device_storage[pix_test_usb_opened_device()]; }
int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *device,
                                              struct libusb_device_descriptor *desc)
{
    const unsigned index = device == (libusb_device *)&fake_device_storage[0] ||
        device == (libusb_device *)&fake_device_storage[1]
            ? pix_test_usb_opened_device() : pix_test_usb_device_index(device);
    memset(desc, 0, sizeof(*desc));
    const char *model = getenv("PIX_TEST_USB_FD_MODEL");
    if (model != NULL && strcmp(model, "invalid") == 0) return LIBUSB_ERROR_IO;
    desc->idVendor = model != NULL && strcmp(model, "wrong-vid") == 0 ? 0x1234 : 0x06b8;
    desc->idProduct = index == 0U ? 0x102b : 0x103b;
    desc->iSerialNumber = 1U;
    return 0;
}
int LIBUSB_CALL libusb_kernel_driver_active(libusb_device_handle *handle, int interface)
{ (void)handle; (void)interface; return 0; }
int LIBUSB_CALL libusb_set_configuration(libusb_device_handle *handle, int config)
{ (void)handle; (void)config; return 0; }
int LIBUSB_CALL libusb_attach_kernel_driver(libusb_device_handle *handle, int interface)
{ (void)handle; (void)interface; return 0; }
int LIBUSB_CALL libusb_detach_kernel_driver(libusb_device_handle *handle, int interface)
{ (void)handle; (void)interface; return 0; }
int LIBUSB_CALL libusb_get_configuration(libusb_device_handle *handle, int *config)
{ (void)handle; *config = 1; return 0; }
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *handle, int interface)
{ (void)handle; (void)interface; return 0; }
int LIBUSB_CALL libusb_release_interface(libusb_device_handle *handle, int interface)
{ (void)handle; (void)interface; return 0; }
void LIBUSB_CALL libusb_close(libusb_device_handle *handle) { (void)handle; }
int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint,
                                     unsigned char *data, int length, int *actual,
                                     unsigned timeout)
{ (void)handle; (void)endpoint; (void)data; (void)timeout; *actual = length; return 0; }
const char * LIBUSB_CALL libusb_error_name(int error) { (void)error; return "offline-loopback"; }

#ifdef _WIN32
/* Resolve every static libusb reference without pulling real USB backends. */
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *handle, uint8_t type,
    uint8_t request, uint16_t value, uint16_t index, unsigned char *data,
    uint16_t length, unsigned int timeout)
{
    (void)handle; (void)type; (void)request; (void)value; (void)index;
    (void)data; (void)length; (void)timeout;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}
int LIBUSB_CALL libusb_interrupt_transfer(libusb_device_handle *handle, unsigned char endpoint,
    unsigned char *data, int length, int *actual, unsigned int timeout)
{
    (void)handle; (void)endpoint; (void)data; (void)length; (void)timeout;
    *actual = 0;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}
#endif

int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context *ctx,
    struct timeval *timeout, int *completed)
{
    if (completed && *completed) return 0;
    return pix_test_libusb_handle_events_timeout(ctx, timeout);
}

struct libusb_transfer * LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{ (void)iso_packets; return calloc(1, sizeof(struct libusb_transfer)); }
void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *transfer)
{ ++frees; fprintf(stderr, "NATIVE transfer_free total=%u\n", frees); free(transfer); }
int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *transfer)
{
    for (unsigned i = 0; i < active_count; ++i)
        if (active[i] == transfer) { ++submissions; return 0; }
    if (active_count >= sizeof(active) / sizeof(active[0])) return LIBUSB_ERROR_NO_MEM;
    transfer->user_data = transfer->user_data;
    active[active_count++] = transfer;
    ++submissions;
    fprintf(stderr, "NATIVE submit callback_registered=1 total=%u\n", submissions);
    return 0;
}
int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *transfer)
{
    if (fail_cancel) {
        fail_cancel = 0;
        transfer->flags |= 0x80;
        fprintf(stderr, "PIX cleanup_error usb_cancel\n");
        return LIBUSB_ERROR_IO;
    }
    transfer->flags |= 0x80;
    ++cancellations;
    return 0;
}
int pix_test_libusb_handle_events_timeout(libusb_context *ctx, struct timeval *timeout)
{
    (void)ctx; (void)timeout;
    for (unsigned i = 0; i < active_count; ++i) {
        struct libusb_transfer *transfer = active[i];
        const int cancelled = (transfer->flags & 0x80) != 0;
        if (!cancelled && queued_offset == queued_length) continue;
        const size_t left = queued_length - queued_offset;
        const size_t amount = left < (size_t)transfer->length ? left : (size_t)transfer->length;
        if (amount) memcpy(transfer->buffer, queued + queued_offset, amount);
        queued_offset += amount;
        transfer->actual_length = (int)amount;
        transfer->status = cancelled ? LIBUSB_TRANSFER_CANCELLED : LIBUSB_TRANSFER_COMPLETED;
        ++completions;
        if (cancelled) fprintf(stderr, "NATIVE late_callback status=cancelled bytes=%zu\n", amount);
        transfer->callback(transfer);
        if (cancelled) {
            active[i] = active[--active_count];
            --i;
        }
        if (queued_offset == queued_length) { queued_offset = queued_length = 0; }
        return 0;
    }
    return 0;
}
int LIBUSB_CALL libusb_handle_events_timeout(libusb_context *ctx, struct timeval *timeout)
{ return pix_test_libusb_handle_events_timeout(ctx, timeout); }
