#include "usb_backend.h"
#include "usb_log.h"
#include "usb_listing.h"
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TRANSFERS 16
struct usb_backend {
    libusb_context *ctx;
    libusb_device_handle *handle;
    struct libusb_transfer *transfers[TRANSFERS];
    uint8_t buffers[TRANSFERS][32768];
    int pending, stopping, error, failure_exit, claimed, detached;
    int diagnostic;
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
    return desc.idVendor == 0x06b8 && desc.idProduct == 0x102b;
}
static void device_info(libusb_device *device, unsigned index, usb_device_info *out) {
    memset(out, 0, sizeof(*out));
    out->index = index;
    out->bus = libusb_get_bus_number(device);
    out->address = libusb_get_device_address(device);
    int count = libusb_get_port_numbers(device, out->ports, sizeof(out->ports));
    if (count > 0)
        out->port_count = (unsigned)count;
}
int usb_list(int json) {
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
        if (model) {
            device_info(list[i], count, &found[count]);
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
int usb_open(usb_backend **result, const usb_selector *selector, int fd, int detach) {
    if (!result)
        return 70;
    *result = NULL;
    if (fd < 0 && !selector)
        return 70;
    usb_backend *b = calloc(1, sizeof(*b));
    if (!b)
        return 70;
    int r = init(&b->ctx, fd);
    if (r < 0) {
        int code = open_failure(r);
        free(b);
        return code;
    }
    if (fd >= 0) {
        if (open_usb_error(b, libusb_wrap_sys_device(b->ctx, (intptr_t)fd, &b->handle), "wrap USB fd"))
            goto error;
        r = model_state(libusb_get_device(b->handle));
        if (r < 0) {
            open_usb_error(b, r, "read USB descriptor");
            goto error;
        }
        if (!r) {
            fprintf(stderr, "USB fd is not a PIX-DT181-PU0\n");
            backend_fail(b, 3);
            goto error;
        }
    } else {
        libusb_device **list;
        ssize_t n = libusb_get_device_list(b->ctx, &list);
        if (n < 0) {
            open_usb_error(b, (int)n, "list USB devices");
            goto error;
        }
        unsigned index = 0;
        int scan_error = 0;
        for (ssize_t i = 0; i < n; i++) {
            int model = model_state(list[i]);
            if (model < 0) {
                if (!scan_error)
                    scan_error = open_failure(model);
                continue;
            }
            if (model) {
                usb_device_info info;
                device_info(list[i], index++, &info);
                if (usb_selector_matches(selector, info.index, info.bus, info.address,
                                         info.ports, info.port_count)) {
                    open_usb_error(b, libusb_open(list[i], &b->handle), "open tuner");
                    break;
                }
            }
        }
        libusb_free_device_list(list, 1);
        if (!b->handle) {
            fprintf(stderr, "Selected PIX-DT181-PU0 not accessible\n");
            backend_fail(b, scan_error ? scan_error : 3);
            goto error;
        }
    }
    int active = libusb_kernel_driver_active(b->handle, 0);
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
    int cfg;
    if (open_usb_error(b, libusb_get_configuration(b->handle, &cfg), "get configuration"))
        goto error;
    if (cfg != 1 && open_usb_error(b, libusb_set_configuration(b->handle, 1), "set configuration"))
        goto error;
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
    if (b->ctx)
        libusb_exit(b->ctx);
    free(b);
}
