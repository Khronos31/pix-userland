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
    size_t pid_gate_offset;
    uint8_t fixture[188 * 16];
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
static uint8_t output_packets[188 * 32];
static size_t output_packet_bytes;
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
    FIXTURE_PRIMARY_AUDIO_ABSENCE,
    FIXTURE_MASK_POLICY
};
static int fixture_mode, route_write_count, route_write_pid273_count, route_write_pid274_count;
static int route_write_pid258_count, route_write_pid902_count;
static int fail_route_write, fail_route_pid, pid258_output_count, pid273_output_count;
extern void dt295_common_test_fixture_finished(void);
extern void dt295_common_test_pid_filter_read_gate(void);
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
    dt295_common_test_fixture_finished();
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
    } else if (fixture_mode == FIXTURE_MASK_POLICY) {
        (*s)->pid_gate_offset = count * 188;
        add_data(*s, &count, 0x111);
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
    if (fixture_mode == FIXTURE_MASK_POLICY && s->generation &&
        s->offset == s->pid_gate_offset)
        dt295_common_test_pid_filter_read_gate();
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
        assert(n <= sizeof(output_packets) - output_packet_bytes);
        memcpy(output_packets + output_packet_bytes, data, n);
        output_packet_bytes += n;
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
static uint8_t sink_packets[188 * 32];
static size_t sink_packet_bytes;
static unsigned reached_cases;
static unsigned host_reached_cases;
static void record_case(const char *name) {
    reached_cases++;
    fprintf(stderr, "DT295 case=%s reached=1\n", name);
}
static void record_host_case(const char *name) {
    host_reached_cases++;
    fprintf(stderr, "DT295 host_case=%s reached=1\n", name);
}
extern int dt295_common_test_begin(const void *, int, size_t);
extern int dt295_common_test_wait_packets(uint64_t);
extern int dt295_common_test_stop_held(const void *);
extern int dt295_common_test_stop(void);
extern int dt295_common_test_drain_active(uint8_t *, size_t);
extern int dt295_common_test_old_read_start(void);
extern int dt295_common_test_old_read_woke(void);
extern int dt295_common_test_old_read_resume(void);
extern int dt295_common_test_save_final(void);
extern int dt295_common_test_release_final(void);
extern int dt295_common_test_stop_with_join_retry(void);
extern int dt295_common_test_finish_slow(uint8_t *, size_t);
extern int dt295_common_test_start_compensation(const void *);
extern size_t dt295_common_test_finish(uint8_t *, size_t);
extern int dt295_common_test_null_mask_rejected(const void *);
extern int dt295_common_test_native_stop_failure(const void *);
extern int dt295_common_test_lifetime_begin(const void *);
extern int dt295_common_test_lifetime_retain(const void *, int);
extern int dt295_common_test_lifetime_release(uint64_t);
extern int dt295_common_test_host_case(const void *, int, int, unsigned, uint64_t, unsigned,
                                       uint8_t *, size_t, size_t *);
extern int dt295_common_test_pid_mask_prepare(const void *);
extern int dt295_common_test_pid_mask_release(uint8_t *, size_t);
extern int dt295_common_test_destructor_begin(const void *);
extern int dt295_common_test_destructor_finish(uint64_t);
extern void dt295_common_test_arm_native_stop_failure(unsigned);
extern unsigned dt295_common_test_disarm_native_stop_failure(void);
extern int dt295_common_test_overflow_native_stop_secondary(const void *);
static void clear_run_measurements(void);
static void run_host_shadow(const char *name, const dt295_capture_config *config,
                            int expected_started, int expected_success,
                            unsigned expected_terminal, uint64_t expected_bytes,
                            unsigned native_stop_target, const uint8_t *expected_data) {
    uint8_t expected_copy[188 * 32];
    assert(expected_bytes <= sizeof(expected_copy));
    if (expected_bytes) {
        assert(expected_data);
        memcpy(expected_copy, expected_data, expected_bytes);
    }
    uint8_t actual[188 * 32];
    size_t actual_bytes = 0;
    clear_run_measurements();
    int result = dt295_common_test_host_case(
        config, expected_started, expected_success, expected_terminal, expected_bytes,
        native_stop_target, actual, sizeof(actual), &actual_bytes);
    assert(result == 0);
    assert(actual_bytes == expected_bytes);
    if (expected_bytes)
        assert(memcmp(actual, expected_copy, expected_bytes) == 0);
    record_host_case(name);
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
static void clear_run_measurements(void) {
    fake_time = 1000;
    opens = boots = prepares = reprepares = stops = cancels = closed = shutdowns = 0;
    output_count = 0;
    saved = 0;
    output_packet_bytes = 0;
    final_usb_counters_seen = 0;
    final_table_counters_seen = 0;
    route_status_count = 0;
    route_status_missing_counter = 0;
    stop_cleanup_seen = shutdown_cleanup_seen = telemetry_values_seen = 0;
    live_add_event_count = live_add_summary_count = 0;
    live_add_summary_value = live_add_after_packets = 0;
    live_add_event_fields_valid = 0;
    route_write_count = route_write_pid273_count = route_write_pid274_count = 0;
    route_write_pid258_count = route_write_pid902_count = 0;
    pid258_output_count = pid273_output_count = 0;
    memset(continuity, 0, sizeof(continuity));
}
static void reset(void) {
    clear_run_measurements();
    fail_assets = fail_recording_output = fail_final_stop = fail_shutdown = fail_start = 0;
    fragment_boundary = 188;
    fail_live_add_event = 0;
    fixture_mode = FIXTURE_BASIC;
    fail_route_write = fail_route_pid = 0;
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
    record_case("standalone_baseline");
    uint8_t baseline_packets[sizeof(output_packets)];
    size_t baseline_packet_bytes = output_packet_bytes;
    uint8_t route_failure_packets[sizeof(output_packets)];
    size_t route_failure_packet_bytes = 0U;
    assert(baseline_packet_bytes == saved && baseline_packet_bytes > 0);
    memcpy(baseline_packets, output_packets, baseline_packet_bytes);
    reset();
    assert(dt295_common_test_begin(&c, 1, 32U) == 0);
    assert(dt295_common_test_stop_held(&c) == 0);
    sink_packet_bytes = dt295_common_test_finish(sink_packets, sizeof(sink_packets));
    assert(sink_packet_bytes == 188U);
    assert(memcmp(sink_packets, baseline_packets, sink_packet_bytes) == 0);
    assert(opens == 1 && prepares == 1 && reprepares == 1 && stops == 2 &&
           cancels == 2 && shutdowns == 1 && closed == 1);
    record_case("held_sink_stop_wait_and_replacement_reject");
    reset();
    assert(dt295_common_test_begin(&c, 0, 32U) == 0);
    int c_wait = dt295_common_test_wait_packets(3U);
    if (c_wait != 0)
        (void)dt295_common_test_stop();
    assert(c_wait == 0);
    assert(dt295_common_test_drain_active(sink_packets, sizeof(sink_packets)) ==
           (int)baseline_packet_bytes);
    assert(memcmp(sink_packets, baseline_packets, baseline_packet_bytes) == 0);
    int old_read_started = dt295_common_test_old_read_start();
    int old_a_stop = old_read_started == 0 ? dt295_common_test_stop() : -1;
    if (old_read_started == 0 && old_a_stop != 0)
        (void)dt295_common_test_stop();
    int old_a_woke = old_read_started == 0 ? dt295_common_test_old_read_woke() : 0;
    int old_a_saved = old_read_started == 0 ? dt295_common_test_save_final() : -1;
    int old_a_released = old_a_saved == 0 ? dt295_common_test_release_final() : -1;
    reset();
    int b_started = dt295_common_test_begin(&c, 0, 32U);
    int b_wait = b_started == 0 ? dt295_common_test_wait_packets(3U) : -1;
    int b_stop = b_started == 0 ? dt295_common_test_stop() : -1;
    int b_saved = b_stop == 0 ? dt295_common_test_save_final() : -1;
    int old_read_resumed = dt295_common_test_old_read_resume();
    sink_packet_bytes = b_saved == 0
        ? dt295_common_test_finish(sink_packets, sizeof(sink_packets))
        : 0U;
    assert(old_read_started == 0 && old_a_stop == 0 && old_a_woke == 1 &&
           old_a_saved == 0 && old_a_released == 0);
    assert(b_started == 0 && b_wait == 0 && b_stop == 0 && b_saved == 0 &&
           old_read_resumed == 0);
    assert(sink_packet_bytes == baseline_packet_bytes);
    assert(memcmp(sink_packets, baseline_packets, baseline_packet_bytes) == 0);
    record_case("blocked_old_read_wake_stale_ticket");
    record_case("same_identity_replacement_bytes_unchanged");
    reset();
    assert(dt295_common_test_begin(&c, 0, 1U) == 0);
    int slow_wait = dt295_common_test_wait_packets(2U);
    int slow_stop = dt295_common_test_stop_with_join_retry();
    assert(slow_wait == 0 && slow_stop == 0);
    sink_packet_bytes = dt295_common_test_finish_slow(sink_packets, sizeof(sink_packets));
    assert(sink_packet_bytes == 188U);
    record_case("slow_consumer_terminal_join_retry");
    reset();
    assert(dt295_common_test_start_compensation(&c) == 0);
    int compensation_wait = dt295_common_test_wait_packets(3U);
    int compensation_stop = dt295_common_test_stop();
    assert(compensation_wait == 0 && compensation_stop == 0);
    sink_packet_bytes = dt295_common_test_finish(sink_packets, sizeof(sink_packets));
    assert(sink_packet_bytes == baseline_packet_bytes);
    assert(memcmp(sink_packets, baseline_packets, baseline_packet_bytes) == 0);
    record_case("host_allocation_and_worker_create_compensation");
    reset();
    assert(dt295_common_test_null_mask_rejected(&c) == 0);
    record_case("null_pid_mask_rejected");
    reset();
    assert(dt295_common_test_native_stop_failure(&c) == 0);
    record_case("native_stop_first_error_and_fatal_final");
    reset();
    assert(dt295_common_test_lifetime_begin(&c) == 0);
    assert(dt295_common_test_lifetime_retain(&c, 0) == 0);
    record_case("destructor_retains_callback_owner_graph");
    assert(dt295_common_test_lifetime_release(188U) == 0);
    record_case("retained_owner_retry_join_and_single_release");
    reset();
    assert(dt295_common_test_lifetime_begin(&c) == 0);
    const int retained_failure_cleanup = dt295_common_test_lifetime_retain(&c, 1);
    assert(retained_failure_cleanup == -1);
    fprintf(stderr, "DT295 case=retained_owner_failure_path_cleanup owner_cleaned=1\n");
    reset();
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
        char case_name[64];
        snprintf(case_name, sizeof(case_name), "fragment_boundary_%u", split);
        run_host_shadow(case_name, &c, 1, 1, 5U, output_packet_bytes, 0U,
                        output_packets);
        record_case(case_name);
    }
    reset();
    fail_assets = 1;
    assert(dt295_capture_run(&c) == 10 && opens == 0 && boots == 0);
    run_host_shadow("asset_load_failure", &c, 0, 0, 0U, 0U, 0U, NULL);
    record_case("asset_load_failure");
    fail_assets = 0;
    run_host_shadow("asset_failure_start_recovery", &c, 1, 1, 5U,
                    baseline_packet_bytes, 0U, baseline_packets);
    reset();
    fail_recording_output = 1;
    assert(dt295_capture_run(&c) == 8 && stops == 2 && cancels == 2 && shutdowns == 1 &&
           closed == 1);
    run_host_shadow("recording_output_failure_mapped_to_common_sink", &c, 1, 1, 5U,
                    baseline_packet_bytes, 0U, baseline_packets);
    record_case("recording_output_failure");
    reset();
    fail_start = 1;
    assert(dt295_capture_run(&c) == 7 && stops == 1 && shutdowns == 1 && closed == 1);
    run_host_shadow("start_failure", &c, 0, 0, 0U, 0U, 0U, NULL);
    record_case("start_failure");
    fail_start = 0;
    run_host_shadow("worker_start_failure_recovery", &c, 1, 1, 5U,
                    baseline_packet_bytes, 0U, baseline_packets);
    reset();
    fail_final_stop = 1;
    assert(dt295_capture_run(&c) == 7 && cancels == 2 && closed == 1);
    assert(stop_cleanup_seen && !shutdown_cleanup_seen);
    run_host_shadow("final_hardware_stop_failure", &c, 1, 0, 4U,
                    output_packet_bytes, 0U, output_packets);
    record_case("final_hardware_stop_failure");
    reset();
    fail_final_stop = fail_shutdown = 1;
    assert(dt295_capture_run(&c) == 7 && stops == 2 && shutdowns == 1 && closed == 1);
    assert(stop_cleanup_seen && shutdown_cleanup_seen);
    run_host_shadow("final_stop_and_shutdown_failure", &c, 1, 0, 4U,
                    output_packet_bytes, 0U, output_packets);
    record_case("final_stop_and_shutdown_failure");
    reset();
    fail_recording_output = fail_final_stop = fail_shutdown = 1;
    assert(dt295_capture_run(&c) == 8 && stops == 2 && shutdowns == 1 && closed == 1);
    assert(stop_cleanup_seen && shutdown_cleanup_seen);
    run_host_shadow("output_and_cleanup_failure", &c, 1, 0, 4U,
                    baseline_packet_bytes, 0U, baseline_packets);
    record_case("output_and_cleanup_failure_precedence");
    reset();
    fixture_mode = FIXTURE_SHARED_ADD;
    assert(!dt295_capture_run(&c));
    assert(route_write_pid273_count == 1 && pid273_output_count == 3);
    assert(live_add_event_count == 0 && live_add_summary_count == 0);
    assert(saved == 9 * 188 && saved % 188 == 0);
    assert(stops == 2 && cancels == 2 && shutdowns == 1 && closed == 1);
    run_host_shadow("shared_pid_addition", &c, 1, 1, 5U,
                    output_packet_bytes, 0U, output_packets);
    record_case("shared_pid_addition");
    reset();
    fixture_mode = FIXTURE_SHARED_ADD;
    assert(!run_with_diagnostic(&c));
    assert(route_write_pid273_count == 1 && live_add_event_count == 1 &&
           live_add_event_fields_valid && live_add_after_packets > 0);
    assert(live_add_summary_count == 1 && live_add_summary_value == 1);
    assert(pid273_output_count == 3 && saved == 9 * 188);
    run_host_shadow("shared_pid_addition_diagnostic", &c, 1, 1, 5U,
                    output_packet_bytes, 0U, output_packets);
    record_case("shared_pid_addition_diagnostic");
    reset();
    fixture_mode = FIXTURE_PRIMARY_AUDIO_ABSENCE;
    assert(!dt295_capture_run(&c));
    assert(pid258_output_count == 2 && route_write_pid258_count == 0);
    assert(live_add_event_count == 0 && live_add_summary_count == 0);
    assert(saved == 4 * 188 && saved % 188 == 0);
    assert(stops == 2 && cancels == 2 && shutdowns == 1 && closed == 1);
    run_host_shadow("primary_audio_absence", &c, 1, 1, 5U,
                    output_packet_bytes, 0U, output_packets);
    record_case("primary_audio_absence");
    reset();
    fixture_mode = FIXTURE_PRIMARY_AUDIO_ABSENCE;
    assert(!run_with_diagnostic(&c));
    assert(pid258_output_count == 2 && route_write_pid258_count == 0);
    assert(live_add_event_count == 0 && live_add_summary_count == 1 &&
           live_add_summary_value == 0);
    run_host_shadow("primary_audio_absence_diagnostic", &c, 1, 1, 5U,
                    output_packet_bytes, 0U, output_packets);
    record_case("primary_audio_absence_diagnostic");
    reset();
    fixture_mode = FIXTURE_NEW_ECM;
    assert(dt295_capture_run(&c) == 8 && route_write_pid273_count == 0 &&
           route_write_pid902_count == 0 && pid273_output_count == 0);
    assert(live_add_event_count == 0 && live_add_summary_count == 0);
    run_host_shadow("new_ecm_refusal", &c, 1, 0, 4U,
                    output_packet_bytes, 0U, output_packets);
    record_case("new_ecm_refusal");
    reset();
    fixture_mode = FIXTURE_NEW_ECM;
    assert(run_with_diagnostic(&c) == 8 && live_add_event_count == 0 &&
           live_add_summary_count == 1 && live_add_summary_value == 0);
    run_host_shadow("new_ecm_refusal_diagnostic", &c, 1, 0, 4U,
                    output_packet_bytes, 0U, output_packets);
    record_case("new_ecm_refusal_diagnostic");
    reset();
    fixture_mode = FIXTURE_REBIND;
    assert(dt295_capture_run(&c) == 8 && route_write_pid273_count == 0 &&
           route_write_pid902_count == 0);
    assert(live_add_event_count == 0 && live_add_summary_count == 0);
    run_host_shadow("rebind_refusal", &c, 1, 0, 4U,
                    output_packet_bytes, 0U, output_packets);
    record_case("rebind_refusal");
    reset();
    fixture_mode = FIXTURE_REBIND;
    assert(run_with_diagnostic(&c) == 8 && live_add_event_count == 0 &&
           live_add_summary_count == 1 && live_add_summary_value == 0);
    run_host_shadow("rebind_refusal_diagnostic", &c, 1, 0, 4U,
                    output_packet_bytes, 0U, output_packets);
    record_case("rebind_refusal_diagnostic");
    reset();
    fixture_mode = FIXTURE_WRITE_FAIL;
    fail_route_write = 1;
    fail_route_pid = 0x111;
    assert(dt295_capture_run(&c) == 8 && route_write_pid273_count == 1 &&
           pid273_output_count == 0);
    assert(stops == 2 && cancels == 2 && shutdowns == 1 && closed == 1);
    assert(live_add_event_count == 0 && live_add_summary_count == 0);
    route_failure_packet_bytes = output_packet_bytes;
    memcpy(route_failure_packets, output_packets, route_failure_packet_bytes);
    run_host_shadow("route_write_failure", &c, 1, 0, 4U,
                    route_failure_packet_bytes, 0U, route_failure_packets);
    record_case("route_write_failure");
    reset();
    fixture_mode = FIXTURE_WRITE_FAIL;
    fail_route_write = 1;
    fail_route_pid = 0x111;
    assert(run_with_diagnostic(&c) == 8 && live_add_event_count == 0 &&
           live_add_summary_count == 1 && live_add_summary_value == 0);
    run_host_shadow("route_write_failure_diagnostic", &c, 1, 0, 4U,
                    route_failure_packet_bytes, 0U, route_failure_packets);
    record_case("route_write_failure_diagnostic");
    reset();
    fixture_mode = FIXTURE_SHARED_ADD;
    fail_live_add_event = 1;
    assert(run_with_diagnostic(&c) == 8 && route_write_pid273_count == 1 &&
           live_add_event_count == 0 && live_add_summary_count == 1 &&
           live_add_summary_value == 1);
    fail_live_add_event = 0;
    clear_run_measurements();
    assert(dt295_capture_run(&c) == 0 && saved == 9U * 188U);
    run_host_shadow("live_add_diagnostic_failure_sink_path", &c, 1, 1, 5U,
                    output_packet_bytes, 0U, output_packets);
    record_case("live_add_diagnostic_failure");
    reset();
    test_apply_plan_does_not_commit_failed_write();
    fixture_mode = FIXTURE_WRITE_FAIL;
    fail_route_write = 1;
    fail_route_pid = 0x111;
    run_host_shadow("failed_route_write_plan_not_committed_host_cleanup", &c, 1, 0,
                    4U, route_failure_packet_bytes, 0U, route_failure_packets);
    record_case("failed_route_write_plan_not_committed");
    fprintf(stderr, "DT295 host_case_coverage expected=207 reached=%u\n",
            host_reached_cases);
    assert(host_reached_cases == 207U);

    reset();
    dt295_common_test_arm_native_stop_failure(1U);
    const int standalone_stop_primary = dt295_capture_run(&c);
    const unsigned standalone_stop_attempts =
        dt295_common_test_disarm_native_stop_failure();
    fprintf(stderr,
            "DT295 standalone_native_stop_observed returned=%d attempts=%u cancels=%d "
            "closed=%d shutdowns=%d\n",
            standalone_stop_primary, standalone_stop_attempts, cancels, closed, shutdowns);
    assert(standalone_stop_primary == 7 && standalone_stop_attempts == 1U &&
           cancels == 0 && closed == 0 && shutdowns == 0);
    fprintf(stderr,
            "DT295 standalone_native_stop primary=7 returned=%d attempts=%u "
            "unquiesced_stream_retained=1 cleanup_closed=%d\n",
            standalone_stop_primary, standalone_stop_attempts, closed);

    reset();
    fixture_mode = FIXTURE_WRITE_FAIL;
    fail_route_write = 1;
    fail_route_pid = 0x111;
    dt295_common_test_arm_native_stop_failure(2U);
    const int standalone_primary_with_stop_failure = dt295_capture_run(&c);
    const unsigned standalone_secondary_attempts =
        dt295_common_test_disarm_native_stop_failure();
    assert(standalone_primary_with_stop_failure == 8 &&
           standalone_secondary_attempts == 2U && stops == 2 && shutdowns == 0 &&
           closed == 0);
    fprintf(stderr,
            "DT295 standalone_secondary_stop primary=8 returned=%d attempts=%u "
            "route_writes=%d unquiesced_stream_retained=1 cleanup_closed=%d\n",
            standalone_primary_with_stop_failure, standalone_secondary_attempts,
            route_write_count, closed);

    reset();
    fixture_mode = FIXTURE_WRITE_FAIL;
    fail_route_write = 1;
    fail_route_pid = 0x111;
    run_host_shadow("primary_route_error_with_native_cleanup_error", &c, 1, 0,
                    4U, route_failure_packet_bytes, 2U, route_failure_packets);

    reset();
    assert(dt295_common_test_overflow_native_stop_secondary(&c) == 0);

    reset();
    fixture_mode = FIXTURE_MASK_POLICY;
    clear_run_measurements();
    const int mask_prepared = dt295_common_test_pid_mask_prepare(&c);
    uint8_t mask_output[188];
    const int mask_released = dt295_common_test_pid_mask_release(mask_output,
                                                                  sizeof(mask_output));
    assert(mask_prepared == 0 && mask_released == 0);

    reset();
    const int destructor_begin = dt295_common_test_destructor_begin(&c);
    const int destructor_finished =
        dt295_common_test_destructor_finish(188U);
    fprintf(stderr, "DT295 destructor_result begin=%d finish=%d expected_bytes=%zu\n",
            destructor_begin, destructor_finished, 188U);
    assert(destructor_begin == 0 && destructor_finished == 0);

    fprintf(stderr, "DT295 case_coverage expected=215 reached=%u\n", reached_cases);
    assert(reached_cases == 215U);
    puts("DT295 sink proof: consumer lifetime, first-error, all fragment/route/failure cases passed");
    return 0;
}

#endif
