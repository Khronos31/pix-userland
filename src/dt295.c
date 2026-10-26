/* PIX-DT295 target-0 protocol. State-changing commands are never retried. */
#include "dt295.h"
#include "crypto.h"
#include "dt295_crypto.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAT_BASE 0x841d0u
#define MAT_SIZE 0x1280u
#define ASSET_SECTIONS 6
#define MAX_BUNDLE (32u * 1024u * 1024u)
#define USB_TIMEOUT (-7)
#define DT295_DEADLINE 1

struct dt295_assets {
    uint8_t *idle, *trc, *material;
    size_t idle_n, trc_n, material_n;
    dt295_field *fields;
    size_t field_count;
    uint8_t key_request[12], iv[16];
};
struct dt295 {
    dt295_io io;
    const dt295_assets *assets;
    uint8_t session_key[16], session_iv[16], ts_key[16];
    uint32_t renewal_counter;
    uint64_t renewal_due;
    uint16_t state;
    uint8_t has_session, has_ts_key, prepared, started, card_notified;
    uint8_t card_notified_state, pause_state, tuned, poisoned, card_recovery_failed, cleanup_only;
    uint64_t card_notification_generation, card_activation_generation;
    uint64_t card_recovery_generation;
    uint8_t card_activation_valid;
    uint8_t card_recovery_generation_valid;
};

static void seterr(char *e, size_t n, const char *s) {
    if (e && n)
        snprintf(e, n, "%s", s);
}
static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void putbe32(uint8_t *p, uint32_t x) {
    p[0] = x >> 24;
    p[1] = x >> 16;
    p[2] = x >> 8;
    p[3] = x;
}
static uint64_t now_ms(dt295 *d) {
    return d->io.now_ms ? d->io.now_ms(d->io.user) : 0;
}
static void sleep_ms(dt295 *d, unsigned ms) {
    if (d->io.sleep_ms)
        d->io.sleep_ms(d->io.user, ms);
}

int dt295_assets_load_bytes(const uint8_t *data, size_t length, dt295_assets **out, char *error,
                            size_t error_size) {
    if (!data || !out) {
        seterr(error, error_size, "invalid asset data/output");
        return -1;
    }
    *out = NULL;
    uint8_t head[8 + ASSET_SECTIONS * 36];
    if (length < sizeof(head) || memcmp(data, "D295A1\0\0", 8)) {
        seterr(error, error_size, "invalid DT295 asset bundle header");
        return -1;
    }
    memcpy(head, data, sizeof(head));
    uint32_t lens[ASSET_SECTIONS];
    const uint8_t *sections[ASSET_SECTIONS];
    size_t total = 0;
    for (unsigned i = 0; i < ASSET_SECTIONS; i++) {
        lens[i] = le32(head + 8 + i * 36);
        if (lens[i] > MAX_BUNDLE - total) {
            seterr(error, error_size, "DT295 asset bundle size limit");
            return -1;
        }
        total += lens[i];
    }
    if (length != sizeof(head) + total) {
        seterr(error, error_size, "DT295 asset bundle length is invalid");
        return -1;
    }
    if (lens[0] == 0 || lens[0] > DT295_ASSET_MAX_IMAGE || lens[1] == 0 ||
        lens[1] > DT295_ASSET_MAX_IMAGE || lens[2] != MAT_SIZE || lens[3] == 0 || lens[3] % 8 ||
        lens[3] > DT295_MAX_FIELDS * 8 || lens[4] != 12 || lens[5] != 16) {
        seterr(error, error_size, "DT295 asset section size is invalid");
        return -1;
    }
    size_t offset = sizeof(head);
    for (unsigned i = 0; i < ASSET_SECTIONS; i++) {
        sections[i] = data + offset;
        uint8_t hash[32];
        dt295_sha256(sections[i], lens[i], hash);
        if (memcmp(hash, head + 8 + i * 36 + 4, 32)) {
            seterr(error, error_size, "DT295 asset section SHA-256 mismatch");
            return -1;
        }
        offset += lens[i];
    }
    dt295_assets *a = calloc(1, sizeof(*a));
    if (!a) {
        seterr(error, error_size, "out of memory loading DT295 assets");
        return -1;
    }
    uint8_t *raw[ASSET_SECTIONS] = {0};
    for (unsigned i = 0; i < ASSET_SECTIONS; i++) {
        raw[i] = malloc(lens[i]);
        if (!raw[i])
            goto bad;
        memcpy(raw[i], sections[i], lens[i]);
    }
    a->idle = raw[0];
    a->idle_n = lens[0];
    raw[0] = NULL;
    a->trc = raw[1];
    a->trc_n = lens[1];
    raw[1] = NULL;
    a->material = raw[2];
    a->material_n = lens[2];
    raw[2] = NULL;
    a->field_count = lens[3] / 8;
    a->fields = calloc(a->field_count, sizeof(*a->fields));
    if (!a->fields)
        goto bad;
    for (size_t i = 0; i < a->field_count; i++) {
        uint8_t *p = raw[3] + i * 8;
        a->fields[i] = (dt295_field){le16(p), p[2], p[3], le16(p + 4)};
        if (le16(p + 6) != 0)
            goto bad;
        dt295_field *x = &a->fields[i];
        if (x->address < 0x1000 || x->address > 0x1afe || (x->address & 1) || x->shift > 15 ||
            !x->width || x->width > 16 - x->shift || x->value >= (1u << x->width))
            goto bad;
    }
    uint16_t dma_config = 0;
    for (size_t i = 0; i < a->field_count; i++) {
        const dt295_field *x = &a->fields[i];
        if (x->address == 0x1002) {
            uint16_t mask = (uint16_t)(((1u << x->width) - 1) << x->shift);
            dma_config = (uint16_t)((dma_config & ~mask) | (x->value << x->shift));
        }
    }
    if (((dma_config >> 8) & 3) > 1)
        goto bad;
    memcpy(a->key_request, raw[4], 12);
    memcpy(a->iv, raw[5], 16);
    for (unsigned i = 0; i < ASSET_SECTIONS; i++)
        free(raw[i]);
    *out = a;
    seterr(error, error_size, "");
    return 0;
bad:
    for (unsigned i = 0; i < ASSET_SECTIONS; i++)
        free(raw[i]);
    dt295_assets_free(a);
    seterr(error, error_size, "DT295 asset bundle integrity or contents invalid");
    return -1;
}

static int read_bundle(FILE *f, uint8_t **data, size_t *length) {
    const size_t limit = MAX_BUNDLE + 8 + ASSET_SECTIONS * 36;
    size_t capacity = 8192, used = 0;
    uint8_t *buffer = malloc(capacity);
    if (!buffer)
        return -1;
    for (;;) {
        if (used == capacity) {
            if (capacity >= limit + 1) {
                secure_clear(buffer, capacity);
                free(buffer);
                return -1;
            }
            size_t next = capacity > (limit + 1) / 2 ? limit + 1 : capacity * 2;
            uint8_t *grown = malloc(next);
            if (!grown) {
                secure_clear(buffer, capacity);
                free(buffer);
                return -1;
            }
            memcpy(grown, buffer, used);
            secure_clear(buffer, capacity);
            free(buffer);
            buffer = grown;
            capacity = next;
        }
        size_t count = fread(buffer + used, 1, capacity - used, f);
        used += count;
        if (!count) {
            if (ferror(f)) {
                secure_clear(buffer, capacity);
                free(buffer);
                return -1;
            }
            break;
        }
    }
    if (used > limit) {
        secure_clear(buffer, capacity);
        free(buffer);
        return -1;
    }
    *data = buffer;
    *length = used;
    return 0;
}

int dt295_assets_load(const char *path, dt295_assets **out, char *error, size_t error_size) {
    if (!path || !out) {
        seterr(error, error_size, "invalid asset path/output");
        return -1;
    }
    *out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (error && error_size)
            snprintf(error, error_size, "cannot open DT295 asset bundle: %s", strerror(errno));
        return -1;
    }
    uint8_t *data = NULL;
    size_t length = 0;
    int read_error = read_bundle(f, &data, &length);
    fclose(f);
    if (read_error) {
        seterr(error, error_size, "cannot read DT295 asset bundle");
        return -1;
    }
    int result = dt295_assets_load_bytes(data, length, out, error, error_size);
    secure_clear(data, length);
    free(data);
    return result;
}
void dt295_assets_free(dt295_assets *a) {
    if (!a)
        return;
    if (a->idle) {
        secure_clear(a->idle, a->idle_n);
        free(a->idle);
    }
    if (a->trc) {
        secure_clear(a->trc, a->trc_n);
        free(a->trc);
    }
    if (a->material) {
        secure_clear(a->material, a->material_n);
        free(a->material);
    }
    if (a->fields) {
        secure_clear(a->fields, a->field_count * sizeof(*a->fields));
        free(a->fields);
    }
    secure_clear(a, sizeof(*a));
    free(a);
}

static int fail(dt295 *d, char *e, size_t n, const char *s) {
    if (d)
        d->poisoned = 1;
    seterr(e, n, s);
    return -1;
}
static int ctl(dt295 *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint8_t *buf,
               size_t len, unsigned ms) {
    size_t got = 0;
    if (!d->io.control || d->io.control(d->io.user, rt, req, val, idx, buf, len, ms, &got) != 0 ||
        got != len)
        return -1;
    return 0;
}
static int rd(dt295 *d, uint32_t addr, uint8_t *out, size_t n) {
    if (addr > 0xffffff || n > 65536 || addr + n > 0x1000000)
        return -1;
    size_t pos = 0;
    while (pos < n) {
        size_t k = n - pos;
        if (k > 64)
            k = 64;
        uint32_t a = addr + (uint32_t)pos;
        if (ctl(d, 0xc0, 0xbc, (uint16_t)((a >> 16) << 8), (uint16_t)a, out + pos, k, 1000))
            return -1;
        pos += k;
    }
    return 0;
}
static int ctl_until(dt295 *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
                     uint8_t *buf, size_t len, uint64_t deadline) {
    const uint64_t now = now_ms(d);
    if (now >= deadline)
        return DT295_DEADLINE;
    uint64_t remaining = deadline - now;
    if (remaining > 1000U)
        remaining = 1000U;
    return ctl(d, rt, req, val, idx, buf, len, (unsigned)remaining);
}
static int rd_until(dt295 *d, uint32_t addr, uint8_t *out, size_t n, uint64_t deadline) {
    if (addr > 0xffffff || n > 65536 || addr + n > 0x1000000)
        return -1;
    size_t pos = 0;
    while (pos < n) {
        size_t k = n - pos;
        if (k > 64)
            k = 64;
        uint32_t a = addr + (uint32_t)pos;
        const int result = ctl_until(d, 0xc0, 0xbc, (uint16_t)((a >> 16) << 8),
                                     (uint16_t)a, out + pos, k, deadline);
        if (result)
            return result;
        pos += k;
    }
    return 0;
}
static int wr_until(dt295 *d, uint32_t addr, const uint8_t *p, size_t n, uint64_t deadline) {
    if (addr > 0xffffff || n > 65536 || addr + n > 0x1000000)
        return -1;
    size_t pos = 0;
    while (pos < n) {
        size_t k = n - pos;
        if (k > 64)
            k = 64;
        uint32_t a = addr + (uint32_t)pos;
        const int result = ctl_until(d, 0x40, 0xbc, (uint16_t)((a >> 16) << 8),
                                     (uint16_t)a, (uint8_t *)p + pos, k, deadline);
        if (result)
            return result;
        pos += k;
    }
    return 0;
}
static int wr(dt295 *d, uint32_t addr, const uint8_t *p, size_t n) {
    if (addr > 0xffffff || n > 65536 || addr + n > 0x1000000)
        return -1;
    size_t pos = 0;
    while (pos < n) {
        size_t k = n - pos;
        if (k > 64)
            k = 64;
        uint32_t a = addr + (uint32_t)pos;
        if (ctl(d, 0x40, 0xbc, (uint16_t)((a >> 16) << 8), (uint16_t)a, (uint8_t *)p + pos, k,
                1000))
            return -1;
        pos += k;
    }
    return 0;
}
static int word(dt295 *d, uint32_t a, uint16_t *v) {
    uint8_t b[2];
    if (rd(d, a, b, 2))
        return -1;
    *v = (uint16_t)b[0] << 8 | b[1];
    if (a == 0x82008)
        d->state = *v;
    return 0;
}
static int word_until(dt295 *d, uint32_t a, uint16_t *v, uint64_t deadline) {
    uint8_t b[2];
    const int result = rd_until(d, a, b, 2, deadline);
    if (result)
        return result;
    *v = (uint16_t)b[0] << 8 | b[1];
    if (a == 0x82008)
        d->state = *v;
    return 0;
}
static int putword(dt295 *d, uint32_t a, uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    return wr(d, a, b, 2);
}
static int putword_until(dt295 *d, uint32_t a, uint16_t v, uint64_t deadline) {
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    return wr_until(d, a, b, 2, deadline);
}
static int field(dt295 *d, uint32_t a, unsigned sh, unsigned width, unsigned value) {
    if (sh > 15 || !width || width > 16 - sh || value >= (1u << width))
        return -1;
    uint16_t v;
    if (word(d, a, &v))
        return -1;
    uint16_t mask = (uint16_t)(((1u << width) - 1) << sh);
    return putword(d, a, (uint16_t)((v & ~mask) | (value << sh)));
}
static int bulk_out(dt295 *d, uint8_t ep, const uint8_t *p, size_t n, unsigned timeout) {
    size_t got = 0;
    if (!d->io.bulk)
        return -1;
    int r = d->io.bulk(d->io.user, ep, (uint8_t *)p, n, timeout, &got);
    return r || got != n ? -1 : 0;
}
static int bulk_in(dt295 *d, uint8_t ep, uint8_t *p, size_t n, unsigned timeout, size_t *got) {
    *got = 0;
    if (!d->io.bulk)
        return -1;
    int r = d->io.bulk(d->io.user, ep, p, n, timeout, got);
    if (r == USB_TIMEOUT)
        return 0;
    return r ? -1 : 0;
}
static int notify(dt295 *d, unsigned ms) {
    uint8_t b[6];
    size_t n = 0;
    if (!d->io.interrupt)
        return -1;
    int r = d->io.interrupt(d->io.user, 0x83, b, sizeof(b), ms, &n);
    return r == USB_TIMEOUT ? 0 : (r || !n ? -1 : 0);
}
static int wait_event(dt295 *d, uint16_t mask, unsigned timeout) {
    uint64_t end = now_ms(d) + timeout;
    for (;;) {
        uint16_t p;
        if (word(d, 0x90074, &p))
            return -1;
        if (p & mask)
            return 0;
        uint64_t now = now_ms(d);
        if (now >= end)
            return -1;
        unsigned left = (unsigned)(end - now);
        if (left > 50)
            left = 50;
        if (notify(d, left))
            return -1;
    }
}
static int drain_state_until(dt295 *d, uint64_t deadline) {
    for (unsigned i = 0; i < 64; i++) {
        uint16_t p, s;
        int result = word_until(d, 0x90074, &p, deadline);
        if (result)
            return result;
        result = word_until(d, 0x82008, &s, deadline);
        if (result)
            return result;
        if (p & 4) {
            result = putword_until(d, 0x90074, 4, deadline);
            if (result)
                return result;
        }
        if (s == 0xffff)
            return -1;
        if (!(p & 4)) {
            d->state = s;
            return 0;
        }
    }
    return -1;
}
static int wait_event_until(dt295 *d, uint16_t mask, uint64_t deadline) {
    for (;;) {
        uint16_t pending;
        const int result = word_until(d, 0x90074, &pending, deadline);
        if (result)
            return result;
        if (pending & mask)
            return 0;
        const uint64_t now = now_ms(d);
        if (now >= deadline)
            return DT295_DEADLINE;
        uint64_t remaining = deadline - now;
        if (remaining > 10U)
            remaining = 10U;
        sleep_ms(d, (unsigned)remaining);
    }
}
static int command_until(dt295 *d, uint8_t cmd, uint8_t sub, uint16_t val, int i2c,
                         uint64_t deadline, int *attempted) {
    uint16_t mask = i2c ? 2 : 8, pending, completion;
    uint8_t b[4] = {cmd, sub, (uint8_t)(val >> 8), (uint8_t)val};
    int result = word_until(d, 0x90074, &pending, deadline);
    if (result)
        return result;
    if (pending & mask)
        return -1;
    result = ctl_until(d, 0x40, i2c ? 0xb9 : 0xb8, 0, 0, b, sizeof(b), deadline);
    if (result == DT295_DEADLINE)
        return result;
    *attempted = 1;
    if (result)
        return -1;
    result = wait_event_until(d, mask, deadline);
    if (result)
        return -1;
    if (word_until(d, i2c ? 0x80016 : 0x80014, &completion, deadline))
        return -1;
    if (putword_until(d, 0x90074, mask, deadline))
        return -1;
    if ((completion >> 8) != cmd || (completion & 255) == 0x80 ||
        (completion & 255) == 0x81)
        return -1;
    return 0;
}
static int command(dt295 *d, uint8_t cmd, uint8_t sub, uint16_t val, int i2c, uint8_t *response) {
    uint16_t mask = i2c ? 2 : 8, pending, completion;
    uint8_t b[4] = {cmd, sub, (uint8_t)(val >> 8), (uint8_t)val};
    if (word(d, 0x90074, &pending) || pending & mask)
        return -1;
    if (ctl(d, 0x40, i2c ? 0xb9 : 0xb8, 0, 0, b, 4, 1000))
        return -1;
    if (wait_event(d, mask, 5000) || word(d, i2c ? 0x80016 : 0x80014, &completion))
        return -1;
    if (cmd == 8 && response && rd(d, 0x83000, response, 512))
        return -1;
    if (putword(d, 0x90074, mask))
        return -1;
    if ((completion >> 8) != cmd || (completion & 255) == 0x80 || (completion & 255) == 0x81)
        return -1;
    return 0;
}
static int secure_command(dt295 *d, uint32_t id, const uint8_t *payload, size_t pn, uint8_t *result,
                          size_t result_cap, size_t *rn) {
    if (pn % 4 || pn > 496 || ((id & 0x600) && !d->has_session))
        return -1;
    uint8_t req[512] = {0}, resp[512];
    putbe32(req, id);
    putbe32(req + 4, 0);
    putbe32(req + 8, (uint32_t)(pn / 4));
    putbe32(req + 12, 0xffffffff);
    if (pn)
        memcpy(req + 16, payload, pn);
    if (id & 0x400) {
        if (dt295_cbc_encrypt(d->session_key, d->session_iv, req + 12, 496))
            return -1;
    }
    if (wr(d, 0x83000, req, sizeof(req)) || command(d, 8, 0, 0, 0, resp))
        goto bad;
    if (id & 0x200 && dt295_payload(d->session_key, d->session_iv, resp + 12, 496))
        goto bad;
    uint32_t rid = be32(resp), status = be32(resp + 4), words = be32(resp + 8);
    if (rid != id || status || words > 124)
        goto bad;
    if (rn) {
        size_t n = (size_t)words * 4;
        if (n > result_cap || (n && !result))
            goto bad;
        if (n)
            memcpy(result, resp + 16, n);
        *rn = n;
    }
    secure_clear(req, sizeof(req));
    secure_clear(resp, sizeof(resp));
    return 0;
bad:
    secure_clear(req, sizeof(req));
    secure_clear(resp, sizeof(resp));
    return -1;
}
static int i2c(dt295 *d, uint8_t addr, const uint8_t *tx, size_t tn, uint8_t *rx, size_t rn) {
    uint16_t pending;
    if (tn > 64 || rn > 64 || (!tn && !rn) || word(d, 0x90074, &pending) || pending & 0x100)
        return -1;
    if (tn) {
        uint8_t buf[64] = {0};
        memcpy(buf, tx, tn);
        if (wr(d, 0x800c0, buf, 64) ||
            command(d, 9, rn ? 2 : 0, (uint16_t)(addr << 8 | tn), 1, NULL))
            return -1;
    }
    if (rn && command(d, 9, tn ? 3 : 1, (uint16_t)(addr << 8 | rn), 1, NULL))
        return -1;
    if (wait_event(d, 0x100, 1000))
        return -1;
    uint8_t b[66];
    if (rd(d, 0x823be, b, 66) || putword(d, 0x90074, 0x100) || b[0] || b[1])
        return -1;
    if (rn)
        memcpy(rx, b + 2, rn);
    return 0;
}
static int drain_state(dt295 *d) {
    for (unsigned i = 0; i < 64; i++) {
        uint16_t p, s;
        if (word(d, 0x90074, &p) || word(d, 0x82008, &s))
            return -1;
        if (p & 4 && putword(d, 0x90074, 4))
            return -1;
        if (s == 0xffff)
            return -1;
        if (!(p & 4)) {
            d->state = s;
            return 0;
        }
    }
    return -1;
}
static int wait_state(dt295 *d, uint16_t expected, unsigned timeout) {
    uint64_t end = now_ms(d) + timeout;
    for (;;) {
        if (drain_state(d))
            return -1;
        if (d->state == expected)
            return 0;
        if (now_ms(d) >= end)
            return -1;
        sleep_ms(d, 10);
    }
}
static int wait_state_after_boot(dt295 *d) {
    uint64_t end = now_ms(d) + 3000;
    for (;;) {
        uint16_t p, s;
        if (word(d, 0x90074, &p) == 0 && word(d, 0x82008, &s) == 0 && (p & 4) &&
            (s == 0x10 || s == 0x11)) {
            d->state = s;
            return putword(d, 0x90074, 4);
        }
        if (now_ms(d) >= end)
            return -1;
        sleep_ms(d, 20);
    }
}
static const uint8_t *mat(dt295 *d, uint32_t va, size_t n) {
    if (va < MAT_BASE || va - MAT_BASE > d->assets->material_n ||
        n > d->assets->material_n - (va - MAT_BASE))
        return NULL;
    return d->assets->material + (va - MAT_BASE);
}
static int cmac_data_material(dt295 *d, uint32_t a, uint32_t b, const uint8_t *p, size_t n,
                              uint8_t out[16]) {
    const uint8_t *x = mat(d, a, 16), *y = mat(d, b, 16);
    uint8_t k[16];
    if (!x || !y)
        return -1;
    for (unsigned i = 0; i < 16; i++)
        k[i] = x[i] ^ y[i];
    dt295_cmac(k, p, n, out);
    secure_clear(k, 16);
    return 0;
}

int dt295_create(dt295 **out, const dt295_io *io, const dt295_assets *assets, char *error,
                 size_t error_size) {
    if (!out || !io || !assets || !io->control || !io->bulk || !io->interrupt || !io->now_ms ||
        !io->sleep_ms) {
        seterr(error, error_size, "DT295 requires complete injected I/O and validated assets");
        return -1;
    }
    dt295 *d = calloc(1, sizeof(*d));
    if (!d) {
        seterr(error, error_size, "out of memory");
        return -1;
    }
    d->io = *io;
    d->assets = assets;
    *out = d;
    seterr(error, error_size, "");
    return 0;
}
void dt295_destroy(dt295 *d) {
    if (!d)
        return;
    secure_clear(d->session_key, 16);
    secure_clear(d->session_iv, 16);
    secure_clear(d->ts_key, 16);
    secure_clear(d, sizeof(*d));
    free(d);
}
uint16_t dt295_state(dt295 *d) {
    if (!d)
        return 0xffff;
    uint16_t s = 0xffff;
    if (word(d, 0x82008, &s))
        return 0xffff;
    return s;
}
int dt295_has_session(const dt295 *d) {
    return d && d->has_session;
}

int dt295_boot(dt295 *d, char *e, size_t en) {
    if (!d || d->poisoned)
        return fail(d, e, en, "DT295 context unavailable");
    uint16_t s;
    if (word(d, 0x82008, &s))
        return fail(d, e, en, "cannot read DT295 firmware state");
    if (s == 0x10) {
        seterr(e, en, "");
        return 0;
    }
    if (s != 0)
        return fail(d, e, en, "firmware boot requires cold state 0 or unauthenticated state 16");
    if (field(d, 0x90194, 12, 3, 1) || putword(d, 0x90198, 0x40))
        return fail(d, e, en, "boot setup failed");
    for (int b = 13; b >= 0; b--)
        if (field(d, 0x90078, (unsigned)b, 1, 0))
            return fail(d, e, en, "boot setup failed");
    for (int b = 11; b >= 0; b--)
        if (field(d, 0x901b4, (unsigned)b, 1, 0))
            return fail(d, e, en, "boot setup failed");
    if (wait_event(d, 4, 100))
        return fail(d, e, en, "firmware upload was not requested");
    uint16_t p;
    if (word(d, 0x90074, &p) || !(p & 4) || field(d, 0x90074, 2, 1, 1))
        return fail(d, e, en, "firmware upload readiness failed");
    for (size_t off = 0; off < d->assets->idle_n; off += 65536) {
        size_t n = d->assets->idle_n - off;
        if (n > 65536)
            n = 65536;
        if (bulk_out(d, 0x02, d->assets->idle + off, n, 300))
            return fail(d, e, en, "idle image upload failed; device state is uncertain");
    }
    for (int b = 13; b >= 0; b--)
        if (field(d, 0x90078, (unsigned)b, 1, b != 4))
            return fail(d, e, en, "firmware start setup failed");
    for (int b = 11; b >= 0; b--)
        if (field(d, 0x901b4, (unsigned)b, 1, 1))
            return fail(d, e, en, "firmware start setup failed");
    /* This trigger is sent once. A timeout is resolved only by observing state. */
    (void)field(d, 0x90070, 2, 1, 1); /* one mutation attempt; verify only by observation */
    if (wait_state_after_boot(d))
        return fail(d, e, en, "firmware boot did not reach idle state");
    if (d->state != 0x10 && d->state != 0x11)
        return fail(d, e, en, "unexpected boot state");
    seterr(e, en, "");
    return 0;
}
int dt295_open_session(dt295 *d, char *e, size_t en) {
    if (!d || d->poisoned || d->has_session)
        return fail(d, e, en, "session open requires a fresh healthy context");
    uint16_t s;
    if (word(d, 0x82008, &s) || s != 0x10)
        return fail(d, e, en, "authentication requires unauthenticated state 0x10");
    uint8_t initial[32] = {0}, response[512] = {0}, reply[20] = {0}, row[32] = {0}, auth[16] = {0},
            session[16] = {0};
    uint8_t selector_data[32] = {0}, auth_input[64] = {0}, seed[64] = {0}, key[16] = {0};
    uint32_t words[8] = {0x7e0, 6, 14, 0x578, 1, 3, 2, 12};
    for (unsigned i = 0; i < 8; i++) {
        uint32_t x = words[i];
        initial[4 * i] = x;
        initial[4 * i + 1] = x >> 8;
        initial[4 * i + 2] = x >> 16;
        initial[4 * i + 3] = x >> 24;
    }
    size_t rn = 0;
    if (secure_command(d, 0x01010000, initial, sizeof(initial), response, sizeof(response), &rn) ||
        rn < 36)
        goto bad;
    memcpy(selector_data, mat(d, 0x85148, 30), 30);
    selector_data[30] = response[32];
    selector_data[31] = initial[6];
    if (cmac_data_material(d, 0x85198, 0x851d8, selector_data, sizeof(selector_data), auth))
        goto bad;
    unsigned sel = auth[15] & 15;
    const uint8_t *r = mat(d, 0x85230 + 32 * sel, 32);
    if (!r)
        goto bad;
    memcpy(row, r, 32);
    memcpy(auth_input, response, 32);
    memcpy(auth_input + 32, row, 32);
    if (cmac_data_material(d, 0x851a8, 0x851e8, auth_input, sizeof(auth_input), reply))
        goto bad;
    if (random_bytes(reply + 16, 4))
        goto bad;
    if (secure_command(d, 0x01020000, reply, sizeof(reply), NULL, 0, NULL))
        goto bad;
    memcpy(seed, reply, 20);
    memcpy(seed + 20, row, 32);
    memcpy(seed + 52, mat(d, 0x85168, 12), 12);
    /* The material at 0x85168 is 12 bytes and the MAC input is 64 bytes. */
    const uint8_t *k1 = mat(d, 0x851b8, 16), *k2 = mat(d, 0x851f8, 16);
    if (!k1 || !k2)
        goto bad;
    for (unsigned i = 0; i < 16; i++)
        key[i] = k1[i] ^ k2[i];
    dt295_cmac(key, seed, sizeof(seed), session);
    secure_clear(key, 16);
    memcpy(d->session_key, session + 12, 4);
    memcpy(d->session_key + 4, session + 8, 4);
    memcpy(d->session_key + 8, session, 8);
    memcpy(d->session_iv, mat(d, 0x85218, 16), 16);
    d->has_session = 1;
    d->renewal_counter = 1;
    d->renewal_due = now_ms(d) + 30000;
    if (field(d, 0x80074, 8, 2, 1) || field(d, 0x80074, 0, 7, 0))
        goto bad_after_auth;
    for (unsigned sh = 12; sh >= 4; sh -= 4)
        if (field(d, 0x80076, sh, 4, 1))
            goto bad_after_auth;
    if (field(d, 0x80078, 8, 8, 0) || field(d, 0x80078, 0, 8, 0) || command(d, 1, 0, 0, 0, NULL) ||
        wait_state(d, 0x11, 5000))
        goto bad_after_auth;
    seterr(e, en, "");
    secure_clear(initial, sizeof(initial));
    secure_clear(response, sizeof(response));
    secure_clear(reply, sizeof(reply));
    secure_clear(row, sizeof(row));
    secure_clear(auth, sizeof(auth));
    secure_clear(session, sizeof(session));
    secure_clear(selector_data, sizeof(selector_data));
    secure_clear(auth_input, sizeof(auth_input));
    secure_clear(seed, sizeof(seed));
    secure_clear(key, sizeof(key));
    return 0;
bad_after_auth:
    /* Authentication completed, so retain only the negotiated session needed
     * for guarded stop/shutdown cleanup. No further setup or renewal is safe. */
    d->cleanup_only = 1;
    d->renewal_counter = 0;
    d->renewal_due = 0;
    goto bad;
bad:
    secure_clear(initial, sizeof(initial));
    secure_clear(response, sizeof(response));
    secure_clear(reply, sizeof(reply));
    secure_clear(row, sizeof(row));
    secure_clear(auth, sizeof(auth));
    secure_clear(session, sizeof(session));
    secure_clear(selector_data, sizeof(selector_data));
    secure_clear(auth_input, sizeof(auth_input));
    secure_clear(seed, sizeof(seed));
    secure_clear(key, sizeof(key));
    return fail(d, e, en, "DT295 authentication failed; state-changing request was not retried");
}

static int card_status(dt295 *d, unsigned *state) {
    for (unsigned i = 0; i < 32; i++) {
        uint16_t pending;
        if (word(d, 0x90074, &pending))
            return -1;
        if (pending & 0x400)
            return -1;
        uint8_t b[0x112];
        size_t n = (pending & 0x200) ? sizeof(b) : 2;
        if (rd(d, 0x822ac, b, n))
            return -1;
        unsigned s = ((unsigned)b[0] << 8 | b[1]) >> 12;
        if (pending & 0x200) {
            d->card_notified = 1;
            d->card_notified_state = (uint8_t)s;
            if (putword(d, 0x90074, 0x200))
                return -1;
            ++d->card_notification_generation;
        } else {
            *state = s;
            return 0;
        }
    }
    return -1;
}
static int card_status_until(dt295 *d, unsigned *state, uint64_t deadline,
                             uint8_t *fresh_notification) {
    const uint64_t generation = d->card_notification_generation;
    for (unsigned i = 0; i < 32; ++i) {
        uint16_t pending;
        const int pending_result = word_until(d, 0x90074, &pending, deadline);
        if (pending_result)
            return pending_result;
        if (pending & 0x400)
            return -1;
        uint8_t b[0x112];
        const size_t n = (pending & 0x200) ? sizeof(b) : 2;
        const int read_result = rd_until(d, 0x822ac, b, n, deadline);
        if (read_result)
            return read_result;
        unsigned s = ((unsigned)b[0] << 8 | b[1]) >> 12;
        if (pending & 0x200) {
            const int ack_result = putword_until(d, 0x90074, 0x200, deadline);
            if (ack_result)
                return ack_result;
            d->card_notified = 1;
            d->card_notified_state = (uint8_t)s;
            ++d->card_notification_generation;
        } else {
            *state = s;
            *fresh_notification = d->card_notification_generation > generation;
            return 0;
        }
    }
    return -1;
}
static void card_snapshot_set(dt295 *d, unsigned state, uint8_t fresh_notification,
                              dt295_card_snapshot *snapshot) {
    *snapshot = (dt295_card_snapshot){
        state,
        d->card_notification_generation,
        d->card_activation_generation,
        d->card_activation_valid,
        fresh_notification,
    };
}
int dt295_card_initialize(dt295 *d, char *e, size_t en) {
    unsigned s;
    if (!d || d->cleanup_only || !d->has_session || drain_state(d) || d->state != 0x11)
        return fail(d, e, en, "card initialization requires authenticated idle state");
    d->card_activation_valid = 0;
    if (command(d, 0x0a, 0, 1, 1, NULL) || card_status(d, &s))
        return fail(d, e, en, "card interface initialization failed");
    seterr(e, en, "");
    return 0;
}
int dt295_card_activate(dt295 *d, unsigned timeout, char *e, size_t en) {
    if (!d || d->cleanup_only || !d->has_session || timeout < 1 || timeout > 5000 || drain_state(d) ||
        d->state != 0x11)
        return fail(d, e, en, "card activation requires authenticated idle state");
    d->card_activation_valid = 0;
    unsigned s;
    if (card_status(d, &s))
        return fail(d, e, en, "card status unavailable");
    if (s == 3) {
        d->card_activation_generation = d->card_notification_generation;
        d->card_activation_valid = 1;
        return 0;
    }
    int initial_zero = (s == 0 && !d->card_notified);
    if (s != 1 && !initial_zero)
        return fail(d, e, en, "card is not ready for activation");
    if (command(d, 0x0a, 1, 1, 1, NULL))
        return fail(d, e, en, "card activation command failed; command was not retried");
    uint64_t end = now_ms(d) + timeout;
    do {
        if (drain_state(d) || d->state != 0x11)
            return fail(d, e, en, "device left idle during card activation");
        if (card_status(d, &s))
            return fail(d, e, en, "card status unavailable");
        if (s == 3) {
            d->card_activation_generation = d->card_notification_generation;
            d->card_activation_valid = 1;
            seterr(e, en, "");
            return 0;
        }
        if (s != 1 && s != 2 && !(s == 0 && !d->card_notified))
            return fail(d, e, en, "card activation failed");
        if (now_ms(d) >= end)
            break;
        sleep_ms(d, 10);
    } while (1);
    return fail(d, e, en, "card activation timed out");
}

static dt295_card_recovery_result card_recovery_fail(dt295 *d, char *e, size_t en,
                                                      const char *message) {
    if (d)
        d->card_recovery_failed = 1;
    return (dt295_card_recovery_result)fail(d, e, en, message);
}

int dt295_card_observe_until(dt295 *d, dt295_card_snapshot *snapshot,
                             uint64_t deadline_ms) {
    if (!d || !snapshot || d->cleanup_only || !d->has_session || d->poisoned)
        return -1;
    unsigned state = 0;
    uint8_t fresh_notification = 0;
    if (card_status_until(d, &state, deadline_ms, &fresh_notification))
        return -1;
    card_snapshot_set(d, state, fresh_notification, snapshot);
    return 0;
}
int dt295_card_observe(dt295 *d, dt295_card_snapshot *snapshot) {
    if (!d)
        return -1;
    return dt295_card_observe_until(d, snapshot, now_ms(d) + 1000U);
}
dt295_card_recovery_result dt295_card_recover(dt295 *d,
                                              const dt295_card_snapshot *observed,
                                              uint64_t deadline_ms,
                                              dt295_card_snapshot *result,
                                              char *e, size_t en) {
    if (!d || !observed || !result || d->cleanup_only || !d->has_session || d->poisoned)
        return DT295_CARD_RECOVERY_FATAL;
    if (d->state != 0x11 || !observed->activation_valid || observed->raw_state != 1U ||
        !observed->fresh_notification ||
        observed->notification_generation <= observed->activation_generation ||
        observed->notification_generation != d->card_notification_generation)
        return DT295_CARD_RECOVERY_NOT_NEEDED;
    if (d->card_recovery_generation_valid &&
        d->card_recovery_generation == observed->notification_generation) {
        *result = *observed;
        return DT295_CARD_RECOVERY_PENDING;
    }
    const int state_result = drain_state_until(d, deadline_ms);
    if (state_result == DT295_DEADLINE) {
        *result = *observed;
        return DT295_CARD_RECOVERY_PENDING;
    }
    if (state_result || d->state != 0x11)
        return card_recovery_fail(d, e, en, "device state unavailable during card recovery");
    int attempted = 0;
    const int command_result = command_until(d, 0x0a, 1, 1, 1, deadline_ms, &attempted);
    if (attempted) {
        d->card_recovery_generation = observed->notification_generation;
        d->card_recovery_generation_valid = 1;
    }
    if (command_result == DT295_DEADLINE) {
        *result = *observed;
        return DT295_CARD_RECOVERY_PENDING;
    }
    if (command_result)
        return card_recovery_fail(d, e, en,
                                  "card recovery command failed or completion is uncertain");
    for (;;) {
        const int state_result = drain_state_until(d, deadline_ms);
        if (state_result == DT295_DEADLINE) {
            *result = *observed;
            return DT295_CARD_RECOVERY_PENDING;
        }
        if (state_result || d->state != 0x11)
            return card_recovery_fail(d, e, en,
                                      "device state unavailable during card recovery");
        unsigned state = 0;
        uint8_t fresh_notification = 0;
        const int status_result = card_status_until(d, &state, deadline_ms,
                                                    &fresh_notification);
        if (status_result == DT295_DEADLINE) {
            *result = *observed;
            return DT295_CARD_RECOVERY_PENDING;
        }
        if (status_result)
            return card_recovery_fail(d, e, en, "card status failed during recovery");
        if (state == 3U) {
            d->card_activation_generation = d->card_notification_generation;
            d->card_activation_valid = 1;
            card_snapshot_set(d, state, fresh_notification, result);
            seterr(e, en, "");
            return DT295_CARD_RECOVERY_ACTIVATED;
        }
        if (state != 1U && state != 2U && state != 0U && state != 4U && state != 5U)
            return card_recovery_fail(d, e, en, "unexpected card state during recovery");
        const uint64_t now = now_ms(d);
        if (now >= deadline_ms) {
            card_snapshot_set(d, state, fresh_notification, result);
            return DT295_CARD_RECOVERY_PENDING;
        }
        uint64_t remaining = deadline_ms - now;
        if (remaining > 10U)
            remaining = 10U;
        sleep_ms(d, (unsigned)remaining);
    }
}
void dt295_card_invalidate_observation(dt295 *d) {
    if (!d)
        return;
    d->card_activation_valid = 0;
    d->card_activation_generation = 0;
}

static int demod_write(dt295 *d, uint8_t r, uint8_t v) {
    uint8_t b[2] = {r, v};
    return i2c(d, 0x20, b, 2, NULL, 0);
}
static int demod_read(dt295 *d, uint8_t r, uint8_t *v) {
    return i2c(d, 0x20, &r, 1, v, 1);
}
static int rf_write(dt295 *d, uint8_t r, uint8_t v) {
    uint8_t b[4] = {0xfe, 0xc0, r, v};
    return i2c(d, 0x20, b, r == 255 ? 3 : 4, NULL, 0);
}
static int rf_read(dt295 *d, uint8_t r, uint8_t *v) {
    uint8_t a[4] = {0xfe, 0xc0, 0xfb, r}, b[2] = {0xfe, 0xc1};
    if (i2c(d, 0x20, a, 4, NULL, 0))
        return -1;
    return i2c(d, 0x20, b, 2, v, 1);
}
static int rf_field(dt295 *d, uint8_t r, uint8_t mask, uint8_t v) {
    uint8_t x;
    if (rf_read(d, r, &x))
        return -1;
    return rf_write(d, r, (uint8_t)((x & mask) | v));
}
static int table_write(dt295 *d, uint32_t va, size_t count, int rf) {
    const uint8_t *p = mat(d, va, count * 8);
    if (!p)
        return -1;
    for (size_t i = 0; i < count; i++) {
        uint32_t r = le32(p + i * 8), v = le32(p + i * 8 + 4);
        if (r > 255 || v > 255)
            return -1;
        if ((rf ? rf_write(d, (uint8_t)r, (uint8_t)v) : demod_write(d, (uint8_t)r, (uint8_t)v)))
            return -1;
    }
    return 0;
}
static int frequency(dt295 *d, uint32_t hz) {
    if (hz < 90000000 || hz > 770000000)
        return -1;
    uint32_t n = (uint32_t)((uint64_t)hz * 64u / 1000000u);
    if (rf_field(d, 0x0d, 0xfe, 0) || rf_write(d, 0x0f, 0x20) || rf_write(d, 0x10, n & 255) ||
        rf_write(d, 0x11, n >> 8) || rf_write(d, 0, 1) ||
        rf_write(d, 0x31, hz < 700000000 ? 0x81 : 0x91) || rf_write(d, 0, 0) ||
        rf_write(d, 0x7c, hz < 700000000 ? 0x1f : 0x9f))
        return -1;
    return 0;
}
int dt295_terrestrial_rf_divider(unsigned channel, uint16_t *divider) {
    if (!divider || channel < 13 || channel > 62)
        return -1;
    uint32_t hz = 473142857u + (channel - 13u) * 6000000u;
    *divider = (uint16_t)((uint64_t)hz * 64u / 1000000u);
    return 0;
}
static int tune_channel(dt295 *d, unsigned ch) {
    uint32_t hz = 473142857u + (ch - 13) * 6000000u;
    if (rf_write(d, 0x0b, 1) || rf_write(d, 0x12, 1))
        return -1;
    sleep_ms(d, 20);
    if (demod_write(d, 3, 0))
        return -1;
    for (uint8_t r = 0x1d; r <= 0x1f; r++)
        if (demod_write(d, r, 0xaa))
            return -1;
    if (demod_write(d, 0x25, 0) || demod_write(d, 0x23, 0x4d) || frequency(d, hz) ||
        rf_write(d, 0x12, 0) || rf_field(d, 0x6e, 0xff, 0x80) || rf_field(d, 0x14, 0xdf, 0) ||
        rf_write(d, 0x12, 1))
        return -1;
    sleep_ms(d, 20);
    if (rf_field(d, 0x6e, 0x7f, 0))
        return -1;
    uint8_t v;
    if (demod_read(d, 0x0e, &v) || demod_write(d, 0x0e, (uint8_t)((v & 0x70) | 1)) ||
        demod_write(d, 0x0f, 0x10) || demod_write(d, 1, 0x40) || demod_write(d, 0x23, 0x4c))
        return -1;
    sleep_ms(d, 100);
    return demod_write(d, 0x1d, 2) || demod_write(d, 0x1e, 0x2a) || demod_write(d, 0x1f, 0x8a) ? -1
                                                                                               : 0;
}
static int tuner_initialize(dt295 *d) {
    if (field(d, 0x90014, 1, 1, 1) || field(d, 0x90018, 1, 1, 0))
        return -1;
    sleep_ms(d, 20);
    if (field(d, 0x90018, 1, 1, 1))
        return -1;
    sleep_ms(d, 5);
    if (table_write(d, 0x841d0, 22, 0) || rf_write(d, 255, 0) || table_write(d, 0x84630, 26, 1) ||
        rf_write(d, 0, 1) || rf_field(d, 0x31, 0x2f, 0xd0) || rf_write(d, 0, 0) ||
        rf_write(d, 1, 0x89) || rf_write(d, 2, 0) || rf_write(d, 0x6d, 10) ||
        rf_write(d, 0x0b, 1) || rf_field(d, 4, 0xc0, 1) || rf_write(d, 5, 0x1b) ||
        rf_field(d, 8, 0xf2, 5) || rf_field(d, 9, 0x80, 0x42) || rf_field(d, 0x5e, 0xef, 0) ||
        rf_write(d, 10, 0) || table_write(d, 0x84700, 28, 1) || rf_write(d, 0x6d, 10) ||
        rf_write(d, 3, 0) || rf_write(d, 3, 1))
        return -1;
    sleep_ms(d, 100);
    if (frequency(d, 473142857) || rf_write(d, 0x0b, 1) || rf_write(d, 0x12, 1))
        return -1;
    sleep_ms(d, 20);
    if (rf_write(d, 0x12, 0) || rf_write(d, 0x0b, 0) || demod_write(d, 0x25, 0) ||
        demod_write(d, 0x23, 0x4d) || demod_write(d, 3, 0x90))
        return -1;
    return 0;
}
int dt295_tune_terrestrial(dt295 *d, unsigned channel, char *e, size_t en) {
    if (!d || d->cleanup_only || !d->has_session || channel < 13 || channel > 62)
        return fail(d, e, en, "only terrestrial channels T13..T62 are supported");
    if (d->card_recovery_failed)
        return fail(d, e, en, "terrestrial tune refused after DT295 card recovery failure");
    if (drain_state(d) || d->state != 0x11)
        return fail(d, e, en, "terrestrial tuning requires authenticated idle state");
    if (tuner_initialize(d))
        return fail(d, e, en, "terrestrial tuner initialization failed");
    if (tune_channel(d, channel))
        return fail(d, e, en, "terrestrial channel tune failed; no command was retried");
    d->tuned = 1;
    seterr(e, en, "");
    return 0;
}

static int sat_wr(dt295 *d, const uint8_t *p, size_t n) {
    return i2c(d, 0x22, p, n, NULL, 0);
}
static int sat_reg_write(dt295 *d, uint8_t r, uint8_t v) {
    uint8_t b[2] = {r, v};
    return sat_wr(d, b, 2);
}
static int sat_reg_read(dt295 *d, uint8_t r, uint8_t *p, size_t n) {
    return i2c(d, 0x22, &r, 1, p, n);
}
static int sat_rf_reg(dt295 *d, uint8_t r, uint8_t v) {
    uint8_t b[4] = {0xfe, 0xc0, r, v};
    return sat_wr(d, b, sizeof(b));
}
static int sat_rf_block(dt295 *d, uint8_t r, const uint8_t *p, size_t n) {
    uint8_t b[16] = {0xfe, 0xc0, r};
    if (n > sizeof(b) - 3)
        return -1;
    memcpy(b + 3, p, n);
    return sat_wr(d, b, 3 + n);
}
static int sat_rf_read(dt295 *d, uint8_t *p, size_t n) {
    uint8_t b[2] = {0xfe, 0xc1};
    return i2c(d, 0x22, b, sizeof(b), p, n);
}
static int sat_mute(dt295 *d, int enabled) {
    return sat_reg_write(d, 0x1c, enabled ? 0xaa : 8) || sat_reg_write(d, 0x1f, enabled ? 0x2a : 8)
               ? -1
               : 0;
}
static int sat_rf_sleep(dt295 *d) {
    if (sat_rf_reg(d, 0x2a, 0xb9) || sat_rf_reg(d, 0x29, 0x70) ||
        sat_rf_reg(d, 0x28, 0x3e) || sat_rf_reg(d, 0x2a, 0x19) ||
        sat_rf_reg(d, 0x1c, 0))
        return -1;
    const uint8_t b[2] = {0xc0, 0xa7};
    return sat_rf_block(d, 0x11, b, sizeof(b));
}
static int sat_rf_wake(dt295 *d) {
    const uint8_t b[2] = {0x00, 0xa7};
    if (sat_rf_block(d, 0x11, b, sizeof(b)))
        return -1;
    return sat_rf_reg(d, 0x2a, 0xb9) || sat_rf_reg(d, 0x1c, 0xc0) ||
                   sat_rf_reg(d, 0x29, 0x71)
               ? -1
               : 0;
}
static int sat_wake(dt295 *d) {
    uint8_t v;
    if (sat_reg_read(d, 0x1e, &v, 1) || sat_reg_write(d, 0x1e, (uint8_t)((v & 0xcf) | 0x20)) ||
        sat_rf_wake(d))
        return -1;
    return sat_reg_write(d, 0x17, 0) || sat_reg_write(d, 0x1e, 0x20) ||
                   sat_reg_write(d, 0x0a, 0xff) || sat_reg_write(d, 3, 1)
               ? -1
               : 0;
}
static int sat_sleep_sequence(dt295 *d) {
    uint8_t v;
    if (sat_reg_write(d, 0x1e, 0x28) || sat_reg_write(d, 0x0a, 0) || sat_reg_write(d, 3, 1) ||
        sat_reg_write(d, 0x17, 1) || sat_reg_read(d, 0x1e, &v, 1) ||
        sat_reg_write(d, 0x1e, (uint8_t)((v & 0xcf) | 0x30)))
        return -1;
    return sat_rf_sleep(d);
}
static int sat_initialize(dt295 *d) {
    const uint8_t *p = mat(d, 0x845a0, 18 * 8);
    if (!p)
        return -1;
    unsigned nonzero = 0;
    for (size_t i = 0; i < 18 * 8; i++)
        nonzero |= p[i];
    if (!nonzero)
        return -1;
    if (sat_reg_write(d, 1, 0x90))
        return -1;
    for (size_t i = 0; i < 18; i++) {
        uint32_t r = le32(p + i * 8), v = le32(p + i * 8 + 4);
        if (r > 255 || v > 255 || sat_reg_write(d, (uint8_t)r, (uint8_t)v))
            return -1;
    }
    sleep_ms(d, 4);
    if (sat_rf_reg(d, 0x2a, 0xb9))
        return -1;
    const uint8_t mode[3] = {0x10, 0x10, 0};
    if (sat_rf_block(d, 6, mode, sizeof(mode)) || sat_rf_reg(d, 0x0a, 0) ||
        sat_rf_reg(d, 0x0e, 0) || sat_rf_sleep(d))
        return -1;
    sleep_ms(d, 6);
    return 0;
}
static int sat_terrestrial_sleep(dt295 *d) {
    return demod_write(d, 0x25, 0) || demod_write(d, 0x23, 0x4d) || demod_write(d, 3, 0x90) ||
                   rf_write(d, 0x12, 0) || rf_write(d, 0x0b, 0)
               ? -1
               : 0;
}
static int sat_frequency(dt295 *d, uint32_t hz) {
    if (hz < 950000000u || hz > 2150000000u)
        return -1;
    uint64_t f = ((uint64_t)hz / 1000u + 500u) / 1000u * 1000u;
    unsigned div = f <= 1100000u ? 4u : 2u;
    uint32_t n = (uint32_t)(((uint64_t)div * f / 2u + 500u) / 1000u);
    static const struct {
        uint32_t limit;
        uint8_t band;
        uint8_t mode;
    } bands[] = {{975000u, 0x1c, 1}, {1050000u, 0x18, 2}, {1150000u, 0x14, 2},
                 {1250000u, 0x10, 3}, {1350000u, 0x0c, 4}, {1450000u, 0x0a, 4},
                 {1600000u, 7, 5},    {1800000u, 4, 2},    {2000000u, 2, 1},
                 {2151000u, 0, 0}};
    uint8_t band = 0, mode = 0;
    for (size_t i = 0; i < sizeof(bands) / sizeof(bands[0]); i++) {
        if (f < bands[i].limit) {
            band = bands[i].band;
            mode = bands[i].mode;
            break;
        }
    }
    const uint8_t params[5] = {(uint8_t)(n >> 7), (uint8_t)(n * 2u), 0, 0,
                               f <= 1100000u ? 0x80 : 0};
    if (sat_rf_block(d, 0, params, sizeof(params)) ||
        sat_rf_reg(d, 9, (uint8_t)(mode << 5 | band)))
        return -1;
    if (sat_rf_reg(d, 0x37, 0xac) || sat_rf_reg(d, 5, 0x80) ||
        sat_rf_reg(d, 0x2a, 0xbb))
        return -1;
    sleep_ms(d, 10);
    return 0;
}
static int sat_mode(dt295 *d) {
    uint8_t v;
    if (sat_reg_read(d, 0xa5, &v, 1) || sat_reg_write(d, 0xa5, (uint8_t)(v | 0x40)) ||
        sat_reg_read(d, 0xa3, &v, 1))
        return -1;
    return sat_reg_write(d, 0xa3, (uint8_t)((v & 0x88) | 0x11));
}
static int sat_select_tsid(dt295 *d, uint16_t tsid) {
    return sat_reg_write(d, 0x8f, (uint8_t)(tsid >> 8)) ||
                   sat_reg_write(d, 0x90, (uint8_t)tsid)
               ? -1
               : 0;
}
static int sat_tune(dt295 *d, uint32_t hz, int tsid) {
    if (sat_wake(d) || sat_mute(d, 1))
        return -1;
    if (tsid >= 0 && sat_select_tsid(d, 0))
        return -1;
    if (sat_reg_write(d, 0x1e, 0x28) || sat_reg_write(d, 0x0a, 0) || sat_reg_write(d, 3, 1) ||
        sat_frequency(d, hz))
        return -1;
    uint8_t clock[2] = {0x0e, 0x77};
    if (i2c(d, 0x20, clock, sizeof(clock), NULL, 0))
        return -1;
    clock[0] = 0x0f;
    if (i2c(d, 0x20, clock, sizeof(clock), NULL, 0))
        return -1;
    uint8_t v;
    if (sat_reg_read(d, 7, &v, 1) || sat_reg_write(d, 7, (uint8_t)((v & 0x70) | 1)) ||
        sat_reg_write(d, 8, 0x10))
        return -1;
    uint8_t rf[2];
    if (sat_rf_read(d, rf, sizeof(rf)))
        return -1;
    if (sat_rf_reg(d, 0x50, (uint8_t)((rf[1] & 0x60) == 0x60 ? 0xf1 : 0xf0)))
        return -1;
    sleep_ms(d, 40);
    if (sat_reg_write(d, 0x1e, 0x20) || sat_reg_write(d, 0x0a, 0xff) ||
        sat_reg_write(d, 3, 1) || sat_reg_write(d, 3, 1))
        return -1;
    sleep_ms(d, 100);
    if (sat_mute(d, 0) || sat_mode(d))
        return -1;
    return tsid >= 0 ? sat_select_tsid(d, (uint16_t)tsid) : 0;
}
int dt295_tune_satellite(dt295 *d, uint32_t if_hz, int tsid, char *e, size_t en) {
    if (!d || d->cleanup_only || !d->has_session || if_hz < 950000000u ||
        if_hz > 2150000000u || tsid < -1 || tsid > 65535)
        return fail(d, e, en, "satellite tuning requires IF 950..2150 MHz and TSID 0..65535");
    if (d->card_recovery_failed)
        return fail(d, e, en, "satellite tune refused after DT295 card recovery failure");
    if (drain_state(d) || d->state != 0x11)
        return fail(d, e, en, "satellite tuning requires authenticated idle state");
    const uint8_t *table = mat(d, 0x845a0, 18 * 8);
    if (!table)
        return fail(d, e, en, "satellite register table is missing from the asset bundle");
    if (tuner_initialize(d) || sat_initialize(d) || sat_terrestrial_sleep(d) ||
        sat_tune(d, if_hz, tsid))
        return fail(d, e, en, "satellite tune failed; no command was retried");
    if (tsid >= 0) {
        dt295_satellite_state state = {0};
        const uint64_t end = now_ms(d) + 3000;
        for (;;) {
            if (dt295_satellite_status(d, &state, e, en))
                return -1;
            if (state.locked && state.selected_tsid == (uint16_t)tsid)
                break;
            if (now_ms(d) >= end)
                return fail(d, e, en, "satellite did not lock the target TSID");
            sleep_ms(d, 100);
        }
    }
    d->tuned = 1;
    seterr(e, en, "");
    return 0;
}
int dt295_sleep_satellite(dt295 *d, char *e, size_t en) {
    if (!d || d->cleanup_only || !d->has_session)
        return fail(d, e, en, "satellite sleep requires authenticated session");
    if (sat_sleep_sequence(d))
        return fail(d, e, en, "satellite sleep failed; no command was retried");
    d->tuned = 0;
    seterr(e, en, "");
    return 0;
}
int dt295_select_satellite_tsid(dt295 *d, uint16_t tsid, char *e, size_t en) {
    if (!d || d->cleanup_only || !d->has_session)
        return fail(d, e, en, "satellite TSID selection requires authenticated session");
    if (sat_select_tsid(d, tsid))
        return fail(d, e, en, "satellite TSID selection failed; command was not retried");
    seterr(e, en, "");
    return 0;
}
int dt295_satellite_wait_lock(dt295 *d, unsigned timeout_ms, char *e, size_t en) {
    if (!d || d->cleanup_only || !d->has_session || !d->tuned)
        return fail(d, e, en, "satellite lock wait requires tuned authenticated session");
    if (timeout_ms == 0 || timeout_ms > 5000)
        return fail(d, e, en, "satellite lock timeout must be 1..5000 ms");
    const uint64_t end = now_ms(d) + timeout_ms;
    for (;;) {
        dt295_satellite_state state = {0};
        if (dt295_satellite_status(d, &state, e, en))
            return -1;
        if (state.locked) {
            seterr(e, en, "");
            return 0;
        }
        if (now_ms(d) >= end)
            return fail(d, e, en, "satellite did not lock before the timeout");
        sleep_ms(d, 100);
    }
}
int dt295_satellite_tsid_table(dt295 *d, uint16_t tsids[8], char *e, size_t en) {
    if (!d || !tsids || d->cleanup_only || !d->has_session || !d->tuned)
        return fail(d, e, en, "satellite TSID table requires tuned authenticated session");
    uint8_t b[16];
    if (sat_reg_read(d, 0xce, b, sizeof(b)))
        return fail(d, e, en, "satellite TSID table read failed");
    for (unsigned i = 0; i < 8; i++)
        tsids[i] = (uint16_t)(b[i * 2] << 8 | b[i * 2 + 1]);
    seterr(e, en, "");
    return 0;
}
int dt295_select_satellite_slot(dt295 *d, unsigned slot, char *e, size_t en) {
    uint16_t tsids[8];
    if (dt295_satellite_wait_lock(d, 3000, e, en))
        return -1;
    if (dt295_satellite_tsid_table(d, tsids, e, en))
        return -1;
    if (slot > 7)
        return fail(d, e, en, "satellite slot must be 0..7");
    const uint16_t tsid = tsids[slot];
    if (tsid == 0 || tsid == 0xffff)
        return fail(d, e, en, "satellite slot has no usable TSID");
    if (dt295_select_satellite_tsid(d, tsid, e, en))
        return -1;
    const uint64_t end = now_ms(d) + 3000;
    for (;;) {
        dt295_satellite_state state = {0};
        if (dt295_satellite_status(d, &state, e, en))
            return -1;
        if (state.locked && state.selected_tsid == tsid)
            break;
        if (now_ms(d) >= end)
            return fail(d, e, en, "satellite slot TSID did not lock");
        sleep_ms(d, 100);
    }
    seterr(e, en, "");
    return 0;
}
int dt295_satellite_status(dt295 *d, dt295_satellite_state *status, char *e, size_t en) {
    if (!d || !status || d->cleanup_only || !d->has_session)
        return fail(d, e, en, "satellite status requires authenticated session");
    uint8_t c3 = 0, ba = 0, tsid[2] = {0, 0}, cn[2] = {0, 0};
    if (sat_reg_read(d, 0xc3, &c3, 1) || sat_reg_read(d, 0xe6, tsid, sizeof(tsid)) ||
        sat_reg_read(d, 0xba, &ba, 1) || sat_reg_read(d, 0xbc, cn, sizeof(cn)))
        return fail(d, e, en, "satellite status read failed");
    status->locked = (c3 & 0x10) ? 0 : 1;
    status->selected_tsid = (uint16_t)(tsid[0] << 8 | tsid[1]);
    status->agc_raw = (uint8_t)(ba & 0x7f);
    status->cn_raw = (uint16_t)(cn[0] << 8 | cn[1]);
    seterr(e, en, "");
    return 0;
}

static int set_route_fields(dt295 *d, const dt295_route_op *op) {
    unsigned s = op->slot;
    if (s > 63 || op->enabled > 1 || op->pid > 8191 || op->ecm_slot > 63)
        return -1;
    if (s == 4)
        return op->pid == 0 ? 0 : -1; /* PAT is implicit. */
    uint32_t pa;
    unsigned enable_shift = 15;
    if (s < 4)
        pa = 0x101e + 2 * s;
    else if (s < 39)
        pa = 0x1028 + 2 * (s - 5);
    else if (s == 39)
        pa = 0x106c;
    else if (s == 40)
        pa = 0x106e;
    else
        pa = 0x1070 + 2 * (s - 41);
    if (field(d, pa, 0, 13, op->enabled ? op->pid : 0) ||
        field(d, pa, enable_shift, 1, op->enabled))
        return -1;
    if (s < 2) {
        if (field(d, 0x10a2, s ? 0 : 8, 7, op->enabled ? op->ecm_slot : 0))
            return -1;
    } else if (s == 5) {
        if (field(d, 0x10a6, 0, 7, op->enabled ? op->ecm_slot : 0))
            return -1;
    } else if (s >= 6) {
        uint32_t ea = 0x10a8 + 2 * ((s - 6) / 2);
        unsigned sh = (s & 1u) ? 0 : 8;
        if (field(d, ea, sh, 7, op->enabled ? op->ecm_slot : 0))
            return -1;
    }
    return 0;
}
int dt295_route_write(dt295 *d, const dt295_route_op *op, char *e, size_t en) {
    if (!d || d->cleanup_only || !op || !d->has_session || !d->started)
        return fail(d, e, en, "live route write requires active authenticated reception");
    if (op->slot > 63 || op->pid > 8191 || op->ecm_slot > 63 || op->enabled > 1)
        return fail(d, e, en, "route operation outside supported range");
    if (op->slot == 4)
        return fail(d, e, en, "PAT slot 4 is implicit");
    uint16_t words[3] = {(uint16_t)(op->slot << 8 | op->ecm_slot),
                         (uint16_t)((op->enabled << 15) | op->pid), op->pid};
    uint8_t bytes[6];
    for (unsigned i = 0; i < 3; i++) {
        bytes[2 * i] = words[i] >> 8;
        bytes[2 * i + 1] = words[i];
    }
    if (wr(d, 0x81000, bytes, sizeof(bytes)) || command(d, 5, 1, 0, 0, NULL))
        return fail(d, e, en, "route command failed; state-changing request was not retried");
    seterr(e, en, "");
    return 0;
}

static int prepare_config(dt295 *d, const dt295_route_op *routes, size_t count, uint16_t program,
                          uint8_t video_type, uint8_t audio_type, int rearm, char *e, size_t en) {
    if (!d || d->poisoned || d->cleanup_only || !d->has_session || !d->tuned || d->prepared || d->started ||
        (!routes && count) || count > 64 || ((rearm != 0) != !!d->has_ts_key))
        return fail(d, e, en, "receiver preparation preconditions failed");
    uint16_t s;
    if (drain_state(d) || d->state != 0x11)
        return fail(d, e, en, "receiver preparation requires authenticated idle state");
    if (word(d, 0x82008, &s) || s != 0x11)
        return fail(d, e, en, "receiver preparation requires state 0x11");
    /* Get the transport key before configuration, matching the driver sequence. */
    if (!rearm) {
        size_t kn = 0;
        if (secure_command(d, 0x07010600, d->assets->key_request, 12, d->ts_key, sizeof(d->ts_key),
                           &kn) ||
            kn != 16)
            return fail(d, e, en, "transport key request failed");
        d->has_ts_key = 1;
    }
    if (command(d, 4, 0, 0, 0, NULL))
        return fail(d, e, en, "TRC upload preparation failed; command was not retried");
    for (size_t off = 0; off < d->assets->trc_n; off += 65536) {
        size_t n = d->assets->trc_n - off;
        if (n > 65536)
            n = 65536;
        if (bulk_out(d, 0x02, d->assets->trc + off, n, 300))
            return fail(d, e, en, "TRC upload failed; device state is uncertain");
    }
    if (word(d, 0x82008, &s) || s != 0x11)
        return fail(d, e, en, "TRC upload did not retain idle state");
    const uint32_t clear[][2] = {{0x1000, 0x500}, {0x1500, 0x500}, {0x1a00, 0x100}};
    uint8_t zero[0x500] = {0};
    for (unsigned i = 0; i < 3; i++)
        if (wr(d, clear[i][0], zero, clear[i][1]))
            return fail(d, e, en, "receive configuration clear failed");
    for (size_t i = 0; i < d->assets->field_count; i++) {
        dt295_field *f = &d->assets->fields[i];
        if (field(d, f->address, f->shift, f->width, f->value))
            return fail(d, e, en, "private receive profile apply failed");
    }
    /* Replace every route register so stale private channel PIDs cannot survive. */
    dt295_route_op blank = {0};
    for (unsigned slot = 0; slot < 64; slot++) {
        blank.slot = (uint8_t)slot;
        blank.enabled = 0;
        blank.pid = 0;
        blank.ecm_slot = 0;
        if (set_route_fields(d, &blank))
            return fail(d, e, en, "route table clear failed");
    }
    for (size_t i = 0; i < count; i++) {
        if (set_route_fields(d, &routes[i]))
            return fail(d, e, en, "initial route apply failed");
    }
    if (putword(d, 0x1018, program) || field(d, 0x101c, 8, 8, video_type) ||
        field(d, 0x101c, 0, 8, audio_type))
        return fail(d, e, en, "program metadata apply failed");
    if (putword(d, 0x82028, 0) || command(d, 4, 0x20, 0, 0, NULL) || wait_state(d, 1, 30000))
        return fail(d, e, en, "receive acquisition did not reach configured state");
    uint16_t dma = 0;
    for (size_t i = 0; i < d->assets->field_count; i++) {
        const dt295_field *f = &d->assets->fields[i];
        if (f->address == 0x1002) {
            uint16_t mask = (uint16_t)(((1u << f->width) - 1) << f->shift);
            dma = (uint16_t)((dma & ~mask) | (f->value << f->shift));
        }
    }
    unsigned input = (dma >> 8) & 3;
    if (input > 1)
        return fail(d, e, en, "unsupported DMA input selection");
    d->pause_state = input ? 3 : 4;
    d->prepared = 1;
    seterr(e, en, "");
    return 0;
}

int dt295_prepare(dt295 *d, const dt295_route_op *routes, size_t count, char *e, size_t en) {
    return prepare_config(d, routes, count, 0, 0, 0, 0, e, en);
}

static int plan_to_ops(const dt295_route_plan *plan, dt295_route_op ops[64], size_t *count) {
    if (!plan || !ops || !count || plan->route_count > 64)
        return -1;
    *count = 0;
    unsigned enabled = 0;
    uint8_t seen[8192] = {0};
    for (unsigned s = 0; s < 64; s++) {
        const dt295_route *r = &plan->routes[s];
        if (!r->enabled)
            continue;
        enabled++;
        if (r->slot != s || r->pid > 8191 || r->ecm_slot > 63 || r->enabled > 1 || r->is_ecm > 1 ||
            seen[r->pid])
            return -1;
        seen[r->pid] = 1;
        if (s != 4 && (r->pid == 0 || r->pid == 8191))
            return -1;
        if (s == 4) {
            if (r->pid != 0 || r->is_ecm)
                return -1;
            continue;
        }
        if (r->is_ecm) {
            if (s < 5 || r->ecm_slot != s || r->pid == 0)
                return -1;
        } else if (r->ecm_slot) {
            const dt295_route *ecm = &plan->routes[r->ecm_slot];
            if (r->ecm_slot < 5 || !ecm->enabled || !ecm->is_ecm || ecm->ecm_slot != r->ecm_slot)
                return -1;
        }
        ops[*count] = (dt295_route_op){(uint8_t)s, 1, r->pid, r->ecm_slot};
        (*count)++;
    }
    if (enabled != plan->route_count)
        return -1;
    return 0;
}
int dt295_prepare_plan(dt295 *d, const dt295_route_plan *plan, char *e, size_t en) {
    dt295_route_op ops[64];
    size_t n;
    if (!plan_to_ops(plan, ops, &n))
        return prepare_config(d, ops, n, plan->primary_program, plan->video_stream_type,
                              plan->audio_stream_type, 0, e, en);
    return fail(d, e, en, "invalid validated DT295 route plan");
}
int dt295_reprepare_plan(dt295 *d, const dt295_route_plan *plan, char *e, size_t en) {
    dt295_route_op ops[64];
    size_t n;
    if (!d || !d->has_ts_key || d->started || d->prepared || plan_to_ops(plan, ops, &n) != 0)
        return fail(
            d, e, en,
            "reprepare requires stopped receiver, retained transport key, and valid route plan");
    return prepare_config(d, ops, n, plan->primary_program, plan->video_stream_type,
                          plan->audio_stream_type, 1, e, en);
}

int dt295_start(dt295 *d, char *e, size_t en) {
    if (!d || d->cleanup_only || !d->prepared || d->started || !d->has_ts_key || drain_state(d) || d->state != 1)
        return fail(d, e, en, "receive start requires prepared state 1");
    d->prepared = 0;
    if (command(d, 5, 0, 2, 0, NULL) || wait_state(d, 2, 30000))
        return fail(d, e, en, "receive start failed; start command was not retried");
    d->started = 1;
    seterr(e, en, "");
    return 0;
}
static int renew(dt295 *d) {
    uint8_t challenge[512] = {0}, reply[20] = {0}, input[64] = {0}, key[16] = {0};
    size_t n = 0;
    uint32_t c = d->renewal_counter;
    int rc = -1;
    if (c == 0 || !d->has_session || d->cleanup_only)
        goto cleanup;
    if (secure_command(d, 0x02010200, NULL, 0, challenge, sizeof(challenge), &n) || n < 32)
        goto cleanup;
    const uint8_t *a = mat(d, 0x851c8, 16), *b = mat(d, 0x85208, 16);
    if (!a || !b)
        goto cleanup;
    for (unsigned i = 0; i < 16; i++)
        key[i] = a[i] ^ b[i];
    memcpy(input, challenge, 32);
    memcpy(input + 32, mat(d, 0x85178, 28), 28);
    input[60] = c >> 24;
    input[61] = c >> 16;
    input[62] = c >> 8;
    input[63] = c;
    dt295_cmac(key, input, sizeof(input), reply);
    if (random_bytes(reply + 16, 4) || secure_command(d, 0x02020400, reply, 20, NULL, 0, NULL))
        goto cleanup;
    d->renewal_counter = c == 0xffffffff ? 1 : c + 1;
    d->renewal_due = now_ms(d) + 30000;
    rc = 0;
cleanup:
    secure_clear(challenge, sizeof(challenge));
    secure_clear(reply, sizeof(reply));
    secure_clear(input, sizeof(input));
    secure_clear(key, sizeof(key));
    return rc;
}
int dt295_renew_if_due(dt295 *d, int *renewed, char *e, size_t en) {
    if (renewed)
        *renewed = 0;
    if (!d || !d->has_session || !d->renewal_counter || d->cleanup_only)
        return fail(d, e, en, "no managed DT295 session");
    if (now_ms(d) < d->renewal_due) {
        seterr(e, en, "");
        return 0;
    }
    if (renew(d))
        return fail(d, e, en, "DT295 session renewal failed; request was not retried");
    if (renewed)
        *renewed = 1;
    seterr(e, en, "");
    return 0;
}

int dt295_stop(dt295 *d, size_t *drained, char *e, size_t en) {
    if (drained)
        *drained = 0;
    if (!d)
        return fail(NULL, e, en, "invalid DT295 context");
    size_t total = 0;
    uint16_t s;
    if (drain_state(d) || word(d, 0x82008, &s))
        return fail(d, e, en, "cannot observe receiver stop state");
    if (s == 2) {
        if (command(d, 5, 0, d->pause_state ? d->pause_state : 4, 0, NULL))
            return fail(d, e, en, "pause command failed; command was not retried");
        uint64_t end = now_ms(d) + 5000;
        for (;;) {
            if (drain_state(d))
                return fail(d, e, en, "pause state notification failed");
            if (d->state == (d->pause_state ? d->pause_state : 4))
                break;
            if (d->state != 2 || now_ms(d) >= end)
                return fail(d, e, en, "receiver pause did not complete");
            uint8_t b[65536];
            size_t n = 0;
            if (bulk_in(d, 0x81, b, sizeof(b), 10, &n))
                return fail(d, e, en, "receive drain failed");
            total += n;
        }
    } else if (s != 1 && s != 3 && s != 4 && s != 0x11)
        return fail(d, e, en, "refusing to stop from unrecognized firmware state");
    if (s == 3 || s == 4 || d->state == 3 || d->state == 4) {
        if (command(d, 5, 0, 1, 0, NULL) || wait_state(d, 1, 5000))
            return fail(d, e, en, "receiver return to state 1 failed");
    }
    if (d->state == 1) {
        if (field(d, 0x80074, 8, 2, 1) || field(d, 0x80074, 0, 7, 0))
            return fail(d, e, en, "idle configuration failed");
        for (unsigned sh = 12; sh >= 4; sh -= 4)
            if (field(d, 0x80076, sh, 4, 1))
                return fail(d, e, en, "idle configuration failed");
        if (field(d, 0x80078, 8, 8, 0) || field(d, 0x80078, 0, 8, 0) ||
            command(d, 1, 0, 0, 0, NULL) || wait_state(d, 0x11, 5000))
            return fail(d, e, en, "idle firmware configuration failed");
    }
    d->started = d->prepared = 0;
    if (drained)
        *drained = total;
    seterr(e, en, "");
    return 0;
}
int dt295_shutdown(dt295 *d, char *e, size_t en) {
    if (!d || !d->has_session)
        return fail(d, e, en, "idle-image shutdown requires authenticated session");
    if (d->started || d->prepared)
        return fail(d, e, en, "stop the receiver before idle-image shutdown");
    if (drain_state(d) || d->state != 0x11)
        return fail(d, e, en, "idle-image shutdown requires healthy state 0x11");
    if (d->assets->idle_n == 0 || d->assets->idle_n > DT295_ASSET_MAX_IMAGE)
        return fail(d, e, en, "idle image size invalid");
    if (command(d, 4, 0, 1, 0, NULL))
        return fail(d, e, en, "idle-image upload preparation failed");
    for (size_t off = 0; off < d->assets->idle_n; off += 65536) {
        size_t n = d->assets->idle_n - off;
        if (n > 65536)
            n = 65536;
        if (bulk_out(d, 0x02, d->assets->idle + off, n, 300))
            return fail(d, e, en, "idle-image upload failed; device state is uncertain");
    }
    if (command(d, 4, 1, 0, 0, NULL) || wait_state(d, 16, 5000))
        return fail(d, e, en, "unauthenticated idle state 16 was not reached");
    secure_clear(d->session_key, 16);
    secure_clear(d->session_iv, 16);
    secure_clear(d->ts_key, 16);
    d->has_session = d->has_ts_key = 0;
    d->renewal_counter = 0;
    d->renewal_due = 0;
    d->cleanup_only = 0;
    seterr(e, en, "");
    return 0;
}
int dt295_decode_packet(dt295 *d, uint8_t packet[188]) {
    if (!d || !packet || !d->has_ts_key)
        return -1;
    return dt295_packet_crypto(d->ts_key, d->assets->iv, packet);
}
