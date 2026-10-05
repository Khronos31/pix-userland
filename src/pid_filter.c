/* SPDX-License-Identifier: MIT */
#include "pid_filter.h"
#include <string.h>
int pix_pid_filter(uint8_t *packets, size_t *length, const uint8_t selected[8192]) {
    if (!packets || !length || !selected || *length % 188) return -1;
    size_t out = 0;
    for (size_t pos = 0; pos < *length; pos += 188) {
        if (packets[pos] != 0x47) return -1;
        unsigned pid = ((packets[pos + 1] & 31u) << 8) | packets[pos + 2];
        if (selected[pid]) { memmove(packets + out, packets + pos, 188); out += 188; }
    }
    *length = out; return 0;
}
