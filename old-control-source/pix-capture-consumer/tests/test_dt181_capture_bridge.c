/* SPDX-License-Identifier: MIT */
#include "dt181_capture_bridge.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct sink_state {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    uint8_t packets[4 * 188];
    size_t length;
    size_t batch_lengths[4];
    size_t batches;
    int entered;
    int release;
};

static void deadline_after(struct timespec *deadline, unsigned milliseconds)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += milliseconds / 1000;
    deadline->tv_nsec += (long)(milliseconds % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

static int capture_sink(void *user, const uint8_t *data, size_t length)
{
    struct sink_state *sink = user;
    struct timespec deadline;
    deadline_after(&deadline, 5000);
    pthread_mutex_lock(&sink->mutex);
    sink->entered = 1;
    pthread_cond_broadcast(&sink->changed);
    while (!sink->release) {
        int rc = pthread_cond_timedwait(&sink->changed, &sink->mutex,
                                        &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&sink->mutex);
            return -1;
        }
    }
    if (sink->batches >= 4 || sink->length + length > sizeof(sink->packets)) {
        pthread_mutex_unlock(&sink->mutex);
        return -1;
    }
    memcpy(sink->packets + sink->length, data, length);
    sink->length += length;
    sink->batch_lengths[sink->batches++] = length;
    pthread_mutex_unlock(&sink->mutex);
    return 0;
}

static void *run_worker(void *arg)
{
    return worker(arg);
}

static void release_worker(dt181_capture_bridge *bridge,
                           struct sink_state *sink, pthread_t thread,
                           int worker_started, int error)
{
    if (bridge) {
        os_mutex_lock(&bridge->mutex);
        bridge->done = 1;
        if (error)
            bridge->error = 1;
        pthread_cond_broadcast(&bridge->cond);
        os_mutex_unlock(&bridge->mutex);
    }
    if (sink) {
        pthread_mutex_lock(&sink->mutex);
        sink->release = 1;
        pthread_cond_broadcast(&sink->changed);
        pthread_mutex_unlock(&sink->mutex);
    }
    if (worker_started)
        pthread_join(thread, NULL);
}

static void destroy_test_storage(dt181_capture_bridge *bridge,
                                 struct sink_state *sink)
{
    os_cond_destroy(&bridge->cond);
    os_mutex_destroy(&bridge->mutex);
    pthread_cond_destroy(&sink->changed);
    pthread_mutex_destroy(&sink->mutex);
    free(bridge);
}

int main(int argc, char **argv)
{
    dt181_capture_bridge *bridge = calloc(1, sizeof(*bridge));
    struct sink_state sink = {0};
    uint8_t input[3 * 188], context[32] = {0}, oversized[32769] = {0};
    pthread_t thread = {0};
    struct timespec deadline;
    int no_sink = argc == 2 && strcmp(argv[1], "--no-sink") == 0;
    int queue_full = argc == 2 && strcmp(argv[1], "--queue-full") == 0;
    int worker_started = 0;

    if (!bridge || pthread_mutex_init(&sink.mutex, NULL) != 0 ||
        pthread_cond_init(&sink.changed, NULL) != 0 ||
        os_mutex_init(&bridge->mutex, NULL) != 0 ||
        os_cond_init(&bridge->cond, NULL) != 0) {
        free(bridge);
        return 70;
    }
    ts_check_init(&bridge->check);
    dt181_capture_bridge_init_decoder(bridge, context, capture_sink, &sink,
                                      &bridge->check);
    if (queue_full) {
        uint8_t byte = 0x47;
        for (unsigned i = 0; i < DT181_CAPTURE_QUEUE_SLOTS; i++) {
            if (enqueue(bridge, &byte, 1) != 0) {
                destroy_test_storage(bridge, &sink);
                return 26;
            }
        }
        int rejected = enqueue(bridge, &byte, 1) != 0 && bridge->error;
        destroy_test_storage(bridge, &sink);
        if (!rejected)
            return 26;
        puts("DT181 queue-full failure: bounded admission rejection: PASS");
        return 25;
    }
    if (pthread_create(&thread, NULL, run_worker, bridge) != 0) {
        release_worker(bridge, &sink, thread, 0, 1);
        destroy_test_storage(bridge, &sink);
        return 70;
    }
    worker_started = 1;

    if (no_sink) {
        int enqueue_rc = enqueue(bridge, oversized, sizeof(oversized));
        release_worker(bridge, &sink, thread, worker_started, 0);
        int expected = enqueue_rc != 0 && bridge->error;
        destroy_test_storage(bridge, &sink);
        if (!expected)
            return 24;
        puts("DT181 no-sink failure: bounded cleanup and worker join: PASS");
        return 23;
    }

    for (size_t i = 0; i < sizeof(input); i++)
        input[i] = (uint8_t)(i * 17U + 3U);
    for (size_t i = 0; i < sizeof(input); i += 188)
        input[i] = 0x47;
    if (enqueue(bridge, input, 187) != 0 ||
        enqueue(bridge, input + 187, sizeof(input) - 187) != 0) {
        release_worker(bridge, &sink, thread, worker_started, 1);
        destroy_test_storage(bridge, &sink);
        return 70;
    }

    deadline_after(&deadline, 2000);
    pthread_mutex_lock(&sink.mutex);
    while (!sink.entered) {
        int rc = pthread_cond_timedwait(&sink.changed, &sink.mutex,
                                        &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&sink.mutex);
            release_worker(bridge, &sink, thread, worker_started, 1);
            destroy_test_storage(bridge, &sink);
            fprintf(stderr, "DT181 held-sink rendezvous timed out\n");
            return 1;
        }
    }
    pthread_mutex_unlock(&sink.mutex);

    release_worker(bridge, &sink, thread, worker_started, 0);
    int passed = sink.batches == 1 && sink.batch_lengths[0] == sizeof(input) &&
                 sink.length == sizeof(input) &&
                 memcmp(sink.packets, input, sizeof(input)) == 0;
    destroy_test_storage(bridge, &sink);
    if (!passed)
        return 1;
    puts("DT181 real decoder sink: split input, packet batch and held callback drain/join lifetime: PASS");
    return 0;
}
