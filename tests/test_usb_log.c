#include "os.h"
#include "usb_log.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    os_mutex mutex;
    os_cond cond;
    int entered, release, fail;
    unsigned count;
    uint64_t last, dropped;
} stalled_writer;
static int write_record(void *user, const usb_log_record *record, uint64_t dropped) {
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
    assert(usb_log_close(log, &dropped) == (fail ? -1 : 0));
    assert(dropped == 16);
    assert(s.count == (fail ? 1 : USB_LOG_SLOTS + 1));
    if (!fail)
        assert(s.last == USB_LOG_SLOTS && s.dropped == 16);
    os_cond_destroy(&s.cond);
    os_mutex_destroy(&s.mutex);
}
int main(void) {
    scenario(0);
    scenario(1);
    puts("Blocked USB diagnostic writer, bounded telemetry drops and I/O failure tests passed");
    return 0;
}
