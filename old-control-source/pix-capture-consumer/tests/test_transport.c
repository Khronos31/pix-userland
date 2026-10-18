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
static void test_key_rotation(void) {
    ts_decoder *d = calloc(1, sizeof(*d));
    struct output o = {0};
    uint8_t table_b[188], table_c[188], iv_b[188];
    assert(d);
    memcpy(table_b, wire, 188);
    memcpy(table_c, wire, 188);
    memcpy(iv_b, wire + 188, 188);
    /* CTR bit flips change only the synthetic key ID, keeping the content key. */
    table_b[30 + 36] ^= 0x80;
    table_c[30 + 36] ^= 0x40;
    iv_b[40] ^= 0x80;
    ts_decoder_init(d, context, sink, &o, NULL);
    assert(!ts_decoder_feed(d, wire, 188));
    assert(!ts_decoder_feed(d, table_b, 188));
    /* A new table can arrive before the stream stops referencing the old one. */
    assert(!ts_decoder_feed(d, wire + 188, sizeof(wire) - 188));
    assert(o.n == sizeof(plain) && !memcmp(o.data, plain, sizeof(plain)));
    o.n = 0;
    assert(!ts_decoder_feed(d, iv_b, 188));
    assert(!ts_decoder_feed(d, wire + 376, sizeof(wire) - 376));
    assert(o.n == sizeof(plain) && !memcmp(o.data, plain, sizeof(plain)));
    assert(!ts_decoder_feed(d, table_c, 188));
    o.n = 0;
    assert(!ts_decoder_feed(d, iv_b, 188));
    assert(!ts_decoder_feed(d, wire + 376, sizeof(wire) - 376));
    assert(o.n == sizeof(plain) && !memcmp(o.data, plain, sizeof(plain)));
    /* Only the immediately preceding generation remains eligible. */
    assert(ts_decoder_feed(d, wire + 188, 188) < 0);
    free(d);
}
struct observed {
    unsigned begin, es, end, cc;
};
static void observe(void *u, const char *kind, unsigned pid, uint64_t packet, unsigned expected,
                    unsigned actual) {
    (void)pid;
    (void)packet;
    (void)expected;
    (void)actual;
    struct observed *o = u;
    if (!strcmp(kind, "pmt_begin"))
        o->begin++;
    if (!strcmp(kind, "pmt_es"))
        o->es++;
    if (!strcmp(kind, "pmt_end"))
        o->end++;
    if (!strcmp(kind, "cc"))
        o->cc++;
}
static void crc_section(uint8_t *p, size_t n) {
    uint32_t crc = ts_crc(p, n - 4);
    for (unsigned i = 0; i < 4; i++)
        p[n - 4 + i] = (uint8_t)(crc >> (24 - i * 8));
}
static void test_pmt_observation(void) {
    ts_check *c = calloc(1, sizeof(*c));
    assert(c);
    ts_check_init(c);
    struct observed o = {0};
    c->issue = observe;
    c->issue_user = &o;
    uint8_t p[188], section[sizeof(pmt_section)];
    make_packet(p, 0, pat_section, sizeof(pat_section));
    ts_check_packet(c, p);
    make_packet(p, 256, pmt_section, sizeof(pmt_section));
    ts_check_packet(c, p);
    assert(o.begin == 1 && o.es == 1 && o.end == 1);
    p[3] = 0x11;
    ts_check_packet(c, p); /* Same PMT in another TS packet: no repeat. */
    assert(o.begin == 1);
    memcpy(section, pmt_section, sizeof(section));
    section[5] += 2;
    section[14] = 0x11;
    crc_section(section, sizeof(section));
    /* Old audio PID history survives PMT changes: observation must not hide errors. */
    c->seen[273] = 1;
    c->cc[273] = 2;
    make_packet(p, 256, section, sizeof(section));
    p[3] = 0x12;
    ts_check_packet(c, p);
    assert(o.begin == 2 && o.es == 2 && c->seen[273] && c->cc[273] == 2);
    memset(p, 255, sizeof(p));
    p[0] = 0x47;
    p[1] = 1;
    p[2] = 17;
    p[3] = 0x15;
    ts_check_packet(c, p);
    assert(o.cc == 1 && c->continuity == 1);
    section[5] &= ~1;
    crc_section(section, sizeof(section));
    make_packet(p, 256, section, sizeof(section));
    p[3] = 0x13;
    ts_check_packet(c, p);
    assert(o.begin == 2); /* Future PMT is not the active map. */
    section[5] |= 1;
    section[16] = 100;
    crc_section(section, sizeof(section));
    make_packet(p, 256, section, sizeof(section));
    p[3] = 0x14;
    ts_check_packet(c, p);
    assert(o.begin == 2); /* CRC-valid but malformed descriptor length. */
    free(c);
}
static void test_resync_splits(void) {
    const size_t chunks[] = {1, 2, 7, 37, 187, 188, 189, 375, 376, 377, 563, 564, 565, 4096, 32768};
    uint8_t data[sizeof(wire) + 2];
    data[0] = 0x12;
    data[1] = 0x34;
    memcpy(data + 2, wire, sizeof(wire));
    for (unsigned i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        ts_decoder *d = calloc(1, sizeof(*d));
        struct output o = {0};
        assert(d);
        ts_decoder_init(d, context, sink, &o, NULL);
        for (size_t off = 0; off < sizeof(data);) {
            size_t n = sizeof(data) - off;
            if (n > chunks[i])
                n = chunks[i];
            assert(!ts_decoder_feed(d, data + off, n));
            off += n;
        }
        assert(d->resync == 2 && !d->waiting && d->iv_updates == 1);
        assert(o.n == sizeof(plain) && !memcmp(o.data, plain, sizeof(plain)));
        free(d);
    }
}
int main(void) {
    test_key_rotation();
    test_pmt_observation();
    test_resync_splits();
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
