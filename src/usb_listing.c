/* SPDX-License-Identifier: MIT */
#include "usb_listing.h"
#include <string.h>
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
        const char *model = d->product == 0x103b ? "PIX-DT295" : "PIX-DT181-PU0";
        unsigned product = d->product ? d->product : 0x102b;
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
            fprintf(out, "}],\"candidates\":[],\"receivers\":[{\"receiver\":0,\"device\":1,\"local\":0,"
                  "\"systems\":%s,\"lnb_15v_supported\":false}]}",
                    product == 0x103b ? "[\"ISDB-T\"]"
                                      : "[\"ISDB-T\",\"ISDB-S\"]");
        } else {
            fprintf(out,
                    "model=%s usb=06b8:%04x bus=%u address=%u port=%s "
                    "status=ready receivers=1 index=%u\n"
                    "receiver=0 device=1 local=0 system=%s lnb_15v_supported=false\n",
                    model, product, d->bus, d->address, have_port ? port : "unknown", d->index,
                    product == 0x103b ? "ISDB-T" : "ISDB-T/S");
        }
    }
    if (json)
        fputs("],\"ungrouped_usb_devices\":[]}\n", out);
    return fflush(out) || ferror(out) ? -1 : 0;
}
