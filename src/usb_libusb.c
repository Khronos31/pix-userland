#include "usb_backend.h"
#include "usb_log.h"
#include "usb_listing.h"
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#endif
#define TRANSFERS 16
#define INTERFACE_READY_TIMEOUT_MS 1000
#define INTERFACE_READY_POLL_MS 10
struct usb_backend {
    libusb_context *ctx;
    libusb_device_handle *handle;
#ifndef _WIN32
    int wrapped_fd;
#endif
    struct libusb_transfer *transfers[TRANSFERS];
    uint8_t buffers[TRANSFERS][32768];
    int pending, stopping, error, failure_exit, claimed, detached;
    int diagnostic, model;
    usb_log *log;
    uint64_t completions, short_reads, last_ms, max_gap, next_log;
    unsigned abnormal_logs;
    usb_sink sink;
    void *user;
};
/* Shared CLI exit classes. Preserve the first cause through cleanup. */
static int backend_fail(usb_backend *b, int code) {
    if (!b->failure_exit)
        b->failure_exit = code;
    b->error = 1;
    return -1;
}
int usb_failure_exit(const usb_backend *b) {
    return b ? b->failure_exit : 70;
}
static int open_failure(int error) {
    return error == LIBUSB_ERROR_BUSY ? 4 : error == LIBUSB_ERROR_NO_MEM ? 70 : 7;
}
static int open_usb_error(usb_backend *b, int error, const char *operation) {
    if (error >= 0)
        return 0;
    fprintf(stderr, "%s: %s\n", operation, libusb_error_name(error));
    return backend_fail(b, open_failure(error));
}
static int usb_configure_interface(usb_backend *b, int *changed) {
    int cfg;
    if (changed)
        *changed = 0;
    if (open_usb_error(b, libusb_get_configuration(b->handle, &cfg), "get configuration"))
        return -1;
    if (cfg != 1) {
        if (open_usb_error(b, libusb_set_configuration(b->handle, 1), "set configuration"))
            return -1;
        if (changed)
            *changed = 1;
    }
    return 0;
}
#if defined(__APPLE__) || (defined(PIX_TEST_CONFIGURE_BEFORE_DRIVER_QUERY) && !defined(_WIN32))
static int usb_wait_for_interface(usb_backend *b) {
    struct timespec start, now, delay = {0, INTERFACE_READY_POLL_MS * 1000000L};
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
        return LIBUSB_ERROR_NOT_FOUND;
    for (;;) {
        int sleep_result;
        do {
            sleep_result = nanosleep(&delay, &delay);
        } while (sleep_result == -1 && errno == EINTR);
        if (sleep_result == -1)
            return LIBUSB_ERROR_NOT_FOUND;
        int active = libusb_kernel_driver_active(b->handle, 0);
        if (active != LIBUSB_ERROR_NOT_FOUND)
            return active;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            return LIBUSB_ERROR_NOT_FOUND;
        int64_t elapsed_ns = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000LL +
                             (int64_t)now.tv_nsec - start.tv_nsec;
        if (elapsed_ns >= INTERFACE_READY_TIMEOUT_MS * 1000000LL)
            return LIBUSB_ERROR_NOT_FOUND;
        delay.tv_sec = 0;
        delay.tv_nsec = INTERFACE_READY_POLL_MS * 1000000L;
    }
}
#endif
void usb_diagnostics(usb_backend *b, int enabled) {
    b->diagnostic = enabled;
}
static int usb_err(int e, const char *op) {
    if (e < 0)
        fprintf(stderr, "%s: %s\n", op, libusb_error_name(e));
    return e < 0 ? -1 : 0;
}
static int init(libusb_context **ctx, int fd) {
    if (fd >= 0) {
        int r = libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
        if (usb_err(r, "disable device discovery"))
            return r;
    }
    int r = libusb_init(ctx);
    usb_err(r, "libusb_init");
    return r;
}
static int model_state(libusb_device *d) {
    struct libusb_device_descriptor desc;
    int r = libusb_get_device_descriptor(d, &desc);
    if (r < 0)
        return r;
    if (desc.idVendor != 0x06b8) return 0;
    return desc.idProduct == 0x102b ? USB_MODEL_DT181 :
           desc.idProduct == 0x103b ? USB_MODEL_DT295 : 0;
}
/* Reads the descriptor serial without claiming an interface. A missing serial
 * number is absent; an open or read failure is unreadable, never an empty
 * string, so callers can distinguish unknown from a real empty value. */
static void read_serial(libusb_device *device, usb_device_info *out) {
    struct libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(device, &desc) < 0) {
        out->serial_state = USB_SERIAL_UNREADABLE;
        return;
    }
    if (!desc.iSerialNumber)
        return;
    libusb_device_handle *handle = NULL;
    if (libusb_open(device, &handle) < 0 || !handle) {
        out->serial_state = USB_SERIAL_UNREADABLE;
        return;
    }
    unsigned char text[USB_SERIAL_MAX];
    int length = libusb_get_string_descriptor_ascii(handle, desc.iSerialNumber, text,
                                                   sizeof(text));
    libusb_close(handle);
    if (length <= 0 || (size_t)length >= sizeof(text)) {
        out->serial_state = USB_SERIAL_UNREADABLE;
        return;
    }
    memcpy(out->serial, text, (size_t)length);
    out->serial[length] = 0;
    out->serial_state = USB_SERIAL_READABLE;
}
static void device_info(libusb_device *device, unsigned index, int model, int serial,
                        usb_device_info *out) {
    memset(out, 0, sizeof(*out));
    out->index = index;
    out->product = model == USB_MODEL_DT295 ? 0x103b : 0x102b;
    out->bus = libusb_get_bus_number(device);
    out->address = libusb_get_device_address(device);
    int count = libusb_get_port_numbers(device, out->ports, sizeof(out->ports));
    if (count > 0)
        out->port_count = (unsigned)count;
    if (serial)
        read_serial(device, out);
}
int usb_list_model(int json, int expected_model) {
    if (expected_model != USB_MODEL_DT181 && expected_model != USB_MODEL_DT295)
        return 70;
    libusb_context *ctx = NULL;
    int initialized = init(&ctx, -1);
    if (initialized) return open_failure(initialized);
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0) {
        usb_err((int)n, "list USB devices");
        libusb_exit(ctx);
        return open_failure((int)n);
    }
    usb_device_info *found = n ? calloc((size_t)n, sizeof(*found)) : NULL;
    if (n && !found) {
        fprintf(stderr, "Cannot allocate USB device list\n");
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        return 70;
    }
    unsigned count = 0;
    for (ssize_t i = 0; i < n; i++) {
        int model = model_state(list[i]);
        if (model < 0) {
            usb_err(model, "read USB descriptor");
            free(found); libusb_free_device_list(list, 1); libusb_exit(ctx);
            return open_failure(model);
        }
        if (model == expected_model) {
            device_info(list[i], count, expected_model, 1, &found[count]);
            count++;
        }
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    int result = usb_listing_write(stdout, found, count, json);
    free(found);
    if (result)
        fprintf(stderr, "Cannot write USB device list\n");
    return result ? 70 : 0;
}
int usb_list(int json) { return usb_list_model(json, USB_MODEL_DT181); }
int usb_open_model(usb_backend **result, const usb_selector *selector, int fd, int detach, int expected_model) {
    if (!result)
        return 70;
    *result = NULL;
    if ((expected_model != USB_MODEL_DT181 && expected_model != USB_MODEL_DT295) || (fd < 0 && !selector))
        return 70;
    usb_backend *b = calloc(1, sizeof(*b));
    if (!b)
        return 70;
#ifndef _WIN32
    b->wrapped_fd = -1;
#endif
#if defined(__APPLE__) || (defined(PIX_TEST_CONFIGURE_BEFORE_DRIVER_QUERY) && !defined(_WIN32))
    int configuration_changed = 0;
#endif
    int r = init(&b->ctx, fd);
    if (r < 0) {
        int code = open_failure(r);
        free(b);
        return code;
    }
    if (fd >= 0) {
#ifndef _WIN32
        b->wrapped_fd = dup(fd);
        if (b->wrapped_fd < 0 || fcntl(b->wrapped_fd, F_SETFD, FD_CLOEXEC) < 0) {
            if (b->wrapped_fd >= 0) close(b->wrapped_fd);
            b->wrapped_fd = -1;
            backend_fail(b, 7);
            goto error;
        }
        const intptr_t device_fd = (intptr_t)b->wrapped_fd;
#else
        const intptr_t device_fd = (intptr_t)fd;
#endif
        if (open_usb_error(b, libusb_wrap_sys_device(b->ctx, device_fd, &b->handle), "wrap USB fd"))
            goto error;
        r = model_state(libusb_get_device(b->handle));
        if (r < 0) {
            open_usb_error(b, r, "read USB descriptor");
            goto error;
        }
        if (r != expected_model) {
            fprintf(stderr, expected_model == USB_MODEL_DT181
                                ? "USB fd is not a PIX-DT181-PU0\n"
                                : "USB fd is not a PIX-DT295\n");
            backend_fail(b, 3);
            goto error;
        }
        b->model = r;
    } else {
        libusb_device **list;
        ssize_t n = libusb_get_device_list(b->ctx, &list);
        if (n < 0) {
            open_usb_error(b, (int)n, "list USB devices");
            goto error;
        }
        unsigned index = 0, matches = 0;
        int scan_error = 0;
        libusb_device *selected = NULL;
        char (*hits)[40] = NULL;
        if (selector->kind == USB_SELECT_SERIAL && n > 0) {
            hits = calloc((size_t)n, sizeof(*hits));
            if (!hits) {
                fprintf(stderr, "Cannot allocate USB candidate list\n");
                libusb_free_device_list(list, 1);
                backend_fail(b, 70);
                goto error;
            }
        }
        for (ssize_t i = 0; i < n; i++) {
            int model = model_state(list[i]);
            if (model < 0) {
                if (!scan_error)
                    scan_error = open_failure(model);
                continue;
            }
            if (model == expected_model) {
                usb_device_info info;
                device_info(list[i], index++, expected_model,
                            selector->kind == USB_SELECT_SERIAL, &info);
                int match = selector->kind == USB_SELECT_SERIAL
                                ? usb_selector_matches_serial(selector, info.serial_state,
                                                              info.serial)
                                : usb_selector_matches(selector, info.index, info.bus,
                                                       info.address, info.ports,
                                                       info.port_count);
                if (match) {
                    if (hits) {
                        if (usb_selector_format_path(hits[matches], sizeof(*hits), info.bus,
                                                     info.ports, info.port_count))
                            snprintf(hits[matches], sizeof(*hits), "%u:%u", info.bus,
                                     info.address);
                    }
                    if (!matches)
                        selected = list[i];
                    matches++;
                }
            }
        }
        if (matches == 1) {
            b->model = expected_model;
            open_usb_error(b, libusb_open(selected, &b->handle), "open tuner");
        }
        if (matches > 1) {
            fprintf(stderr, expected_model == USB_MODEL_DT181
                                ? "Selected PIX-DT181-PU0 serial is ambiguous (%u candidates)"
                                : "Selected PIX-DT295 serial is ambiguous (%u candidates)",
                    matches);
            for (unsigned i = 0; hits && i < matches; i++)
                fprintf(stderr, " %s", hits[i]);
            fputc('\n', stderr);
        }
        free(hits);
        libusb_free_device_list(list, 1);
        if (!b->handle) {
            if (matches <= 1)
                fprintf(stderr, expected_model == USB_MODEL_DT181
                                    ? "Selected PIX-DT181-PU0 not accessible\n"
                                    : "Selected PIX-DT295 not accessible\n");
            backend_fail(b, scan_error ? scan_error : 3);
            goto error;
        }
    }
#if defined(__APPLE__) || defined(PIX_TEST_CONFIGURE_BEFORE_DRIVER_QUERY)
    if (b->model == USB_MODEL_DT181 && usb_configure_interface(b, &configuration_changed))
        goto error;
#endif
    int active = libusb_kernel_driver_active(b->handle, 0);
#if defined(__APPLE__) || (defined(PIX_TEST_CONFIGURE_BEFORE_DRIVER_QUERY) && !defined(_WIN32))
    if (active == LIBUSB_ERROR_NOT_FOUND && configuration_changed)
        active = usb_wait_for_interface(b);
#endif
    if (active < 0 && active != LIBUSB_ERROR_NOT_SUPPORTED) {
        open_usb_error(b, active, "query kernel driver");
        goto error;
    }
    if (active == 1) {
        if (!detach) {
            fprintf(stderr,
                    "USB interface is bound; use --detach-kernel-driver to detach explicitly\n");
            backend_fail(b, 4);
            goto error;
        }
        if (open_usb_error(b, libusb_detach_kernel_driver(b->handle, 0), "detach"))
            goto error;
        b->detached = 1;
    }
#if defined(__APPLE__) || defined(PIX_TEST_CONFIGURE_BEFORE_DRIVER_QUERY)
    if (b->model != USB_MODEL_DT181 && usb_configure_interface(b, NULL))
        goto error;
#else
    if (usb_configure_interface(b, NULL))
        goto error;
#endif
    if (open_usb_error(b, libusb_claim_interface(b->handle, 0), "claim interface"))
        goto error;
    b->claimed = 1;
    *result = b;
    return 0;
error:;
    int code = usb_failure_exit(b);
    usb_close(b);
    return code ? code : 70;
}
int usb_open(usb_backend **out, const usb_selector *selector, int fd, int detach) {
    return usb_open_model(out, selector, fd, detach, USB_MODEL_DT181);
}
int usb_model(const usb_backend *b) { return b ? b->model : 0; }
void *usb_native_context(usb_backend *b) { return b ? b->ctx : NULL; }
void *usb_native_handle(usb_backend *b) { return b ? b->handle : NULL; }
static int bulk(void *v, uint8_t ep, uint8_t *p, size_t *n, unsigned timeout) {
    usb_backend *b = v;
    int actual = 0;
    size_t wanted = *n;
    int r = libusb_bulk_transfer(b->handle, ep, p, (int)*n, &actual, timeout);
    *n = (size_t)actual;
    if (usb_err(r, "bulk transfer"))
        return backend_fail(b, 7);
    if (!(ep & 128) && wanted != (size_t)actual) {
        fprintf(stderr, "Short USB write\n");
        return backend_fail(b, 7);
    }
    return 0;
}
pixy_io usb_io(usb_backend *b) {
    pixy_io io = {b, bulk};
    return io;
}
static int log_record(void *user, const usb_log_record *r, uint64_t dropped, usb_log *log) {
    (void)user;
    uint64_t last_write_ms, max_write_ms;
    usb_log_write_timing(log, &last_write_ms, &max_write_ms);
    char line[512];
    int length = snprintf(line, sizeof(line),
                   "usb_diagnostic mono_ms=%llu completions=%llu slot=%u status=%d actual=%d "
                   "requested=%d short_reads=%llu max_gap_ms=%llu pending=%d gap_ms=%llu "
                   "log_dropped=%llu log_previous_write_ms=%llu log_max_write_ms=%llu\n",
                   (unsigned long long)r->mono_ms, (unsigned long long)r->completions, r->slot,
                   r->status, r->actual, r->requested, (unsigned long long)r->short_reads,
                   (unsigned long long)r->max_gap_ms, r->pending, (unsigned long long)r->gap_ms,
                   (unsigned long long)dropped, (unsigned long long)last_write_ms,
                   (unsigned long long)max_write_ms);
    if (length < 0 || (size_t)length >= sizeof(line))
        return -1;
    return usb_log_write_fd(log, 2, line, (size_t)length);
}
static void close_log(usb_backend *b) {
    if (!b->log)
        return;
    uint64_t dropped = 0;
    if (usb_log_close(b->log, &dropped))
        backend_fail(b, 70);
    b->log = NULL;
#ifndef _WIN32
    /* Best effort only. On Windows a synchronous summary in the caller could
     * block after the cancellable logger thread has already been joined. */
    if (dropped) {
        char line[96];
        int length = snprintf(line, sizeof(line), "usb_diagnostic log_dropped_total=%llu\n",
                              (unsigned long long)dropped);
        if (length < 0 || (size_t)length >= sizeof(line) ||
            usb_log_write_fd(NULL, 2, line, (size_t)length) < 0)
            backend_fail(b, 70);
    }
#endif
}
static void LIBUSB_CALL complete(struct libusb_transfer *t) {
    usb_backend *b = t->user_data;
    b->pending--;
    if (b->diagnostic) {
        uint64_t now = now_ms();
        b->completions++;
        uint64_t gap = b->last_ms ? now - b->last_ms : 0;
        if (gap > b->max_gap)
            b->max_gap = gap;
        b->last_ms = now;
        if (t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length != t->length)
            b->short_reads++;
        int abnormal =
            t->status != LIBUSB_TRANSFER_COMPLETED || t->actual_length != t->length || gap >= 100;
        if (now >= b->next_log || (abnormal && b->abnormal_logs++ < 128)) {
            unsigned slot = 0;
            while (slot < TRANSFERS && b->transfers[slot] != t)
                slot++;
            usb_log_record record = {now,       b->completions, b->short_reads, b->max_gap,
                                     gap,       slot,           t->status,      t->actual_length,
                                     t->length, b->pending};
            if (usb_log_submit(b->log, &record))
                backend_fail(b, 70);
            b->next_log = now + 30000;
        }
    }
    if (t->status == LIBUSB_TRANSFER_COMPLETED ||
        (b->stopping && t->status == LIBUSB_TRANSFER_CANCELLED)) {
        if (t->actual_length && b->sink(b->user, t->buffer, (size_t)t->actual_length))
            backend_fail(b, 8);
    } else {
        fprintf(stderr, "USB stream completion status=%d\n", t->status);
        backend_fail(b, 7);
    }
    if (!b->stopping && !b->error) {
        int r = libusb_submit_transfer(t);
        if (r < 0) {
            usb_err(r, "resubmit stream");
            backend_fail(b, 7);
        } else
            b->pending++;
    }
}
int usb_stream_start(usb_backend *b, usb_sink sink, void *user) {
    if (b->error)
        return -1;
    if (!sink || b->pending || b->log)
        return backend_fail(b, 70);
    for (unsigned i = 0; i < TRANSFERS; i++)
        if (b->transfers[i])
            return backend_fail(b, 70);
    b->stopping = 0;
    /* A deliberate retune pause is not an observed USB completion gap. */
    b->last_ms = 0;
    b->next_log = 0;
    b->abnormal_logs = 0;
    b->sink = sink;
    b->user = user;
    if (b->diagnostic && !(b->log = usb_log_open(log_record, NULL))) {
        fprintf(stderr, "Cannot start USB diagnostic logger\n");
        backend_fail(b, 70);
        return -1;
    }
    for (unsigned i = 0; i < TRANSFERS; i++) {
        b->transfers[i] = libusb_alloc_transfer(0);
        if (!b->transfers[i]) {
            backend_fail(b, 70);
            return -1;
        }
        libusb_fill_bulk_transfer(b->transfers[i], b->handle, 0x82, b->buffers[i], 32768, complete,
                                  b, 0);
        if (usb_err(libusb_submit_transfer(b->transfers[i]), "submit stream")) {
            backend_fail(b, 7);
            return -1;
        }
        b->pending++;
    }
    return 0;
}
int usb_poll(usb_backend *b, unsigned ms) {
    struct timeval t = {(long)(ms / 1000), (long)(ms % 1000) * 1000};
    int r = libusb_handle_events_timeout(b->ctx, &t);
    if (r < 0 && r != LIBUSB_ERROR_INTERRUPTED) {
        backend_fail(b, 7);
        return usb_err(r, "USB events");
    }
    return b->error ? -1 : 0;
}
int usb_stream_stop(usb_backend *b) {
    b->stopping = 1;
    for (unsigned i = 0; i < TRANSFERS; i++)
        if (b->transfers[i]) {
            int r = libusb_cancel_transfer(b->transfers[i]);
            if (r < 0 && r != LIBUSB_ERROR_NOT_FOUND) {
                backend_fail(b, 7);
                usb_err(r, "cancel stream");
            }
        }
    while (b->pending) {
        struct timeval t = {0, 50000};
        int r = libusb_handle_events_timeout(b->ctx, &t);
        if (r < 0 && r != LIBUSB_ERROR_INTERRUPTED) {
            usb_err(r, "drain cancellations");
            backend_fail(b, 7);
        }
    }
    close_log(b);
    for (unsigned i = 0; i < TRANSFERS; i++) {
        if (b->transfers[i]) {
            libusb_free_transfer(b->transfers[i]);
            b->transfers[i] = NULL;
        }
    }
    b->sink = NULL;
    b->user = NULL;
    return b->error ? -1 : 0;
}
void usb_close(usb_backend *b) {
    if (!b)
        return;
    if (b->pending)
        usb_stream_stop(b);
    close_log(b);
    for (unsigned i = 0; i < TRANSFERS; i++)
        if (b->transfers[i])
            libusb_free_transfer(b->transfers[i]);
    if (b->handle) {
        if (b->claimed)
            libusb_release_interface(b->handle, 0);
        if (b->detached)
            libusb_attach_kernel_driver(b->handle, 0);
        libusb_close(b->handle);
    }
#ifndef _WIN32
    if (b->wrapped_fd >= 0)
        close(b->wrapped_fd);
#endif
    if (b->ctx)
        libusb_exit(b->ctx);
    free(b);
}
