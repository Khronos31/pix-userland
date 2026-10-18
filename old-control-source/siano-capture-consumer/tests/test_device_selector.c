/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../device-selector.h"
#include "../usb-location.h"

#include <assert.h>
#include <string.h>

int main(void)
{
    struct siano_device_selector selector;
    char location[128];

    assert(siano_parse_device_selector("12", &selector) == 0);
    assert(selector.kind == SIANO_DEVICE_SELECTOR_INDEX && selector.index == 12);
    assert(siano_parse_device_selector("1-2", &selector) == 0);
    assert(selector.kind == SIANO_DEVICE_SELECTOR_PORT && selector.bus == 1 &&
           selector.port_count == 1 && selector.ports[0] == 2);
    assert(siano_format_usb_location(location, sizeof(location), selector.bus, 4,
                                     selector.ports, (int)selector.port_count) == 0);
    assert(strcmp(location, " bus=1 address=4 port=1-2") == 0);
    assert(siano_parse_device_selector("1-4.3", &selector) == 0);
    assert(selector.port_count == 2 && selector.ports[0] == 4 && selector.ports[1] == 3);
    assert(siano_format_usb_location(location, sizeof(location), selector.bus, 4,
                                     selector.ports, (int)selector.port_count) == 0);
    assert(strcmp(location, " bus=1 address=4 port=1-4.3") == 0);
    assert(siano_parse_device_selector("0-1.3", &selector) == 0);
    assert(selector.kind == SIANO_DEVICE_SELECTOR_PORT && selector.bus == 0 &&
           selector.port_count == 2 && selector.ports[0] == 1 && selector.ports[1] == 3);
    assert(siano_parse_device_selector("0-1.1", &selector) == 0);
    assert(selector.kind == SIANO_DEVICE_SELECTOR_PORT && selector.bus == 0 &&
           selector.port_count == 2 && selector.ports[0] == 1 && selector.ports[1] == 1);
    assert(siano_parse_device_selector("3:17", &selector) == 0);
    assert(selector.kind == SIANO_DEVICE_SELECTOR_BUS_ADDRESS && selector.bus == 3 &&
           selector.address == 17);
    assert(siano_parse_device_selector("0:4", &selector) == 0);
    assert(selector.kind == SIANO_DEVICE_SELECTOR_BUS_ADDRESS && selector.bus == 0 &&
           selector.address == 4);
    assert(siano_parse_device_selector("0:3", &selector) == 0);
    assert(selector.kind == SIANO_DEVICE_SELECTOR_BUS_ADDRESS && selector.bus == 0 &&
           selector.address == 3);
    assert(siano_parse_device_selector("", &selector) < 0);
    assert(siano_parse_device_selector("1-0", &selector) < 0);
    assert(siano_parse_device_selector("1-2.x", &selector) < 0);
    assert(siano_parse_device_selector("1:256", &selector) < 0);
    assert(siano_parse_device_selector("0-0.1", &selector) < 0);
    assert(siano_parse_device_selector("0-1.0", &selector) < 0);
    assert(siano_parse_device_selector("0-256.1", &selector) < 0);
    assert(siano_parse_device_selector("0-1.256", &selector) < 0);
    assert(siano_parse_device_selector("0:0", &selector) < 0);
    assert(siano_parse_device_selector("256:4", &selector) < 0);
    assert(siano_parse_device_selector("x", &selector) < 0);
    return 0;
}
