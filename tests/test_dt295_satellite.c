/* Deterministic satellite register transcript with synthetic assets and a memory USB. */
#define main dt295_protocol_fixture_unused_main
#include "test_dt295_protocol.c"
#undef main

static uint8_t sat_regs[256];
static uint8_t sat_pending_reg;
static int sat_rf_read_pending;

static int sat_take_write(fake *f, uint8_t sub) {
    uint8_t p[64];
    memcpy(p, f->mem + 0x800c0, sizeof(p));
    if (p[0] == 0xfe && p[1] == 0xc1) {
        sat_rf_read_pending = 1;
        return 0;
    }
    if (p[0] == 0xfe && p[1] == 0xc0) {
        if (sub == 0 && p[2] == 0x11) {
            sat_regs[0x11] = p[4];
        }
        return 0;
    }
    if (sub == 0) {
        sat_regs[p[0]] = p[1];
        if (p[0] == 0x8f)
            sat_regs[0xe6] = p[1];
        if (p[0] == 0x90)
            sat_regs[0xe7] = p[1];
    } else if (sub == 2 || sub == 1) {
        sat_pending_reg = p[0];
        sat_rf_read_pending = 0;
    }
    return 0;
}

static int sat_fill_read(fake *f, uint8_t sub, uint16_t length) {
    unsigned n = length & 255u;
    uint8_t *reply = f->mem + 0x823be + 2;
    memset(reply, 0, 66);
    if (sat_rf_read_pending) {
        if (n > 0)
            reply[0] = 0;
        if (n > 1)
            reply[1] = 0x60;
        sat_rf_read_pending = 0;
        return 0;
    }
    if (sub != 1 && sub != 3)
        return 0;
    for (unsigned i = 0; i < n; i++)
        reply[i] = sat_regs[(uint8_t)(sat_pending_reg + i)];
    return 0;
}

static int sat_control(void *u, uint8_t rt, uint8_t req, uint16_t value, uint16_t index,
                       uint8_t *data, size_t n, unsigned timeout, size_t *actual) {
    fake *f = u;
    int satellite = req == 0xb9 && n == 4 && data[0] == 9 && data[2] == 0x22;
    if (satellite)
        sat_take_write(f, data[1]);
    int r = control(u, rt, req, value, index, data, n, timeout, actual);
    if (satellite)
        sat_fill_read(f, data[1], (uint16_t)(data[2] << 8 | data[3]));
    return r;
}

static void check(const char *what, int condition) {
    if (!condition) {
        fprintf(stderr, "FAILED %s\n", what);
        abort();
    }
}

int main(void) {
    const char *path = "tests/.dt295-satellite-assets.tmp";
    fake f = {0};
    f.mem = calloc(1, MEMSIZE);
    assert(f.mem);
    f.now = 1000;
    wr16(f.mem + 0x82008, 0x10);
    f.mem[0x822ac] = 0x30;
    create_assets(path, &f);
    dt295_assets *assets = NULL;
    char error[256];
    assert(dt295_assets_load(path, &assets, error, sizeof(error)) == 0);
    remove(path);
    dt295_io io = {&f, sat_control, bulk, interrupt, now, sleep_fake};
    dt295 *d = NULL;
    assert(dt295_create(&d, &io, assets, error, sizeof(error)) == 0);
    assert(dt295_boot(d, error, sizeof(error)) == 0);
    assert(dt295_open_session(d, error, sizeof(error)) == 0);
    assert(dt295_card_initialize(d, error, sizeof(error)) == 0);
    assert(dt295_card_activate(d, 2000, error, sizeof(error)) == 0);

    sat_regs[0xba] = 0xff;
    sat_regs[0xbc] = 0x12;
    sat_regs[0xbd] = 0x34;
    check("BS tune", dt295_tune_satellite(d, 1049480000u, 16400, error, sizeof(error)) == 0);
    check("satellite table applied", sat_regs[0x20] == 1 && sat_regs[0x31] == 18);
    dt295_satellite_state st = {0};
    check("BS status", dt295_satellite_status(d, &st, error, sizeof(error)) == 0);
    check("BS TSID", st.selected_tsid == 16400);
    check("BS lock", st.locked == 1);
    check("BS AGC", st.agc_raw == 0x7f);
    check("BS CN", st.cn_raw == 0x1234);
    sat_regs[0xc3] = 0x10;
    check("unlock status", dt295_satellite_status(d, &st, error, sizeof(error)) == 0);
    check("unlock value", st.locked == 0);
    sat_regs[0xc3] = 0;
    check("TSID reselect", dt295_select_satellite_tsid(d, 29024, error, sizeof(error)) == 0);
    check("TSID reselect status", dt295_satellite_status(d, &st, error, sizeof(error)) == 0);
    check("TSID reselect value", st.selected_tsid == 29024);
    check("CS tune", dt295_tune_satellite(d, 2013000000u, 16400, error, sizeof(error)) == 0);
    check("CS tune status", dt295_satellite_status(d, &st, error, sizeof(error)) == 0);
    check("CS tune TSID", st.selected_tsid == 16400);
    check("IF range low", dt295_tune_satellite(d, 949999999u, 1, error, sizeof(error)) < 0);
    check("IF range high", dt295_tune_satellite(d, 2150000001u, 1, error, sizeof(error)) < 0);
    check("TSID range", dt295_tune_satellite(d, 1049480000u, 65536, error, sizeof(error)) < 0);
    check("invalid TSID select", dt295_select_satellite_tsid(d, 0, error, sizeof(error)) == 0);
    dt295_destroy(d);
    dt295_assets_free(assets);
    free(f.mem);
    return 0;
}
