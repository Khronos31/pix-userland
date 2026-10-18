/* SPDX-License-Identifier: MIT */
#include "usb_selector.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    usb_selector s;
    assert(!usb_selector_parse("0", &s));
    assert(s.kind == USB_SELECT_INDEX && usb_selector_matches(&s, 0, 3, 7, NULL, 0));
    assert(!usb_selector_matches(&s, 1, 3, 7, NULL, 0));
    char index[32];
    snprintf(index, sizeof(index), "%u", UINT_MAX);
    assert(!usb_selector_parse(index, &s) && s.index == UINT_MAX);
    assert(!usb_selector_parse("001:007", &s));
    assert(s.kind == USB_SELECT_ADDRESS && usb_selector_matches(&s, 4, 1, 7, NULL, 0));
    assert(!usb_selector_matches(&s, 4, 2, 7, NULL, 0));
    assert(!usb_selector_matches(&s, 4, 1, 8, NULL, 0));
    uint8_t ports[] = {2, 4, 1};
    assert(!usb_selector_parse("003-02.4.1", &s));
    assert(s.kind == USB_SELECT_PATH && usb_selector_matches(&s, 0, 3, 42, ports, 3));
    assert(!usb_selector_matches(&s, 0, 3, 42, ports, 2));
    assert(!usb_selector_matches(&s, 0, 4, 42, ports, 3));
    assert(!usb_selector_matches(&s, 0, 3, 42, NULL, 0));
    char path[40];
    assert(!usb_selector_format_path(path, sizeof(path), 3, ports, 3));
    assert(!strcmp(path, "3-2.4.1"));
    assert(!usb_selector_parse(path, &s));
    assert(usb_selector_matches(&s, 99, 3, 1, ports, 3));
    assert(usb_selector_format_path(path, 4, 3, ports, 3));
    assert(usb_selector_format_path(path, sizeof(path), 3, ports, 0));
    const char *bad[] = {"", "-1", "+1", " 1", "1 ", "0x1", "1:2:3", "1:", ":2",
                         "256:1", "1:256", "1-", "1-0", "1-256", "1-2.", "1-.2",
                         "1-2..3", "1-2:3", "1-2.3.4.5.6.7.8.9", "9999999999999999999999"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++)
        assert(usb_selector_parse(bad[i], &s));
    assert(usb_selector_parse(NULL, &s));
    assert(usb_selector_parse("0", NULL));
    assert(!usb_selector_parse("255-255.255.255.255.255.255.255", &s));
    puts("USB selectors: index, address, topology and invalid-input tests passed");
    return 0;
}
