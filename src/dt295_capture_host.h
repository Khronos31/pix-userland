/* SPDX-License-Identifier: MIT */
#ifndef PIX_DT295_CAPTURE_HOST_H
#define PIX_DT295_CAPTURE_HOST_H

#include <stddef.h>
#include <stdint.h>

typedef struct dt295_capture_host dt295_capture_host;
typedef int (*dt295_capture_packet_sink)(void *, const uint8_t *, size_t);

int dt295_capture_host_start(dt295_capture_host **, const void *,
                             const char *, dt295_capture_packet_sink, void *);
int dt295_capture_host_stop(dt295_capture_host *);
int dt295_capture_host_quiesced(const dt295_capture_host *);
void dt295_capture_host_destroy(dt295_capture_host *);

#endif
