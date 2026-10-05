/* SPDX-License-Identifier: MIT */
#include "usb_selector.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
static int decimal(const char **cursor, unsigned max, unsigned *out) {
    const char *p = *cursor;
    unsigned value = 0;
    if (*p < '0' || *p > '9')
        return -1;
    do {
        unsigned digit = (unsigned)(*p++ - '0');
        if (value > max / 10 || (value == max / 10 && digit > max % 10))
            return -1;
        value = value * 10 + digit;
    } while (*p >= '0' && *p <= '9');
    *cursor = p;
    *out = value;
    return 0;
}
int usb_selector_parse(const char *text, usb_selector *out) {
    usb_selector result = {0};
    unsigned first, value;
    if (!text || !out || decimal(&text, UINT_MAX, &first))
        return -1;
    if (!*text) {
        result.kind = USB_SELECT_INDEX;
        result.index = first;
    } else {
        if (first > UINT8_MAX)
            return -1;
        result.bus = (uint8_t)first;
        if (*text == ':') {
            text++;
            if (decimal(&text, UINT8_MAX, &value) || *text)
                return -1;
            result.kind = USB_SELECT_ADDRESS;
            result.address = (uint8_t)value;
        } else if (*text == '-') {
            result.kind = USB_SELECT_PATH;
            do {
                text++;
                if (result.port_count == USB_SELECTOR_MAX_PORTS ||
                    decimal(&text, UINT8_MAX, &value) || !value)
                    return -1;
                result.ports[result.port_count++] = (uint8_t)value;
            } while (*text == '.');
            if (*text)
                return -1;
        } else
            return -1;
    }
    *out = result;
    return 0;
}
int usb_selector_matches(const usb_selector *selector, unsigned index, unsigned bus,
                         unsigned address, const uint8_t *ports, unsigned port_count) {
    if (!selector)
        return 0;
    if (selector->kind == USB_SELECT_INDEX)
        return index == selector->index;
    if (bus != selector->bus)
        return 0;
    if (selector->kind == USB_SELECT_ADDRESS)
        return address == selector->address;
    return selector->kind == USB_SELECT_PATH && ports && port_count &&
           port_count <= USB_SELECTOR_MAX_PORTS && port_count == selector->port_count &&
           !memcmp(ports, selector->ports, port_count);
}
int usb_selector_format_path(char *out, size_t size, unsigned bus, const uint8_t *ports,
                             unsigned port_count) {
    if (!out || !size || !ports || !port_count || port_count > USB_SELECTOR_MAX_PORTS ||
        bus > UINT8_MAX)
        return -1;
    int n = snprintf(out, size, "%u", bus);
    if (n < 0 || (size_t)n >= size)
        return -1;
    size_t used = (size_t)n;
    for (unsigned i = 0; i < port_count; i++) {
        if (!ports[i])
            return -1;
        n = snprintf(out + used, size - used, "%c%u", i ? '.' : '-', ports[i]);
        if (n < 0 || (size_t)n >= size - used)
            return -1;
        used += (size_t)n;
    }
    return 0;
}
