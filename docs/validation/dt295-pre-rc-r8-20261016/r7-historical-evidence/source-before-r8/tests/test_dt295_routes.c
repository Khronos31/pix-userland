/* SPDX-License-Identifier: MIT */
#include "dt295_routes.h"
#include "transport.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_SECTION_MAX 1024u

static uint32_t mpeg_crc(const uint8_t *p, size_t n) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= (uint32_t)p[i] << 24;
        for (unsigned b = 0; b < 8; ++b)
            crc = crc & 0x80000000u ? (crc << 1) ^ 0x04c11db7u : crc << 1;
    }
    return crc;
}
static size_t make_table(uint8_t *out, uint8_t tid, uint16_t ext, unsigned version, unsigned number,
                         unsigned last, const uint8_t *payload, size_t payload_n) {
    size_t total = 12 + payload_n, section_length = total - 3;
    out[0] = tid;
    out[1] = (uint8_t)(0xb0 | (section_length >> 8));
    out[2] = (uint8_t)section_length;
    out[3] = (uint8_t)(ext >> 8);
    out[4] = (uint8_t)ext;
    out[5] = (uint8_t)(0xc1 | ((version & 31) << 1));
    out[6] = (uint8_t)number;
    out[7] = (uint8_t)last;
    if (payload_n)
        memcpy(out + 8, payload, payload_n);
    uint32_t crc = mpeg_crc(out, total - 4);
    out[total - 4] = (uint8_t)(crc >> 24);
    out[total - 3] = (uint8_t)(crc >> 16);
    out[total - 2] = (uint8_t)(crc >> 8);
    out[total - 1] = (uint8_t)crc;
    assert(ts_crc(out, total) == 0);
    return total;
}
static unsigned continuity[8192];
static void feed_section(dt295_routes *r, uint16_t pid, const uint8_t *section, size_t n) {
    assert(n <= 183);
    uint8_t p[188];
    memset(p, 0xff, sizeof(p));
    p[0] = 0x47;
    p[1] = (uint8_t)(0x40 | (pid >> 8));
    p[2] = (uint8_t)pid;
    p[3] = (uint8_t)(0x10 | (continuity[pid]++ & 15));
    p[4] = 0;
    memcpy(p + 5, section, n);
    assert(dt295_routes_feed(r, p) == 0);
}
static size_t make_pat(uint8_t *b, unsigned ver, unsigned sn, unsigned last, uint16_t program,
                       uint16_t pmt_pid) {
    uint8_t entry[4] = {(uint8_t)(program >> 8), (uint8_t)program, (uint8_t)(0xe0 | (pmt_pid >> 8)),
                        (uint8_t)pmt_pid};
    return make_table(b, 0, 0x1234, ver, sn, last, entry, sizeof(entry));
}
static size_t make_pat_av_and_data(uint8_t *b, unsigned ver) {
    uint8_t entries[8] = {
        0x00, 0x01, 0xe1, 0x00, /* program 1 -> PMT PID 0x100 */
        0xff, 0xf0, 0xfc, 0xf0, /* program 65520 -> PMT PID 7408 */
    };
    return make_table(b, 0, 0x1234, ver, 0, 0, entries, sizeof(entries));
}
static size_t make_cat(uint8_t *b, unsigned ver, uint16_t ecm_pid) {
    uint8_t desc[6] = {0xf6, 0x04, 0x00, 0x05, (uint8_t)(0xe0 | (ecm_pid >> 8)), (uint8_t)ecm_pid};
    return make_table(b, 1, 1, ver, 0, 0, desc, sizeof(desc));
}
static size_t make_empty_cat(uint8_t *b, unsigned ver) {
    return make_table(b, 1, 1, ver, 0, 0, NULL, 0);
}
static size_t make_pmt(uint8_t *b, unsigned ver, uint16_t program, uint16_t pmt_pid,
                       uint16_t ecm_pid, int ca) {
    uint8_t data[64];
    size_t n = 0;
    uint16_t video = (uint16_t)(0x101 + (program - 1) * 2), audio = (uint16_t)(video + 1);
    (void)pmt_pid;
    data[n++] = (uint8_t)(0xe0 | (video >> 8));
    data[n++] = (uint8_t)video;
    size_t pil = ca ? 6 : 0;
    data[n++] = (uint8_t)(0xf0 | (pil >> 8));
    data[n++] = (uint8_t)pil;
    if (ca) {
        data[n++] = 0x09;
        data[n++] = 4;
        data[n++] = 0;
        data[n++] = 5;
        data[n++] = (uint8_t)(0xe0 | (ecm_pid >> 8));
        data[n++] = (uint8_t)ecm_pid;
    }
    data[n++] = 0x1b;
    data[n++] = (uint8_t)(0xe0 | (video >> 8));
    data[n++] = (uint8_t)video;
    data[n++] = 0xf0;
    data[n++] = 0;
    data[n++] = 0x0f;
    data[n++] = (uint8_t)(0xe0 | (audio >> 8));
    data[n++] = (uint8_t)audio;
    data[n++] = 0xf0;
    data[n++] = 0;
    return make_table(b, 2, program, ver, 0, 0, data, n);
}
static size_t make_pmt_video_only(uint8_t *b, unsigned ver, uint16_t program, uint16_t ecm_pid) {
    uint8_t data[32];
    size_t n = 0;
    uint16_t video = 0x101;
    data[n++] = (uint8_t)(0xe0 | (video >> 8));
    data[n++] = (uint8_t)video;
    data[n++] = 0xf0;
    data[n++] = 6;
    data[n++] = 0x09;
    data[n++] = 4;
    data[n++] = 0;
    data[n++] = 5;
    data[n++] = (uint8_t)(0xe0 | (ecm_pid >> 8));
    data[n++] = (uint8_t)ecm_pid;
    data[n++] = 0x1b;
    data[n++] = (uint8_t)(0xe0 | (video >> 8));
    data[n++] = (uint8_t)video;
    data[n++] = 0xf0;
    data[n++] = 0;
    return make_table(b, 2, program, ver, 0, 0, data, n);
}
static size_t make_pmt_two_program_ecms(uint8_t *b, unsigned ver, uint16_t program,
                                         uint16_t first_ecm, uint16_t second_ecm) {
    uint8_t data[64];
    size_t n = 0;
    uint16_t video = 0x101, audio = 0x102;
    data[n++] = (uint8_t)(0xe0 | (video >> 8));
    data[n++] = (uint8_t)video;
    data[n++] = 0xf0;
    data[n++] = 12;
    for (unsigned i = 0; i < 2; ++i) {
        uint16_t ecm = i ? second_ecm : first_ecm;
        data[n++] = 0x09;
        data[n++] = 4;
        data[n++] = 0;
        data[n++] = 5;
        data[n++] = (uint8_t)(0xe0 | (ecm >> 8));
        data[n++] = (uint8_t)ecm;
    }
    data[n++] = 0x1b;
    data[n++] = (uint8_t)(0xe0 | (video >> 8));
    data[n++] = (uint8_t)video;
    data[n++] = 0xf0;
    data[n++] = 0;
    data[n++] = 0x0f;
    data[n++] = (uint8_t)(0xe0 | (audio >> 8));
    data[n++] = (uint8_t)audio;
    data[n++] = 0xf0;
    data[n++] = 0;
    return make_table(b, 2, program, ver, 0, 0, data, n);
}
static size_t make_pmt_data_only_no_pcr(uint8_t *b, unsigned ver, uint16_t program,
                                        uint16_t es_pid) {
    uint8_t data[9];
    size_t n = 0;
    data[n++] = 0xff;
    data[n++] = 0xff; /* PCR_PID == 0x1fff: no PCR */
    data[n++] = 0xf0;
    data[n++] = 0;
    data[n++] = 0x06; /* private data stream */
    data[n++] = (uint8_t)(0xe0 | (es_pid >> 8));
    data[n++] = (uint8_t)es_pid;
    data[n++] = 0xf0;
    data[n++] = 0;
    return make_table(b, 2, program, ver, 0, 0, data, n);
}
static void feed_pat(dt295_routes *r, unsigned ver, unsigned sn, unsigned last, uint16_t program,
                     uint16_t pid) {
    uint8_t b[TEST_SECTION_MAX];
    size_t n = make_pat(b, ver, sn, last, program, pid);
    feed_section(r, 0, b, n);
}
static void bootstrap(dt295_routes *r, dt295_route_plan *live) {
    char error[DT295_ROUTE_ERROR];
    dt295_route_plan plan, candidate;
    dt295_route_op ops[DT295_ROUTE_OP_LIMIT];
    size_t count;
    assert(dt295_routes_initial_plan(r, &plan, error) == 0);
    assert(dt295_routes_initial_ops(&plan, ops, &count, error) == 0);
    memset(live, 0, sizeof(*live));
    live->routes[4] = (dt295_route){4, 1, 0, 0, 0, 0};
    live->route_count = 1;
    assert(dt295_routes_guard(live, ops, count, &candidate, error) == 0);
    dt295_routes_commit(live, &candidate);
}

static void test_completeness_versions_and_planning(void) {
    memset(continuity, 0, sizeof(continuity));
    dt295_routes *r = dt295_routes_create();
    assert(r);
    dt295_routes_snapshot s;
    feed_pat(r, 31, 0, 1, 1, 0x100);
    dt295_routes_snapshot_get(r, &s);
    assert(!s.have_pat);
    feed_pat(r, 31, 1, 1, 2, 0x110);
    dt295_routes_snapshot_get(r, &s);
    assert(s.have_pat && s.program_count == 2 && !s.ready && s.missing_pmts == 2);
    /* The current CAT is part of acquisition readiness; CAT/PAT versions are independent. */
    uint8_t b[TEST_SECTION_MAX];
    size_t n = make_cat(b, 0, 0x1ef);
    feed_section(r, 1, b, n);
    feed_pat(r, 0, 0, 1, 1, 0x100);
    feed_pat(r, 0, 1, 1, 2, 0x110);
    dt295_routes_snapshot_get(r, &s);
    assert(s.have_pat && s.have_cat && s.missing_pmts == 2 && !s.ready);
    feed_pat(r, 31, 0, 1, 9, 0x120); /* delayed previous version is ignored */
    feed_pat(r, 30, 0, 1, 9, 0x120); /* older versions cannot roll the route map back */
    feed_pat(r, 16, 0, 1, 9, 0x120); /* half-cycle ordering is unknown and fails closed */
    dt295_routes_snapshot_get(r, &s);
    assert(s.program_count == 2 && s.programs[0].program == 1 && s.unsupported_tables > 0);
    dt295_route_plan live;
    bootstrap(r, &live);
    for (unsigned i = 0; i < 64; ++i)
        assert(!live.routes[i].enabled ||
               (live.routes[i].pid != 0x1ed && live.routes[i].pid != 0x1ef));
    n = make_pmt(b, 0, 1, 0x100, 0x1ed, 1);
    feed_section(r, 0x100, b, n);
    n = make_pmt(b, 0, 2, 0x110, 0, 0);
    feed_section(r, 0x110, b, n);
    dt295_routes_snapshot_get(r, &s);
    assert(s.ready && s.missing_pmts == 0);
    dt295_route_plan desired, candidate;
    char error[DT295_ROUTE_ERROR];
    assert(dt295_routes_build(r, 1, &live, &desired, error) == 0);
    assert(desired.ready && desired.has_primary && desired.primary_program == 1);
    assert(desired.dedicated_pid[0] == 0x101 && desired.dedicated_pid[3] == 0x101);
    assert(desired.video_stream_type == 0x1b && desired.audio_stream_type == 0x0f);
    int cat_ca_routed = 0;
    for (unsigned i = 0; i < 64; ++i)
        if (desired.routes[i].enabled && desired.routes[i].pid == 0x1ef)
            cat_ca_routed = 1;
    assert(cat_ca_routed);
    int ecm_slot = -1, video_slot = -1;
    for (unsigned i = 0; i < 64; ++i)
        if (desired.routes[i].enabled) {
            if (desired.routes[i].is_ecm && desired.routes[i].pid == 0x1ed)
                ecm_slot = (int)i;
            if (desired.routes[i].pid == 0x101)
                video_slot = (int)i;
        }
    assert(ecm_slot >= 5 && video_slot >= 0 && desired.routes[video_slot].ecm_slot == ecm_slot);
    dt295_route_op ops[DT295_ROUTE_OP_LIMIT];
    size_t count;
    assert(dt295_routes_plan_ops(&live, &desired, ops, &count, error) == 0);
    assert(count > 0 && ops[0].slot == ecm_slot); /* ECM is registered before its streams. */
    assert(dt295_routes_guard(&live, ops, count, &candidate, error) == 0);
    dt295_routes_commit(&live, &candidate);
    assert(dt295_routes_build(r, 1, &live, &desired, error) ==
           0); /* retained nontrivial ECM PID/slot */
    assert(desired.routes[ecm_slot].pid == 0x1ed && desired.routes[ecm_slot].is_ecm);
    live = desired; /* model the post-reprepare configured plan */
    /* A naturally disappearing audio PID stays registered without forcing a stop. */
    n = make_pmt_video_only(b, 1, 1, 0x1ed);
    feed_section(r, 0x100, b, n);
    assert(dt295_routes_build(r, 0, &live, &desired, error) == 0);
    assert(desired.dedicated_pid[1] == 0);
    int audio_retained = 0;
    for (unsigned i = 0; i < 64; ++i)
        if (desired.routes[i].enabled && desired.routes[i].pid == 0x102)
            audio_retained = 1;
    assert(audio_retained);
    /* A binding change on a still-advertised route is rejected after registration. */
    n = make_pmt_video_only(b, 2, 1, 0x1ee);
    feed_section(r, 0x100, b, n);
    assert(dt295_routes_build(r, 0, &live, &desired, error) != 0);
    assert(strstr(error, "rebind") != NULL);
    dt295_routes_destroy(r);
}

static void test_stable_bootstrap_slots(void) {
    memset(continuity, 0, sizeof(continuity));
    dt295_routes *r = dt295_routes_create();
    assert(r);
    feed_pat(r, 1, 0, 0, 1, 0x100);
    dt295_route_plan first, live, next;
    char error[DT295_ROUTE_ERROR];
    assert(dt295_routes_initial_plan(r, &first, error) == 0);
    live = first;
    unsigned pmt_slot = 0;
    for (unsigned i = 0; i < 64; ++i)
        if (live.routes[i].enabled && live.routes[i].pid == 0x100)
            pmt_slot = i;
    assert(pmt_slot >= 5);
    feed_pat(r, 2, 0, 0, 1, 0x110);
    assert(dt295_routes_bootstrap_plan(r, &live, &next, error) == 0);
    for (unsigned i = 0; i < 64; ++i)
        if (live.routes[i].enabled)
            assert(next.routes[i].enabled && next.routes[i].pid == live.routes[i].pid);
    int new_pmt = 0;
    for (unsigned i = 0; i < 64; ++i)
        if (next.routes[i].enabled && next.routes[i].pid == 0x110)
            new_pmt = 1;
    assert(new_pmt && next.routes[pmt_slot].pid == 0x100);
    dt295_routes_destroy(r);
}

static void test_unsupported_multi_ecm_pmt_update_preserves_commit(void) {
    memset(continuity, 0, sizeof(continuity));
    dt295_routes *r = dt295_routes_create();
    assert(r);
    uint8_t b[TEST_SECTION_MAX];
    feed_pat(r, 1, 0, 0, 1, 0x100);
    size_t n = make_cat(b, 0, 0x1ed);
    feed_section(r, 1, b, n);
    dt295_route_plan live, before, after;
    char error[DT295_ROUTE_ERROR];
    bootstrap(r, &live);
    n = make_pmt(b, 1, 1, 0x100, 0x1ed, 1);
    feed_section(r, 0x100, b, n);
    assert(dt295_routes_build(r, 0, &live, &before, error) == 0);
    dt295_routes_snapshot s;
    dt295_routes_snapshot_get(r, &s);
    assert(s.ready && s.unsupported_tables == 0);

    n = make_pmt_two_program_ecms(b, 2, 1, 0x1ed, 0x1ee);
    feed_section(r, 0x100, b, n);
    dt295_routes_snapshot_get(r, &s);
    assert(s.ready && s.missing_pmts == 0 && s.unsupported_tables > 0);
    /* The rejected update must not replace the last usable PMT/routes. */
    assert(dt295_routes_build(r, 0, &live, &after, error) == 0);
    assert(after.primary_program == before.primary_program);
    assert(after.route_count == before.route_count);
    assert(memcmp(after.routes, before.routes, sizeof(after.routes)) == 0);
    assert(memcmp(after.dedicated_pid, before.dedicated_pid, sizeof(after.dedicated_pid)) == 0);
    dt295_routes_destroy(r);
}

static void test_av_and_data_program_with_null_pcr(void) {
    memset(continuity, 0, sizeof(continuity));
    dt295_routes *r = dt295_routes_create();
    assert(r);
    uint8_t b[TEST_SECTION_MAX];
    size_t n = make_pat_av_and_data(b, 1);
    feed_section(r, 0, b, n);
    n = make_empty_cat(b, 0);
    feed_section(r, 1, b, n);

    dt295_route_plan live, full;
    char error[DT295_ROUTE_ERROR];
    assert(dt295_routes_initial_plan(r, &live, error) == 0);
    n = make_pmt(b, 1, 1, 0x100, 0, 0);
    feed_section(r, 0x100, b, n);
    n = make_pmt_data_only_no_pcr(b, 1, 65520, 0x1300);
    feed_section(r, 7408, b, n);

    dt295_routes_snapshot snapshot;
    dt295_routes_snapshot_get(r, &snapshot);
    assert(snapshot.have_pat && snapshot.have_cat && snapshot.ready);
    assert(snapshot.program_count == 2 && snapshot.missing_pmts == 0);
    assert(dt295_routes_build(r, 0, NULL, &full, error) == 0);
    assert(full.ready && full.has_primary && full.primary_program == 1);
    assert(full.dedicated_pid[3] == 0x101); /* AV program keeps its valid PCR PID. */
    int data_es_routed = 0;
    for (unsigned i = 0; i < DT295_SLOT_COUNT; ++i) {
        assert(!full.routes[i].enabled || full.routes[i].pid != 0x1fff);
        if (full.routes[i].enabled && full.routes[i].pid == 0x1300)
            data_es_routed = 1;
    }
    assert(data_es_routed);
    dt295_routes_destroy(r);
}

static void test_guard_atomicity_and_no_rebind(void) {
    dt295_route_plan live = {0}, candidate, before;
    char error[DT295_ROUTE_ERROR];
    live.routes[4] = (dt295_route){4, 1, 0, 0, 0, 0};
    live.route_count = 1;
    dt295_route_op good = {63, 1, 0x1ed, 63};
    assert(dt295_routes_guard(&live, &good, 1, &candidate, error) == 0);
    dt295_routes_commit(&live, &candidate);
    dt295_route_op bad_batch[2] = {{62, 1, 0x101, 63}, {61, 1, 0x101, 0}};
    before = live;
    candidate = before;
    assert(dt295_routes_guard(&live, bad_batch, 2, &candidate, error) != 0);
    assert(memcmp(&candidate, &before, sizeof(before)) == 0);
    dt295_route_op remove_ecm = {63, 0, 0x1ed, 63};
    dt295_route_op dep = {5, 1, 0x101, 63};
    assert(dt295_routes_guard(&live, &dep, 1, &candidate, error) == 0);
    dt295_routes_commit(&live, &candidate);
    before = live;
    assert(dt295_routes_guard(&live, &remove_ecm, 1, &candidate, error) != 0);
    assert(memcmp(&live, &before, sizeof(live)) == 0);
    dt295_route_op second_ecm = {62, 1, 0x1ee, 62};
    assert(dt295_routes_guard(&live, &second_ecm, 1, &candidate, error) == 0);
    dt295_routes_commit(&live, &candidate);
    dt295_route_op rebind = {5, 1, 0x101, 62};
    assert(dt295_routes_guard(&live, &rebind, 1, &candidate, error) != 0);
    dt295_route_op replace = {5, 1, 0x102, 63};
    assert(dt295_routes_guard(&live, &replace, 1, &candidate, error) != 0);
}

static void test_bad_crc_same_version_and_capacity(void) {
    memset(continuity, 0, sizeof(continuity));
    dt295_routes *r = dt295_routes_create();
    assert(r);
    uint8_t b[TEST_SECTION_MAX];
    size_t n = make_pat(b, 1, 0, 0, 1, 0x100);
    b[n - 1] ^= 1;
    feed_section(r, 0, b, n);
    dt295_routes_snapshot s;
    dt295_routes_snapshot_get(r, &s);
    assert(!s.have_pat && s.invalid_sections);
    b[n - 1] ^= 1;
    feed_section(r, 0, b, n);
    dt295_routes_snapshot_get(r, &s);
    assert(s.have_pat);
    uint32_t unsupported_before = s.unsupported_tables;
    n = make_pat(b, 1, 0, 0, 2, 0x110);
    feed_section(r, 0, b, n);
    dt295_routes_snapshot_get(r, &s);
    assert(s.program_count == 1 && s.programs[0].program == 1 && s.invalid_sections >= 2);
    assert(s.unsupported_tables > unsupported_before);
    dt295_route_plan base = {0}, desired;
    base.routes[4] = (dt295_route){4, 1, 0, 0, 0, 0};
    base.route_count = 1;
    for (unsigned i = 5; i < 64; ++i) {
        base.routes[i] = (dt295_route){(uint8_t)i, 1, 0, 0, (uint16_t)(100 + i), 0};
        base.route_count++;
    }
    char error[DT295_ROUTE_ERROR];
    assert(dt295_routes_build(r, 0, &base, &desired, error) !=
           0); /* unresolved PMT/primary or no free capacity */
    dt295_routes_destroy(r);

    memset(continuity, 0, sizeof(continuity));
    r = dt295_routes_create();
    assert(r);
    for (unsigned sn = 0; sn < 2; ++sn) {
        uint8_t payload[128];
        for (unsigned j = 0; j < 32; ++j) {
            unsigned program = sn * 32 + j + 1, pid = 0x100 + program;
            payload[j * 4] = (uint8_t)(program >> 8);
            payload[j * 4 + 1] = (uint8_t)program;
            payload[j * 4 + 2] = (uint8_t)(0xe0 | (pid >> 8));
            payload[j * 4 + 3] = (uint8_t)pid;
        }
        n = make_table(b, 0, 0x4321, 2, sn, 1, payload, sizeof(payload));
        feed_section(r, 0, b, n);
    }
    dt295_route_plan bootstrap_plan;
    assert(dt295_routes_initial_plan(r, &bootstrap_plan, error) != 0);
    assert(strstr(error, "capacity") != NULL);
    dt295_routes_destroy(r);
}

int main(void) {
    test_completeness_versions_and_planning();
    test_stable_bootstrap_slots();
    test_unsupported_multi_ecm_pmt_update_preserves_commit();
    test_av_and_data_program_with_null_pcr();
    test_guard_atomicity_and_no_rebind();
    test_bad_crc_same_version_and_capacity();
    puts("dt295 routes ok");
    return 0;
}
