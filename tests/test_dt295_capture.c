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
    size_t fixture_length;
    unsigned split_boundary;
    uint8_t fixture[188 * 16];
};
struct ts_output {
    ts_output_stats stats;
    int diagnostic;
};
static uint64_t fake_time;
static int opens, boots, prepares, reprepares, stops, cancels, closed, shutdowns;
static int satellite_tunes, satellite_slots;
static int fail_assets, fail_recording_output, fail_final_stop, fail_shutdown, fail_start,
    output_count;
static uint8_t continuity[8192];
static uint64_t saved;
static int final_usb_counters_seen;
static int final_table_counters_seen;
static unsigned route_status_count;
static int route_status_missing_counter;
static int stop_cleanup_seen, shutdown_cleanup_seen, telemetry_values_seen;
static unsigned live_add_event_count, live_add_summary_count;
static uint64_t live_add_summary_value, live_add_after_packets;
static int live_add_event_fields_valid, fail_live_add_event;
static unsigned diagnostic_run_id;
static unsigned fragment_boundary = 188;
enum {
    FIXTURE_BASIC,
    FIXTURE_SHARED_ADD,
    FIXTURE_NEW_ECM,
    FIXTURE_REBIND,
    FIXTURE_WRITE_FAIL,
    FIXTURE_PRIMARY_AUDIO_ABSENCE
};
static int fixture_mode, route_write_count, route_write_pid273_count, route_write_pid274_count;
static int route_write_pid258_count, route_write_pid902_count;
static int fail_route_write, fail_route_pid, pid258_output_count, pid273_output_count;
uint64_t capture_test_now(void) {
    return fake_time;
}
static void table_version(uint8_t p[188], unsigned pid, unsigned tid, unsigned ext, unsigned version,
                         const uint8_t *payload, size_t bytes) {
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
    s[3] = (uint8_t)(ext >> 8);
    s[4] = (uint8_t)ext;
    s[5] = (uint8_t)(0xc1 | ((version & 31u) << 1));
    s[6] = s[7] = 0;
    if (bytes)
        memcpy(s + 8, payload, bytes);
    uint32_t crc = ts_crc(s, n - 4);
    for (unsigned i = 0; i < 4; i++)
        s[n - 4 + i] = (uint8_t)(crc >> (24 - 8 * i));
}
static size_t capture_pmt(uint8_t out[64], unsigned program,
                          uint16_t program_ecm, int include_extra, uint16_t extra_ecm) {
    size_t n = 0;
    uint16_t video = (uint16_t)(0x101 + (program - 1) * 2), audio = (uint16_t)(video + 1);
    out[n++] = (uint8_t)(0xe0 | (video >> 8));
    out[n++] = (uint8_t)video;
    out[n++] = 0xf0;
    out[n++] = program_ecm ? 6 : 0;
    if (program_ecm) {
        out[n++] = 0x09; out[n++] = 4; out[n++] = 0; out[n++] = 5;
        out[n++] = (uint8_t)(0xe0 | (program_ecm >> 8)); out[n++] = (uint8_t)program_ecm;
    }
    out[n++] = 0x1b; out[n++] = (uint8_t)(0xe0 | (video >> 8)); out[n++] = (uint8_t)video;
    out[n++] = 0xf0; out[n++] = 0;
    out[n++] = 0x0f; out[n++] = (uint8_t)(0xe0 | (audio >> 8)); out[n++] = (uint8_t)audio;
    out[n++] = 0xf0; out[n++] = 0;
    if (include_extra) {
        out[n++] = 0x0f; out[n++] = 0xe1; out[n++] = 0x11;
        out[n++] = 0xf0;
        out[n++] = (uint8_t)(extra_ecm ? 6 : 0);
        if (extra_ecm) {
            out[n++] = 0x09; out[n++] = 4; out[n++] = 0; out[n++] = 5;
            out[n++] = (uint8_t)(0xe0 | (extra_ecm >> 8)); out[n++] = (uint8_t)extra_ecm;
        }
    }
    return n;
}
static size_t capture_pmt_video_only(uint8_t out[64], unsigned program, uint16_t program_ecm) {
    size_t n = 0;
    uint16_t video = (uint16_t)(0x101 + (program - 1) * 2);
    out[n++] = (uint8_t)(0xe0 | (video >> 8));
    out[n++] = (uint8_t)video;
    out[n++] = 0xf0;
    out[n++] = program_ecm ? 6 : 0;
    if (program_ecm) {
        out[n++] = 0x09; out[n++] = 4; out[n++] = 0; out[n++] = 5;
        out[n++] = (uint8_t)(0xe0 | (program_ecm >> 8)); out[n++] = (uint8_t)program_ecm;
    }
    out[n++] = 0x1b; out[n++] = (uint8_t)(0xe0 | (video >> 8)); out[n++] = (uint8_t)video;
    out[n++] = 0xf0; out[n++] = 0;
    return n;
}
static void add_table(dt295_stream *s, size_t *count, unsigned pid, unsigned tid,
                      unsigned ext, unsigned version, const uint8_t *payload, size_t bytes) {
    assert(*count < sizeof(s->fixture) / 188);
    table_version(s->fixture + (*count)++ * 188, pid, tid, ext, version, payload, bytes);
}
static void add_data(dt295_stream *s, size_t *count, unsigned pid) {
    assert(*count < sizeof(s->fixture) / 188);
    uint8_t *p = s->fixture + (*count)++ * 188;
    memset(p, 0xff, 188);
    p[0] = 0x47;
    p[1] = (uint8_t)(pid >> 8);
    p[2] = (uint8_t)pid;
    p[3] = (uint8_t)(0x10 | (continuity[pid]++ & 15));
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
int dt295_tune_satellite(dt295 *d, uint32_t hz, int tsid, char *e, size_t n) {
    (void)d;
    (void)e;
    (void)n;
    assert(hz == 1049480000u && (tsid == 16400 || tsid == -1));
    ++satellite_tunes;
    return 0;
}
int dt295_select_satellite_slot(dt295 *d, unsigned slot, char *e, size_t n) {
    (void)d;
    (void)e;
    (void)n;
    assert(slot == 0);
    ++satellite_slots;
    return 0;
}
int dt295_satellite_status(dt295 *d, dt295_satellite_state *st, char *e, size_t n) {
    (void)d;
    (void)e;
    (void)n;
    st->selected_tsid = 1;
    st->locked = 1;
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
    assert(d->state == 2 && op->enabled);
    route_write_count++;
    route_write_pid273_count += op->pid == 0x111;
    route_write_pid274_count += op->pid == 0x112;
    route_write_pid258_count += op->pid == 0x102;
    route_write_pid902_count += op->pid == 0x902;
    if (fail_route_write && (!fail_route_pid || op->pid == fail_route_pid)) {
        snprintf(e, n, "synthetic route write failure");
        return -1;
    }
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
    size_t count = 0;
    if (!(*s)->generation) {
        uint8_t pat[8] = {0, 1, 0xe1, 0, 0, 2, 0xe1, 0x10};
        uint8_t cat[6] = {0x09, 4, 0, 5, 0xe9, 1};
        uint8_t pmt[64];
        if (fixture_mode == FIXTURE_SHARED_ADD) {
            add_table(*s, &count, 0, 0, 1, 0, pat, sizeof(pat));
            add_table(*s, &count, 1, 1, 1, 0, cat, sizeof(cat));
            size_t n = capture_pmt(pmt, 1, 0x901, 0, 0);
            add_table(*s, &count, 256, 2, 1, 0, pmt, n);
            n = capture_pmt(pmt, 2, 0x901, 0, 0);
            add_table(*s, &count, 272, 2, 2, 0, pmt, n);
        } else {
            uint8_t one_pat[4] = {0, 1, 0xe1, 0};
            add_table(*s, &count, 0, 0, 1, 0, one_pat, sizeof(one_pat));
            add_table(*s, &count, 1, 1, 1, 0, cat, sizeof(cat));
            size_t n = capture_pmt(pmt, 1, 0x901, 0, 0);
            add_table(*s, &count, 256, 2, 1, 0, pmt, n);
        }
    } else if (fixture_mode == FIXTURE_SHARED_ADD || fixture_mode == FIXTURE_WRITE_FAIL) {
        uint8_t pmt[64];
        size_t n = capture_pmt(pmt, 1, 0x901, 1, 0);
        add_table(*s, &count, 256, 2, 1, 1, pmt, n);
        n = capture_pmt(pmt, 2, 0x901, 1, 0);
        add_table(*s, &count, 272, 2, 2, 1, pmt, n);
        add_data(*s, &count, 0x111);
        n = capture_pmt(pmt, 1, 0x901, 0, 0);
        add_table(*s, &count, 256, 2, 1, 2, pmt, n);
        n = capture_pmt(pmt, 2, 0x901, 0, 0);
        add_table(*s, &count, 272, 2, 2, 2, pmt, n);
        add_data(*s, &count, 0x111);
        n = capture_pmt(pmt, 1, 0x901, 1, 0);
        add_table(*s, &count, 256, 2, 1, 3, pmt, n);
        n = capture_pmt(pmt, 2, 0x901, 1, 0);
        add_table(*s, &count, 272, 2, 2, 3, pmt, n);
        add_data(*s, &count, 0x111);
        } else if (fixture_mode == FIXTURE_NEW_ECM) {
        uint8_t pmt[64];
        size_t n = capture_pmt(pmt, 1, 0x901, 1, 0x902);
        add_table(*s, &count, 256, 2, 1, 1, pmt, n);
        add_data(*s, &count, 0x111);
    } else if (fixture_mode == FIXTURE_REBIND) {
        uint8_t pmt[64];
        size_t n = capture_pmt(pmt, 1, 0x902, 0, 0);
        add_table(*s, &count, 256, 2, 1, 1, pmt, n);
        add_data(*s, &count, 0x111);
    } else if (fixture_mode == FIXTURE_PRIMARY_AUDIO_ABSENCE) {
        uint8_t pmt[64];
        size_t n = capture_pmt_video_only(pmt, 1, 0x901);
        add_table(*s, &count, 256, 2, 1, 1, pmt, n);
        add_data(*s, &count, 0x102);
        n = capture_pmt(pmt, 1, 0x901, 0, 0);
        add_table(*s, &count, 256, 2, 1, 2, pmt, n);
        add_data(*s, &count, 0x102);
    } else {
        uint8_t pat[4] = {0, 1, 0xe1, 0};
        uint8_t cat[6] = {0x09, 4, 0, 5, 0xe9, 1};
        uint8_t pmt[64];
        add_table(*s, &count, 0, 0, 1, 0, pat, sizeof(pat));
        add_table(*s, &count, 1, 1, 1, 0, cat, sizeof(cat));
        size_t n = capture_pmt(pmt, 1, 0x901, 0, 0);
        add_table(*s, &count, 256, 2, 1, 0, pmt, n);
    }
    assert(count > 0);
    (*s)->fixture_length = count * 188;
    return 0;
}
int dt295_stream_read(dt295_stream *s, void *buffer, size_t cap, uint64_t *sequence, uint64_t *ns,
                      unsigned ms) {
    (void)ms;
    assert(cap > 0);
    if (s->offset >= s->fixture_length) {
        if (s->generation)
            fake_time += 1000;
        return 0;
    }
    *sequence = s->sequence++;
    *ns = fake_time * 1000000;
    size_t packet_offset = s->offset % 188;
    size_t boundary = packet_offset < s->split_boundary ? s->split_boundary : 188;
    size_t amount = boundary - packet_offset;
    if (amount > s->fixture_length - s->offset)
        amount = s->fixture_length - s->offset;
    if (amount > cap)
        amount = cap;
    memcpy(buffer, s->fixture + s->offset, amount);
    s->offset += amount;
    s->delivered += amount;
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
        if (strstr(text, "\"event\":\"dt295_live_pid_add\"")) {
            if (fail_live_add_event) {
                o->stats.rejected_bytes += n;
                return EAGAIN;
            }
            live_add_event_count++;
            unsigned long long after = 0;
            const char *field = strstr(text, "\"after_packets\":");
            if (field)
                (void)sscanf(field, "\"after_packets\":%llu", &after);
            live_add_after_packets = (uint64_t)after;
            live_add_event_fields_valid = strstr(text, "\"phase\":2") &&
                                          strstr(text, "\"pid\":273") &&
                                          strstr(text, "\"slot\":") &&
                                          strstr(text, "\"ecm_slot\":63") &&
                                          strstr(text, "\"ecm_pid\":2305") &&
                                          strstr(text, "\"previously_present\":0") &&
                                          strstr(text, "\"existing_routes_and_ecms_preserved\":1");
        }
        if (strstr(text, "\"event\":\"dt295_live_add_summary\"")) {
            unsigned long long count = 0;
            const char *field = strstr(text, "\"live_add_count\":");
            if (field)
                (void)sscanf(field, "\"live_add_count\":%llu", &count);
            live_add_summary_count++;
            live_add_summary_value = (uint64_t)count;
        }
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
    if (!o->diagnostic) {
        saved += n;
        const uint8_t *packets = data;
        for (size_t at = 0; at + 188 <= n; at += 188) {
            unsigned pid = ((unsigned)(packets[at + 1] & 31u) << 8) | packets[at + 2];
            if (packets[at] == 0x47 && pid == 0x111)
                pid273_output_count++;
            if (packets[at] == 0x47 && pid == 0x102)
                pid258_output_count++;
        }
    }
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
    satellite_tunes = 0;
    satellite_slots = 0;
    fail_assets = fail_recording_output = fail_final_stop = fail_shutdown = fail_start =
        output_count = 0;
    saved = 0;
    fragment_boundary = 188;
    final_usb_counters_seen = 0;
    final_table_counters_seen = 0;
    route_status_count = 0;
    route_status_missing_counter = 0;
    stop_cleanup_seen = shutdown_cleanup_seen = telemetry_values_seen = 0;
    live_add_event_count = live_add_summary_count = 0;
    live_add_summary_value = live_add_after_packets = 0;
    live_add_event_fields_valid = fail_live_add_event = 0;
    fixture_mode = FIXTURE_BASIC;
    route_write_count = route_write_pid273_count = route_write_pid274_count = 0;
    route_write_pid258_count = route_write_pid902_count = 0;
    fail_route_write = fail_route_pid = pid258_output_count = pid273_output_count = 0;
    memset(continuity, 0, sizeof(continuity));
}
static int run_with_diagnostic(const dt295_capture_config *base) {
    char directory[160];
    int n = snprintf(directory, sizeof(directory), "/tmp/pix-dt295-capture-test-%ld-%u",
                     (long)getpid(), diagnostic_run_id++);
    assert(n > 0 && (size_t)n < sizeof(directory));
    dt295_capture_config config = *base;
    config.diagnostic = directory;
    int rc = dt295_capture_run(&config);
    assert(rmdir(directory) == 0);
    return rc;
}
static void test_apply_plan_does_not_commit_failed_write(void) {
    dt295 *d = calloc(1, sizeof(*d));
    assert(d);
    d->state = 2;
    dt295_route_plan live = {0}, desired, before;
    live.routes[4] = (dt295_route){4, 1, 0, 0, 0, 0};
    live.routes[5] = (dt295_route){5, 1, 0, 0, 0x101, 63};
    live.routes[6] = (dt295_route){6, 1, 0, 0, 0x102, 63};
    live.routes[63] = (dt295_route){63, 1, 1, 0, 0x901, 63};
    live.ecm_pid[63] = 0x901;
    live.route_count = 4;
    live.ready = live.cat_received = live.has_primary = 1;
    live.primary_program = 1;
    live.dedicated_pid[0] = 0x101;
    live.dedicated_pid[1] = 0x102;
    live.dedicated_pid[2] = 0x100;
    live.dedicated_pid[3] = 0x101;
    live.video_stream_type = 0x1b;
    live.audio_stream_type = 0x0f;
    desired = live;
    desired.routes[7] = (dt295_route){7, 1, 0, 0, 0x111, 63};
    desired.routes[8] = (dt295_route){8, 1, 0, 0, 0x112, 63};
    desired.route_count += 2;
    before = live;
    fail_route_write = 1;
    fail_route_pid = 0x112;
    char error[DT295_ROUTE_ERROR] = {0};
    assert(apply_plan(d, &live, &desired, 1, NULL, NULL, error, sizeof(error)) != 0);
    assert(strstr(error, "synthetic route write failure"));
    assert(route_write_count == 2 && route_write_pid273_count == 1 &&
           route_write_pid274_count == 1 && memcmp(&live, &before, sizeof(live)) == 0);
    fail_route_write = 0;
    fail_route_pid = 0;
    route_write_count = route_write_pid273_count = route_write_pid274_count = 0;
    desired = before;
    desired.video_stream_type ^= 1;
    assert(apply_plan(d, &live, &desired, 1, NULL, NULL, error, sizeof(error)) != 0);
    assert(strstr(error, "primary or dedicated PID metadata") && route_write_count == 0 &&
           memcmp(&live, &before, sizeof(live)) == 0);
    free(d);
}
int main(void) {
    uint8_t mask[8192] = {0};
    volatile sig_atomic_t stop = 0;
    dt295_capture_config c = {"/synthetic", "-", NULL, {0}, 27, 1, -1, 0, 0, mask, 0, &stop,
                                  PIXY_TERRESTRIAL, 0, -1, -1};
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
    dt295_capture_config sat = c;
    sat.system = PIXY_BS;
    sat.if_hz = 1049480000u;
    sat.tsid = 16400;
    sat.channel = 1;
    sat.slot = -1;
    assert(!dt295_capture_run(&sat));
    assert(satellite_tunes == 1 && opens == 1 && shutdowns == 1 && closed == 1);
    sat.tsid = -1;
    assert(dt295_capture_run(&sat) == 2 && opens == 1 && satellite_tunes == 1);
    sat.tsid = -1;
    sat.slot = 0;
    assert(!dt295_capture_run(&sat));
    assert(satellite_tunes == 2 && satellite_slots == 1);
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
    reset();
    fixture_mode = FIXTURE_SHARED_ADD;
    assert(!dt295_capture_run(&c));
    assert(route_write_pid273_count == 1 && pid273_output_count == 3);
    assert(live_add_event_count == 0 && live_add_summary_count == 0);
    assert(saved == 9 * 188 && saved % 188 == 0);
    assert(stops == 2 && cancels == 2 && shutdowns == 1 && closed == 1);
    reset();
    fixture_mode = FIXTURE_SHARED_ADD;
    assert(!run_with_diagnostic(&c));
    assert(route_write_pid273_count == 1 && live_add_event_count == 1 &&
           live_add_event_fields_valid && live_add_after_packets > 0);
    assert(live_add_summary_count == 1 && live_add_summary_value == 1);
    assert(pid273_output_count == 3 && saved == 9 * 188);
    reset();
    fixture_mode = FIXTURE_PRIMARY_AUDIO_ABSENCE;
    assert(!dt295_capture_run(&c));
    assert(pid258_output_count == 2 && route_write_pid258_count == 0);
    assert(live_add_event_count == 0 && live_add_summary_count == 0);
    assert(saved == 4 * 188 && saved % 188 == 0);
    assert(stops == 2 && cancels == 2 && shutdowns == 1 && closed == 1);
    reset();
    fixture_mode = FIXTURE_PRIMARY_AUDIO_ABSENCE;
    assert(!run_with_diagnostic(&c));
    assert(pid258_output_count == 2 && route_write_pid258_count == 0);
    assert(live_add_event_count == 0 && live_add_summary_count == 1 &&
           live_add_summary_value == 0);
    reset();
    fixture_mode = FIXTURE_NEW_ECM;
    assert(dt295_capture_run(&c) == 8 && route_write_pid273_count == 0 &&
           route_write_pid902_count == 0 && pid273_output_count == 0);
    assert(live_add_event_count == 0 && live_add_summary_count == 0);
    reset();
    fixture_mode = FIXTURE_NEW_ECM;
    assert(run_with_diagnostic(&c) == 8 && live_add_event_count == 0 &&
           live_add_summary_count == 1 && live_add_summary_value == 0);
    reset();
    fixture_mode = FIXTURE_REBIND;
    assert(dt295_capture_run(&c) == 8 && route_write_pid273_count == 0 &&
           route_write_pid902_count == 0);
    assert(live_add_event_count == 0 && live_add_summary_count == 0);
    reset();
    fixture_mode = FIXTURE_REBIND;
    assert(run_with_diagnostic(&c) == 8 && live_add_event_count == 0 &&
           live_add_summary_count == 1 && live_add_summary_value == 0);
    reset();
    fixture_mode = FIXTURE_WRITE_FAIL;
    fail_route_write = 1;
    fail_route_pid = 0x111;
    assert(dt295_capture_run(&c) == 8 && route_write_pid273_count == 1 &&
           pid273_output_count == 0);
    assert(stops == 2 && cancels == 2 && shutdowns == 1 && closed == 1);
    assert(live_add_event_count == 0 && live_add_summary_count == 0);
    reset();
    fixture_mode = FIXTURE_WRITE_FAIL;
    fail_route_write = 1;
    fail_route_pid = 0x111;
    assert(run_with_diagnostic(&c) == 8 && live_add_event_count == 0 &&
           live_add_summary_count == 1 && live_add_summary_value == 0);
    reset();
    fixture_mode = FIXTURE_SHARED_ADD;
    fail_live_add_event = 1;
    assert(run_with_diagnostic(&c) == 8 && route_write_pid273_count == 1 &&
           live_add_event_count == 0 && live_add_summary_count == 1 &&
           live_add_summary_value == 1);
    reset();
    test_apply_plan_does_not_commit_failed_write();
    puts("DT295 capture orchestration: bootstrap/rearm, validation-before-USB, full queue and "
         "cleanup/live-addition regressions passed");
    return 0;
}

#endif
