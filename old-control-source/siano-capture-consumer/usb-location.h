/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIANO_USB_LOCATION_H
#define SIANO_USB_LOCATION_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Where a device sits on the bus, for --list: " bus=B address=A port=P".
 * B and A name the usbfs node (/dev/bus/usb/BBB/AAA on Linux).  P is the
 * device's name under /sys/bus/usb/devices on Linux (bus-port.port...), or
 * "-" when libusb cannot tell the port path.  A device without a serial,
 * such as the PX-S1UD, is told apart by P.  None of this opens the device.
 *
 * Returns 0, or -1 when the buffer is too small.
 */
static inline int siano_format_usb_location(char *buffer, size_t size, uint8_t bus,
                                            uint8_t address, const uint8_t *ports,
                                            int port_count)
{
    size_t used;
    int written;
    int index;

    written = snprintf(buffer, size, " bus=%u address=%u port=", (unsigned int)bus,
                       (unsigned int)address);
    if (written < 0 || (size_t)written >= size)
        return -1;
    used = (size_t)written;
    if (port_count <= 0) {
        written = snprintf(buffer + used, size - used, "-");
        return written < 0 || (size_t)written >= size - used ? -1 : 0;
    }
    written = snprintf(buffer + used, size - used, "%u-%u", (unsigned int)bus,
                       (unsigned int)ports[0]);
    if (written < 0 || (size_t)written >= size - used)
        return -1;
    used += (size_t)written;
    for (index = 1; index < port_count; index++) {
        written = snprintf(buffer + used, size - used, ".%u", (unsigned int)ports[index]);
        if (written < 0 || (size_t)written >= size - used)
            return -1;
        used += (size_t)written;
    }
    return 0;
}

#endif
