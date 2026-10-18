/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "control-parse.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

enum control_command_type parse_control_line(const char *line, uint32_t *value)
{
    char command[16];
    char number[32];
    char extra;
    unsigned long parsed = 0;
    size_t i;

    while (*line == ' ' || *line == '\t' || *line == '\r')
        line++;
    if (*line == '\0' || *line == '\n')
        return CONTROL_EMPTY;
    if (sscanf(line, "%15s %31s %c", command, number, &extra) == 1 &&
        strcmp(command, "quit") == 0)
        return CONTROL_QUIT;
    if (sscanf(line, "%15s %31s %c", command, number, &extra) != 2)
        return CONTROL_INVALID;
    for (i = 0; number[i] != '\0'; i++) {
        if (number[i] < '0' || number[i] > '9')
            return CONTROL_INVALID;
        if (parsed > (UINT32_MAX - (unsigned long)(number[i] - '0')) / 10U)
            return CONTROL_INVALID;
        parsed = parsed * 10U + (unsigned long)(number[i] - '0');
    }
    if (strcmp(command, "channel") == 0 && parsed >= 13 && parsed <= 62) {
        *value = (uint32_t)parsed;
        return CONTROL_CHANNEL;
    }
    if (strcmp(command, "tune") == 0 && parsed >= 1) {
        *value = (uint32_t)parsed;
        return CONTROL_TUNE;
    }
    return CONTROL_INVALID;
}
