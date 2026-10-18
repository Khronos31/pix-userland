/* SPDX-License-Identifier: MIT */
#include "channel.h"
#include <limits.h>
#include <string.h>
static int decimal(const char **cursor, unsigned *result) {
    const char *p = *cursor;
    unsigned n = 0, digits = 0;
    while (*p >= '0' && *p <= '9') {
        unsigned d = (unsigned)(*p++ - '0');
        if (n > (UINT_MAX - d) / 10) return -1;
        n = n * 10 + d; digits++;
    }
    if (!digits) return -1;
    *cursor = p; *result = n; return 0;
}
int pix_channel_parse(const char *text, pixy_system system, pix_channel *out) {
    if (!text || !out) return -1;
    const char *p = text;
    int named = 0, slot = -1;
    if (*p == 'T') { system = PIXY_TERRESTRIAL; p++; named = 1; }
    else if (!strncmp(p, "BS", 2)) { system = PIXY_BS; p += 2; named = 1; }
    else if (!strncmp(p, "CS", 2)) { system = PIXY_CS; p += 2; named = 1; }
    unsigned ch, n;
    if (decimal(&p, &ch)) return -1;
    if (*p == '_' && named && system != PIXY_TERRESTRIAL) {
        p++;
        if (decimal(&p, &n) || n > 7) return -1;
        slot = (int)n;
    }
    uint32_t hz;
    if (*p || pixy_frequency(system, ch, &hz)) return -1;
    *out = (pix_channel){system, ch, slot}; return 0;
}
int pix_select_tsid(const uint8_t ids[16], int requested_tsid, int slot) {
    if (!ids || slot < -1 || slot > 7 || (slot >= 0 && requested_tsid >= 0)) return -1;
    for (unsigned i = 0; i < 8; i++) {
        int id = ids[i * 2] * 256 + ids[i * 2 + 1];
        if (slot >= 0 && i != (unsigned)slot) continue;
        if (id != 0 && id != 65535 && (requested_tsid < 0 || requested_tsid == id)) return id;
    }
    return -1;
}
