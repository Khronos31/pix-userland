/* SPDX-License-Identifier: MIT */
#include "usb_listing.h"
#include <assert.h>
#include <string.h>
static void output(const usb_device_info *devices, size_t count, int json, char *text, size_t size) {
    FILE *file = tmpfile();
    assert(file);
    assert(!usb_listing_write(file, devices, count, json));
    rewind(file);
    size_t n = fread(text, 1, size - 1, file);
    assert(!ferror(file) && n < size - 1);
    text[n] = 0;
    fclose(file);
}
int main(int argc, char **argv) {
    (void)argv;
    usb_device_info devices[] = {{0, 1, 7, {2, 3}, 2, 0x102b, USB_SERIAL_ABSENT, {0}},
                                 {1, 9, 42, {0}, 0, 0x103b, USB_SERIAL_ABSENT, {0}}};
    devices[1].serial_state = USB_SERIAL_READABLE;
    strcpy(devices[1].serial, "Case-9");
    /* Allows an independent JSON parser to inspect synthetic output. */
    if (argc > 1)
        return usb_listing_write(stdout, devices, 2, 1) ? 1 : 0;
    char text[4096];
    output(NULL, 0, 1, text, sizeof(text));
    assert(!strcmp(text, "{\"enclosures\":[],\"ungrouped_usb_devices\":[]}\n"));
    output(NULL, 0, 0, text, sizeof(text));
    assert(!text[0]);
    usb_device_info legacy = {.index = 0, .bus = 1, .address = 7,
                             .ports = {2, 3}, .port_count = 2, .product = 0};
    output(&legacy, 1, 0, text, sizeof(text));
    assert(!strcmp(text,
        "model=PIX-DT181-PU0 usb=06b8:102b bus=1 address=7 port=1-2.3 status=ready receivers=1 index=0\n"
        "receiver=0 device=1 local=0 system=ISDB-T/S lnb_15v_supported=false\n"));
    output(&legacy, 1, 1, text, sizeof(text));
    assert(strstr(text, "\"model\":\"PIX-DT181-PU0\",\"usb\":\"06b8:102b\""));
    output(devices, 2, 0, text, sizeof(text));
    assert(strstr(text, "model=PIX-DT181-PU0 usb=06b8:102b bus=1 address=7 port=1-2.3"));
    assert(strstr(text, "model=PIX-DT295 usb=06b8:103b bus=9 address=42 port=unknown"));
    assert(strstr(text, "system=ISDB-T/S lnb_15v_supported=false"));
    assert(strstr(text, "receiver=0 device=1 local=0 system=ISDB-T/S lnb_15v_supported=false"));
    output(devices, 2, 1, text, sizeof(text));
    assert(strstr(text, "\"port\":\"1-2.3\""));
    assert(strstr(text, "\"port\":null"));
    assert(strstr(text, "\"index\":0,\"serial\":null,\"model\":\"PIX-DT181-PU0\""));
    assert(strstr(text, "\"serial_unique\":null"));
    assert(strstr(text, "\"serial\":\"Case-9\",\"model\":\"PIX-DT295\",\"usb\":\"06b8:103b\","
                       "\"status\":\"ready\",\"serial_unique\":true"));
    assert(strstr(text, "\"systems\":[\"ISDB-T\",\"ISDB-S\"],\"lnb_15v_supported\":false"));
    assert(strstr(text, "\"model\":\"PIX-DT295\""));
    assert(strstr(text, "\"usb\":\"06b8:103b\""));
    assert(strstr(text, "\"index\":1,\"serial\":\"Case-9\",\"model\":\"PIX-DT295\""));
    assert(strstr(text, "\"systems\":[\"ISDB-T\",\"ISDB-S\"],\"lnb_15v_supported\":false"));

    /* Two readable devices share a serial: neither is unique. An unreadable
     * serial makes uniqueness unknown instead of claiming true or false. */
    usb_device_info duplicate[] = {
        {0, 1, 7, {2}, 1, 0x103b, USB_SERIAL_ABSENT, {0}},
        {1, 1, 8, {3}, 1, 0x103b, USB_SERIAL_ABSENT, {0}}};
    for (unsigned i = 0; i < 2; i++) {
        duplicate[i].serial_state = USB_SERIAL_READABLE;
        strcpy(duplicate[i].serial, "Case-9");
    }
    output(duplicate, 2, 1, text, sizeof(text));
    assert(!strstr(text, "\"serial_unique\":true"));
    assert(strstr(text, "\"serial_unique\":false"));
    duplicate[1].serial_state = USB_SERIAL_UNREADABLE;
    duplicate[1].serial[0] = 0;
    output(duplicate, 2, 1, text, sizeof(text));
    assert(strstr(text, "\"index\":0,\"serial\":\"Case-9\""));
    assert(strstr(text, "\"serial_unique\":null"));
    assert(strstr(text, "\"index\":1,\"serial\":null"));

    /* Device-provided text is escaped and keeps its case. */
    duplicate[1].serial_state = USB_SERIAL_READABLE;
    strcpy(duplicate[1].serial, "A\"b\\c");
    output(duplicate, 2, 1, text, sizeof(text));
    assert(strstr(text, "\"serial\":\"A\\\"b\\\\c\""));
    assert(strstr(text, "\"serial_unique\":true"));
    puts("USB enumeration text/JSON, serial identity and unknown-topology tests passed");
    return 0;
}
