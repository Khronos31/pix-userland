/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../usb-location.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    char buffer[64];
    const uint8_t one[] = { 2 };
    const uint8_t hub[] = { 2, 3, 1 };
    const uint8_t deep[] = { 1, 2, 3, 4, 5, 6, 7 };

    assert(siano_format_usb_location(buffer, sizeof(buffer), 1, 4, one, 1) == 0);
    assert(strcmp(buffer, " bus=1 address=4 port=1-2") == 0);

    assert(siano_format_usb_location(buffer, sizeof(buffer), 3, 17, hub, 3) == 0);
    assert(strcmp(buffer, " bus=3 address=17 port=3-2.3.1") == 0);

    assert(siano_format_usb_location(buffer, sizeof(buffer), 255, 127, deep, 7) == 0);
    assert(strcmp(buffer, " bus=255 address=127 port=255-1.2.3.4.5.6.7") == 0);

    /* libusb could not tell the port path (LIBUSB_ERROR_OVERFLOW / not supported). */
    assert(siano_format_usb_location(buffer, sizeof(buffer), 1, 4, NULL, 0) == 0);
    assert(strcmp(buffer, " bus=1 address=4 port=-") == 0);
    assert(siano_format_usb_location(buffer, sizeof(buffer), 1, 4, NULL, -8) == 0);
    assert(strcmp(buffer, " bus=1 address=4 port=-") == 0);

    /* Too small at each stage. */
    assert(siano_format_usb_location(buffer, 8, 1, 4, one, 1) == -1);
    assert(siano_format_usb_location(buffer, 24, 3, 17, hub, 3) == -1);
    assert(siano_format_usb_location(buffer, 27, 3, 17, hub, 3) == -1);
    assert(siano_format_usb_location(buffer, 23, 1, 4, NULL, 0) == -1);
    return 0;
}
