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
    usb_device_info devices[] = {{0, 1, 7, {2, 3}, 2}, {1, 9, 42, {0}, 0}};
    /* Allows an independent JSON parser to inspect synthetic output. */
    if (argc > 1)
        return usb_listing_write(stdout, devices, 2, 1) ? 1 : 0;
    char text[4096];
    output(NULL, 0, 1, text, sizeof(text));
    assert(!strcmp(text, "{\"enclosures\":[],\"ungrouped_usb_devices\":[]}\n"));
    output(NULL, 0, 0, text, sizeof(text));
    assert(!text[0]);
    output(devices, 2, 0, text, sizeof(text));
    assert(strstr(text, "model=PIX-DT181-PU0 usb=06b8:102b bus=1 address=7 port=1-2.3"));
    assert(strstr(text, "bus=9 address=42 port=unknown"));
    assert(strstr(text, "receiver=0 device=1 local=0 system=ISDB-T/S lnb_15v_supported=false"));
    output(devices, 2, 1, text, sizeof(text));
    assert(strstr(text, "\"port\":\"1-2.3\""));
    assert(strstr(text, "\"port\":null"));
    assert(strstr(text, "\"serial_unique\":false"));
    assert(strstr(text, "\"system\":\"ISDB-T/S\",\"lnb_15v_supported\":false"));
    puts("USB enumeration text/JSON and unknown-topology tests passed");
    return 0;
}
