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
    int pending, stopping, error, claimed, detached;
    int diagnostic;
    usb_log *log;
    uint64_t completions, short_reads, last_ms, max_gap, next_log;
    unsigned abnormal_logs;
    usb_sink sink;
    void *user;
};
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
            return -1;
    }
    return usb_err(libusb_init(ctx), "libusb_init");
}
static int is_model(libusb_device *d) {
    struct libusb_device_descriptor desc;
    return !libusb_get_device_descriptor(d, &desc) && desc.idVendor == 0x06b8 &&
           desc.idProduct == 0x102b;
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
    if (init(&ctx, -1))
        return -1;
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0) {
        usb_err((int)n, "list USB devices");
        libusb_exit(ctx);
        return -1;
    }
    usb_device_info *found = n ? calloc((size_t)n, sizeof(*found)) : NULL;
    if (n && !found) {
        fprintf(stderr, "Cannot allocate USB device list\n");
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        return -1;
    }
    unsigned count = 0;
    for (ssize_t i = 0; i < n; i++) {
        if (is_model(list[i])) {
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
    return result;
}
int usb_open(usb_backend **result, const usb_selector *selector, int fd, int detach) {
    usb_backend *b = calloc(1, sizeof(*b));
    if (!b)
        return -1;
    if (init(&b->ctx, fd)) {
        free(b);
        return -1;
    }
    if (fd >= 0) {
        if (usb_err(libusb_wrap_sys_device(b->ctx, (intptr_t)fd, &b->handle), "wrap USB fd"))
            goto error;
        if (!is_model(libusb_get_device(b->handle))) {
            fprintf(stderr, "USB fd is not a PIX-DT181-PU0\n");
            goto error;
        }
    } else {
        libusb_device **list;
        ssize_t n = libusb_get_device_list(b->ctx, &list);
        if (n < 0)
            goto error;
        unsigned index = 0;
        for (ssize_t i = 0; i < n; i++)
            if (is_model(list[i])) {
                usb_device_info info;
                device_info(list[i], index++, &info);
                if (usb_selector_matches(selector, info.index, info.bus, info.address,
                                         info.ports, info.port_count)) {
                    int r = libusb_open(list[i], &b->handle);
                    usb_err(r, "open tuner");
                    break;
                }
            }
        libusb_free_device_list(list, 1);
        if (!b->handle) {
            fprintf(stderr, "Selected PIX-DT181-PU0 not accessible\n");
            goto error;
        }
    }
    int active = libusb_kernel_driver_active(b->handle, 0);
    if (active == 1) {
        if (!detach) {
            fprintf(stderr,
                    "USB interface is bound; use --detach-kernel-driver to detach explicitly\n");
            goto error;
        }
        if (usb_err(libusb_detach_kernel_driver(b->handle, 0), "detach"))
            goto error;
        b->detached = 1;
    }
    int cfg;
    if (usb_err(libusb_get_configuration(b->handle, &cfg), "get configuration"))
        goto error;
    if (cfg != 1 && usb_err(libusb_set_configuration(b->handle, 1), "set configuration"))
        goto error;
    if (usb_err(libusb_claim_interface(b->handle, 0), "claim interface"))
        goto error;
    b->claimed = 1;
    *result = b;
    return 0;
error:
    usb_close(b);
    return -1;
}
static int bulk(void *v, uint8_t ep, uint8_t *p, size_t *n, unsigned timeout) {
    usb_backend *b = v;
    int actual = 0;
    size_t wanted = *n;
    int r = libusb_bulk_transfer(b->handle, ep, p, (int)*n, &actual, timeout);
    *n = (size_t)actual;
    if (usb_err(r, "bulk transfer"))
        return -1;
    if (!(ep & 128) && wanted != (size_t)actual) {
        fprintf(stderr, "Short USB write\n");
        return -1;
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
        b->error = 1;
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
            b->error = 1;
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
                b->error = 1;
            b->next_log = now + 30000;
        }
    }
    if (t->status == LIBUSB_TRANSFER_COMPLETED ||
        (b->stopping && t->status == LIBUSB_TRANSFER_CANCELLED)) {
        if (t->actual_length && b->sink(b->user, t->buffer, (size_t)t->actual_length))
            b->error = 1;
    } else {
        fprintf(stderr, "USB stream completion status=%d\n", t->status);
        b->error = 1;
    }
    if (!b->stopping && !b->error) {
        int r = libusb_submit_transfer(t);
        if (r < 0) {
            usb_err(r, "resubmit stream");
            b->error = 1;
        } else
            b->pending++;
    }
}
int usb_stream_start(usb_backend *b, usb_sink sink, void *user) {
    if (!sink || b->error || b->pending || b->log)
        return -1;
    for (unsigned i = 0; i < TRANSFERS; i++)
        if (b->transfers[i])
            return -1;
    b->stopping = 0;
    /* A deliberate retune pause is not an observed USB completion gap. */
    b->last_ms = 0;
    b->next_log = 0;
    b->abnormal_logs = 0;
    b->sink = sink;
    b->user = user;
    if (b->diagnostic && !(b->log = usb_log_open(log_record, NULL))) {
        fprintf(stderr, "Cannot start USB diagnostic logger\n");
        b->error = 1;
        return -1;
    }
    for (unsigned i = 0; i < TRANSFERS; i++) {
        b->transfers[i] = libusb_alloc_transfer(0);
        if (!b->transfers[i]) {
            b->error = 1;
            return -1;
        }
        libusb_fill_bulk_transfer(b->transfers[i], b->handle, 0x82, b->buffers[i], 32768, complete,
                                  b, 0);
        if (usb_err(libusb_submit_transfer(b->transfers[i]), "submit stream")) {
            b->error = 1;
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
        b->error = 1;
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
                b->error = 1;
                usb_err(r, "cancel stream");
            }
        }
    while (b->pending) {
        struct timeval t = {0, 50000};
        int r = libusb_handle_events_timeout(b->ctx, &t);
        if (r < 0 && r != LIBUSB_ERROR_INTERRUPTED) {
            usb_err(r, "drain cancellations");
            b->error = 1;
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
