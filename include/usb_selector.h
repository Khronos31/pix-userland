#ifndef PIX_USB_SELECTOR_H
#define PIX_USB_SELECTOR_H
#include <stddef.h>
#include <stdint.h>
#define USB_SELECTOR_MAX_PORTS 7
/* All selector components are decimal. A physical path has at least one port. */
typedef enum { USB_SELECT_INDEX, USB_SELECT_ADDRESS, USB_SELECT_PATH, USB_SELECT_SERIAL }
    usb_selector_kind;
typedef enum { USB_SERIAL_ABSENT, USB_SERIAL_READABLE, USB_SERIAL_UNREADABLE } usb_serial_state;
typedef struct {
    usb_selector_kind kind;
    unsigned index;
    uint8_t bus, address, ports[USB_SELECTOR_MAX_PORTS];
    unsigned port_count;
    /* serial:TEXT keeps the caller's bytes; comparison is exact and case preserving. */
    const char *serial;
} usb_selector;
int usb_selector_parse(const char *, usb_selector *);
int usb_selector_matches(const usb_selector *, unsigned index, unsigned bus, unsigned address,
                         const uint8_t *ports, unsigned port_count);
int usb_selector_matches_serial(const usb_selector *, usb_serial_state, const char *);
/* Returns 0 on success. Unknown/empty paths are not formatted. */
int usb_selector_format_path(char *, size_t, unsigned bus, const uint8_t *, unsigned port_count);
#endif
