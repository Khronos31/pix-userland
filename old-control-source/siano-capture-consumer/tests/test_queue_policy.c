/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../queue-policy.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#ifdef _WIN32
#include "../siano-os.h"
#define TEST_COND_CLOCK CLOCK_MONOTONIC
#else
#include <pthread.h>
#define TEST_COND_CLOCK CLOCK_REALTIME
#endif

struct wake_test {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool ready;
    bool closed;
    bool overflowed;
    int result;
};

struct retune_test {
    pthread_mutex_t mutex;
    pthread_mutex_t barrier_mutex;
    pthread_cond_t barrier_changed;
    bool callback_ready;
    bool discarding;
    uint64_t epoch;
    uint64_t transfer_epoch;
    bool accepted;
};

static void *retune_callback_thread(void *arg)
{
    struct retune_test *test = arg;

#ifdef _WIN32
    assert(!TryEnterCriticalSection(&test->mutex));
#else
    assert(pthread_mutex_trylock(&test->mutex) == EBUSY);
#endif
    pthread_mutex_lock(&test->barrier_mutex);
    test->callback_ready = true;
    pthread_cond_broadcast(&test->barrier_changed);
    pthread_mutex_unlock(&test->barrier_mutex);

    pthread_mutex_lock(&test->mutex);
    test->accepted = siano_queue_epoch_accepts(test->epoch, test->discarding,
                                                test->transfer_epoch);
    pthread_mutex_unlock(&test->mutex);
    return NULL;
}

static pthread_t start_callback_waiting_on_queue(struct retune_test *test,
                                                uint64_t transfer_epoch)
{
    pthread_t callback;
    struct timespec deadline;

    test->callback_ready = false;
    test->transfer_epoch = transfer_epoch;
    pthread_mutex_lock(&test->mutex);
    assert(pthread_create(&callback, NULL, retune_callback_thread, test) == 0);
    clock_gettime(TEST_COND_CLOCK, &deadline);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&test->barrier_mutex);
    while (!test->callback_ready)
        assert(pthread_cond_timedwait(&test->barrier_changed,
                                      &test->barrier_mutex, &deadline) == 0);
    pthread_mutex_unlock(&test->barrier_mutex);
    return callback;
}

static void finish_waiting_callback(struct retune_test *test, pthread_t callback)
{
    pthread_mutex_unlock(&test->mutex);
    assert(pthread_join(callback, NULL) == 0);
    assert(!test->accepted);
}

static void *writer_thread(void *arg)
{
    struct wake_test *test = arg;
    struct timespec deadline;

    clock_gettime(TEST_COND_CLOCK, &deadline);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&test->mutex);
    test->ready = true;
    pthread_cond_broadcast(&test->changed);
    while (!test->overflowed) {
        int rc = pthread_cond_timedwait(&test->changed, &test->mutex, &deadline);
        if (rc != 0) {
            test->result = rc;
            break;
        }
    }
    if (test->overflowed)
        test->result = siano_queue_overflow_result(test->overflowed);
    pthread_mutex_unlock(&test->mutex);
    return NULL;
}

static void test_overflow_wakes_writer(void)
{
    for (int iteration = 0; iteration < 64; iteration++) {
        struct wake_test test = {0};
        pthread_t writer;
        struct timespec deadline;

        assert(pthread_mutex_init(&test.mutex, NULL) == 0);
        assert(pthread_cond_init(&test.changed, NULL) == 0);
        assert(pthread_create(&writer, NULL, writer_thread, &test) == 0);
        clock_gettime(TEST_COND_CLOCK, &deadline);
        deadline.tv_sec += 5;
        pthread_mutex_lock(&test.mutex);
        while (!test.ready) {
            assert(pthread_cond_timedwait(&test.changed, &test.mutex, &deadline) == 0);
        }
        assert(siano_queue_note_drop(true, &test.closed, &test.overflowed));
        pthread_cond_broadcast(&test.changed);
        pthread_mutex_unlock(&test.mutex);
        assert(pthread_join(writer, NULL) == 0);
        assert(test.closed && test.result == -ENOBUFS);
        pthread_cond_destroy(&test.changed);
        pthread_mutex_destroy(&test.mutex);
    }
}

static void test_retune_epoch_lifecycle_and_mutex_races(void)
{
    struct retune_test test = {0};
    pthread_t callback;
    size_t head = 2;
    size_t tail = 7;
    size_t count = 5;
    size_t hold_len = 37;

    assert(pthread_mutex_init(&test.mutex, NULL) == 0);
    assert(pthread_mutex_init(&test.barrier_mutex, NULL) == 0);
    assert(pthread_cond_init(&test.barrier_changed, NULL) == 0);

    /* A callback carrying the pre-retune transfer epoch waits across begin. */
    callback = start_callback_waiting_on_queue(&test, test.epoch);
    siano_queue_retune_begin(&test.epoch, &test.discarding);
    assert(test.discarding && test.epoch == 1);
    finish_waiting_callback(&test, callback);

    /* A callback submitted during discard is rejected while discard is active. */
    callback = start_callback_waiting_on_queue(&test, test.epoch);
    finish_waiting_callback(&test, callback);

    /* A callback carrying the discard epoch cannot cross successful finish. */
    callback = start_callback_waiting_on_queue(&test, test.epoch);
    assert(siano_queue_retune_finish(0, &test.epoch, &test.discarding,
                                     &head, &tail, &count, &hold_len));
    finish_waiting_callback(&test, callback);
    assert(!test.discarding && test.epoch == 2);
    assert(head == 0 && tail == 0 && count == 0 && hold_len == 0);
    assert(siano_queue_epoch_accepts(test.epoch, test.discarding, test.epoch));

    /* Failed tune releases discard while preserving queued TS and alignment. */
    head = 3;
    tail = 6;
    count = 3;
    hold_len = 29;
    pthread_mutex_lock(&test.mutex);
    siano_queue_retune_begin(&test.epoch, &test.discarding);
    pthread_mutex_unlock(&test.mutex);
    callback = start_callback_waiting_on_queue(&test, test.epoch);
    assert(!siano_queue_retune_finish(-ETIMEDOUT, &test.epoch, &test.discarding,
                                      &head, &tail, &count, &hold_len));
    finish_waiting_callback(&test, callback);
    assert(!test.discarding && test.epoch == 4);
    assert(head == 3 && tail == 6 && count == 3 && hold_len == 29);
    assert(siano_queue_epoch_accepts(test.epoch, test.discarding, test.epoch));
    assert(!siano_queue_epoch_accepts(test.epoch, test.discarding, test.epoch - 1));

    pthread_cond_destroy(&test.barrier_changed);
    pthread_mutex_destroy(&test.barrier_mutex);
    pthread_mutex_destroy(&test.mutex);
}

int main(void)
{
    bool closed = false;
    bool overflowed = false;

    assert(!siano_queue_note_drop(false, &closed, &overflowed));
    assert(!closed && !overflowed);
    assert(siano_queue_overflow_result(overflowed) == 0);

    assert(siano_queue_note_drop(true, &closed, &overflowed));
    assert(closed && overflowed);
    assert(siano_queue_overflow_result(overflowed) == -ENOBUFS);
    assert(siano_thread_create_error(true) == ENOMEM);
    assert(siano_thread_create_error(false) == EAGAIN);
    test_retune_epoch_lifecycle_and_mutex_races();
    test_overflow_wakes_writer();

    puts("queue policy tests: PASS");
    return 0;
}
