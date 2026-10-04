#include "usb_log.h"
#include "os.h"
#include <stdlib.h>
struct usb_log {
    os_mutex mutex;
    os_cond cond;
    os_thread thread;
    usb_log_record records[USB_LOG_SLOTS];
    unsigned head, count;
    uint64_t dropped;
    int done, failed;
    usb_log_writer write;
    void *user;
};
static void *logger(void *user) {
    usb_log *log = user;
    for (;;) {
        os_mutex_lock(&log->mutex);
        while (!log->count && !log->done)
            os_cond_wait(&log->cond, &log->mutex);
        if (!log->count) {
            os_mutex_unlock(&log->mutex);
            break;
        }
        usb_log_record record = log->records[log->head];
        log->head = (log->head + 1) % USB_LOG_SLOTS;
        log->count--;
        uint64_t dropped = log->dropped;
        int failed = log->failed;
        os_mutex_unlock(&log->mutex);
        /* The writer may block. Never hold the producer's mutex during I/O. */
        if (!failed && log->write(log->user, &record, dropped)) {
            os_mutex_lock(&log->mutex);
            log->failed = 1;
            os_mutex_unlock(&log->mutex);
        }
    }
    return NULL;
}
usb_log *usb_log_open(usb_log_writer writer, void *user) {
    if (!writer)
        return NULL;
    usb_log *log = calloc(1, sizeof(*log));
    if (!log)
        return NULL;
    log->write = writer;
    log->user = user;
    if (os_mutex_init(&log->mutex, NULL)) {
        free(log);
        return NULL;
    }
    if (os_cond_init(&log->cond, NULL)) {
        os_mutex_destroy(&log->mutex);
        free(log);
        return NULL;
    }
    if (os_thread_create(&log->thread, NULL, logger, log)) {
        os_cond_destroy(&log->cond);
        os_mutex_destroy(&log->mutex);
        free(log);
        return NULL;
    }
    return log;
}
int usb_log_submit(usb_log *log, const usb_log_record *record) {
    if (!log)
        return 0;
    os_mutex_lock(&log->mutex);
    int failed = log->failed || log->done;
    if (!failed) {
        if (log->count == USB_LOG_SLOTS)
            log->dropped++;
        else {
            log->records[(log->head + log->count) % USB_LOG_SLOTS] = *record;
            log->count++;
            os_cond_signal(&log->cond);
        }
    }
    os_mutex_unlock(&log->mutex);
    return failed ? -1 : 0;
}
int usb_log_close(usb_log *log, uint64_t *dropped) {
    if (!log)
        return 0;
    os_mutex_lock(&log->mutex);
    log->done = 1;
    os_cond_signal(&log->cond);
    os_mutex_unlock(&log->mutex);
    os_thread_join(log->thread, 0);
    int result = log->failed ? -1 : 0;
    if (dropped)
        *dropped = log->dropped;
    os_cond_destroy(&log->cond);
    os_mutex_destroy(&log->mutex);
    free(log);
    return result;
}
