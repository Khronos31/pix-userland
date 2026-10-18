/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIANO_CONTROL_PARSE_H
#define SIANO_CONTROL_PARSE_H

#include <stdint.h>

enum control_command_type {
    CONTROL_EMPTY,
    CONTROL_INVALID,
    CONTROL_CHANNEL,
    CONTROL_TUNE,
    CONTROL_QUIT
};

enum control_command_type parse_control_line(const char *line, uint32_t *value);

#endif
