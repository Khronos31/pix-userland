/* SPDX-License-Identifier: MIT */
#ifndef PIX_DT181_CAPTURE_HOST_H
#define PIX_DT181_CAPTURE_HOST_H

#include "usb_backend.h"

typedef struct dt181_capture_host dt181_capture_host;

int dt181_capture_host_start(dt181_capture_host **, void *, usb_sink, void *);
int dt181_capture_host_poll(dt181_capture_host *, unsigned);
int dt181_capture_host_certify(dt181_capture_host *);
int dt181_capture_host_stop(dt181_capture_host *);
int dt181_capture_host_quiesced(const dt181_capture_host *);
void dt181_capture_host_destroy(dt181_capture_host *);

#endif
