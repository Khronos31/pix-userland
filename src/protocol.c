#include "channel.h"
#include "pixy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static void le32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int fail_code(pixy *d, int code, const char *s) {
    if (!d->failure_exit) d->failure_exit = code;
    snprintf(d->error, sizeof(d->error), "%s", s);
    return -1;
}
static int fail(pixy *d, const char *s) {
    return fail_code(d, d->failure_context ? d->failure_context : 7, s);
}
void pixy_init(pixy *d, pixy_io io) {
    memset(d, 0, sizeof(*d));
    d->io = io;
}
static int bulk(pixy *d, uint8_t ep, uint8_t *p, size_t *n, unsigned ms) {
    if (d->io.bulk(d->io.user, ep, p, n, ms))
        return fail_code(d, 7, "USB bulk transfer failed");
    return 0;
}
static int fx(pixy *d, uint8_t op, uint32_t param, const uint8_t *p, size_t n, uint8_t out[64],
              int check) {
    uint8_t req[128];
    if (n > 122)
        return fail(d, "FX2 request too long");
    req[0] = op;
    req[1] = ++d->fxseq;
    le32(req + 2, param);
    if (n)
        memcpy(req + 6, p, n);
    size_t len = n + 6;
    if (bulk(d, 1, req, &len, 2000))
        return -1;
    len = 64;
    if (bulk(d, 0x81, out, &len, 2000))
        return -1;
    if (len < 6 || out[1] != d->fxseq)
        return fail(d, "Invalid FX2 response");
    if (check && out[0])
        return fail(d, "FX2 command rejected");
    return (int)len;
}
static int simple(pixy *d, uint8_t op, uint32_t par, const uint8_t *p, size_t n) {
    uint8_t out[64];
    return fx(d, op, par, p, n, out, 1) < 0 ? -1 : 0;
}
static int gpio_read(pixy *d, uint8_t g[2]) {
    uint8_t out[64];
    if (fx(d, 7, 0, NULL, 0, out, 1) < 8)
        return fail(d, "Truncated GPIO");
    memcpy(g, out + 6, 2);
    return 0;
}
static int gpio_write(pixy *d, uint8_t g[2]) {
    return simple(d, 7, 2, g, 2);
}
int pixy_power(pixy *d, const uint8_t *fw, size_t n) {
    uint8_t z = 0, g[2];
    if (n != 699673)
        return fail_code(d, 10, "DT181 FPGA size must be 699673 bytes");
    d->powered = 1;
    if (simple(d, 2, 1, &z, 1) || simple(d, 9, 0, NULL, 0) || gpio_read(d, g))
        return -1;
    g[1] &= 0x74;
    if (gpio_write(d, g))
        return -1;
    sleep_ms(200);
    g[1] |= 8;
    if (gpio_write(d, g))
        return -1;
    sleep_ms(5);
    g[0] |= 1;
    if (gpio_write(d, g))
        return -1;
    sleep_ms(15);
    if (simple(d, 8, 0, NULL, 0) || simple(d, 10, 0, NULL, 0))
        return -1;
    size_t chunk = n >> 4;
    uint8_t *expanded = malloc(chunk * 16);
    if (!expanded)
        return fail_code(d, 70, "Out of memory");
    for (size_t off = 0; off < n;) {
        size_t take = n - off < chunk ? n - off : chunk;
        for (size_t i = 0; i < take; i++)
            for (unsigned bit = 0; bit < 8; bit++) {
                uint8_t v = (fw[off + i] >> bit) & 1;
                expanded[16 * i + 2 * bit] = v;
                expanded[16 * i + 2 * bit + 1] = v | 2;
            }
        size_t len = take * 16;
        if (bulk(d, 8, expanded, &len, 20000)) {
            free(expanded);
            return -1;
        }
        off += take;
    }
    free(expanded);
    z = 1;
    if (simple(d, 11, 0, NULL, 0) || simple(d, 12, 1, &z, 1) || gpio_read(d, g))
        return -1;
    sleep_ms(50);
    g[1] |= 3;
    if (gpio_write(d, g) || gpio_read(d, g))
        return -1;
    g[1] |= 0x80;
    if (gpio_write(d, g))
        return -1;
    return simple(d, 9, 0, NULL, 0);
}
static int command(pixy *d, uint8_t op, const uint8_t *p, size_t n, uint8_t flags, uint8_t *out,
                   size_t *outn) {
    uint8_t b[4096], sum = 0;
    size_t off = (flags & 64) ? 18 : 2;
    if (n > 4000)
        return fail(d, "FPGA request too large");
    b[0] = flags;
    if (flags & 64) {
        if (random_bytes(d->tx, 16))
            return fail_code(d, 70, "Random source failed");
        memcpy(b + 1, d->tx, 16);
    }
    b[off - 1] = ++d->seq;
    b[off] = op;
    le32(b + off + 1, (uint32_t)n);
    for (size_t i = 0; i < off + 5; i++)
        sum += b[i];
    b[off + 5] = sum;
    if (n)
        memcpy(b + off + 6, p, n);
    size_t len = off + 6 + n;
    if (flags & 128) {
        if (len < off + 16) {
            if (random_bytes(b + len, off + 16 - len))
                return fail_code(d, 70, "Random source failed");
            len = off + 16;
        }
        aes_ctr c;
        ctr_init(&c, d->key, d->tx, 8);
        ctr_xor(&c, b + off, len - off);
        memcpy(d->tx, c.iv, 16);
        secure_clear(&c, sizeof(c));
    }
    if (bulk(d, 8, b, &len, 2000))
        return -1;
    len = sizeof(b);
    if (bulk(d, 0x86, b, &len, 3000))
        return -1;
    if (len < 1)
        return fail(d, "Empty FPGA response");
    off = (b[0] & 64) ? 18 : 2;
    if (len < off + 6)
        return fail(d, "Truncated FPGA response");
    if (b[0] & 64)
        memcpy(d->rx, b + 1, 16);
    if (b[0] & 128) {
        aes_ctr c;
        ctr_init(&c, d->key, d->rx, 8);
        ctr_xor(&c, b + off, len - off);
        memcpy(d->rx, c.iv, 16);
        secure_clear(&c, sizeof(c));
    }
    sum = 0;
    for (size_t i = 0; i < off + 5; i++)
        sum += b[i];
    if (b[off - 1] != d->seq || sum != b[off + 5])
        return fail(d, "FPGA response sequence/checksum mismatch");
    size_t got = get32(b + off + 1);
    if (got > len - off - 6 || got > *outn)
        return fail(d, "Invalid FPGA response length");
    if (b[off]) {
        if (!d->failure_exit) d->failure_exit = d->failure_context ? d->failure_context : 7;
        snprintf(d->error, sizeof(d->error), "FPGA opcode 0x%02x status %u", op, b[off]);
        return -1;
    }
    if (got)
        memcpy(out, b + off + 6, got);
    *outn = got;
    secure_clear(b, sizeof(b));
    return 0;
}
int pixy_certify(pixy *d) {
    uint8_t p[8], r[8];
    size_t n = 8;
    if (random_bytes(p, 4))
        return fail_code(d, 70, "Random source failed");
    memcpy(p + 4, d->token, 4);
    if (command(d, 4, p, 8, 0xc0, r, &n))
        return -1;
    if (n != 8)
        return fail(d, "Invalid keepalive");
    memcpy(d->token, r + 4, 4);
    return 0;
}
int pixy_session(pixy *d, const uint8_t bootstrap[16], const uint8_t validation[64]) {
    uint8_t p[228] = {0}, reply[128], mod[128], priv[128], secret[128];
    size_t n = 4;
    int rc = -1;
    memcpy(d->key, bootstrap, 16);
    if (command(d, 0, NULL, 0, 0xc0, p, &n) || n != 4)
        return fail(d, "Preparation failed");
    if (rsa_generate(mod, priv))
        return fail_code(d, 70, "Session RSA generation failed");
    le32(p + 4, (uint32_t)time(NULL));
    /* Locally administered per-session host identifier; not a network MAC lookup. */
    if (random_bytes(p + 14, 6)) {
        fail_code(d, 70, "Random source failed");
        goto done;
    }
    p[14] = (p[14] & 0xfe) | 2;
    memcpy(p + 20, mod, 128);
    memcpy(p + 164, validation, 64);
    n = 128;
    if (command(d, 1, p, sizeof(p), 0x80, reply, &n))
        goto done;
    if (n != 128) { fail(d, "Invalid session response length"); goto done; }
    if (rsa_apply(reply, priv, mod, secret)) {
        fail(d, "Invalid session RSA response");
        goto done;
    }
    aes128 a;
    aes_init(&a, secret + 112);
    aes_encrypt(&a, p + 148, d->key);
    secure_clear(&a, sizeof(a));
    n = 128;
    if (command(d, 0x90, NULL, 0, 0xc0, reply, &n) || pixy_certify(d))
        goto done;
    d->session_ready = 1;
    rc = 0;
done:
    secure_clear(priv, sizeof(priv));
    secure_clear(secret, sizeof(secret));
    secure_clear(p, sizeof(p));
    return rc;
}
int pixy_status(pixy *d, uint8_t *r, size_t *n) {
    return command(d, 0x80, NULL, 0, 0xc0, r, n);
}
int pixy_fifo_clear(pixy *d) {
    return simple(d, 5, 0, NULL, 0);
}
int pixy_start(pixy *d, uint8_t context[32]) {
    size_t n = 32;
    if (command(d, 0x10, NULL, 0, 0xc0, context, &n))
        return -1;
    d->streaming = 1;
    return n == 32 ? 0 : fail(d, "Bad stream context");
}
int pixy_stop(pixy *d) {
    if (!d->streaming)
        return 0;
    d->streaming = 0;
    uint8_t r[64];
    size_t n = 64;
    return command(d, 0x11, NULL, 0, 0xc0, r, &n);
}
int pixy_shutdown(pixy *d) {
    int rc = 0;
    if (d->streaming && pixy_stop(d))
        rc = -1;
    if (!d->powered)
        return rc;
    d->powered = 0;
    uint8_t g[2], z = 0;
    if (gpio_read(d, g))
        return -1;
    const uint8_t masks[] = {0x7f, 0xfd, 0xfe};
    for (unsigned i = 0; i < 3; i++) {
        g[1] &= masks[i];
        if (gpio_write(d, g))
            rc = -1;
    }
    sleep_ms(15);
    g[0] &= 0xfe;
    if (gpio_write(d, g))
        rc = -1;
    sleep_ms(5);
    g[1] &= 0xf7;
    if (gpio_write(d, g))
        rc = -1;
    if (simple(d, 12, 1, &z, 1))
        rc = -1;
    secure_clear(d->key, 16);
    return rc;
}
static int i2c(pixy *d, uint8_t op, const uint8_t *p, size_t n, uint8_t r[64]) {
    for (unsigned i = 0; i < 5; i++) {
        int len = fx(d, op, (uint32_t)n, p, n, r, 0);
        if (len < 0)
            return -1;
        if (!r[0])
            return len;
        if (r[0] != 1)
            break;
    }
    return fail(d, "I2C command rejected");
}
static int wr(pixy *d, uint8_t slave, uint8_t reg, uint8_t val) {
    uint8_t p[] = {2, 0, 0, 0, slave, reg, val}, r[64];
    return i2c(d, 3, p, sizeof(p), r) < 0 ? -1 : 0;
}
static int rd(pixy *d, uint8_t slave, uint8_t reg, uint8_t *data, uint8_t n) {
    uint8_t p[] = {1, 0, n, 0, slave, reg}, r[64];
    int len = i2c(d, 4, p, sizeof(p), r);
    if (len < 6 + n)
        return fail(d, "Truncated I2C response");
    memcpy(data, r + 6, n);
    return 0;
}
static int through(pixy *d, uint8_t slave, const uint8_t *v, size_t n) {
    uint8_t p[100] = {0}, r[64];
    if (n > 90)
        return -1;
    p[0] = (uint8_t)(n + 2);
    p[4] = slave;
    p[5] = 254;
    p[6] = 192;
    memcpy(p + 7, v, n);
    return i2c(d, 3, p, n + 7, r) < 0 ? -1 : 0;
}
static int pairs(pixy *d, uint8_t slave, const uint8_t *p, size_t n) {
    for (size_t i = 0; i + 1 < n; i += 2) {
        if (p[i] == 255)
            break;
        if (wr(d, slave, p[i], p[i + 1]))
            return -1;
    }
    return 0;
}
static const uint8_t ofdm_init[] = {
    0x1,  0x0,  0x2,  0x0,  0x3,  0x0,  0x4,  0x2,  0x5,  0x0,  0x6,  0x80, 0x7,  0x0,  0x8,  0x0,
    0xc,  0x0,  0xf,  0x34, 0x11, 0x27, 0x12, 0xc,  0x13, 0x33, 0x14, 0x20, 0x15, 0x40, 0x16, 0x0,
    0x17, 0x70, 0x18, 0x31, 0x19, 0x13, 0x1a, 0x31, 0x1b, 0x13, 0x1c, 0x38, 0x1d, 0x88, 0x1e, 0xa2,
    0x1f, 0x8,  0x20, 0x0,  0x21, 0xff, 0x22, 0x80, 0x23, 0x4c, 0x24, 0x4c, 0x25, 0x0,  0x26, 0x0,
    0x27, 0xc,  0x28, 0x60, 0x29, 0x6b, 0x2a, 0x40, 0x2b, 0x40, 0x2c, 0x5a, 0x2d, 0x5,  0x2e, 0xff,
    0x2f, 0x0,  0x30, 0x20, 0x31, 0xd,  0x32, 0x21, 0x34, 0xf,  0x38, 0x0,  0x39, 0xaa, 0x3a, 0x2d,
    0x3b, 0x21, 0x3c, 0x3f, 0x3d, 0x10, 0x3e, 0x8,  0x3f, 0xc,  0x40, 0xc,  0x41, 0x0,  0x42, 0x0,
    0x43, 0x4f, 0x44, 0xff, 0x46, 0x20, 0x47, 0x0,  0x48, 0x90, 0x49, 0xe6, 0x4a, 0x2,  0x4b, 0x80,
    0x4c, 0x0,  0x4f, 0x5,  0x50, 0x0,  0x51, 0x68, 0x52, 0x20, 0x54, 0x57, 0x55, 0xf1, 0x56, 0x20,
    0x57, 0x70, 0x5c, 0x50, 0x5d, 0x0,  0x5f, 0x80, 0x70, 0x38, 0x71, 0x0,  0x72, 0x0,  0x75, 0x2,
    0x76, 0x0,  0x77, 0x1,  0x7c, 0x0,  0x7d, 0xd2, 0xba, 0x0,  0xbb, 0x0,  0xbc, 0x0,  0xc7, 0x0,
    0xe4, 0xf4, 0xec, 0x0,  0xef, 0x1};
static const uint8_t psk_init[] = {
    0x3,  0x0,  0x4,  0x2,  0x6,  0x40, 0x7,  0x41, 0x8,  0x0,  0x9,  0x0,  0xa,  0xff, 0xc,  0x59,
    0xe,  0xf0, 0xf,  0x50, 0x10, 0xb2, 0x11, 0x0,  0x12, 0x30, 0x13, 0x80, 0x14, 0x0,  0x15, 0x0,
    0x17, 0x0,  0x1a, 0x0,  0x1b, 0x0,  0x1c, 0xa,  0x1d, 0xaa, 0x1e, 0x20, 0x1f, 0x88, 0x20, 0x0,
    0x38, 0x40, 0x39, 0x10, 0x3b, 0x90, 0x51, 0xb0, 0x52, 0x89, 0x53, 0xb3, 0x5a, 0x2d, 0x5b, 0xd3,
    0x85, 0x69, 0x87, 0x0,  0x8d, 0x0,  0xa3, 0x11, 0xa4, 0x0,  0xa5, 0x40, 0xa6, 0x4};
#define TH(d, s, ...)                                                                              \
    through(d, s, (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))
int pixy_tuner_init(pixy *d) {
    if (pairs(d, 24, ofdm_init, sizeof(ofdm_init)) || TH(d, 24, 255))
        return -1;
    sleep_ms(50);
    if (TH(d, 24, 0x00, 0x3f, 0x02, 0, 0x03, 0x98, 4, 0, 5, 9, 6, 0x10, 0x2e, 0x15, 0x30, 0x10,
           0x45, 0x58, 0x48, 0x19, 0x52, 3, 0x53, 0x44, 0x6a, 0x4b, 0x76, 0, 0x78, 0x18, 0x7a, 0x17,
           0x85, 6) ||
        TH(d, 24, 1, 1) || wr(d, 25, 1, 0x90) || pairs(d, 25, psk_init, sizeof(psk_init)))
        return -1;
    if (TH(d, 25, 9, 3) || TH(d, 25, 6, 0x2a, 0xe8, 0x84) ||
        TH(d, 25, 0x0a, 0x20, 0x78, 0x1e, 0x0a) || TH(d, 25, 0x0f, 8, 0x0f) ||
        TH(d, 25, 0x18, 0x40))
        return -1;
    return TH(d, 24, 4, 0);
}
int pixy_frequency(pixy_system s, unsigned ch, uint32_t *hz) {
    if (s == PIXY_TERRESTRIAL) {
        if (ch < 13 || ch > 62)
            return -1;
        *hz = 473142857 + (ch - 13) * 6000000;
    } else if (s == PIXY_BS) {
        if (ch < 1 || ch > 23 || !(ch & 1))
            return -1;
        *hz = 1049480000 + (ch - 1) / 2 * 38360000;
    } else {
        if (ch < 2 || ch > 24 || (ch & 1))
            return -1;
        *hz = 1613000000 + (ch - 2) / 2 * 40000000;
    }
    return 0;
}
int pixy_tune(pixy *d, pixy_system s, uint32_t hz, int lna, int *tsid) {
    return pixy_tune_slot(d, s, hz, lna, tsid, -1);
}
int pixy_tune_slot(pixy *d, pixy_system s, uint32_t hz, int lna, int *tsid, int slot) {
    if (slot < -1 || slot > 7 || (slot >= 0 && (*tsid >= 0 || s == PIXY_TERRESTRIAL)))
        return fail_code(d, 2, "Invalid satellite slot selection");
    uint8_t v, a, b;
    if (s == PIXY_TERRESTRIAL) {
        if (wr(d, 24, 0x0f, 0x1c) || wr(d, 25, 0x1c, 0xaa) || wr(d, 25, 0x1f, 0xaa) ||
            wr(d, 24, 0x1e, 0xa2) || wr(d, 24, 0x1f, 8) || rd(d, 24, 0x1c, &v, 1) ||
            wr(d, 24, 0x1c, (v & 12) | (lna ? 32 : 48)))
            return -1;
        uint64_t magic = 274877907;
        uint32_t word = (uint32_t)((magic * (hz + 500) >> 32) & 536870848);
        word = (uint32_t)((magic * (word + 500) >> 32) >> 6) & 65535;
        if (wr(d, 24, 37, 0) || wr(d, 24, 35, 77) || TH(d, 24, 15, 0) || TH(d, 24, 12, 21) ||
            TH(d, 24, 13, word & 255, 14, word >> 8) ||
            TH(d, 24, 31, 135, 32, 31, 33, 135, 34, 31) || TH(d, 24, 128, word >= 21312 ? 65 : 1) ||
            TH(d, 24, 15, 1) || wr(d, 24, 1, 64) || wr(d, 24, 35, 76))
            return -1;
        for (unsigned i = 0; i < 30; i++) {
            sleep_ms(100);
            if (rd(d, 24, 128, &a, 1) || rd(d, 24, 176, &b, 1))
                return -1;
            if (!(a & 0x28) && (b & 15) > 7)
                return 0;
            if (a & 128)
                break;
        }
    } else {
        if (wr(d, 24, 15, 0x34) || wr(d, 24, 30, 0xaa) || wr(d, 24, 31, 0xa8) ||
            wr(d, 25, 28, 10) || wr(d, 25, 31, 0x88) || rd(d, 24, 28, &v, 1) ||
            wr(d, 24, 28, (v & 0x30) | 4) || wr(d, 25, 28, lna ? 7 : 6) ||
            TH(d, 25, 4, lna ? 0 : 0x40))
            return -1;
        int id = *tsid < 0 ? 0 : *tsid;
        if (wr(d, 25, 0x8f, (uint8_t)(id >> 8)) || wr(d, 25, 0x90, (uint8_t)id))
            return -1;
        uint32_t div = (uint32_t)(((uint64_t)hz * (hz < 1075000000 ? 2 : 1) + 500000) / 1000000);
        uint8_t band = hz < 1100000000   ? 1
                       : hz < 1200000000 ? 0x21
                       : hz < 1400000000 ? 0x41
                       : hz < 1500000000 ? 0x51
                       : hz < 1600000000 ? 0x61
                                         : 0x71;
        if (TH(d, 25, 0, div >> 8, div & 255, band, hz < 1075000000 ? 8 : 0) ||
            TH(d, 25, 2, band | 2))
            return -1;
        sleep_ms(100);
        if (wr(d, 25, 3, 1))
            return -1;
        for (unsigned i = 0; i < 20; i++) {
            sleep_ms(100);
            if (rd(d, 25, 0xc3, &a, 1))
                return -1;
            if (!(a & 0x10)) {
                uint8_t ids[16];
                if (rd(d, 25, 0xce, ids, 16))
                    return -1;
                int chosen = pix_select_tsid(ids, *tsid, slot);
                if (chosen < 0)
                    continue;
                *tsid = chosen;
                if (wr(d, 25, 0x8f, (uint8_t)(chosen >> 8)) || wr(d, 25, 0x90, (uint8_t)chosen))
                    return -1;
                sleep_ms(200);
                return 0;
            }
        }
    }
    return fail_code(d, 5, "No signal lock or requested satellite TSID absent");
}
