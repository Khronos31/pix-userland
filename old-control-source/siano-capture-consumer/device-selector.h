/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIANO_DEVICE_SELECTOR_H
#define SIANO_DEVICE_SELECTOR_H

#include <stdint.h>

enum siano_device_selector_kind {
    SIANO_DEVICE_SELECTOR_INDEX,
    SIANO_DEVICE_SELECTOR_PORT,
    SIANO_DEVICE_SELECTOR_BUS_ADDRESS,
};

struct siano_device_selector {
    enum siano_device_selector_kind kind;
    unsigned index;
    uint8_t bus;
    uint8_t address;
    uint8_t ports[8];
    unsigned port_count;
};

int siano_parse_device_selector(const char *text, struct siano_device_selector *selector);

#endif
