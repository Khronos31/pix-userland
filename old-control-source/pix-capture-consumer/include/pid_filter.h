/* SPDX-License-Identifier: MIT */
#ifndef PIX_PID_FILTER_H
#define PIX_PID_FILTER_H
#include <stdint.h>
#include <stddef.h>
/* Compact complete TS packets in place, preserving bytes and packet order. */
int pix_pid_filter(uint8_t *, size_t *, const uint8_t selected[8192]);
#endif
