/* SPDX-License-Identifier: MIT */
#ifndef PIX_CONTROL_H
#define PIX_CONTROL_H
#include "channel.h"
typedef struct { char line[256]; unsigned used; int discard; } pix_control;
typedef struct { int quit, frequency; uint32_t hz; pix_channel channel; } pix_command;
int pix_command_parse(const char *, pixy_system, pix_command *);
/* 1 line ready, 0 no data, 2 EOF, -1 invalid/I/O failure. */
int pix_control_poll(pix_control *, pixy_system, pix_command *);
#endif
