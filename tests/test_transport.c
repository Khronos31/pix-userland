#include "transport.h"
#include "vectors.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct output {
    uint8_t data[1024];
    size_t n;
};
static int sink(void *u, const uint8_t *p, size_t n) {
    struct output *o = u;
    assert(o->n + n <= sizeof(o->data));
    memcpy(o->data + o->n, p, n);
    o->n += n;
    return 0;
}
static void make_packet(uint8_t p[188], unsigned pid, const uint8_t *section, size_t n) {
    memset(p, 255, 188);
    p[0] = 0x47;
    p[1] = 0x40 | (pid >> 8);
    p[2] = pid & 255;
    p[3] = 0x10;
    p[4] = 0;
    memcpy(p + 5, section, n);
}
int main(void) {
    const size_t chunks[] = {1, 2, 7, 37, 187, 188, 189, 376, 752};
    for (unsigned c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        ts_decoder *d = calloc(1, sizeof(*d));
        assert(d);
        struct output o = {0};
        ts_decoder_init(d, context, sink, &o, NULL);
        for (size_t i = 0; i < sizeof(wire);) {
            size_t n = sizeof(wire) - i;
            if (n > chunks[c])
                n = chunks[c];
            assert(!ts_decoder_feed(d, wire + i, n));
            i += n;
        }
        assert(o.n == sizeof(plain) && !memcmp(o.data, plain, sizeof(plain)));
        assert(d->markers == 2 && d->output == 2 && !d->resync);
        free(d);
    }
    ts_decoder *d = calloc(1, sizeof(*d));
    struct output o = {0};
    ts_decoder_init(d, context, sink, &o, NULL);
    assert(!ts_decoder_feed(d, wire + 376, sizeof(wire) - 376));
    assert(!o.n && d->waiting == 2);
    ts_decoder_init(d, context, sink, &o, NULL);
    uint8_t malformed[188];
    memcpy(malformed, wire, 188);
    malformed[29] = 64;
    assert(ts_decoder_feed(d, malformed, 188) < 0);
    free(d);
    ts_check *check = calloc(1, sizeof(*check));
    assert(check);
    ts_check_init(check);
    uint8_t p[188];
    assert(ts_crc((const uint8_t *)"123456789", 9) == 0x0376e6e7);
    make_packet(p, 0, pat_section, sizeof(pat_section));
    ts_check_packet(check, p);
    assert(check->pat == 1 && check->psi_count == 2);
    ts_check_packet(check, p);
    assert(check->pat == 1 && !check->continuity); /* Duplicate packet. */
    make_packet(p, 256, pmt_section, sizeof(pmt_section));
    ts_check_packet(check, p);
    assert(check->pmt == 1 && !check->crc_errors);
    p[3] = 0x12;
    ts_check_packet(check, p);
    assert(check->continuity == 1);
    p[3] = 0x13;
    p[10] ^= 1;
    ts_check_packet(check, p);
    assert(check->crc_errors == 1);
    memset(p, 255, 188);
    p[0] = 0x47;
    p[1] = 1;
    p[2] = 0;
    p[3] = 0x3e;
    p[4] = 1;
    p[5] = 128;
    ts_check_packet(check, p);
    assert(check->continuity == 1); /* Explicit discontinuity. */
    free(check);
    puts("Transport vectors, USB chunk boundaries, PSI CRC, duplicate/discontinuity tests passed");
    return 0;
}
