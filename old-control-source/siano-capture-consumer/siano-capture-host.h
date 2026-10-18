/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIANO_CAPTURE_HOST_H
#define SIANO_CAPTURE_HOST_H

#include <stddef.h>
#include <stdint.h>

typedef struct siano_capture_host siano_capture_host;
typedef int (*siano_capture_sink)(void *, const uint8_t *);

#ifdef __cplusplus
extern "C" {
#endif

int siano_capture_host_start(siano_capture_host **, void *, siano_capture_sink,
                             void *, uint64_t *);
int siano_capture_host_stop(siano_capture_host *);
int siano_capture_host_quiesced(const siano_capture_host *);
void siano_capture_host_destroy(siano_capture_host *);

#ifdef __cplusplus
}
#endif

#endif
