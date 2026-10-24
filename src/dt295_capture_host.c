/* SPDX-License-Identifier: MIT */
#include "dt295_capture_host.h"

#include "dt295_capture.h"
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int dt295_capture_run_to_sink(const dt295_capture_config *, dt295_capture_packet_sink,
                              void *, _Atomic sig_atomic_t *, int *,
                              void (*)(void *), void *, const char *);

struct dt295_capture_host {
    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    dt295_capture_config config;
    char *assets_path;
    char *runtime_directory;
    int owned_fd;
    unsigned char pid_mask[8192];
    dt295_capture_packet_sink sink;
    void *sink_context;
    _Atomic sig_atomic_t interrupted;
    int worker_started;
    int start_reported;
    int started;
    int done;
    int joined;
    int native_quiesced;
    atomic_int first_error;
};

static void capture_started(void *);

static void record_first_error(dt295_capture_host *host, int error)
{
    int expected = 0;
    if (error)
        atomic_compare_exchange_strong(&host->first_error, &expected, error);
}

static void *capture_worker(void *opaque)
{
    dt295_capture_host *host = opaque;
    int quiesced = 0;
    host->config.interrupted = NULL;
    int result = dt295_capture_run_to_sink(
        &host->config, host->sink, host->sink_context, &host->interrupted, &quiesced,
        capture_started, host, host->runtime_directory);
    record_first_error(host, result);
    pthread_mutex_lock(&host->mutex);
    host->native_quiesced = quiesced;
    host->done = 1;
    pthread_cond_broadcast(&host->changed);
    pthread_mutex_unlock(&host->mutex);
    return NULL;
}

static void capture_started(void *opaque)
{
    dt295_capture_host *host = opaque;
    pthread_mutex_lock(&host->mutex);
    host->started = 1;
    host->start_reported = 1;
    pthread_cond_broadcast(&host->changed);
    pthread_mutex_unlock(&host->mutex);
}

int dt295_capture_host_start(dt295_capture_host **out, const void *configuration,
                             const char *runtime_directory,
                             dt295_capture_packet_sink sink, void *sink_context)
{
    if (!out || !configuration || !runtime_directory || runtime_directory[0] == '\0' || !sink)
        return -1;
    *out = NULL;
    dt295_capture_host *host = calloc(1, sizeof(*host));
    if (!host)
        return -1;
    if (pthread_mutex_init(&host->mutex, NULL) != 0) {
        free(host);
        return -1;
    }
    if (pthread_cond_init(&host->changed, NULL) != 0) {
        pthread_mutex_destroy(&host->mutex);
        free(host);
        return -1;
    }
    host->sink = sink;
    host->config = *(const dt295_capture_config *)configuration;
    host->owned_fd = -1;
    atomic_init(&host->first_error, 0);
    atomic_init(&host->interrupted, 0);
    if (!host->config.assets) {
        pthread_cond_destroy(&host->changed);
        pthread_mutex_destroy(&host->mutex);
        free(host);
        return -1;
    }
    if (host->config.filter_pids) {
        if (!host->config.pid_mask) {
            pthread_cond_destroy(&host->changed);
            pthread_mutex_destroy(&host->mutex);
            free(host);
            return -1;
        }
        memcpy(host->pid_mask, host->config.pid_mask, sizeof(host->pid_mask));
        host->config.pid_mask = host->pid_mask;
    }
    host->assets_path = strdup(host->config.assets);
    host->runtime_directory = strdup(runtime_directory);
    if (!host->assets_path || !host->runtime_directory) {
        free(host->runtime_directory);
        free(host->assets_path);
        pthread_cond_destroy(&host->changed);
        pthread_mutex_destroy(&host->mutex);
        free(host);
        return -1;
    }
    if (host->config.fd >= 0) {
        host->owned_fd = fcntl(host->config.fd, F_DUPFD_CLOEXEC, 0);
        if (host->owned_fd < 0) {
            free(host->runtime_directory);
            free(host->assets_path);
            pthread_cond_destroy(&host->changed);
            pthread_mutex_destroy(&host->mutex);
            free(host);
            return -1;
        }
        host->config.fd = host->owned_fd;
    }
    host->config.assets = host->assets_path;
    host->config.output = NULL;
    host->config.diagnostic = NULL;
    host->sink_context = sink_context;
    host->native_quiesced = 1;
    if (pthread_create(&host->worker, NULL, capture_worker, host) != 0) {
        if (host->owned_fd >= 0)
            close(host->owned_fd);
        free(host->runtime_directory);
        free(host->assets_path);
        pthread_cond_destroy(&host->changed);
        pthread_mutex_destroy(&host->mutex);
        free(host);
        return -1;
    }
    host->worker_started = 1;
    pthread_mutex_lock(&host->mutex);
    while (!host->start_reported && !host->done)
        pthread_cond_wait(&host->changed, &host->mutex);
    int started = host->started;
    pthread_mutex_unlock(&host->mutex);
    if (!started) {
        int rc = dt295_capture_host_stop(host);
        if (dt295_capture_host_quiesced(host)) {
            dt295_capture_host_destroy(host);
            return rc ? rc : -1;
        }
    }
    *out = host;
    return started ? 0 : -1;
}

int dt295_capture_host_stop(dt295_capture_host *host)
{
    if (!host)
        return -1;
    atomic_store(&host->interrupted, 1);
    if (host->worker_started && !host->joined) {
        int rc = pthread_join(host->worker, NULL);
        if (rc) {
            record_first_error(host, rc);
            return rc;
        }
        host->joined = 1;
        host->worker_started = 0;
    }
    return atomic_load(&host->first_error);
}

int dt295_capture_host_quiesced(const dt295_capture_host *host)
{
    return host && host->joined && host->native_quiesced;
}

void dt295_capture_host_destroy(dt295_capture_host *host)
{
    if (host && dt295_capture_host_quiesced(host)) {
        if (host->owned_fd >= 0)
            close(host->owned_fd);
        free(host->runtime_directory);
        free(host->assets_path);
        pthread_cond_destroy(&host->changed);
        pthread_mutex_destroy(&host->mutex);
        free(host);
    }
}
