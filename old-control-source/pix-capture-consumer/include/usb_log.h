#ifndef PIX_USB_LOG_H
#define PIX_USB_LOG_H
#include <stdint.h>
#include <stddef.h>
#define USB_LOG_SLOTS 128
/* Metadata only. No USB payload, session state or keys. */
typedef struct {
    uint64_t mono_ms, completions, short_reads, max_gap_ms, gap_ms;
    unsigned slot;
    int status, actual, requested, pending;
} usb_log_record;
typedef struct usb_log usb_log;
/* Writers run without the queue mutex. Return 0 on success, -1 on I/O failure,
 * or +1 to abandon pending telemetry during cooperative shutdown. A custom
 * writer must eventually return; close cannot forcibly stop arbitrary I/O. */
typedef int (*usb_log_writer)(void *, const usb_log_record *, uint64_t dropped, usb_log *);
int usb_log_stopping(usb_log *);
/* Completed writer calls only; the current call appears in the next record. */
void usb_log_write_timing(usb_log *, uint64_t *last_ms, uint64_t *max_ms);
/* Low-level sink: avoids shared stdio locks and checks shutdown while waiting
 * for fd readiness on POSIX. Lines must be <=512 bytes. NULL log requests no
 * waiting for readiness on POSIX. Windows uses the existing cancellable logger
 * thread join for blocked writes. No fd flags are changed.
 * Intended for regular files or a pipe with a single writer. OS-blocked regular
 * file writes and competing pipe writers cannot have a portable hard deadline. */
int usb_log_write_fd(usb_log *, int fd, const char *, size_t);
usb_log *usb_log_open(usb_log_writer, void *);
/* Never waits for the writer. A full queue drops telemetry, not USB data. */
int usb_log_submit(usb_log *, const usb_log_record *);
/* 0: complete, +1: telemetry discarded, -1: writer I/O failure. Incomplete
 * telemetry must not be represented as a fully verified diagnostic run. */
int usb_log_close(usb_log *, uint64_t *dropped);
#endif
