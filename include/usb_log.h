#ifndef PIX_USB_LOG_H
#define PIX_USB_LOG_H
#include <stdint.h>
#define USB_LOG_SLOTS 128
/* Metadata only. No USB payload, session state or keys. */
typedef struct {
    uint64_t mono_ms, completions, short_reads, max_gap_ms, gap_ms;
    unsigned slot;
    int status, actual, requested, pending;
} usb_log_record;
typedef struct usb_log usb_log;
typedef int (*usb_log_writer)(void *, const usb_log_record *, uint64_t dropped);
usb_log *usb_log_open(usb_log_writer, void *);
/* Never waits for the writer. A full queue drops telemetry, not USB data. */
int usb_log_submit(usb_log *, const usb_log_record *);
int usb_log_close(usb_log *, uint64_t *dropped);
#endif
