/* SPDX-License-Identifier: MIT */
/* Exercise the real backend lifetime logic with deterministic USB completions.
 * Device discovery/claiming are never invoked. */
#include <assert.h>
#ifdef _WIN32
#include <limits.h>
#include <windows.h>
#else
#include <fcntl.h>
#endif
#include <libusb.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "usb_log.h"
static struct libusb_transfer *LIBUSB_CALL test_alloc(int);
static void LIBUSB_CALL test_free(struct libusb_transfer *);
static int LIBUSB_CALL test_submit(struct libusb_transfer *);
static int LIBUSB_CALL test_cancel(struct libusb_transfer *);
static int LIBUSB_CALL test_events(libusb_context *, struct timeval *);
static usb_log *test_log_open(usb_log_writer, void *);
static int test_log_submit(usb_log *, const usb_log_record *);
static int test_log_close(usb_log *, uint64_t *);
static int LIBUSB_CALL test_init(libusb_context **);
static void LIBUSB_CALL test_exit(libusb_context *);
static int LIBUSB_CALL test_option(libusb_context *, enum libusb_option, ...);
static ssize_t LIBUSB_CALL test_list(libusb_context *, libusb_device ***);
static void LIBUSB_CALL test_free_list(libusb_device **, int);
static int LIBUSB_CALL test_descriptor(libusb_device *, struct libusb_device_descriptor *);
static uint8_t LIBUSB_CALL test_bus(libusb_device *);
static uint8_t LIBUSB_CALL test_address(libusb_device *);
static int LIBUSB_CALL test_ports(libusb_device *, uint8_t *, int);
static int LIBUSB_CALL test_open(libusb_device *, libusb_device_handle **);
static int LIBUSB_CALL test_string_ascii(libusb_device_handle *, uint8_t, unsigned char *, int);
static int LIBUSB_CALL test_wrap(libusb_context *, intptr_t, libusb_device_handle **);
static libusb_device *LIBUSB_CALL test_device(libusb_device_handle *);
static int LIBUSB_CALL test_active(libusb_device_handle *, int);
static int LIBUSB_CALL test_interface(libusb_device_handle *, int);
static int LIBUSB_CALL test_get_config(libusb_device_handle *, int *);
static int LIBUSB_CALL test_claim(libusb_device_handle *, int);
static void LIBUSB_CALL test_close(libusb_device_handle *);
static int LIBUSB_CALL test_bulk(libusb_device_handle *, unsigned char, unsigned char *, int, int *, unsigned);
static void *test_calloc(size_t, size_t);
#define libusb_init test_init
#define libusb_exit test_exit
#define libusb_set_option test_option
#define libusb_get_device_list test_list
#define libusb_free_device_list test_free_list
#define libusb_get_device_descriptor test_descriptor
#define libusb_get_bus_number test_bus
#define libusb_get_device_address test_address
#define libusb_get_port_numbers test_ports
#define libusb_open test_open
#define libusb_wrap_sys_device test_wrap
#define libusb_get_device test_device
#define libusb_kernel_driver_active test_active
#define libusb_detach_kernel_driver test_interface
#define libusb_attach_kernel_driver test_interface
#define libusb_release_interface test_interface
#define libusb_set_configuration test_interface
#define libusb_get_configuration test_get_config
#define libusb_claim_interface test_claim
#define libusb_close test_close
#define libusb_get_string_descriptor_ascii test_string_ascii
#define libusb_bulk_transfer test_bulk
#define calloc test_calloc
#define libusb_alloc_transfer test_alloc
#define libusb_free_transfer test_free
#define libusb_submit_transfer test_submit
#define libusb_cancel_transfer test_cancel
#define libusb_handle_events_timeout test_events
#define usb_log_open test_log_open
#define usb_log_submit test_log_submit
#define usb_log_close test_log_close
#include "../src/usb_libusb.c"
#undef libusb_init
#undef libusb_exit
#undef libusb_set_option
#undef libusb_get_device_list
#undef libusb_free_device_list
#undef libusb_get_device_descriptor
#undef libusb_get_bus_number
#undef libusb_get_device_address
#undef libusb_get_port_numbers
#undef libusb_open
#undef libusb_wrap_sys_device
#undef libusb_get_device
#undef libusb_kernel_driver_active
#undef libusb_detach_kernel_driver
#undef libusb_attach_kernel_driver
#undef libusb_release_interface
#undef libusb_set_configuration
#undef libusb_get_configuration
#undef libusb_claim_interface
#undef libusb_close
#undef libusb_get_string_descriptor_ascii
#undef libusb_bulk_transfer
#undef calloc

#undef libusb_alloc_transfer
#undef libusb_free_transfer
#undef libusb_submit_transfer
#undef libusb_cancel_transfer
#undef libusb_handle_events_timeout
#undef usb_log_open
#undef usb_log_submit
#undef usb_log_close
static int descriptor_product = 0x102b;
static int init_result, list_result = 1, descriptor_result, wrong_model, open_result;
static int active_result, claim_result, malloc_failure, bulk_result, short_write;
static int fake_context, fake_handle;
typedef struct { uint16_t product; uint8_t bus, address, port; const char *serial; } fake_usb_device;
static fake_usb_device fake_devices[2] = {{0x102b, 1, 7, 2, NULL}, {0x103b, 1, 8, 3, NULL}};
static unsigned fake_device_count = 1;
static libusb_device *last_opened_device;
static const char *opened_serial;
static int string_result;
static intptr_t wrapped_fd;
#ifdef _WIN32
typedef HANDLE test_fd;
static int test_pipe_create(test_fd fd[2]) {
    return CreatePipe(&fd[0], &fd[1], NULL, 0) ? 0 : -1;
}
static int test_fd_number(test_fd fd) {
    const uintptr_t value = (uintptr_t)fd;
    assert(value > 0 && value <= INT_MAX);
    return (int)value;
}
static int test_fd_is_open(test_fd fd) {
    DWORD flags;
    return GetHandleInformation(fd, &flags) != 0;
}
static void test_fd_close(test_fd fd) { assert(CloseHandle(fd)); }
static int test_fd_ownership_ok(test_fd original, int passed) {
    return wrapped_fd == (intptr_t)passed && test_fd_is_open(original);
}
static int test_invalid_fd_number(void) {
    HANDLE handle = CreateEvent(NULL, FALSE, FALSE, NULL);
    assert(handle);
    const int invalid_fd = test_fd_number(handle);
    assert(CloseHandle(handle));
    assert(!test_fd_is_open(handle));
    return invalid_fd;
}
#else
typedef int test_fd;
static int test_pipe_create(test_fd fd[2]) { return pipe(fd); }
static int test_fd_number(test_fd fd) { return fd; }
static int test_fd_is_open(test_fd fd) { return fcntl(fd, F_GETFD) >= 0; }
static void test_fd_close(test_fd fd) { close(fd); }
static int test_fd_ownership_ok(test_fd original, int passed) {
    return wrapped_fd != passed && test_fd_is_open(original) &&
           !test_fd_is_open((int)wrapped_fd);
}
#endif
static int LIBUSB_CALL test_init(libusb_context **ctx) {
    *ctx = init_result ? NULL : (libusb_context *)&fake_context;
    return init_result;
}
static void LIBUSB_CALL test_exit(libusb_context *ctx) { (void)ctx; }
static int LIBUSB_CALL test_option(libusb_context *ctx, enum libusb_option option, ...) {
    (void)ctx; (void)option; return 0;
}
static ssize_t LIBUSB_CALL test_list(libusb_context *ctx, libusb_device ***out) {
    (void)ctx;
    static libusb_device *devices[3];
    devices[0] = (libusb_device *)&fake_devices[0];
    devices[1] = fake_device_count > 1 ? (libusb_device *)&fake_devices[1] : NULL;
    devices[fake_device_count] = NULL;
    *out = devices;
    return list_result;
}
static void LIBUSB_CALL test_free_list(libusb_device **list, int unref) { (void)list; (void)unref; }
static int LIBUSB_CALL test_descriptor(libusb_device *d, struct libusb_device_descriptor *desc) {
    fake_usb_device *device = (fake_usb_device *)d;
    memset(desc, 0, sizeof(*desc));
    desc->idVendor = wrong_model ? 1 : 0x06b8;
    desc->idProduct = fake_device_count == 1 ? descriptor_product : device->product;
    desc->iSerialNumber = device->serial ? 1 : 0;
    return descriptor_result;
}
static uint8_t LIBUSB_CALL test_bus(libusb_device *d) { return ((fake_usb_device *)d)->bus; }
static uint8_t LIBUSB_CALL test_address(libusb_device *d) { return ((fake_usb_device *)d)->address; }
static int LIBUSB_CALL test_ports(libusb_device *d, uint8_t *p, int n) {
    assert(n >= 1); *p = ((fake_usb_device *)d)->port; return 1;
}
static int LIBUSB_CALL test_open(libusb_device *d, libusb_device_handle **h) {
    last_opened_device = d;
    opened_serial = ((fake_usb_device *)d)->serial;
    *h = open_result ? NULL : (libusb_device_handle *)&fake_handle; return open_result;
}
static int LIBUSB_CALL test_string_ascii(libusb_device_handle *h, uint8_t index,
                                         unsigned char *data, int length) {
    (void)h;
    if (index != 1 || !opened_serial)
        return LIBUSB_ERROR_NOT_FOUND;
    if (string_result)
        return string_result;
    size_t n = strlen(opened_serial);
    if (n >= (size_t)length)
        n = (size_t)length - 1;
    memcpy(data, opened_serial, n);
    return (int)n;
}
static int LIBUSB_CALL test_wrap(libusb_context *ctx, intptr_t fd, libusb_device_handle **h) {
    (void)ctx; wrapped_fd = fd;
#ifdef _WIN32
    if (!test_fd_is_open((HANDLE)(uintptr_t)fd))
        return LIBUSB_ERROR_NO_DEVICE;
#endif
    return test_open((libusb_device *)&fake_devices[0], h);
}
static libusb_device *LIBUSB_CALL test_device(libusb_device_handle *h) {
    (void)h; return (libusb_device *)&fake_devices[0];
}
static int LIBUSB_CALL test_active(libusb_device_handle *h, int i) {
    (void)h; (void)i; return active_result;
}
static int LIBUSB_CALL test_interface(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
static int LIBUSB_CALL test_get_config(libusb_device_handle *h, int *cfg) {
    (void)h; *cfg = 1; return 0;
}
static int LIBUSB_CALL test_claim(libusb_device_handle *h, int i) {
    (void)h; (void)i; return claim_result;
}
static void LIBUSB_CALL test_close(libusb_device_handle *h) { (void)h; }
static int LIBUSB_CALL test_bulk(libusb_device_handle *h, unsigned char ep, unsigned char *p,
                                  int len, int *actual, unsigned timeout) {
    (void)h; (void)ep; (void)p; (void)timeout;
    *actual = short_write ? len - 1 : len;
    return bulk_result;
}
static void *test_calloc(size_t count, size_t size) {
    return malloc_failure ? NULL : calloc(count, size);
}
static struct {
    struct libusb_transfer *t;
    int pending, cancelled;
} transfers[16];
static unsigned allocations, frees, submits, log_opens, log_closes, log_records;
static unsigned fail_submit_at;
static int event_failure, logger_failure, sink_failure, alloc_failure;
static int completion_failure, cancel_failure, logger_open_failure, logger_submit_failure;
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
    if (alloc_failure)
        return NULL;
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
    if (cancel_failure) {
        cancel_failure = 0;
        return LIBUSB_ERROR_IO;
    }
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
        if (completion_failure) {
            t->status = LIBUSB_TRANSFER_NO_DEVICE;
            t->actual_length = 0;
            completion_failure = 0;
        }
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
    if (logger_open_failure)
        return NULL;
    log_opens++;
    return (usb_log *)&logger_marker;
}
static int test_log_submit(usb_log *log, const usb_log_record *record) {
    assert(log == (usb_log *)&logger_marker && log_opens == log_closes + 1);
    (void)record;
    log_records++;
    return logger_submit_failure ? -1 : 0;
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
    return sink_failure ? -1 : 0;
}
static void stopped(usb_backend *b) {
    assert(!b->pending && !b->log && !b->sink && !b->user);
    assert(allocations == frees && log_opens == log_closes);
    for (unsigned i = 0; i < 16; i++)
        assert(!b->transfers[i]);
}
static void open_tests(void) {
    usb_selector selector;
    assert(!usb_selector_parse("0", &selector));
    usb_backend *b = NULL;
    assert(!usb_open(&b, &selector, -1, 0));
    assert(b && !usb_failure_exit(b));
    usb_close(b);
    list_result = 0;
    assert(usb_open(&b, &selector, -1, 0) == 3 && !b);
    list_result = LIBUSB_ERROR_IO;
    assert(usb_open(&b, &selector, -1, 0) == 7 && !b);
    list_result = 1;
    open_result = LIBUSB_ERROR_ACCESS;
    assert(usb_open(&b, &selector, -1, 0) == 7 && !b);
    open_result = LIBUSB_ERROR_BUSY;
    assert(usb_open(&b, &selector, -1, 0) == 4 && !b);
    open_result = 0;
    active_result = 1;
    assert(usb_open(&b, &selector, -1, 0) == 4 && !b);
    assert(!usb_open(&b, &selector, -1, 1));
    usb_close(b);
    active_result = 0;
    claim_result = LIBUSB_ERROR_BUSY;
    assert(usb_open(&b, &selector, -1, 0) == 4 && !b);
    claim_result = 0;
    init_result = LIBUSB_ERROR_NO_MEM;
    assert(usb_open(&b, &selector, -1, 0) == 70 && !b);
    init_result = 0;
    malloc_failure = 1;
    assert(usb_open(&b, &selector, -1, 0) == 70 && !b);
    malloc_failure = 0;
    test_fd fd[2];
    assert(test_pipe_create(fd) == 0);
    const int borrowed_fd = test_fd_number(fd[0]);
    wrong_model = 1;
    assert(usb_open(&b, NULL, borrowed_fd, 0) == 3 && !b);
    assert(test_fd_ownership_ok(fd[0], borrowed_fd));
    wrong_model = 0;
    test_fd_close(fd[0]); test_fd_close(fd[1]);
#ifdef _WIN32
    assert(usb_open(&b, NULL, test_invalid_fd_number(), 0) == 7 && !b);
#else
    assert(usb_open(&b, NULL, 9, 0) == 7 && !b);
#endif
    descriptor_result = LIBUSB_ERROR_IO;
    assert(usb_open(&b, &selector, -1, 0) == 7 && !b);
    descriptor_result = 0;
}
static void failure_tests(void) {
    unsigned received = 0;
    for (unsigned which = 0; which < 8; which++) {
        usb_backend *b = calloc(1, sizeof(*b));
        assert(b);
        b->diagnostic = 1;
        int expected = 70;
        if (which == 0) {
            alloc_failure = 1;
            assert(usb_stream_start(b, receive, &received) < 0);
        } else if (which == 1) {
            logger_open_failure = 1;
            assert(usb_stream_start(b, receive, &received) < 0);
        } else {
            assert(!usb_stream_start(b, receive, &received));
            if (which == 2) {
                assert(usb_stream_start(b, receive, &received) < 0);
            } else if (which == 3) {
                sink_failure = 1; expected = 8;
                assert(usb_poll(b, 1) < 0);
            } else if (which == 4) {
                completion_failure = 1; expected = 7;
                assert(usb_poll(b, 1) < 0);
            } else if (which == 5) {
                cancel_failure = 1; expected = 7;
                assert(usb_stream_stop(b) < 0);
            } else if (which == 6) {
                logger_submit_failure = 1;
                assert(usb_poll(b, 1) < 0);
            } else {
                fail_submit_at = submits + 1; expected = 7;
                assert(usb_poll(b, 1) < 0);
            }
        }
        assert(usb_failure_exit(b) == expected);
        /* A later diagnostic failure must never overwrite the primary cause. */
        logger_failure = 1;
        assert(usb_stream_stop(b) < 0 && usb_failure_exit(b) == expected);
        stopped(b);
        assert(usb_stream_start(b, receive, &received) < 0);
        usb_close(b);
        logger_failure = sink_failure = alloc_failure = logger_open_failure = logger_submit_failure = 0;
    }
    for (int short_case = 0; short_case < 2; short_case++) {
        usb_backend *b = calloc(1, sizeof(*b));
        assert(b);
        bulk_result = short_case ? 0 : LIBUSB_ERROR_NO_DEVICE;
        short_write = short_case;
        uint8_t bytes[4] = {0};
        size_t size = sizeof(bytes);
        pixy_io io = usb_io(b);
        assert(io.bulk(io.user, 8, bytes, &size, 50) < 0);
        assert(usb_failure_exit(b) == 7);
        backend_fail(b, 70);
        assert(usb_failure_exit(b) == 7);
        usb_close(b);
    }
    bulk_result = short_write = 0;
}
static void dt295_model_selection(void) {
    usb_selector selector; assert(!usb_selector_parse("0", &selector));
    usb_backend *b = NULL;
    descriptor_product = 0x103b;
    assert(usb_open(&b, &selector, -1, 0) == 3 && !b);
    assert(!usb_open_model(&b, &selector, -1, 0, USB_MODEL_DT295));
    assert(usb_model(b) == USB_MODEL_DT295 && usb_native_context(b) && usb_native_handle(b));
    usb_close(b);
    descriptor_product = 0x102b;
    assert(usb_open_model(&b, &selector, -1, 0, USB_MODEL_DT295) == 3 && !b);
    assert(!usb_open(&b, &selector, -1, 0));
    assert(usb_model(b) == USB_MODEL_DT181); usb_close(b);

    /* A mixed enumeration must preserve the old DT181-local index and path
     * behavior even when a DT295 occurs first in libusb's device list. */
    fake_device_count = 2;
    fake_devices[0] = (fake_usb_device){0x103b, 1, 9, 4, NULL};
    fake_devices[1] = (fake_usb_device){0x102b, 1, 7, 2, NULL};
    list_result = 2;
    assert(!usb_selector_parse("0", &selector));
    assert(!usb_open(&b, &selector, -1, 0));
    assert(usb_model(b) == USB_MODEL_DT181);
    assert(last_opened_device == (libusb_device *)&fake_devices[1]);
    usb_close(b);
    assert(!usb_open_model(&b, &selector, -1, 0, USB_MODEL_DT295));
    assert(usb_model(b) == USB_MODEL_DT295);
    assert(last_opened_device == (libusb_device *)&fake_devices[0]);
    usb_close(b);
    assert(!usb_selector_parse("1-2", &selector));
    assert(!usb_open(&b, &selector, -1, 0));
    assert(last_opened_device == (libusb_device *)&fake_devices[1]);
    usb_close(b);
    assert(!usb_selector_parse("1", &selector));
    assert(usb_open(&b, &selector, -1, 0) == 3 && !b);

    /* serial: selects exactly the readable descriptor serial and never falls
     * back to the first matching model; a duplicate stays ambiguous. */
    fake_devices[0] = (fake_usb_device){0x103b, 1, 9, 4, "serial-A"};
    fake_devices[1] = (fake_usb_device){0x102b, 1, 7, 2, NULL};
    assert(!usb_selector_parse("serial:serial-A", &selector));
    assert(!usb_open_model(&b, &selector, -1, 0, USB_MODEL_DT295));
    assert(last_opened_device == (libusb_device *)&fake_devices[0]);
    usb_close(b);
    assert(!usb_selector_parse("serial:Serial-A", &selector));
    assert(usb_open_model(&b, &selector, -1, 0, USB_MODEL_DT295) == 3 && !b);
    assert(!usb_selector_parse("serial:serial-A", &selector));
    assert(usb_open(&b, &selector, -1, 0) == 3 && !b);
    assert(!usb_selector_parse("usb:1-4", &selector));
    assert(!usb_open_model(&b, &selector, -1, 0, USB_MODEL_DT295));
    assert(last_opened_device == (libusb_device *)&fake_devices[0]);
    usb_close(b);
    fake_devices[1] = (fake_usb_device){0x103b, 1, 8, 5, "serial-A"};
    assert(!usb_selector_parse("serial:serial-A", &selector));
    assert(usb_open_model(&b, &selector, -1, 0, USB_MODEL_DT295) == 3 && !b);

    fake_devices[0] = (fake_usb_device){0x103b, 1, 9, 4, NULL};
    fake_devices[1] = (fake_usb_device){0x102b, 1, 7, 2, NULL};

    char text[4096];
    FILE *capture = tmpfile(); assert(capture);
    fflush(stdout);
    int saved_stdout = dup(STDOUT_FILENO); assert(saved_stdout >= 0);
    assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    assert(!usb_list(1));
    fflush(stdout);
    assert(dup2(saved_stdout, STDOUT_FILENO) >= 0); close(saved_stdout);
    rewind(capture); size_t n = fread(text, 1, sizeof(text) - 1, capture);
    assert(n < sizeof(text) - 1); text[n] = 0; fclose(capture);
    assert(strstr(text, "\"index\":0,\"serial\":null,\"model\":\"PIX-DT181-PU0\""));
    assert(!strstr(text, "PIX-DT295"));

    capture = tmpfile(); assert(capture);
    fflush(stdout); saved_stdout = dup(STDOUT_FILENO); assert(saved_stdout >= 0);
    assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    assert(!usb_list_model(1, USB_MODEL_DT295));
    fflush(stdout);
    assert(dup2(saved_stdout, STDOUT_FILENO) >= 0); close(saved_stdout);
    rewind(capture); n = fread(text, 1, sizeof(text) - 1, capture);
    assert(n < sizeof(text) - 1); text[n] = 0; fclose(capture);
    assert(strstr(text, "\"index\":0,\"serial\":null,\"model\":\"PIX-DT295\""));
    assert(!strstr(text, "PIX-DT181-PU0"));

    /* The borrowed-fd path remains model checked and preserves caller ownership. */
    fake_devices[0] = (fake_usb_device){0x102b, 1, 7, 2, NULL};
    fake_devices[1] = (fake_usb_device){0x103b, 1, 9, 4, NULL};
    test_fd fd[2];
    assert(test_pipe_create(fd) == 0);
    const int borrowed_fd = test_fd_number(fd[0]);
    assert(!usb_open(&b, NULL, borrowed_fd, 0));
    assert(usb_model(b) == USB_MODEL_DT181);
    usb_close(b);
    assert(test_fd_ownership_ok(fd[0], borrowed_fd));
    test_fd_close(fd[0]); test_fd_close(fd[1]);

    fake_device_count = 1;
    list_result = 1;
    descriptor_product = 0x102b;
}
int main(void) {
    open_tests();
    failure_tests();
    usb_backend *b = calloc(1, sizeof(*b));
    assert(b);
    b->diagnostic = 1;
    unsigned received = 0;
    for (unsigned run = 0; run < 3; run++) {
        assert(!usb_stream_start(b, receive, &received));
        assert(b->pending == 16 && !b->stopping && !b->last_ms);
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
    assert(b->pending == 3 && usb_failure_exit(b) == 7);
    assert(usb_stream_stop(b) < 0);
    stopped(b);
    assert(usb_stream_start(b, receive, &received) < 0);
    usb_close(b);
    b = calloc(1, sizeof(*b));
    assert(b);
    assert(!usb_stream_start(b, receive, &received));
    event_failure = 1;
    assert(usb_poll(b, 1) < 0 && usb_failure_exit(b) == 7);
    assert(usb_stream_stop(b) < 0);
    stopped(b);
    assert(usb_stream_start(b, receive, &received) < 0);
    usb_close(b);
    b = calloc(1, sizeof(*b));
    assert(b);
    b->diagnostic = 1;
    assert(!usb_stream_start(b, receive, &received));
    logger_failure = 1;
    assert(usb_stream_stop(b) < 0 && usb_failure_exit(b) == 70);
    stopped(b);
    assert(usb_stream_start(b, receive, &received) < 0);
    usb_close(b);
    dt295_model_selection();
    puts("USB backend restart, transfer lifetime and shared failure classification tests passed");
    return 0;
}
