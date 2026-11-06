#ifndef PIX_USB_LISTING_H
#define PIX_USB_LISTING_H
#include "usb_selector.h"
#include <stdio.h>
#define USB_SERIAL_MAX 256
typedef struct {
    unsigned index, bus, address;
    uint8_t ports[USB_SELECTOR_MAX_PORTS];
    unsigned port_count;
    uint16_t product; /* zero preserves legacy DT181 synthetic callers. */
    usb_serial_state serial_state;
    char serial[USB_SERIAL_MAX];
} usb_device_info;
/* A PIX boot-mode identity this build can name, whether or not it can drive it.
 * "supported" means a receiver path exists here; the other entries exist so a
 * plugged-in sibling explains itself instead of looking like no device. */
typedef struct {
    unsigned vendor, product;
    const char *name;
    int supported;
    const char *flag; /* --model value that selects it; NULL if undrivable. */
} usb_pix_model;
const usb_pix_model *usb_pix_model_find(unsigned vendor, unsigned product);
/* DT181/DT295 descriptor enumeration only; never claims the receiver. A serial
 * string is read without claiming and is absent, unreadable, or readable; JSON
 * exposes unreadable/absent as null rather than guessing. "ready" describes the
 * known receiver layout, not successful interface claiming or signal reception. */
int usb_listing_write(FILE *, const usb_device_info *, size_t count, int json);
#endif
