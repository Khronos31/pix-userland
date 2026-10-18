/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "device-selector.h"

#include <errno.h>
#include <stddef.h>

static int parse_number(const char **cursor, unsigned max, unsigned *value)
{
    const char *p = *cursor;
    unsigned parsed = 0;
    unsigned digits = 0;

    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p - '0');
        if (parsed > (max - digit) / 10U)
            return -EINVAL;
        parsed = parsed * 10U + digit;
        p++;
        digits++;
    }
    if (digits == 0)
        return -EINVAL;
    *cursor = p;
    *value = parsed;
    return 0;
}

int siano_parse_device_selector(const char *text, struct siano_device_selector *selector)
{
    const char *cursor = text;
    unsigned first;
    unsigned second;

    if (!text || !selector || !*text)
        return -EINVAL;
    *selector = (struct siano_device_selector){0};
    if (parse_number(&cursor, UINT32_MAX, &first) < 0)
        return -EINVAL;
    if (*cursor == '\0') {
        selector->kind = SIANO_DEVICE_SELECTOR_INDEX;
        selector->index = first;
        return 0;
    }
    if (*cursor == ':') {
        cursor++;
        if (first > UINT8_MAX ||
            parse_number(&cursor, UINT8_MAX, &second) < 0 || second == 0 || *cursor != '\0')
            return -EINVAL;
        selector->kind = SIANO_DEVICE_SELECTOR_BUS_ADDRESS;
        selector->bus = (uint8_t)first;
        selector->address = (uint8_t)second;
        return 0;
    }
    if (*cursor == '-') {
        cursor++;
        if (first > UINT8_MAX ||
            parse_number(&cursor, UINT8_MAX, &second) < 0 || second == 0)
            return -EINVAL;
        selector->kind = SIANO_DEVICE_SELECTOR_PORT;
        selector->bus = (uint8_t)first;
        selector->ports[0] = (uint8_t)second;
        selector->port_count = 1;
        while (*cursor == '.') {
            unsigned port;
            cursor++;
            if (selector->port_count == sizeof(selector->ports) ||
                parse_number(&cursor, UINT8_MAX, &port) < 0 || port == 0)
                return -EINVAL;
            selector->ports[selector->port_count++] = (uint8_t)port;
        }
        return *cursor == '\0' ? 0 : -EINVAL;
    }
    return -EINVAL;
}
