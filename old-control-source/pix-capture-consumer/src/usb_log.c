#include "usb_log.h"
#include "os.h"
#include "pixy.h"
#include <stdlib.h>
#include <errno.h>
#ifndef _WIN32
#include <poll.h>
#endif
struct usb_log {
    os_mutex mutex;
    os_cond cond;
    os_thread thread;
    usb_log_record records[USB_LOG_SLOTS];
    unsigned head, count;
    uint64_t dropped, last_write_ms, max_write_ms;
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
        if (!failed) {
            uint64_t started = now_ms();
            int result = log->write(log->user, &record, dropped, log);
            uint64_t elapsed = now_ms() - started;
            os_mutex_lock(&log->mutex);
            log->last_write_ms = elapsed;
            if (elapsed > log->max_write_ms)
                log->max_write_ms = elapsed;
            os_mutex_unlock(&log->mutex);
            if (result) {
                os_mutex_lock(&log->mutex);
                if (result < 0)
                    log->failed = 1;
                else {
                    log->dropped += 1 + log->count;
                    log->count = 0;
                    log->done = 1;
                }
                os_mutex_unlock(&log->mutex);
            }
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
    int result = log->failed ? -1 : log->dropped ? 1 : 0;
    if (dropped)
        *dropped = log->dropped;
    os_cond_destroy(&log->cond);
    os_mutex_destroy(&log->mutex);
    free(log);
    return result;
}

int usb_log_stopping(usb_log *log) {
    if (!log)
        return 1;
    os_mutex_lock(&log->mutex);
    int stopping = log->done;
    os_mutex_unlock(&log->mutex);
    return stopping;
}
int usb_log_write_fd(usb_log *log, int fd, const char *line, size_t length) {
    if (fd < 0 || length > 512) {
        errno = EINVAL;
        return -1;
    }
    while (length) {
#ifndef _WIN32
        struct pollfd pollfd = {fd, POLLOUT, 0};
        int ready = poll(&pollfd, 1, usb_log_stopping(log) ? 0 : 50);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (!ready) {
            if (usb_log_stopping(log))
                return 1;
            continue;
        }
        if (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -1;
#endif
        /* USB telemetry lines are below the POSIX minimum PIPE_BUF (512).
         * With one pipe writer, readiness suffices for this atomic write. */
#ifdef _WIN32
        int written = _write(fd, line, (unsigned)length);
#else
        ssize_t written = write(fd, line, length);
#endif
        if (written > 0) {
            line += written;
            length -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (usb_log_stopping(log))
                return 1;
            continue;
        }
        return -1;
    }
    return 0;
}

void usb_log_write_timing(usb_log *log, uint64_t *last_ms, uint64_t *max_ms) {
    os_mutex_lock(&log->mutex);
    *last_ms = log->last_write_ms;
    *max_ms = log->max_write_ms;
    os_mutex_unlock(&log->mutex);
}
