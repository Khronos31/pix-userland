/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "siano-capture-host.h"

#include "output-writer.h"
#include "siano-device.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

struct siano_capture_host {
    struct siano_device *device;
    struct siano_output_pump pump;
    pthread_t worker;
    uint64_t generation;
    int worker_started;
    int worker_error;
};

static void *capture_worker(void *opaque)
{
    siano_capture_host *host = opaque;
    uint8_t data[USB_TRANSFER_SIZE];
    for (;;) {
        size_t length = 0U;
        int rc = ts_pop_capture(&host->device->ts, host->generation,
                                data, &length);
        if (rc == -ECANCELED) break;
        if (rc == -EAGAIN) continue;
        if (rc != 0 || length > sizeof(data)) {
            host->worker_error = 1;
            break;
        }
        siano_output_pump_feed(&host->pump, data, length);
        if (siano_output_pump_step(&host->pump) != 0) break;
    }
    return NULL;
}

int siano_capture_host_start(siano_capture_host **out, void *opaque_device,
                             siano_capture_sink sink, void *user,
                             uint64_t *generation)
{
    siano_capture_host *host;
    uint64_t current_generation = 0U;
    int rc;
    if (!out || !opaque_device || !sink || !generation) return -EINVAL;
    *out = NULL;
    if (!((struct siano_device *)opaque_device)->ts_initialized) return -EINVAL;
    host = calloc(1U, sizeof(*host));
    if (!host) return -ENOMEM;
    host->device = opaque_device;
    rc = ts_queue_capture_start(&host->device->ts, &current_generation);
    if (rc != 0) { free(host); return rc; }
    rc = siano_output_pump_init_sink(&host->pump, sink, user);
    if (rc != 0) {
        ts_queue_capture_stop(&host->device->ts);
        free(host);
        return rc;
    }
    host->generation = current_generation;
    rc = pthread_create(&host->worker, NULL, capture_worker, host);
    if (rc != 0) {
        ts_queue_capture_stop(&host->device->ts);
        siano_output_pump_destroy(&host->pump);
        free(host);
        return -rc;
    }
    host->worker_started = 1;
    *generation = current_generation;
    *out = host;
    return 0;
}

int siano_capture_host_stop(siano_capture_host *host)
{
    int rc;
    if (!host || !host->device) return -EINVAL;
    if (!host->worker_started) return 0;
    ts_queue_capture_stop(&host->device->ts);
    rc = pthread_join(host->worker, NULL);
    if (rc != 0) return -rc;
    host->worker_started = 0;
    siano_output_pump_destroy(&host->pump);
    return host->worker_error ? -EIO : 0;
}

int siano_capture_host_quiesced(const siano_capture_host *host)
{
    return host && !host->worker_started;
}

void siano_capture_host_destroy(siano_capture_host *host)
{
    if (!host || host->worker_started) return;
    free(host);
}
