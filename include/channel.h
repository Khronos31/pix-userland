/* SPDX-License-Identifier: MIT */
#ifndef PIX_CHANNEL_H
#define PIX_CHANNEL_H
#include "pixy.h"
typedef struct { pixy_system system; unsigned channel; int slot; } pix_channel;
/* Numeric physical channel, T27, BS01_0, or CS02. No service-name table. */
int pix_channel_parse(const char *, pixy_system default_system, pix_channel *);
int pix_select_tsid(const uint8_t ids[16], int requested_tsid, int slot);
#endif
