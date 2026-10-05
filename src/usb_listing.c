/* SPDX-License-Identifier: MIT */
#include "usb_listing.h"
int usb_listing_write(FILE *out, const usb_device_info *devices, size_t count, int json) {
    if (json)
        fputs("{\"enclosures\":[", out);
    for (size_t i = 0; i < count; i++) {
        const usb_device_info *d = &devices[i];
        char port[40];
        int have_port = !usb_selector_format_path(port, sizeof(port), d->bus, d->ports,
                                                  d->port_count);
        if (json) {
            if (i)
                fputc(',', out);
            /* Strings are fixed identifiers or decimal-only topology paths;
             * no device-provided strings enter this document. */
            fprintf(out,
                    "{\"index\":%u,\"serial\":null,\"model\":\"PIX-DT181-PU0\","
                    "\"usb\":\"06b8:102b\",\"status\":\"ready\",\"serial_unique\":false,"
                    "\"devices\":[{\"device\":1,\"serial\":null,\"bus\":%u,\"address\":%u,\"port\":",
                    d->index, d->bus, d->address);
            if (have_port)
                fprintf(out, "\"%s\"", port);
            else
                fputs("null", out);
            fputs("}],\"candidates\":[],\"receivers\":[{\"receiver\":0,\"device\":1,\"local\":0,"
                  "\"system\":\"ISDB-T/S\",\"lnb_15v_supported\":false}]}", out);
        } else {
            fprintf(out,
                    "model=PIX-DT181-PU0 usb=06b8:102b bus=%u address=%u port=%s "
                    "status=ready receivers=1 index=%u\n"
                    "receiver=0 device=1 local=0 system=ISDB-T/S lnb_15v_supported=false\n",
                    d->bus, d->address, have_port ? port : "unknown", d->index);
        }
    }
    if (json)
        fputs("],\"ungrouped_usb_devices\":[]}\n", out);
    return fflush(out) || ferror(out) ? -1 : 0;
}
