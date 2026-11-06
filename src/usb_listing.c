/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "usb_listing.h"
#include <string.h>

/* Sibling boot-mode identities of the mb86 series, as listed by mAgicVISION's
 * MIT driver. Only the first two have a receiver path in this tree; the rest are
 * named so enumeration can say what it saw without ever claiming the device.
 * usb_listing_write names the supported ones from this table. */
static const usb_pix_model pix_models[] = {
    {0x06b8, 0x102b, "PIX-DT181-PU0", 1, "dt181"},
    {0x06b8, 0x103b, "PIX-DT295", 1, "dt295"},
    {0x06b8, 0x1038, "PIX-DT410", 0, NULL},
    {0x06b8, 0x1039, "PIX-DT420", 0, NULL},
    {0x06b8, 0x103c, "PIX-DT295W", 0, NULL},
    {0x04c5, 0x202f, "PIX-DTxxx", 0, NULL},
};

const usb_pix_model *usb_pix_model_find(unsigned vendor, unsigned product) {
    for (size_t i = 0; i < sizeof pix_models / sizeof *pix_models; i++)
        if (pix_models[i].vendor == vendor && pix_models[i].product == product)
            return &pix_models[i];
    return NULL;
}

/* The listing keeps its two-model contract: unsupported siblings are never
 * listed, so anything outside the supported set falls back to legacy DT181. */
static const char *listed_model(unsigned product) {
    const usb_pix_model *known = usb_pix_model_find(0x06b8u, product);
    if (known && known->supported)
        return known->name;
    return product == 0x103b ? "PIX-DT295" : "PIX-DT181-PU0";
}
static void json_serial(FILE *out, const usb_device_info *device) {
    if (device->serial_state != USB_SERIAL_READABLE) {
        fputs("null", out);
        return;
    }
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)device->serial; *p; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', out);
            fputc(*p, out);
        } else if (*p < 0x20)
            fprintf(out, "\\u%04x", *p);
        else
            fputc(*p, out);
    }
    fputc('"', out);
}
/* -1 unknown, otherwise uniqueness within this scan. A serial-less device has
 * no uniqueness to report, and one unreadable device makes the answer unknown
 * for the others rather than claiming true or false. */
static int serial_unique(const usb_device_info *devices, size_t count,
                         const usb_device_info *device) {
    if (device->serial_state != USB_SERIAL_READABLE)
        return -1;
    for (size_t i = 0; i < count; i++) {
        if (&devices[i] == device)
            continue;
        if (devices[i].serial_state == USB_SERIAL_UNREADABLE)
            return -1;
        if (devices[i].serial_state == USB_SERIAL_READABLE &&
            !strcmp(devices[i].serial, device->serial))
            return 0;
    }
    return 1;
}
int usb_listing_write(FILE *out, const usb_device_info *devices, size_t count, int json) {
    if (json)
        fputs("{\"enclosures\":[", out);
    for (size_t i = 0; i < count; i++) {
        const usb_device_info *d = &devices[i];
        unsigned product = d->product ? d->product : 0x102b;
        const char *model = listed_model(product);
        char port[40];
        int have_port = !usb_selector_format_path(port, sizeof(port), d->bus, d->ports,
                                                  d->port_count);
        if (json) {
            if (i)
                fputc(',', out);
            /* Identifiers and topology paths are generated here; the serial is
             * the only device-provided string and is escaped. */
            fprintf(out, "{\"index\":%u,\"serial\":", d->index);
            json_serial(out, d);
            fprintf(out,
                    ",\"model\":\"%s\","
                    "\"usb\":\"06b8:%04x\",\"status\":\"ready\",\"serial_unique\":",
                    model, product);
            int unique = serial_unique(devices, count, d);
            fputs(unique < 0 ? "null" : unique ? "true" : "false", out);
            fprintf(out,
                    ",\"devices\":[{\"device\":1,\"serial\":");
            json_serial(out, d);
            fprintf(out, ",\"bus\":%u,\"address\":%u,\"port\":", d->bus, d->address);
            if (have_port)
                fprintf(out, "\"%s\"", port);
            else
                fputs("null", out);
            fputs("}],\"candidates\":[],\"receivers\":[{\"receiver\":0,\"device\":1,"
                  "\"local\":0,\"systems\":[\"ISDB-T\",\"ISDB-S\"],"
                  "\"lnb_15v_supported\":false}]}", out);
        } else {
            fprintf(out,
                    "model=%s usb=06b8:%04x bus=%u address=%u port=%s "
                    "status=ready receivers=1 index=%u\n"
                    "receiver=0 device=1 local=0 system=ISDB-T/S lnb_15v_supported=false\n",
                    model, product, d->bus, d->address, have_port ? port : "unknown", d->index);
        }
    }
    if (json)
        fputs("],\"ungrouped_usb_devices\":[]}\n", out);
    return fflush(out) || ferror(out) ? -1 : 0;
}
