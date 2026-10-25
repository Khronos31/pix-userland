/* Deterministic protocol transcript with synthetic assets and a memory USB. */
#include "crypto.h"
#include "dt295.h"
#include "dt295_crypto.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEMSIZE 0x1000000u
typedef struct {
    uint8_t *mem;
    uint64_t now;
    uint8_t material[0x1280];
    uint8_t iv[16];
    uint8_t ts_iv[16];
    uint8_t session[16];
    unsigned key_requests, i2c_commands, key_words, acquire_commands;
    unsigned card_activation_commands;
    unsigned auth_requests, init_commands, shutdown_commands, start_commands;
    unsigned renewal_challenges, renewal_applies;
    unsigned status_notifications, status_acks, status_reads, control_delay_ms;
    uint32_t fail_secure_id;
    int pause_pending, fail_state_read_once, inject_post_auth_fault;
    int hold_card_activation_state, fail_card_activation_command;
} fake;
int random_bytes(void *p, size_t n) {
    memset(p, 0, n);
    return 0;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] << 8 | p[1];
}
static void wr16(uint8_t *p, uint16_t x) {
    p[0] = x >> 8;
    p[1] = x;
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void wr32(uint8_t *p, uint32_t x) {
    p[0] = x >> 24;
    p[1] = x >> 16;
    p[2] = x >> 8;
    p[3] = x;
}
static void cmac_key(fake *f, uint32_t a, uint32_t b, const uint8_t *data, size_t n,
                     uint8_t out[16]) {
    uint8_t key[16];
    memcpy(key, f->material + (a - 0x841d0), 16);
    for (unsigned i = 0; i < 16; i++)
        key[i] ^= f->material[(b - 0x841d0) + i];
    dt295_cmac(key, data, n, out);
    secure_clear(key, 16);
}

static void respond(fake *f, uint32_t id) {
    uint8_t request[512];
    memcpy(request, f->mem + 0x83000, sizeof(request));
    uint8_t *req = request, *resp = f->mem + 0x83000;
    uint32_t got = rd32(req);
    assert(got == id);
    memset(resp, 0, 512);
    wr32(resp, id);
    wr32(resp + 4, 0);
    if (id == 0x01010000) {
        uint8_t initial[32] = {0};
        uint32_t v[8] = {0x7e0, 6, 14, 0x578, 1, 3, 2, 12};
        for (unsigned i = 0; i < 8; i++) {
            initial[4 * i] = v[i];
            initial[4 * i + 1] = v[i] >> 8;
            initial[4 * i + 2] = v[i] >> 16;
            initial[4 * i + 3] = v[i] >> 24;
        }
        assert(memcmp(req + 16, initial, 32) == 0);
        wr32(resp + 8, 9);
        for (unsigned i = 0; i < 36; i++)
            resp[16 + i] = (uint8_t)(0x31 + i * 7);
    } else if (id == 0x01020000) {
        uint8_t challenge[36];
        for (unsigned i = 0; i < 36; i++)
            challenge[i] = (uint8_t)(0x31 + i * 7);
        uint8_t select_data[32], mac[16], row[32], input[64], reply[20], session_input[64],
            session_mac[16];
        memcpy(select_data, f->material + (0x85148 - 0x841d0), 30);
        select_data[30] = challenge[32];
        select_data[31] = 0;
        cmac_key(f, 0x85198, 0x851d8, select_data, 32, mac);
        unsigned selector = mac[15] & 15;
        memcpy(row, f->material + (0x85230 - 0x841d0) + 32 * selector, 32);
        memcpy(input, challenge, 32);
        memcpy(input + 32, row, 32);
        cmac_key(f, 0x851a8, 0x851e8, input, 64, reply);
        memset(reply + 16, 0, 4);
        if (memcmp(req + 16, reply, 20) != 0) {
            fprintf(stderr, "auth reply mismatch selector=%u id=%08x words=%u\n", selector,
                    rd32(req), rd32(req + 8));
            for (unsigned i = 0; i < 20; i++)
                fprintf(stderr, "%02x", req[16 + i]);
            fprintf(stderr, "\n");
            for (unsigned i = 0; i < 20; i++)
                fprintf(stderr, "%02x", reply[i]);
            fprintf(stderr, "\n");
            abort();
        }
        memcpy(session_input, reply, 20);
        memcpy(session_input + 20, row, 32);
        memcpy(session_input + 52, f->material + (0x85168 - 0x841d0), 12);
        cmac_key(f, 0x851b8, 0x851f8, session_input, 64, session_mac);
        memcpy(f->session, session_mac + 12, 4);
        memcpy(f->session + 4, session_mac + 8, 4);
        memcpy(f->session + 8, session_mac, 8);
        memcpy(f->iv, f->material + (0x85218 - 0x841d0), 16);
        wr32(resp + 8, 0);
        f->auth_requests++;
    } else if (id == 0x07010600) {
        f->key_requests++;
        unsigned words = f->key_words ? f->key_words : 4;
        wr32(resp + 8, words);
        for (unsigned i = 0; i < words * 4; i++)
            resp[16 + i] = (uint8_t)(0x70 + i);
    } else if (id == 0x02010200) {
        f->renewal_challenges++;
        wr32(resp + 8, 8);
        for (unsigned i = 0; i < 32; i++)
            resp[16 + i] = (uint8_t)(0xa0 + i);
    } else if (id == 0x02020400) {
        f->renewal_applies++;
        uint8_t plain[496];
        memcpy(plain, req + 12, sizeof(plain));
        assert(dt295_payload(f->session, f->iv, plain, sizeof(plain)) == 0);
        assert(rd32(plain) == 0xffffffff);
        uint8_t input[64], want[16];
        for (unsigned i = 0; i < 32; i++)
            input[i] = (uint8_t)(0xa0 + i);
        memcpy(input + 32, f->material + (0x85178 - 0x841d0), 28);
        input[60] = 0;
        input[61] = 0;
        input[62] = 0;
        input[63] = 1;
        cmac_key(f, 0x851c8, 0x85208, input, 64, want);
        assert(memcmp(plain + 4, want, 16) == 0);
        assert(memcmp(plain + 20, (uint8_t[4]){0}, 4) == 0);
        wr32(resp + 8, 0);
        secure_clear(plain, sizeof(plain));
    } else
        assert(!"unexpected secure command");
    if (id & 0x200) {
        if (dt295_cbc_encrypt(f->session, f->iv, resp + 12, 496))
            abort();
    }
    wr16(f->mem + 0x80014, (uint16_t)(8u << 8));
    wr16(f->mem + 0x90074, (uint16_t)(rd16(f->mem + 0x90074) | 8));
}

static int control(void *u, uint8_t rt, uint8_t req, uint16_t value, uint16_t index, uint8_t *data,
                   size_t n, unsigned timeout, size_t *actual) {
    fake *f = u;
    if (f->control_delay_ms != 0U) {
        const unsigned elapsed = f->control_delay_ms < timeout ? f->control_delay_ms : timeout;
        f->now += elapsed;
        if (elapsed < f->control_delay_ms) {
            *actual = 0U;
            return -1;
        }
    }
    *actual = n;
    if (req == 0xbc) {
        uint32_t addr = (uint32_t)(value >> 8) << 16 | index;
        if (addr + n > MEMSIZE)
            return -1;
        if ((rt & 0x80) && addr == 0x90074 && n == 2)
            ++f->status_reads;
        if ((rt & 0x80) && addr == 0x82008 && f->fail_state_read_once) {
            f->fail_state_read_once = 0;
            return -1;
        }
        if ((rt & 0x80) && addr == 0x90074 && n == 2 && f->status_notifications != 0U)
            wr16(data, 0x200);
        else if (rt & 0x80)
            memcpy(data, f->mem + addr, n);
        else if (addr == 0x90074 && n == 2) {
            if (rd16(data) == 0x200) {
                ++f->status_acks;
                if (f->status_notifications != 0U)
                    --f->status_notifications;
            }
            wr16(f->mem + addr, (uint16_t)(rd16(f->mem + addr) & ~rd16(data)));
        } else
            memcpy(f->mem + addr, data, n);
        return 0;
    }
    if (req == 0xb8 || req == 0xb9) {
        if (n != 4)
            return -1;
        uint8_t cmd = data[0], sub = data[1];
        uint16_t val = rd16(data + 2);
        if (cmd == 8 && f->fail_secure_id && f->fail_secure_id == rd32(f->mem + 0x83000)) {
            f->fail_secure_id = 0;
            return -1;
        } else if (cmd == 8)
            respond(f, rd32(f->mem + 0x83000));
        else if (req == 0xb9) {
            if (cmd == 0x0a && sub == 1U) {
                if (f->fail_card_activation_command) {
                    f->fail_card_activation_command = 0;
                    return -1;
                }
                ++f->card_activation_commands;
                if (!f->hold_card_activation_state)
                    wr16(f->mem + 0x822ac, 0x3000U);
            }
            f->i2c_commands++;
            wr16(f->mem + 0x80016, (uint16_t)(cmd << 8));
            wr16(f->mem + 0x823be, 0);
            uint16_t bits = (uint16_t)(2 | (cmd == 9 ? 0x100 : 0));
            wr16(f->mem + 0x90074, (uint16_t)(rd16(f->mem + 0x90074) | bits));
        } else {
            wr16(f->mem + 0x80014, (uint16_t)(cmd << 8));
            uint16_t state = rd16(f->mem + 0x82008);
            if (cmd == 1) {
                f->init_commands++;
                state = 0x11;
                if (f->inject_post_auth_fault) {
                    f->inject_post_auth_fault = 0;
                    f->fail_state_read_once = 1;
                }
            }
            else if (cmd == 4 && sub == 1)
                state = 0x10;
            else if (cmd == 4 && sub == 0x20) {
                assert(val == 0);
                f->acquire_commands++;
                state = 1;
            } else if (cmd == 4 && val == 0x20) {
                assert(!"acquire must use command 4/subcommand 0x20/value 0");
            }
            else if (cmd == 4 && sub == 0 && val == 1)
                f->shutdown_commands++;
            else if (cmd == 5 && val == 2) {
                f->start_commands++;
                state = 2;
            }
            else if (cmd == 5 && val == 1)
                state = 1;
            else if (cmd == 5 && (val == 3 || val == 4)) {
                f->pause_pending = 1;
                state = 2;
            }
            wr16(f->mem + 0x82008, state);
            wr16(f->mem + 0x90074, (uint16_t)(rd16(f->mem + 0x90074) | 8 | 4));
        }
        return 0;
    }
    return -1;
}
static int bulk(void *, uint8_t, uint8_t *, size_t, unsigned, size_t *);
static int interrupt(void *, uint8_t, uint8_t *, size_t, unsigned, size_t *);
static uint64_t now(void *);
static void sleep_fake(void *, unsigned);
static void test_card_recovery_fatal(const dt295_assets *assets, const fake *source) {
    fake f = {0};
    f.mem = calloc(1, MEMSIZE);
    assert(f.mem);
    f.now = 1000;
    memcpy(f.material, source->material, sizeof(f.material));
    wr16(f.mem + 0x82008, 0x10);
    wr16(f.mem + 0x822ac, 0x3000U);
    dt295_io io = {&f, control, bulk, interrupt, now, sleep_fake};
    dt295 *d = NULL;
    char error[256];
    assert(dt295_create(&d, &io, assets, error, sizeof(error)) == 0);
    assert(dt295_boot(d, error, sizeof(error)) == 0);
    assert(dt295_open_session(d, error, sizeof(error)) == 0);
    assert(dt295_card_initialize(d, error, sizeof(error)) == 0);
    assert(dt295_card_activate(d, 2000U, error, sizeof(error)) == 0);
    wr16(f.mem + 0x822ac, 0x1000U);
    f.status_notifications = 1U;
    dt295_card_snapshot observed = {0}, recovered = {0};
    assert(dt295_card_observe(d, &observed) == 0);
    assert(observed.raw_state == 1U && observed.activation_valid &&
           observed.notification_generation > observed.activation_generation);
    f.fail_card_activation_command = 1;
    const dt295_card_recovery_result outcome = dt295_card_recover(
        d, &observed, f.now + 3000U, &recovered, error, sizeof(error));
    assert(outcome == DT295_CARD_RECOVERY_FATAL);
    assert(dt295_card_observe(d, &recovered) != 0);
    size_t drained = 0U;
    assert(dt295_stop(d, &drained, error, sizeof(error)) == 0);
    assert(dt295_shutdown(d, error, sizeof(error)) == 0);
    dt295_destroy(d);
    free(f.mem);
}
static int bulk(void *u, uint8_t ep, uint8_t *p, size_t n, unsigned timeout, size_t *actual) {
    (void)p;
    (void)timeout;
    fake *f = u;
    *actual = 0;
    if (ep == 0x02) {
        *actual = n;
        return 0;
    }
    if (ep == 0x81 && f->pause_pending) {
        f->pause_pending = 0;
        wr16(f->mem + 0x82008, 4);
        wr16(f->mem + 0x90074, (uint16_t)(rd16(f->mem + 0x90074) | 4));
        return -7;
    }
    if (ep == 0x81)
        return -7;
    return -1;
}
static int interrupt(void *u, uint8_t ep, uint8_t *p, size_t n, unsigned timeout, size_t *actual) {
    (void)u;
    (void)ep;
    (void)p;
    (void)n;
    (void)timeout;
    *actual = 0;
    return -7;
}
static uint64_t now(void *u) {
    return ((fake *)u)->now;
}
static void sleep_fake(void *u, unsigned ms) {
    ((fake *)u)->now += ms;
}

static void create_assets(const char *path, fake *f) {
    uint8_t idle[1] = {0x11}, trc[1] = {0x22}, field[8] = {0},
            keyreq[12] = {0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3};
    for (size_t i = 0; i < sizeof(f->material); i++)
        f->material[i] = (uint8_t)(i * 17 + 3);
    memset(f->material, 0, 22 * 8);
    memset(f->material + (0x84630 - 0x841d0), 0, 26 * 8);
    memset(f->material + (0x84700 - 0x841d0), 0, 28 * 8);
    memcpy(f->material + (0x85218 - 0x841d0), (uint8_t[16]){0}, 16);
    for (unsigned i = 0; i < 18; i++) {
        uint8_t *row = f->material + (0x845a0 - 0x841d0) + i * 8;
        row[0] = (uint8_t)(0x20 + i);
        row[1] = row[2] = row[3] = 0;
        row[4] = (uint8_t)(i + 1);
        row[5] = row[6] = row[7] = 0;
    }
    memset(f->iv, 0, 16);
    for (unsigned i = 0; i < sizeof(f->ts_iv); i++)
        f->ts_iv[i] = (uint8_t)(0xa0 + i);
    assert(memcmp(f->ts_iv, f->material + (0x85218 - 0x841d0), 16) != 0);
    field[0] = 0x00;
    field[1] = 0x10;
    field[2] = 0;
    field[3] = 1;
    field[4] = 1; /* address 1000, bit 0 = 1 */
    FILE *fp = fopen(path, "wb");
    assert(fp);
    assert(fwrite("D295A1\0\0", 1, 8, fp) == 8);
    uint8_t sections[6][1] = {{0}};
    (void)sections;
    uint8_t *items[6] = {idle, trc, f->material, field, keyreq, f->ts_iv};
    size_t sizes[6] = {sizeof(idle),  sizeof(trc),    sizeof(f->material),
                       sizeof(field), sizeof(keyreq), sizeof(f->ts_iv)};
    uint8_t header[6 * 36];
    for (unsigned i = 0; i < 6; i++) {
        uint32_t n = (uint32_t)sizes[i];
        header[i * 36] = (uint8_t)n;
        header[i * 36 + 1] = (uint8_t)(n >> 8);
        header[i * 36 + 2] = (uint8_t)(n >> 16);
        header[i * 36 + 3] = (uint8_t)(n >> 24);
        dt295_sha256(items[i], sizes[i], header + i * 36 + 4);
    }
    assert(fwrite(header, 1, sizeof(header), fp) == sizeof(header));
    for (unsigned i = 0; i < 6; i++)
        assert(fwrite(items[i], 1, sizes[i], fp) == sizes[i]);
    assert(fclose(fp) == 0);
}
static void rewrite_asset_file(const char *path, const uint8_t *bytes, size_t n) {
    FILE *fp = fopen(path, "wb");
    assert(fp);
    assert(fwrite(bytes, 1, n, fp) == n);
    assert(fclose(fp) == 0);
}
static void test_asset_rejection(const char *path) {
    FILE *fp = fopen(path, "rb");
    assert(fp);
    assert(fseek(fp, 0, SEEK_END) == 0);
    long sz = ftell(fp);
    assert(sz > 0);
    assert(fseek(fp, 0, SEEK_SET) == 0);
    uint8_t *data = malloc((size_t)sz);
    assert(data);
    assert(fread(data, 1, (size_t)sz, fp) == (size_t)sz);
    fclose(fp);
    dt295_assets *a = NULL;
    char error[128];
    uint8_t saved[4];
    memcpy(saved, data + 8, 4);
    memset(data + 8, 0xff, 4);
    rewrite_asset_file(path, data, (size_t)sz);
    assert(dt295_assets_load(path, &a, error, sizeof(error)) < 0 && a == NULL);
    memcpy(data + 8, saved, 4);
    data[12] ^= 1;
    rewrite_asset_file(path, data, (size_t)sz);
    assert(dt295_assets_load(path, &a, error, sizeof(error)) < 0 && a == NULL);
    rewrite_asset_file(path, data, 12);
    assert(dt295_assets_load(path, &a, error, sizeof(error)) < 0 && a == NULL);
    memcpy(data + 12, (uint8_t[1]){(uint8_t)(data[12] ^ 1)}, 1);
    rewrite_asset_file(path, data, (size_t)sz);
    free(data);
}

static void test_authenticated_setup_failure_cleanup(const dt295_assets *assets,
                                                     const fake *source) {
    fake f = {0};
    f.mem = calloc(1, MEMSIZE);
    assert(f.mem);
    f.now = 1000;
    memcpy(f.material, source->material, sizeof(f.material));
    wr16(f.mem + 0x82008, 0x10);
    f.mem[0x822ac] = 0x30;
    f.inject_post_auth_fault = 1;
    dt295_io io = {&f, control, bulk, interrupt, now, sleep_fake};
    dt295 *d = NULL;
    char error[256];
    assert(dt295_create(&d, &io, assets, error, sizeof(error)) == 0);
    assert(dt295_boot(d, error, sizeof(error)) == 0);
    assert(dt295_open_session(d, error, sizeof(error)) < 0);
    assert(strcmp(error,
                  "DT295 authentication failed; state-changing request was not retried") == 0);
    assert(dt295_has_session(d));
    assert(f.auth_requests == 1 && f.init_commands == 1);

    /* The retained session is cleanup-only: setup, renewal, and start paths
     * must not issue a second initialization or any new state-changing work. */
    dt295_route_plan empty = {0};
    assert(dt295_card_initialize(d, error, sizeof(error)) < 0);
    assert(dt295_card_activate(d, 1000, error, sizeof(error)) < 0);
    assert(dt295_tune_terrestrial(d, 27, error, sizeof(error)) < 0);
    assert(dt295_prepare_plan(d, &empty, error, sizeof(error)) < 0);
    assert(dt295_start(d, error, sizeof(error)) < 0);
    int renewed = 0;
    assert(dt295_renew_if_due(d, &renewed, error, sizeof(error)) < 0 && renewed == 0);
    assert(dt295_open_session(d, error, sizeof(error)) < 0);
    assert(f.auth_requests == 1 && f.init_commands == 1 && f.start_commands == 0);

    size_t drained = 0;
    assert(dt295_stop(d, &drained, error, sizeof(error)) == 0);
    assert(dt295_state(d) == 0x11);
    assert(dt295_shutdown(d, error, sizeof(error)) == 0);
    assert(f.shutdown_commands == 1 && !dt295_has_session(d));
    dt295_destroy(d);
    free(f.mem);
}

int main(void) {
    const char *path = "tests/.dt295-synthetic-assets.tmp";
    fake f = {0};
    f.mem = calloc(1, MEMSIZE);
    assert(f.mem);
    f.now = 1000;
    wr16(f.mem + 0x82008, 0x10);
    f.mem[0x822ac] = 0x30;
    create_assets(path, &f);
    test_asset_rejection(path);
    dt295_assets *assets = NULL;
    char error[256];
    assert(dt295_assets_load(path, &assets, error, sizeof(error)) == 0);
    remove(path);
    test_authenticated_setup_failure_cleanup(assets, &f);
    dt295_io io = {&f, control, bulk, interrupt, now, sleep_fake};
    dt295 *d = NULL;
    assert(dt295_create(&d, &io, assets, error, sizeof(error)) == 0);
    assert(dt295_boot(d, error, sizeof(error)) == 0);
    if (dt295_open_session(d, error, sizeof(error)) != 0) {
        fprintf(stderr, "open_session: %s\n", error);
        abort();
    }
    assert(dt295_has_session(d));
    assert(dt295_card_initialize(d, error, sizeof(error)) == 0);
    assert(dt295_card_activate(d, 2000, error, sizeof(error)) == 0);
    dt295_card_snapshot card_snapshot = {0};
    assert(dt295_card_observe(d, &card_snapshot) == 0);
    assert(card_snapshot.raw_state == 3 && card_snapshot.activation_valid);
    assert(card_snapshot.notification_generation == card_snapshot.activation_generation);
    wr16(f.mem + 0x822ac, 0);
    assert(dt295_card_observe(d, &card_snapshot) == 0);
    assert(card_snapshot.raw_state == 0 && !card_snapshot.fresh_notification);
    assert(card_snapshot.notification_generation == card_snapshot.activation_generation);
    f.status_notifications = 1;
    assert(dt295_card_observe(d, &card_snapshot) == 0);
    assert(card_snapshot.raw_state == 0 && card_snapshot.fresh_notification);
    assert(card_snapshot.notification_generation > card_snapshot.activation_generation);
    assert(f.status_acks == 1U && f.status_notifications == 0U);
    wr16(f.mem + 0x90074, 0x400);
    const unsigned acks_before_response = f.status_acks;
    assert(dt295_card_observe(d, &card_snapshot) != 0);
    assert(rd16(f.mem + 0x90074) == 0x400 && f.status_acks == acks_before_response);
    wr16(f.mem + 0x90074, 0);
    wr16(f.mem + 0x822ac, 0x1000U);
    f.status_notifications = 1U;
    assert(dt295_card_observe(d, &card_snapshot) == 0);
    assert(card_snapshot.raw_state == 1U && card_snapshot.activation_valid &&
           card_snapshot.notification_generation > card_snapshot.activation_generation);
    const unsigned activations_before_recovery = f.card_activation_commands;
    dt295_card_snapshot recovered_snapshot = {0};
    assert(dt295_card_recover(d, &card_snapshot, f.now + 3000U, &recovered_snapshot,
                              error, sizeof(error)) == DT295_CARD_RECOVERY_ACTIVATED);
    assert(recovered_snapshot.raw_state == 3U && recovered_snapshot.activation_valid &&
           recovered_snapshot.notification_generation ==
               recovered_snapshot.activation_generation);
    assert(f.card_activation_commands == activations_before_recovery + 1U);
    assert(dt295_card_recover(d, &card_snapshot, f.now + 3000U, &recovered_snapshot,
                              error, sizeof(error)) == DT295_CARD_RECOVERY_PENDING);
    assert(f.card_activation_commands == activations_before_recovery + 1U);

    wr16(f.mem + 0x822ac, 0x1000U);
    f.status_notifications = 1U;
    assert(dt295_card_observe(d, &card_snapshot) == 0);
    f.hold_card_activation_state = 1;
    const unsigned activations_before_pending = f.card_activation_commands;
    const uint64_t recovery_deadline = f.now + 100U;
    assert(dt295_card_recover(d, &card_snapshot, recovery_deadline, &recovered_snapshot,
                              error, sizeof(error)) == DT295_CARD_RECOVERY_PENDING);
    assert(f.now <= recovery_deadline && f.card_activation_commands ==
           activations_before_pending + 1U);
    assert(dt295_card_observe(d, &recovered_snapshot) == 0);
    assert(recovered_snapshot.raw_state == 1U && recovered_snapshot.activation_valid);
    assert(dt295_card_recover(d, &card_snapshot, f.now + 3000U, &recovered_snapshot,
                              error, sizeof(error)) == DT295_CARD_RECOVERY_PENDING);
    assert(f.card_activation_commands == activations_before_pending + 1U);

    f.hold_card_activation_state = 0;
    f.status_notifications = 1U;
    assert(dt295_card_observe(d, &card_snapshot) == 0);
    const unsigned activations_before_new_generation = f.card_activation_commands;
    assert(dt295_card_recover(d, &card_snapshot, f.now + 3000U, &recovered_snapshot,
                              error, sizeof(error)) == DT295_CARD_RECOVERY_ACTIVATED);
    assert(f.card_activation_commands == activations_before_new_generation + 1U);
    test_card_recovery_fatal(assets, &f);

    wr16(f.mem + 0x822ac, 0x3000);
    f.status_notifications = 32U;
    f.control_delay_ms = 100U;
    const uint64_t observation_started = f.now;
    assert(dt295_card_observe(d, &card_snapshot) != 0);
    assert(f.now - observation_started <= 1000U);
    assert(f.status_notifications != 0U);
    f.control_delay_ms = 0U;
    f.status_notifications = 0U;
    wr16(f.mem + 0x90074, 0);
    wr16(f.mem + 0x822ac, 0x3000);
    if (dt295_tune_terrestrial(d, 27, error, sizeof(error)) != 0) {
        fprintf(stderr, "tune: %s (%u I2C commands pending=%04x)\n", error, f.i2c_commands,
                rd16(f.mem + 0x90074));
        abort();
    }
    dt295_route_plan plan = {0};
    plan.routes[0] = (dt295_route){0, 1, 0, 0, 0x301, 6};
    plan.routes[1] = (dt295_route){1, 1, 0, 0, 0x302, 63};
    plan.routes[4] = (dt295_route){4, 1, 0, 0, 0, 0};
    plan.routes[6] = (dt295_route){6, 1, 1, 0, 0x206, 6};
    plan.routes[39] = (dt295_route){39, 1, 0, 0, 0x101, 0};
    plan.routes[40] = (dt295_route){40, 1, 0, 0, 0x102, 0};
    plan.routes[63] = (dt295_route){63, 1, 1, 0, 0x3ff, 63};
    plan.route_count = 7;
    plan.primary_program = 0x1234;
    plan.video_stream_type = 2;
    plan.audio_stream_type = 4;
    wr16(f.mem + 0x1070, 0xffff);
    if (dt295_prepare_plan(d, &plan, error, sizeof(error)) != 0) {
        fprintf(stderr, "prepare: %s\n", error);
        abort();
    }
    assert(f.key_requests == 1);
    assert(rd16(f.mem + 0x1018) == 0x1234);
    assert(rd16(f.mem + 0x101c) == 0x0204);
    assert(rd16(f.mem + 0x101e) == 0x8301);
    assert(rd16(f.mem + 0x1020) == 0x8302);
    assert(rd16(f.mem + 0x10a2) == 0x063f);
    assert(rd16(f.mem + 0x10a6) == 0);
    assert(rd16(f.mem + 0x10a8) == 0x0600);
    assert(rd16(f.mem + 0x106c) == 0x8101);
    assert(rd16(f.mem + 0x106e) == 0x8102);
    assert(rd16(f.mem + 0x1070) == 0);
    assert(rd16(f.mem + 0x109c) == 0x83ff);
    assert(rd16(f.mem + 0x10e0) == 0x003f);
    assert(dt295_start(d, error, sizeof(error)) == 0);
    assert(f.acquire_commands == 1);
    size_t drained = 0;
    assert(dt295_stop(d, &drained, error, sizeof(error)) == 0);
    assert(dt295_state(d) == 0x11);
    if (dt295_reprepare_plan(d, &plan, error, sizeof(error)) != 0) {
        fprintf(stderr, "reprepare: %s\n", error);
        abort();
    }
    assert(f.key_requests == 1);
    assert(f.acquire_commands == 2);
    assert(dt295_start(d, error, sizeof(error)) == 0);
    assert(dt295_stop(d, &drained, error, sizeof(error)) == 0);
    f.now += 30000;
    int renewed = 0;
    f.fail_secure_id = 0x02020400;
    assert(dt295_renew_if_due(d, &renewed, error, sizeof(error)) < 0 && renewed == 0);
    assert(f.renewal_challenges == 1 && f.renewal_applies == 0);
    /* A failed renewal leaves its original deadline and counter intact. */
    assert(dt295_renew_if_due(d, &renewed, error, sizeof(error)) == 0 && renewed == 1);
    assert(f.renewal_challenges == 2 && f.renewal_applies == 1);
    assert(dt295_shutdown(d, error, sizeof(error)) == 0);
    assert(dt295_state(d) == 0x10);
    assert(!dt295_has_session(d));
    dt295_destroy(d);
    free(f.mem);

    /* Malformed oversized key response must be rejected before touching the
     * 16-byte in-context TS-key buffer (ASan/UBSan run by the test target). */
    fake bad = {0};
    bad.mem = calloc(1, MEMSIZE);
    assert(bad.mem);
    bad.now = 1000;
    bad.key_words = 5;
    memcpy(bad.material, f.material, sizeof(bad.material));
    memcpy(bad.iv, f.iv, sizeof(bad.iv));
    wr16(bad.mem + 0x82008, 0x10);
    bad.mem[0x822ac] = 0x30;
    dt295_io bad_io = {&bad, control, bulk, interrupt, now, sleep_fake};
    dt295 *bad_device = NULL;
    assert(dt295_create(&bad_device, &bad_io, assets, error, sizeof(error)) == 0);
    assert(dt295_boot(bad_device, error, sizeof(error)) == 0);
    assert(dt295_open_session(bad_device, error, sizeof(error)) == 0);
    assert(dt295_card_initialize(bad_device, error, sizeof(error)) == 0);
    assert(dt295_card_activate(bad_device, 2000, error, sizeof(error)) == 0);
    assert(dt295_tune_terrestrial(bad_device, 27, error, sizeof(error)) == 0);
    assert(dt295_prepare_plan(bad_device, &plan, error, sizeof(error)) < 0);
    assert(bad.key_requests == 1);
    dt295_destroy(bad_device);
    dt295_assets_free(assets);
    free(bad.mem);
    return 0;
}
