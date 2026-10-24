/* SPDX-License-Identifier: MIT */
#include "dt181_capture_host.h"

#include "dt181_capture_bridge.h"
#include "dt181_session.h"
#include <stdlib.h>
#include <string.h>

struct dt181_capture_host {
    dt181_session *session;
    dt181_capture_bridge *bridge;
    os_thread worker_thread;
    int worker_started;
    int usb_started;
    int first_error;
};

int dt181_capture_host_start(dt181_capture_host **out, void *opaque_session,
                             usb_sink sink, void *user)
{
    dt181_capture_host *host;
    dt181_session *session = opaque_session;
    uint8_t context[32];
    int rc;
    if (!out || !session || !sink) return -1;
    *out = NULL;
    host = calloc(1, sizeof(*host));
    if (!host) return -1;
    host->bridge = calloc(1, sizeof(*host->bridge));
    if (!host->bridge) { free(host); return -1; }
    host->session = session;
    if (os_mutex_init(&host->bridge->mutex, NULL) != 0) goto fail;
    if (os_cond_init(&host->bridge->cond, NULL) != 0) {
        os_mutex_destroy(&host->bridge->mutex);
        goto fail;
    }
    ts_check_init(&host->bridge->check);
    rc = dt181_session_prepare(session, enqueue, host->bridge, context);
    if (rc != 0) {
        (void)dt181_session_stop_hardware(session);
        (void)dt181_session_stop_usb(session);
        goto fail_sync;
    }
    host->usb_started = 1;
    dt181_capture_bridge_init_decoder(host->bridge, context, sink, user,
                                     &host->bridge->check);
    rc = os_thread_create(&host->worker_thread, NULL, worker, host->bridge);
    if (rc != 0) {
        (void)dt181_session_stop_hardware(session);
        (void)dt181_session_stop_usb(session);
        host->usb_started = 0;
        goto fail_sync;
    }
    host->worker_started = 1;
    *out = host;
    return 0;
fail_sync:
    os_cond_destroy(&host->bridge->cond);
    os_mutex_destroy(&host->bridge->mutex);
fail:
    free(host->bridge);
    free(host);
    return -1;
}

int dt181_capture_host_poll(dt181_capture_host *host, unsigned ms)
{
    return host && host->session ? dt181_session_poll(host->session, ms) : -1;
}

int dt181_capture_host_certify(dt181_capture_host *host)
{
    return host && host->session ? dt181_session_certify(host->session) : -1;
}

int dt181_capture_host_stop(dt181_capture_host *host)
{
    if (!host || !host->session) return -1;
    if (host->worker_started) {
        if (dt181_session_stop_hardware(host->session) != 0 && !host->first_error)
            host->first_error = -1;
        if (host->usb_started && dt181_session_stop_usb(host->session) != 0 &&
            !host->first_error)
            host->first_error = -1;
        host->usb_started = 0;
        os_mutex_lock(&host->bridge->mutex);
        host->bridge->done = 1;
        os_cond_signal(&host->bridge->cond);
        os_mutex_unlock(&host->bridge->mutex);
        if (os_thread_join(host->worker_thread, 0) != 0) {
            if (!host->first_error) host->first_error = -1;
            return host->first_error;
        }
        host->worker_started = 0;
    }
    if (host->bridge->error && !host->first_error) host->first_error = -1;
    return host->first_error;
}

int dt181_capture_host_quiesced(const dt181_capture_host *host)
{
    return host && !host->worker_started;
}

void dt181_capture_host_destroy(dt181_capture_host *host)
{
    if (!host) return;
    if (host->worker_started) return;
    if (host->bridge) {
        os_cond_destroy(&host->bridge->cond);
        os_mutex_destroy(&host->bridge->mutex);
        free(host->bridge);
    }
    free(host);
}
