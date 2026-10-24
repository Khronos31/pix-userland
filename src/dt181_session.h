#ifndef PIX_DT181_SESSION_H
#define PIX_DT181_SESSION_H
#include "usb_backend.h"

typedef struct {
    usb_backend *usb;
    pixy device;
    int opened;
} dt181_session;

int dt181_session_open(dt181_session *, const usb_selector *, int fd, int detach);
void dt181_session_diagnostics(dt181_session *, int enabled);
int dt181_session_initialize(dt181_session *, const uint8_t *, const uint8_t[16],
                             const uint8_t[64]);
int dt181_session_tune_slot(dt181_session *, pixy_system, uint32_t, int, int *, int);
int dt181_session_status(dt181_session *, uint8_t *, size_t *);
int dt181_session_prepare(dt181_session *, usb_sink, void *, uint8_t[32]);
int dt181_session_poll(dt181_session *, unsigned);
int dt181_session_certify(dt181_session *);
int dt181_session_stop_hardware(dt181_session *);
int dt181_session_stop_usb(dt181_session *);
int dt181_session_shutdown(dt181_session *);
void dt181_session_close(dt181_session *);
int dt181_session_failure_exit(const dt181_session *);
int dt181_session_device_exit(const dt181_session *);

#endif
