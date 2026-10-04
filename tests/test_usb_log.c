#include "os.h"
#include "usb_log.h"
#include "pixy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#endif
typedef struct {
    os_mutex mutex;
    os_cond cond;
    int entered, release, fail;
    unsigned count;
    uint64_t last, dropped;
} stalled_writer;
static int write_record(void *user, const usb_log_record *record, uint64_t dropped, usb_log *log) {
    (void)log;
    stalled_writer *s = user;
    os_mutex_lock(&s->mutex);
    s->entered = 1;
    os_cond_signal(&s->cond);
    while (!s->release)
        os_cond_wait(&s->cond, &s->mutex);
    if (s->count)
        assert(record->completions == s->last + 1);
    s->last = record->completions;
    s->dropped = dropped;
    s->count++;
    int fail = s->fail;
    os_mutex_unlock(&s->mutex);
    return fail ? -1 : 0;
}
static void scenario(int fail) {
    stalled_writer s = {0};
    assert(!os_mutex_init(&s.mutex, NULL));
    assert(!os_cond_init(&s.cond, NULL));
    s.fail = fail;
    usb_log *log = usb_log_open(write_record, &s);
    assert(log);
    usb_log_record record = {0};
    assert(!usb_log_submit(log, &record));
    os_mutex_lock(&s.mutex);
    while (!s.entered)
        os_cond_wait(&s.cond, &s.mutex);
    os_mutex_unlock(&s.mutex);
    /* Simulate a log destination blocked for longer than USB buffering can cover.
     * The producer must still finish all submits before releasing that writer. */
    for (unsigned i = 1; i <= USB_LOG_SLOTS + 16; i++) {
        record.completions = i;
        assert(!usb_log_submit(log, &record));
    }
    os_mutex_lock(&s.mutex);
    s.release = 1;
    os_cond_signal(&s.cond);
    os_mutex_unlock(&s.mutex);
    uint64_t dropped = 0;
    assert(usb_log_close(log, &dropped) == (fail ? -1 : 1));
    assert(dropped == 16);
    assert(s.count == (fail ? 1 : USB_LOG_SLOTS + 1));
    if (!fail)
        assert(s.last == USB_LOG_SLOTS && s.dropped == 16);
    os_cond_destroy(&s.cond);
    os_mutex_destroy(&s.mutex);
}

#ifndef _WIN32
typedef struct {
    int fd;
    stalled_writer gate;
} pipe_writer;
static int write_pipe(void *user, const usb_log_record *record, uint64_t dropped, usb_log *log) {
    (void)record;
    (void)dropped;
    pipe_writer *s = user;
    os_mutex_lock(&s->gate.mutex);
    s->gate.entered = 1;
    os_cond_signal(&s->gate.cond);
    os_mutex_unlock(&s->gate.mutex);
    return usb_log_write_fd(log, s->fd, "diagnostic\n", 11);
}
static void blocked_pipe_close(void) {
    int fds[2];
    assert(!pipe(fds));
    int flags = fcntl(fds[1], F_GETFL);
    assert(flags >= 0 && !fcntl(fds[1], F_SETFL, flags | O_NONBLOCK));
    char fill[512] = {0};
    while (write(fds[1], fill, sizeof(fill)) > 0) {
    }
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    assert(!fcntl(fds[1], F_SETFL, flags));
    flags = fcntl(fds[1], F_GETFL);
    assert(flags >= 0 && !(flags & O_NONBLOCK));
    pipe_writer s = {0};
    s.fd = fds[1];
    assert(!os_mutex_init(&s.gate.mutex, NULL));
    assert(!os_cond_init(&s.gate.cond, NULL));
    usb_log *log = usb_log_open(write_pipe, &s);
    assert(log);
    usb_log_record record = {0};
    assert(!usb_log_submit(log, &record));
    os_mutex_lock(&s.gate.mutex);
    while (!s.gate.entered)
        os_cond_wait(&s.gate.cond, &s.gate.mutex);
    os_mutex_unlock(&s.gate.mutex);
    /* The reader never drains the full pipe. Close must discard telemetry. */
    uint64_t dropped = 0;
    alarm(5);
    assert(usb_log_close(log, &dropped) == 1);
    alarm(0);
    assert(dropped == 1);
    assert(fcntl(fds[1], F_GETFL) == flags);
    /* The final dropped-count summary must not block either. */
    assert(usb_log_write_fd(NULL, fds[1], "summary\n", 8) == 1);
    assert(usb_log_write_fd(NULL, -1, "error\n", 6) == -1);
    os_cond_destroy(&s.gate.cond);
    os_mutex_destroy(&s.gate.mutex);
    close(fds[0]);
    close(fds[1]);
}
static void ready_pipe_write(void) {
    int fds[2];
    assert(!pipe(fds));
    assert(!usb_log_write_fd(NULL, fds[1], "ready\n", 6));
    char readback[6];
    assert(read(fds[0], readback, sizeof(readback)) == 6);
    assert(!memcmp(readback, "ready\n", 6));
    close(fds[0]);
    close(fds[1]);
}
#endif


static int timing_writer(void *user, const usb_log_record *r, uint64_t dropped, usb_log *log) {
    (void)r;
    (void)dropped;
    unsigned *calls = user;
    if ((*calls)++) {
        uint64_t last_ms, max_ms;
        usb_log_write_timing(log, &last_ms, &max_ms);
        assert(last_ms >= 20 && max_ms >= last_ms);
    } else
        sleep_ms(30);
    return 0;
}
static void timing_scenario(void) {
    unsigned calls = 0;
    usb_log *log = usb_log_open(timing_writer, &calls);
    assert(log);
    usb_log_record r = {0};
    assert(!usb_log_submit(log, &r));
    assert(!usb_log_submit(log, &r));
    uint64_t dropped = 99;
    assert(!usb_log_close(log, &dropped));
    assert(calls == 2 && !dropped);
}

int main(void) {
    scenario(0);
    scenario(1);
    timing_scenario();
#ifndef _WIN32
    blocked_pipe_close();
    ready_pipe_write();
#endif
    puts("USB log queue, cooperative shutdown, writer timing and I/O failure tests passed");
    return 0;
}
