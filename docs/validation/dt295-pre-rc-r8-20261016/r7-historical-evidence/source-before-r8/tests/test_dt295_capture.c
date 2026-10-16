#ifdef _WIN32
#include <stdio.h>
int main(void) {
    puts("DT295 capture orchestration: skipped (POSIX implementation only)");
    return 0;
}
#else
/* SPDX-License-Identifier: MIT */
/* Exercise the real capture orchestrator and routing watcher with synthetic
 * broadcast tables and injected protocol/USB/output. No libusb discovery. */
#define DT295_CAPTURE_TESTING
#define now_ms capture_test_now
#include "../src/dt295_capture.c"
#undef now_ms
#include <assert.h>

struct dt295_assets {
    int valid;
};
struct dt295 {
    int state, session, starts;
};
struct usb_backend {
    int unused;
};
struct dt295_stream {
    int generation;
    uint64_t sequence;
    uint64_t delivered;
    size_t offset;
    unsigned split_boundary;
    uint8_t fixture[564];
};
struct ts_output {
    ts_output_stats stats;
    int diagnostic;
};
static uint64_t fake_time;
static int opens, boots, prepares, reprepares, stops, cancels, closed, shutdowns;
static int fail_assets, fail_recording_output, fail_final_stop, fail_shutdown, fail_start,
    output_count;
static uint8_t continuity[8192];
static uint64_t saved;
static int final_usb_counters_seen;
static int final_table_counters_seen;
static unsigned route_status_count;
static int route_status_missing_counter;
static int stop_cleanup_seen, shutdown_cleanup_seen, telemetry_values_seen;
static unsigned fragment_boundary = 188;
uint64_t capture_test_now(void) {
    return fake_time;
}
static void table(uint8_t p[188], unsigned pid, unsigned tid, const uint8_t *payload,
                  size_t bytes) {
    memset(p, 255, 188);
    p[0] = 0x47;
    p[1] = (uint8_t)(0x40 | (pid >> 8));
    p[2] = (uint8_t)pid;
    p[3] = (uint8_t)(0x10 | (continuity[pid]++ & 15));
    p[4] = 0;
    uint8_t *s = p + 5;
    size_t n = 12 + bytes;
    s[0] = (uint8_t)tid;
    s[1] = (uint8_t)(0xb0 | ((n - 3) >> 8));
    s[2] = (uint8_t)(n - 3);
    s[3] = 0;
    s[4] = 1;
    s[5] = 0xc1;
    s[6] = s[7] = 0;
    if (bytes)
        memcpy(s + 8, payload, bytes);
    uint32_t crc = ts_crc(s, n - 4);
    for (unsigned i = 0; i < 4; i++)
        s[n - 4 + i] = (uint8_t)(crc >> (24 - 8 * i));
}
int dt295_assets_load(const char *p, dt295_assets **out, char *e, size_t n) {
    assert(strstr(p, "/dt295.assets"));
    if (fail_assets) {
        snprintf(e, n, "synthetic bad assets");
        return -1;
    }
    *out = calloc(1, sizeof(**out));
    return *out ? 0 : -1;
}
void dt295_assets_free(dt295_assets *a) {
    free(a);
}
int usb_open_model(usb_backend **u, const usb_selector *s, int fd, int detach, int model) {
    (void)s;
    (void)fd;
    (void)detach;
    assert(model == USB_MODEL_DT295);
    opens++;
    *u = calloc(1, sizeof(**u));
    return 0;
}
void usb_close(usb_backend *u) {
    if (u) {
        closed++;
        free(u);
    }
}
void *usb_native_context(usb_backend *u) {
    return u;
}
void *usb_native_handle(usb_backend *u) {
    return u;
}
int dt295_create(dt295 **d, const dt295_io *io, const dt295_assets *a, char *e, size_t n) {
    (void)io;
    (void)a;
    (void)e;
    (void)n;
    *d = calloc(1, sizeof(**d));
    (*d)->state = 16;
    return 0;
}
void dt295_destroy(dt295 *d) {
    free(d);
}
int dt295_boot(dt295 *d, char *e, size_t n) {
    (void)d;
    (void)e;
    (void)n;
    boots++;
    return 0;
}
int dt295_open_session(dt295 *d, char *e, size_t n) {
    (void)e;
    (void)n;
    d->session = 1;
    d->state = 17;
    return 0;
}
int dt295_card_initialize(dt295 *d, char *e, size_t n) {
    (void)e;
    (void)n;
    assert(d->state == 17);
    return 0;
}
int dt295_card_activate(dt295 *d, unsigned ms, char *e, size_t n) {
    (void)d;
    (void)ms;
    (void)e;
    (void)n;
    return 0;
}
int dt295_tune_terrestrial(dt295 *d, unsigned c, char *e, size_t n) {
    (void)d;
    (void)e;
    (void)n;
    assert(c == 27);
    return 0;
}
int dt295_prepare_plan(dt295 *d, const dt295_route_plan *p, char *e, size_t n) {
    (void)e;
    (void)n;
    assert(!p->ready);
    prepares++;
    d->state = 1;
    return 0;
}
int dt295_reprepare_plan(dt295 *d, const dt295_route_plan *p, char *e, size_t n) {
    (void)e;
    (void)n;
    assert(p->ready && p->has_primary && d->state == 17);
    reprepares++;
    d->state = 1;
    return 0;
}
int dt295_route_write(dt295 *d, const dt295_route_op *op, char *e, size_t n) {
    (void)e;
    (void)n;
    assert(d->state == 2 && op->enabled);
    return 0;
}
int dt295_start(dt295 *d, char *e, size_t n) {
    (void)e;
    (void)n;
    assert(d->state == 1);
    d->state = 2;
    d->starts++;
    return fail_start ? -1 : 0;
}
int dt295_renew_if_due(dt295 *d, int *renewed, char *e, size_t n) {
    (void)d;
    (void)e;
    (void)n;
    *renewed = 0;
    return 0;
}
int dt295_stop(dt295 *d, size_t *drained, char *e, size_t n) {
    stops++;
    *drained = 188;
    d->state = 17;
    if (fail_final_stop && stops == 2) {
        snprintf(e, n, "synthetic pause failure");
        return -1;
    }
    return 0;
}
int dt295_shutdown(dt295 *d, char *e, size_t n) {
    shutdowns++;
    if (fail_shutdown) {
        snprintf(e, n, "synthetic shutdown failure");
        return -1;
    }
    d->state = 16;
    d->session = 0;
    return 0;
}
uint16_t dt295_state(dt295 *d) {
    return d ? (uint16_t)d->state : 65535;
}
int dt295_has_session(const dt295 *d) {
    return d && d->session;
}
int dt295_decode_packet(dt295 *d, uint8_t p[188]) {
    (void)d;
    return p[0] == 0x47 ? 0 : -1;
}
int dt295_stream_start(libusb_context *ctx, libusb_device_handle *h, dt295_stream **s) {
    (void)ctx;
    (void)h;
    *s = calloc(1, sizeof(**s));
    assert(*s);
    (*s)->generation = reprepares;
    (*s)->split_boundary = fragment_boundary;
    uint8_t pat[] = {0, 1, 0xe1, 0},
            pmt[] = {0xe1, 1, 0xf0, 0, 0x1b, 0xe1, 1, 0xf0, 0, 0x0f, 0xe1, 2, 0xf0, 0};
    table((*s)->fixture, 0, 0, pat, sizeof(pat));
    table((*s)->fixture + 188, 1, 1, NULL, 0);
    table((*s)->fixture + 376, 256, 2, pmt, sizeof(pmt));
    return 0;
}
int dt295_stream_read(dt295_stream *s, void *buffer, size_t cap, uint64_t *sequence, uint64_t *ns,
                      unsigned ms) {
    (void)ms;
    assert(cap > 0 && s->offset < sizeof(s->fixture));
    *sequence = s->sequence++;
    *ns = fake_time * 1000000;
    size_t packet_offset = s->offset % 188;
    size_t boundary = packet_offset < s->split_boundary ? s->split_boundary : 188;
    size_t amount = boundary - packet_offset;
    if (amount > sizeof(s->fixture) - s->offset)
        amount = sizeof(s->fixture) - s->offset;
    if (amount > cap)
        amount = cap;
    memcpy(buffer, s->fixture + s->offset, amount);
    s->offset += amount;
    s->delivered += amount;
    if (s->generation && s->offset == sizeof(s->fixture))
        fake_time += 1000;
    return (int)amount;
}
void dt295_stream_snapshot(dt295_stream *s, dt295_stream_stats *v) {
    memset(v, 0, sizeof(*v));
    v->delivered_bytes = v->completed_bytes = s->delivered;
    v->late_actual_bytes = 188;
}
int dt295_stream_stop(dt295_stream *s, unsigned ms) {
    (void)s;
    (void)ms;
    cancels++;
    return 0;
}
int dt295_stream_destroy(dt295_stream *s) {
    free(s);
    return 0;
}
int ts_output_open(const char *p, int fd, const ts_output_config *c, ts_output **o) {
    (void)p;
    (void)fd;
    (void)c;
    *o = calloc(1, sizeof(**o));
    (*o)->diagnostic = output_count++;
    if (!(*o)->diagnostic) {
        (*o)->stats.writer_write_ns_total = 101;
        (*o)->stats.writer_write_ns_max = 102;
        (*o)->stats.writer_write_records = 103;
        (*o)->stats.max_pending_age_ns = 104;
        (*o)->stats.oldest_pending_age_ns = 105;
        (*o)->stats.max_parent_poll_gap_ns = 106;
        (*o)->stats.max_parent_ack_gap_ns = 107;
        (*o)->stats.send_eagain_count = 108;
        (*o)->stats.ack_eagain_count = 109;
        (*o)->stats.zero_poll_budget_exhaustions = 110;
    }
    return 0;
}
int ts_output_submit(ts_output *o, const void *data, size_t n, uint64_t *id) {
    assert(data);
    if (o->diagnostic && n < 4096) {
        char text[4096];
        memcpy(text, data, n);
        text[n] = 0;
        if (strstr(text, "synthetic pause failure"))
            stop_cleanup_seen = 1;
        if (strstr(text, "synthetic shutdown failure"))
            shutdown_cleanup_seen = 1;
        if (strstr(text, "\"phase\":3") && strstr(text, "\"usb_late_actual_bytes\":188"))
            final_usb_counters_seen = 1;
        if (strstr(text, "\"phase\":3") && strstr(text, "\"have_pat\":1") &&
            strstr(text, "\"have_cat\":1") && strstr(text, "\"program_count\":1") &&
            strstr(text, "\"missing_pmts\":0") && strstr(text, "\"tables_ready\":1") &&
            strstr(text, "\"invalid_sections\":0") &&
            strstr(text, "\"continuity_errors\":0") &&
            strstr(text, "\"unsupported_tables\":0"))
            final_table_counters_seen = 1;
        if (strstr(text, "\"model\":\"DT295\"")) {
            static const char *const keys[] = {"\"have_pat\":", "\"have_cat\":",
                                               "\"program_count\":", "\"missing_pmts\":",
                                               "\"tables_ready\":", "\"invalid_sections\":",
                                               "\"continuity_errors\":", "\"unsupported_tables\":",
                                               "\"writer_write_ns_total\":", "\"writer_write_ns_max\":",
                                               "\"writer_write_records\":", "\"max_pending_age_ns\":",
                                               "\"oldest_pending_age_ns\":", "\"max_parent_poll_gap_ns\":",
                                               "\"max_parent_ack_gap_ns\":", "\"send_eagain_count\":",
                                               "\"ack_eagain_count\":", "\"zero_poll_budget_exhaustions\":"};
            route_status_count++;
            if (strstr(text, "\"writer_write_ns_total\":101") &&
                strstr(text, "\"writer_write_ns_max\":102") &&
                strstr(text, "\"writer_write_records\":103") &&
                strstr(text, "\"max_pending_age_ns\":104") &&
                strstr(text, "\"oldest_pending_age_ns\":105") &&
                strstr(text, "\"max_parent_poll_gap_ns\":106") &&
                strstr(text, "\"max_parent_ack_gap_ns\":107") &&
                strstr(text, "\"send_eagain_count\":108") &&
                strstr(text, "\"ack_eagain_count\":109") &&
                strstr(text, "\"zero_poll_budget_exhaustions\":110"))
                telemetry_values_seen = 1;
            for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
                if (!strstr(text, keys[i]))
                    route_status_missing_counter = 1;
        }
    }
    if (id)
        *id = 1;
    o->stats.submitted_bytes += n;
    if (!o->diagnostic && fail_recording_output) {
        o->stats.rejected_bytes += n;
        return EAGAIN;
    }
    o->stats.confirmed_bytes += n;
    if (!o->diagnostic)
        saved += n;
    return 0;
}
int ts_output_poll(ts_output *o, unsigned ms) {
    (void)o;
    (void)ms;
    return 0;
}
void ts_output_snapshot(ts_output *o, ts_output_stats *v) {
    *v = o->stats;
}
int ts_output_finish(ts_output *o, unsigned ms) {
    (void)o;
    (void)ms;
    return 0;
}
int ts_output_destroy(ts_output *o) {
    free(o);
    return 0;
}
/* These are present solely to prove a test cannot reach physical I/O. */
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *h, uint8_t a, uint8_t b, uint16_t c,
                                        uint16_t d, unsigned char *p, uint16_t n, unsigned ms) {
    (void)h;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)p;
    (void)n;
    (void)ms;
    assert(!"physical USB control reached");
    return -1;
}
int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *p,
                                     int n, int *got, unsigned ms) {
    (void)h;
    (void)ep;
    (void)p;
    (void)n;
    (void)got;
    (void)ms;
    assert(!"physical USB bulk reached");
    return -1;
}
int LIBUSB_CALL libusb_interrupt_transfer(libusb_device_handle *h, unsigned char ep,
                                          unsigned char *p, int n, int *got, unsigned ms) {
    (void)h;
    (void)ep;
    (void)p;
    (void)n;
    (void)got;
    (void)ms;
    assert(!"physical USB interrupt reached");
    return -1;
}
static void reset(void) {
    fake_time = 1000;
    opens = boots = prepares = reprepares = stops = cancels = closed = shutdowns = 0;
    fail_assets = fail_recording_output = fail_final_stop = fail_shutdown = fail_start =
        output_count = 0;
    saved = 0;
    fragment_boundary = 188;
    final_usb_counters_seen = 0;
    final_table_counters_seen = 0;
    route_status_count = 0;
    route_status_missing_counter = 0;
    stop_cleanup_seen = shutdown_cleanup_seen = telemetry_values_seen = 0;
    memset(continuity, 0, sizeof(continuity));
}
int main(void) {
    uint8_t mask[8192] = {0};
    volatile sig_atomic_t stop = 0;
    dt295_capture_config c = {"/synthetic", "-", NULL, {0}, 27, 1, -1, 0, 0, mask, 0, &stop};
    reset();
    assert(!dt295_capture_run(&c));
    assert(opens == 1 && prepares == 1 && reprepares == 1 && stops == 2 && cancels == 2 &&
           shutdowns == 1 && closed == 1);
    assert(saved > 0 && saved % 188 == 0);
    assert(final_usb_counters_seen);
    assert(final_table_counters_seen);
    assert(route_status_count >= 2 && !route_status_missing_counter);
    assert(telemetry_values_seen);
    for (unsigned split = 1; split < 188; split++) {
        reset();
        fragment_boundary = split;
        assert(!dt295_capture_run(&c));
        assert(opens == 1 && prepares == 1 && reprepares == 1 && stops == 2 && cancels == 2 &&
               shutdowns == 1 && closed == 1);
        assert(saved == 3 * 188 && saved % 188 == 0);
        assert(final_usb_counters_seen);
        assert(final_table_counters_seen);
        assert(route_status_count >= 2 && !route_status_missing_counter);
        assert(telemetry_values_seen);
    }
    reset();
    fail_assets = 1;
    assert(dt295_capture_run(&c) == 10 && opens == 0 && boots == 0);
    reset();
    fail_recording_output = 1;
    assert(dt295_capture_run(&c) == 8 && stops == 2 && cancels == 2 && shutdowns == 1 &&
           closed == 1);
    reset();
    fail_start = 1;
    assert(dt295_capture_run(&c) == 7 && stops == 1 && shutdowns == 1 && closed == 1);
    reset();
    fail_final_stop = 1;
    assert(dt295_capture_run(&c) == 7 && cancels == 2 && closed == 1);
    assert(stop_cleanup_seen && !shutdown_cleanup_seen);
    reset();
    fail_final_stop = fail_shutdown = 1;
    assert(dt295_capture_run(&c) == 7 && stops == 2 && shutdowns == 1 && closed == 1);
    assert(stop_cleanup_seen && shutdown_cleanup_seen);
    reset();
    fail_recording_output = fail_final_stop = fail_shutdown = 1;
    assert(dt295_capture_run(&c) == 8 && stops == 2 && shutdowns == 1 && closed == 1);
    assert(stop_cleanup_seen && shutdown_cleanup_seen);
    puts("DT295 capture orchestration: bootstrap/rearm, validation-before-USB, full queue and "
         "cleanup regressions passed");
    return 0;
}

#endif
