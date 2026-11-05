/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PIX_DT181_CAPTURE_HOST_H
#define PIX_DT181_CAPTURE_HOST_H

#include "usb_backend.h"

typedef struct dt181_capture_host dt181_capture_host;

int dt181_capture_host_start(dt181_capture_host **, void *, usb_sink, void *);
/* Internal L3 feed variant: decode caller-supplied raw USB chunks without
 * owning or starting the USB stream. The caller keeps the session/handle. */
int dt181_capture_host_start_feed(dt181_capture_host **, const uint8_t context[32],
                                  usb_sink, void *);
int dt181_capture_host_feed(dt181_capture_host *, const uint8_t *, size_t);
int dt181_capture_host_stop_feed(dt181_capture_host *);
int dt181_capture_host_poll(dt181_capture_host *, unsigned);
int dt181_capture_host_certify(dt181_capture_host *);
int dt181_capture_host_stop(dt181_capture_host *);
int dt181_capture_host_quiesced(const dt181_capture_host *);
void dt181_capture_host_destroy(dt181_capture_host *);

#endif
