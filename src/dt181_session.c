#include "dt181_session.h"
#include <stdio.h>
#include <string.h>

int dt181_session_open(dt181_session *s, const usb_selector *selector, int fd, int detach) {
    int rc = usb_open(&s->usb, selector, fd, detach);
    if (!rc) s->opened = 1;
    return rc;
}

void dt181_session_diagnostics(dt181_session *s, int enabled) {
    usb_diagnostics(s->usb, enabled);
}

int dt181_session_initialize(dt181_session *s, const uint8_t *firmware,
                             const uint8_t bootstrap[16], const uint8_t validation[64]) {
    pixy_init(&s->device, usb_io(s->usb));
    fprintf(stderr, "Initializing PIX-DT181-PU0...\n");
    s->device.failure_context = 10;
    if (pixy_power(&s->device, firmware, 699673) ||
        pixy_session(&s->device, bootstrap, validation))
        return -1;
    s->device.failure_context = 7;
    return pixy_tuner_init(&s->device);
}

int dt181_session_tune_slot(dt181_session *s, pixy_system system, uint32_t frequency,
                            int lna, int *tsid, int slot) {
    return pixy_tune_slot(&s->device, system, frequency, lna, tsid, slot);
}

int dt181_session_status(dt181_session *s, uint8_t *status, size_t *length) {
    return pixy_status(&s->device, status, length);
}

int dt181_session_prepare(dt181_session *s, usb_sink sink, void *user, uint8_t context[32]) {
    if (pixy_fifo_clear(&s->device) || usb_stream_start(s->usb, sink, user))
        return -1;
    return pixy_start(&s->device, context);
}

int dt181_session_poll(dt181_session *s, unsigned ms) {
    return usb_poll(s->usb, ms);
}

int dt181_session_certify(dt181_session *s) {
    return pixy_certify(&s->device);
}

int dt181_session_stop_hardware(dt181_session *s) {
    return pixy_stop(&s->device);
}

int dt181_session_stop_usb(dt181_session *s) {
    return usb_stream_stop(s->usb);
}

int dt181_session_shutdown(dt181_session *s) {
    return pixy_shutdown(&s->device);
}

void dt181_session_close(dt181_session *s) {
    usb_close(s->usb);
    s->usb = NULL;
    s->opened = 0;
}

int dt181_session_failure_exit(const dt181_session *s) {
    return usb_failure_exit(s->usb);
}

int dt181_session_device_exit(const dt181_session *s) {
    int code = dt181_session_failure_exit(s);
    if (code) return code;
    return s->device.failure_exit ? s->device.failure_exit : 70;
}
