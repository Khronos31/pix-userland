/* SPDX-License-Identifier: MIT */
#ifndef PIX_DT295_CAPTURE_H
#define PIX_DT295_CAPTURE_H
#include "usb_selector.h"
#include <signal.h>
typedef struct {
    const char *assets, *output, *diagnostic;
    usb_selector selector;
    unsigned channel, seconds;
    int fd, detach, verbose;
    const unsigned char *pid_mask;
    int filter_pids;
    volatile sig_atomic_t *interrupted;
} dt295_capture_config;
/* Explicit target-0 terrestrial DT295 path; no automatic retries. */
int dt295_capture_run(const dt295_capture_config *);
#endif
