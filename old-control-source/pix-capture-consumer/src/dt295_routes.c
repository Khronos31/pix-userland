/* SPDX-License-Identifier: MIT */
#include "dt295_routes.h"
#include "transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTION_MAX 1024u
#define TABLE_SECTIONS 16u
#define TABLE_LIMIT 66u
#define PMT_STREAM_LIMIT 96u
#define CA_LIMIT 96u

typedef struct {
    uint16_t pid;
    uint8_t used, have_cc, cc;
    size_t used_bytes;
    uint8_t bytes[SECTION_MAX];
    uint8_t last_packet[188];
} psi_pid;

typedef struct {
    uint16_t length[TABLE_SECTIONS];
    uint8_t data[TABLE_SECTIONS][SECTION_MAX];
    uint8_t mask[TABLE_SECTIONS / 8];
    uint8_t version, last, valid;
} section_set;

typedef struct {
    uint8_t tid, used;
    uint16_t pid, ext;
    section_set committed, pending;
    uint8_t previous_version, have_previous;
} table_state;

typedef struct {
    uint16_t pid;
    uint8_t type;
    uint16_t ecm_pid;
} pmt_stream;
typedef struct {
    uint16_t program, pmt_pid, pcr_pid;
    uint16_t program_ecm;
    uint16_t ca_count;
    uint16_t ca_pid[CA_LIMIT];
    uint16_t streams;
    pmt_stream stream[PMT_STREAM_LIMIT];
} pmt_info;

struct dt295_routes {
    psi_pid psi[TABLE_LIMIT];
    table_state table[TABLE_LIMIT];
    dt295_pat_program programs[64];
    uint16_t program_count, tsid;
    uint8_t have_pat, have_cat;
    pmt_info pmts[64];
    uint16_t pmt_count;
    uint16_t network_pid[64];
    uint16_t network_count;
    uint16_t cat_pid[CA_LIMIT];
    uint16_t cat_count;
    uint32_t invalid_sections, continuity_errors, unsupported_tables;
};

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint16_t pid13(const uint8_t *p) {
    return (uint16_t)(((p[0] & 0x1fu) << 8) | p[1]);
}
static void errset(char dst[DT295_ROUTE_ERROR], const char *s) {
    if (dst) {
        (void)snprintf(dst, DT295_ROUTE_ERROR, "%s", s);
    }
}
static int has_pid(const uint16_t *pids, unsigned n, uint16_t pid) {
    for (unsigned i = 0; i < n; ++i)
        if (pids[i] == pid)
            return 1;
    return 0;
}

dt295_routes *dt295_routes_create(void) {
    return (dt295_routes *)calloc(1, sizeof(dt295_routes));
}
void dt295_routes_destroy(dt295_routes *r) {
    free(r);
}
void dt295_routes_reset(dt295_routes *r) {
    if (r)
        memset(r, 0, sizeof(*r));
}

static psi_pid *psi_for(dt295_routes *r, uint16_t pid, int create) {
    psi_pid *empty = NULL;
    for (unsigned i = 0; i < TABLE_LIMIT; ++i) {
        if (r->psi[i].used && r->psi[i].pid == pid)
            return &r->psi[i];
        if (!r->psi[i].used && !empty)
            empty = &r->psi[i];
    }
    if (!create || !empty)
        return NULL;
    memset(empty, 0, sizeof(*empty));
    empty->used = 1;
    empty->pid = pid;
    return empty;
}
static table_state *table_for(dt295_routes *r, uint16_t pid, uint8_t tid, uint16_t ext,
                              int create) {
    table_state *empty = NULL;
    for (unsigned i = 0; i < TABLE_LIMIT; ++i) {
        table_state *t = &r->table[i];
        if (t->used && t->pid == pid && t->tid == tid && t->ext == ext)
            return t;
        if (!t->used && !empty)
            empty = t;
    }
    if (!create || !empty)
        return NULL;
    memset(empty, 0, sizeof(*empty));
    empty->used = 1;
    empty->pid = pid;
    empty->tid = tid;
    empty->ext = ext;
    return empty;
}
static int section_bit(const section_set *s, unsigned n) {
    return (s->mask[n / 8] >> (n % 8)) & 1;
}
static void set_section_bit(section_set *s, unsigned n) {
    s->mask[n / 8] |= (uint8_t)(1u << (n % 8));
}

static int ca_descriptors(const uint8_t *p, size_t n, uint16_t *ecm, uint16_t *all,
                          unsigned *all_n) {
    size_t i = 0;
    while (i < n) {
        if (n - i < 2 || (size_t)p[i + 1] > n - i - 2)
            return -1;
        uint8_t tag = p[i], len = p[i + 1];
        if (tag == 0x09 || tag == 0xf6) {
            if (len < 4)
                return -1;
            uint16_t system = be16(p + i + 2), pid = pid13(p + i + 4);
            if ((p[i + 4] & 0xe0) != 0xe0)
                return -1;
            if (pid != 0x1fff) {
                if (pid < 0x10)
                    return -1;
                if (!has_pid(all, *all_n, pid)) {
                    if (*all_n >= CA_LIMIT)
                        return -2;
                    all[(*all_n)++] = pid;
                }
                if (tag == 0x09 && system == 0x0005) {
                    if (*ecm && *ecm != pid)
                        return -3;
                    *ecm = pid;
                }
            }
        }
        i += (size_t)len + 2;
    }
    return 0;
}

static pmt_info *pmt_for_program(dt295_routes *r, uint16_t program) {
    for (unsigned i = 0; i < r->pmt_count; ++i)
        if (r->pmts[i].program == program)
            return &r->pmts[i];
    return NULL;
}

static int parse_pat(dt295_routes *r, const section_set *s, uint16_t tsid) {
    dt295_pat_program programs[64];
    uint16_t network[64];
    unsigned np = 0, nn = 0;
    for (unsigned sn = 0; sn <= s->last; ++sn) {
        const uint8_t *b = s->data[sn];
        size_t len = s->length[sn];
        if (len < 12 || (len - 12) % 4)
            return -1;
        for (size_t i = 8; i < len - 4; i += 4) {
            uint16_t program = be16(b + i), pid = pid13(b + i + 2);
            if ((b[i + 2] & 0xe0) != 0xe0)
                return -1;
            if (pid == 0x1fff || pid < 0x10)
                return -1;
            if (!program) {
                if (!has_pid(network, nn, pid)) {
                    if (nn == 64)
                        return -2;
                    network[nn++] = pid;
                }
                continue;
            }
            int found = 0;
            for (unsigned j = 0; j < np; ++j)
                if (programs[j].program == program) {
                    if (programs[j].pmt_pid != pid)
                        return -1;
                    found = 1;
                }
            if (!found) {
                if (np == 64)
                    return -2;
                programs[np++] = (dt295_pat_program){program, pid};
            }
        }
    }
    if (r->have_pat && r->tsid != tsid) {
        r->pmt_count = 0;
        r->have_cat = 0;
        r->cat_count = 0;
        for (unsigned i = 0; i < TABLE_LIMIT; ++i)
            if (r->table[i].used && (r->table[i].tid == 1 || r->table[i].tid == 2))
                memset(&r->table[i], 0, sizeof(r->table[i]));
        for (unsigned i = 0; i < TABLE_LIMIT; ++i)
            if (r->psi[i].used && r->psi[i].pid != 0)
                memset(&r->psi[i], 0, sizeof(r->psi[i]));
    }
    pmt_info keep[64];
    unsigned nk = 0;
    if (r->have_pat && r->tsid == tsid) {
        for (unsigned i = 0; i < r->pmt_count; ++i)
            for (unsigned j = 0; j < np; ++j)
                if (r->pmts[i].program == programs[j].program &&
                    r->pmts[i].pmt_pid == programs[j].pmt_pid) {
                    keep[nk++] = r->pmts[i];
                    break;
                }
    }
    memcpy(r->programs, programs, np * sizeof(programs[0]));
    r->program_count = (uint16_t)np;
    memcpy(r->network_pid, network, nn * sizeof(network[0]));
    r->network_count = (uint16_t)nn;
    memcpy(r->pmts, keep, nk * sizeof(keep[0]));
    r->pmt_count = (uint16_t)nk;
    for (unsigned i = 0; i < TABLE_LIMIT; ++i) {
        table_state *t = &r->table[i];
        if (!t->used || t->tid != 2)
            continue;
        int live = 0;
        for (unsigned j = 0; j < np; ++j)
            if (programs[j].program == t->ext && programs[j].pmt_pid == t->pid)
                live = 1;
        if (!live)
            memset(t, 0, sizeof(*t));
    }
    for (unsigned i = 0; i < TABLE_LIMIT; ++i) {
        psi_pid *p = &r->psi[i];
        if (!p->used || p->pid <= 1)
            continue;
        int live = 0;
        for (unsigned j = 0; j < np; ++j)
            if (programs[j].pmt_pid == p->pid)
                live = 1;
        if (!live)
            memset(p, 0, sizeof(*p));
    }
    r->tsid = tsid;
    r->have_pat = 1;
    return 0;
}

static int parse_cat(dt295_routes *r, const section_set *s) {
    uint16_t pids[CA_LIMIT];
    unsigned n = 0;
    for (unsigned sn = 0; sn <= s->last; ++sn) {
        const uint8_t *b = s->data[sn];
        size_t len = s->length[sn];
        uint16_t dummy = 0;
        int rc = ca_descriptors(b + 8, len - 12, &dummy, pids, &n);
        if (rc)
            return rc;
    }
    memcpy(r->cat_pid, pids, n * sizeof(pids[0]));
    r->cat_count = (uint16_t)n;
    r->have_cat = 1;
    return 0;
}

static int parse_pmt(dt295_routes *r, uint16_t pid, uint16_t program, const section_set *s) {
    pmt_info out;
    memset(&out, 0, sizeof(out));
    out.program = program;
    out.pmt_pid = pid;
    uint16_t allca[CA_LIMIT];
    unsigned allca_n = 0;
    int have_pcr = 0;
    for (unsigned sn = 0; sn <= s->last; ++sn) {
        const uint8_t *b = s->data[sn];
        size_t len = s->length[sn];
        if (len < 16)
            return -1;
        uint16_t pcr = pid13(b + 8);
        if ((b[8] & 0xe0) != 0xe0 || (b[10] & 0xf0) != 0xf0)
            return -1;
        /* 0x1fff is the MPEG-TS declaration for no PCR PID. */
        if (pcr != 0x1fff && pcr < 0x10)
            return -1;
        if (have_pcr && out.pcr_pid != pcr)
            return -1;
        out.pcr_pid = pcr;
        have_pcr = 1;
        size_t end = len - 4, info = be16(b + 10) & 0x0fff;
        if (12 + info > end)
            return -1;
        int rc = ca_descriptors(b + 12, info, &out.program_ecm, allca, &allca_n);
        if (rc)
            return rc;
        size_t i = 12 + info;
        while (i < end) {
            if (end - i < 5)
                return -1;
            uint8_t type = b[i];
            uint16_t espid = pid13(b + i + 1);
            if ((b[i + 1] & 0xe0) != 0xe0 || (b[i + 3] & 0xf0) != 0xf0)
                return -1;
            size_t dlen = be16(b + i + 3) & 0x0fff;
            if (dlen > end - i - 5 || espid == 0x1fff || espid < 0x10)
                return -1;
            uint16_t explicit_ecm = 0;
            rc = ca_descriptors(b + i + 5, dlen, &explicit_ecm, allca, &allca_n);
            if (rc)
                return rc;
            unsigned ix;
            for (ix = 0; ix < out.streams; ++ix)
                if (out.stream[ix].pid == espid)
                    break;
            uint16_t binding = explicit_ecm ? explicit_ecm : out.program_ecm;
            if (ix < out.streams) {
                if (out.stream[ix].type != type || out.stream[ix].ecm_pid != binding)
                    return -1;
            } else {
                if (out.streams == PMT_STREAM_LIMIT)
                    return -2;
                out.stream[out.streams++] = (pmt_stream){espid, type, binding};
            }
            i += 5 + dlen;
        }
    }
    if (!have_pcr)
        return -1;
    out.ca_count = (uint16_t)allca_n;
    memcpy(out.ca_pid, allca, allca_n * sizeof(allca[0]));
    pmt_info *old = pmt_for_program(r, program);
    if (old && old->pmt_pid == pid) {
        *old = out;
        return 0;
    }
    if (old)
        return -1;
    if (r->pmt_count == 64)
        return -2;
    r->pmts[r->pmt_count++] = out;
    return 0;
}

static void apply_complete(dt295_routes *r, table_state *t) {
    int rc;
    if (t->tid == 0)
        rc = parse_pat(r, &t->pending, t->ext);
    else if (t->tid == 1)
        rc = parse_cat(r, &t->pending);
    else
        rc = parse_pmt(r, t->pid, t->ext, &t->pending);
    if (rc) {
        if (rc == -2 || rc == -3)
            r->unsupported_tables++;
        else
            r->invalid_sections++;
        memset(&t->pending, 0, sizeof(t->pending));
        return;
    }
    t->previous_version = t->committed.version;
    t->have_previous = t->committed.valid;
    t->committed = t->pending;
    t->committed.valid = 1;
    memset(&t->pending, 0, sizeof(t->pending));
}

static void section_bytes(dt295_routes *r, uint16_t pid, const uint8_t *b, size_t n) {
    if (n < 12 || n > SECTION_MAX || !(b[1] & 0x80) || (b[1] & 0x30) != 0x30 ||
        (b[5] & 0xc0) != 0xc0) {
        r->invalid_sections++;
        return;
    }
    size_t declared = 3u + (be16(b + 1) & 0x0fffu);
    if (declared != n || ts_crc(b, n) != 0 || !(b[5] & 1)) {
        r->invalid_sections++;
        return;
    }
    uint8_t tid = b[0];
    if ((tid == 0 && pid != 0) || (tid == 1 && pid != 1) || (tid != 0 && tid != 1 && tid != 2)) {
        r->unsupported_tables++;
        return;
    }
    uint16_t ext = be16(b + 3);
    if (tid == 2) {
        int found = 0;
        for (unsigned i = 0; i < r->program_count; ++i)
            if (r->programs[i].program == ext && r->programs[i].pmt_pid == pid)
                found = 1;
        if (!r->have_pat || !found)
            return;
    }
    unsigned number = b[6], last = b[7], version = (b[5] >> 1) & 31;
    if (number > last || last >= TABLE_SECTIONS) {
        r->unsupported_tables++;
        return;
    }
    table_state *t = table_for(r, pid, tid, ext, 1);
    if (!t) {
        r->unsupported_tables++;
        return;
    }
    if (t->committed.valid && t->committed.version == version) {
        section_set *c = &t->committed;
        if (number > c->last || c->length[number] != n || memcmp(c->data[number], b, n)) {
            r->invalid_sections++;
            /* Same-version content changes make the committed route map stale. */
            r->unsupported_tables++;
        }
        return;
    }
    if (t->have_previous && t->previous_version == version)
        return; /* delayed prior table */
    if (t->committed.valid) {
        unsigned delta = (version - t->committed.version) & 31u;
        /* Accept bounded forward steps, including 31->0. Unknown backward or
         * half-cycle transitions cannot be ordered safely and fail closed. */
        if (!delta || delta >= 16) {
            r->unsupported_tables++;
            return;
        }
    }
    section_set *p = &t->pending;
    if (!p->valid || p->version != version || p->last != last) {
        memset(p, 0, sizeof(*p));
        p->valid = 1;
        p->version = (uint8_t)version;
        p->last = (uint8_t)last;
    }
    if (section_bit(p, number)) {
        if (p->length[number] != n || memcmp(p->data[number], b, n)) {
            memset(p, 0, sizeof(*p));
            r->invalid_sections++;
        }
        return;
    }
    memcpy(p->data[number], b, n);
    p->length[number] = (uint16_t)n;
    set_section_bit(p, number);
    for (unsigned i = 0; i <= last; ++i)
        if (!section_bit(p, i))
            return;
    apply_complete(r, t);
}

static void append_psi(dt295_routes *r, psi_pid *p, const uint8_t *data, size_t n) {
    while (n) {
        if (!p->used_bytes && data[0] == 0xff)
            return;
        if (p->used_bytes < 3) {
            size_t take = 3 - p->used_bytes;
            if (take > n)
                take = n;
            memcpy(p->bytes + p->used_bytes, data, take);
            p->used_bytes += take;
            data += take;
            n -= take;
            if (p->used_bytes < 3)
                return;
            size_t total = 3 + (be16(p->bytes + 1) & 0x0fff);
            if (total < 3 || total > SECTION_MAX) {
                p->used_bytes = 0;
                r->invalid_sections++;
                return;
            }
        }
        size_t total = 3 + (be16(p->bytes + 1) & 0x0fff);
        size_t take = total - p->used_bytes;
        if (take > n)
            take = n;
        memcpy(p->bytes + p->used_bytes, data, take);
        p->used_bytes += take;
        data += take;
        n -= take;
        if (p->used_bytes == total) {
            section_bytes(r, p->pid, p->bytes, total);
            p->used_bytes = 0;
        }
    }
}

int dt295_routes_feed(dt295_routes *r, const uint8_t packet[188]) {
    if (!r || !packet)
        return -1;
    if (packet[0] != 0x47 || (packet[1] & 0x80) || (packet[3] & 0xc0))
        return 0;
    uint16_t pid = (uint16_t)(((packet[1] & 0x1f) << 8) | packet[2]);
    if (pid != 0 && pid != 1) {
        int wanted = 0;
        for (unsigned i = 0; i < r->program_count; ++i)
            if (r->programs[i].pmt_pid == pid)
                wanted = 1;
        if (!wanted)
            return 0;
    }
    unsigned afc = (packet[3] >> 4) & 3;
    if (!(afc & 1))
        return 0;
    size_t pos = 4;
    int discontinuity = 0;
    if (afc & 2) {
        if (pos >= TS_SIZE)
            return 0;
        unsigned n = packet[pos++];
        if (n > TS_SIZE - pos)
            return 0;
        discontinuity = n && (packet[pos] & 0x80);
        pos += n;
    }
    if (pos >= TS_SIZE)
        return 0;
    psi_pid *p = psi_for(r, pid, 1);
    if (!p) {
        r->unsupported_tables++;
        return 0;
    }
    unsigned cc = packet[3] & 15;
    if (discontinuity) {
        p->have_cc = 0;
        p->used_bytes = 0;
    }
    if (p->have_cc && cc == p->cc) {
        if (memcmp(p->last_packet, packet, TS_SIZE) == 0)
            return 0;
        r->continuity_errors++;
        p->used_bytes = 0;
        return 0;
    }
    if (p->have_cc && cc != ((p->cc + 1) & 15)) {
        r->continuity_errors++;
        p->used_bytes = 0;
    }
    p->cc = (uint8_t)cc;
    p->have_cc = 1;
    memcpy(p->last_packet, packet, TS_SIZE);
    const uint8_t *data = packet + pos;
    size_t n = TS_SIZE - pos;
    if (packet[1] & 0x40) {
        if (!n)
            return 0;
        unsigned pointer = data[0];
        data++;
        n--;
        if (pointer > n) {
            p->used_bytes = 0;
            r->invalid_sections++;
            return 0;
        }
        if (p->used_bytes && pointer)
            append_psi(r, p, data, pointer);
        p->used_bytes = 0;
        data += pointer;
        n -= pointer;
        if (n)
            append_psi(r, p, data, n);
    } else if (p->used_bytes)
        append_psi(r, p, data, n);
    return 0;
}

void dt295_routes_snapshot_get(const dt295_routes *r, dt295_routes_snapshot *s) {
    if (!s)
        return;
    memset(s, 0, sizeof(*s));
    if (!r)
        return;
    s->have_pat = r->have_pat;
    s->have_cat = r->have_cat;
    s->tsid = r->tsid;
    s->program_count = r->program_count;
    memcpy(s->programs, r->programs, r->program_count * sizeof(r->programs[0]));
    s->missing_pmts = r->program_count > r->pmt_count ? r->program_count - r->pmt_count : 0;
    s->ready = r->have_pat && r->have_cat && s->missing_pmts == 0;
    s->invalid_sections = r->invalid_sections;
    s->continuity_errors = r->continuity_errors;
    s->unsupported_tables = r->unsupported_tables;
}

static int plan_add(dt295_route_plan *p, unsigned slot, uint16_t pid, uint16_t ecm_slot,
                    int is_ecm) {
    if (slot >= DT295_SLOT_COUNT || pid >= 0x1fff || p->routes[slot].enabled)
        return -1;
    for (unsigned i = 0; i < DT295_SLOT_COUNT; ++i)
        if (p->routes[i].enabled && p->routes[i].pid == pid)
            return -2;
    p->routes[slot] = (dt295_route){(uint8_t)slot, 1, (uint8_t)is_ecm, 0, pid, ecm_slot};
    p->route_count++;
    if (is_ecm)
        p->ecm_pid[slot] = pid;
    return 0;
}
static int plan_find_pid(const dt295_route_plan *p, uint16_t pid) {
    for (unsigned i = 0; i < DT295_SLOT_COUNT; ++i)
        if (p->routes[i].enabled && p->routes[i].pid == pid)
            return (int)i;
    return -1;
}
static int plan_find_free(const dt295_route_plan *p, unsigned low, unsigned high, int reverse) {
    if (reverse) {
        for (int i = (int)high; i >= (int)low; --i)
            if (!p->routes[i].enabled)
                return i;
    } else {
        for (unsigned i = low; i <= high; ++i)
            if (!p->routes[i].enabled)
                return (int)i;
    }
    return -1;
}
static int add_general(dt295_route_plan *p, uint16_t pid, uint16_t ecm_slot, int is_ecm) {
    if (pid == 0)
        return 0; /* PAT is implicitly registered at logical slot 4. */
    int existing = plan_find_pid(p, pid);
    if (existing >= 0) {
        dt295_route *r = &p->routes[existing];
        if (r->is_ecm != !!is_ecm || r->ecm_slot != ecm_slot)
            return -2;
        return 0;
    }
    int slot = plan_find_free(p, 5, 63, is_ecm);
    if (slot < 0)
        return -1;
    return plan_add(p, (unsigned)slot, pid, is_ecm ? (uint16_t)slot : ecm_slot, is_ecm);
}

int dt295_routes_bootstrap_plan(const dt295_routes *r, const dt295_route_plan *current,
                                dt295_route_plan *out, char error[DT295_ROUTE_ERROR]) {
    if (!r || !out) {
        errset(error, "invalid route planner arguments");
        return -1;
    }
    dt295_route_plan p;
    memset(&p, 0, sizeof(p));
    if (current)
        p = *current;
    else {
        p.routes[4] = (dt295_route){4, 1, 0, 0, 0, 0};
        p.route_count = 1;
    }
    if (p.routes[4].enabled && p.routes[4].pid != 0) {
        errset(error, "invalid implicit PAT slot");
        return -1;
    }
    if (!p.routes[4].enabled) {
        p.routes[4] = (dt295_route){4, 1, 0, 0, 0, 0};
        p.route_count++;
    }
    unsigned actual = 0;
    for (unsigned i = 0; i < 64; ++i)
        if (p.routes[i].enabled) {
            actual++;
            if (p.routes[i].slot != i || p.routes[i].pid >= 0x1fff) {
                errset(error, "invalid current bootstrap route");
                return -1;
            }
            for (unsigned j = i + 1; j < 64; ++j)
                if (p.routes[j].enabled && p.routes[j].pid == p.routes[i].pid) {
                    errset(error, "duplicate current bootstrap PID");
                    return -1;
                }
        }
    if (actual != p.route_count) {
        errset(error, "current bootstrap route count mismatch");
        return -1;
    }
    uint16_t wanted[256];
    unsigned n = 0;
    static const uint16_t fixed[] = {1, 16, 17, 18, 20, 35, 36, 37, 39, 40, 41};
    for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i)
        wanted[n++] = fixed[i];
    for (unsigned i = 0; i < r->network_count; ++i)
        wanted[n++] = r->network_pid[i];
    for (unsigned i = 0; i < r->program_count; ++i)
        wanted[n++] = r->programs[i].pmt_pid;
    for (unsigned i = 0; i < n; ++i) {
        if (wanted[i] == 0x1fff || plan_find_pid(&p, wanted[i]) >= 0)
            continue;
        if (has_pid(wanted, i, wanted[i]))
            continue;
        if (add_general(&p, wanted[i], 0, 0)) {
            errset(error, "bootstrap PID slot capacity exceeded");
            return -1;
        }
    }
    if (r->have_pat) {
        p.missing_pmts = (uint16_t)(r->program_count - r->pmt_count);
        p.ready = r->have_cat && p.missing_pmts == 0;
    }
    p.cat_received = r->have_cat;
    *out = p;
    if (error)
        error[0] = 0;
    return 0;
}

int dt295_routes_initial_plan(const dt295_routes *r, dt295_route_plan *out,
                              char error[DT295_ROUTE_ERROR]) {
    return dt295_routes_bootstrap_plan(r, NULL, out, error);
}

static int stream_video(uint8_t t) {
    return t == 1 || t == 2 || t == 0x1b || t == 0x24;
}
static int stream_audio(uint8_t t) {
    return t == 3 || t == 4 || t == 0x0f || t == 0x11;
}
static int program_has_av(const pmt_info *p) {
    int v = 0, a = 0;
    for (unsigned i = 0; i < p->streams; ++i) {
        v |= stream_video(p->stream[i].type);
        a |= stream_audio(p->stream[i].type);
    }
    return v && a;
}
static int pat_has(const dt295_routes *r, uint16_t program, uint16_t *pid) {
    for (unsigned i = 0; i < r->program_count; ++i)
        if (r->programs[i].program == program) {
            if (pid)
                *pid = r->programs[i].pmt_pid;
            return 1;
        }
    return 0;
}
static int required_add(uint16_t *pids, unsigned *n, uint16_t pid) {
    if (pid >= 0x1fff)
        return 0;
    if (!has_pid(pids, *n, pid)) {
        if (*n >= 256)
            return -1;
        pids[(*n)++] = pid;
    }
    return 0;
}
int dt295_routes_build(const dt295_routes *r, uint16_t primary, const dt295_route_plan *current,
                       dt295_route_plan *desired, char error[DT295_ROUTE_ERROR]) {
    if (!r || !desired || !r->have_pat) {
        errset(error, "current PAT is required before route planning");
        return -1;
    }
    dt295_route_plan p;
    memset(&p, 0, sizeof(p));
    if (current) {
        if (current->route_count > DT295_SLOT_COUNT) {
            errset(error, "invalid current route count");
            return -1;
        }
        p = *current;
    } else {
        p.routes[4] = (dt295_route){4, 1, 0, 0, 0, 0};
        p.route_count = 1;
    }
    for (unsigned i = 0; i < DT295_SLOT_COUNT; ++i) {
        const dt295_route *x = &p.routes[i];
        if (x->enabled && (x->slot != i || (i == 4 && x->pid != 0) || x->pid >= 0x1fff)) {
            errset(error, "invalid slot or PID in current route state");
            return -1;
        }
        for (unsigned j = i + 1; x->enabled && j < DT295_SLOT_COUNT; ++j)
            if (p.routes[j].enabled && p.routes[j].pid == x->pid) {
                errset(error, "duplicate PID in current route state");
                return -1;
            }
    }
    p.ready = 0;
    p.missing_pmts = (uint16_t)(r->program_count - r->pmt_count);
    p.cat_received = r->have_cat;
    uint16_t pmt_pid = 0;
    int retained_primary = 0;
    if (!primary) {
        if (current && current->has_primary && pat_has(r, current->primary_program, NULL) &&
            pmt_for_program((dt295_routes *)r, current->primary_program)) {
            primary = current->primary_program;
            retained_primary = 1;
        } else {
            for (unsigned i = 0; i < r->program_count; ++i) {
                pmt_info *q = pmt_for_program((dt295_routes *)r, r->programs[i].program);
                if (q && program_has_av(q)) {
                    primary = q->program;
                    break;
                }
            }
        }
    }
    pmt_info *primary_pmt = NULL;
    if (primary) {
        if (!pat_has(r, primary, &pmt_pid)) {
            errset(error, "requested primary program is absent from current PAT");
            return -1;
        }
        primary_pmt = pmt_for_program((dt295_routes *)r, primary);
        if (!primary_pmt) {
            errset(error, "primary program PMT is incomplete");
            return -1;
        }
        if (!retained_primary && !program_has_av(primary_pmt)) {
            errset(error, "primary program has no supported video and audio pair");
            return -1;
        }
        p.has_primary = 1;
        p.primary_program = primary;
    } else if (!r->program_count) {
        errset(error, "PAT contains no programs");
        return -1;
    } else if (!p.missing_pmts && r->have_cat) {
        errset(error, "no advertised program has a supported video and audio pair");
        return -1;
    }
    uint16_t needed[DT295_PID_LIMIT];
    unsigned nn = 0;
    static const uint16_t fixed[] = {1, 16, 17, 18, 20, 35, 36, 37, 39, 40, 41};
    for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i)
        if (required_add(needed, &nn, fixed[i])) {
            errset(error, "PID set capacity exceeded");
            return -1;
        }
    for (unsigned i = 0; i < r->network_count; ++i)
        if (required_add(needed, &nn, r->network_pid[i])) {
            errset(error, "PID set capacity exceeded");
            return -1;
        }
    for (unsigned i = 0; i < r->program_count; ++i)
        if (required_add(needed, &nn, r->programs[i].pmt_pid)) {
            errset(error, "PID set capacity exceeded");
            return -1;
        }
    for (unsigned i = 0; i < r->cat_count; ++i)
        if (required_add(needed, &nn, r->cat_pid[i])) {
            errset(error, "PID set capacity exceeded");
            return -1;
        }
    uint16_t ecms[64];
    unsigned ne = 0;
    for (unsigned i = 0; i < r->pmt_count; ++i) {
        const pmt_info *q = &r->pmts[i];
        if (required_add(needed, &nn, q->pcr_pid)) {
            errset(error, "PID set capacity exceeded");
            return -1;
        }
        for (unsigned j = 0; j < q->ca_count; ++j)
            if (required_add(needed, &nn, q->ca_pid[j])) {
                errset(error, "PID set capacity exceeded");
                return -1;
            }
        if (q->program_ecm && !has_pid(ecms, ne, q->program_ecm)) {
            if (ne == 64) {
                errset(error, "more than 64 ECM PIDs advertised");
                return -1;
            }
            ecms[ne++] = q->program_ecm;
        }
        for (unsigned j = 0; j < q->streams; ++j) {
            if (required_add(needed, &nn, q->stream[j].pid)) {
                errset(error, "PID set capacity exceeded");
                return -1;
            }
            if (q->stream[j].ecm_pid && !has_pid(ecms, ne, q->stream[j].ecm_pid)) {
                if (ne == 64) {
                    errset(error, "more than 64 ECM PIDs advertised");
                    return -1;
                }
                ecms[ne++] = q->stream[j].ecm_pid;
            }
        }
    }
    /* Retain every registered mapping and its ECM ownership across PMT versions. */
    for (unsigned i = 0; i < DT295_SLOT_COUNT; ++i) {
        dt295_route *x = &p.routes[i];
        if (!x->enabled || i == 4)
            continue;
        if (x->is_ecm) {
            if (i != x->ecm_slot || i < 5 || p.ecm_pid[i] != x->pid) {
                errset(error, "invalid existing ECM slot mapping");
                return -1;
            }
            if (!has_pid(ecms, ne, x->pid)) {
                if (ne == 64) {
                    errset(error, "more than 64 retained ECM PIDs");
                    return -1;
                }
                ecms[ne++] = x->pid;
            }
        }
    }
    /* Register new ECMs first, retaining a live slot whenever possible. */
    for (unsigned k = 0; k < ne; ++k) {
        uint16_t ep = ecms[k];
        int existing = plan_find_pid(&p, ep);
        if (existing >= 0) {
            if (!p.routes[existing].is_ecm) {
                errset(error, "advertised ECM PID is already bound as a non-ECM route");
                return -1;
            }
            continue;
        }
        int slot = plan_find_free(&p, 5, 63, 1);
        if (slot < 0 || plan_add(&p, (unsigned)slot, ep, (uint16_t)slot, 1)) {
            errset(error, "ECM slot capacity exceeded");
            return -1;
        }
    }
    /* Dedicated roles are logical aliases; a shared PID gets one hardware slot. */
    if (primary_pmt) {
        uint16_t vp = 0, ap = 0;
        uint8_t vt = 0, at = 0;
        for (unsigned i = 0; i < primary_pmt->streams; ++i) {
            if (!vp && stream_video(primary_pmt->stream[i].type)) {
                vp = primary_pmt->stream[i].pid;
                vt = primary_pmt->stream[i].type;
            }
            if (!ap && stream_audio(primary_pmt->stream[i].type)) {
                ap = primary_pmt->stream[i].pid;
                at = primary_pmt->stream[i].type;
            }
        }
        p.video_stream_type = vt;
        p.audio_stream_type = at;
        uint16_t roles[4] = {vp, ap, primary_pmt->pmt_pid, primary_pmt->pcr_pid};
        for (unsigned i = 0; i < 4; ++i) {
            p.dedicated_pid[i] = roles[i];
            if (!roles[i])
                continue;
            int prior = plan_find_pid(&p, roles[i]);
            if (prior >= 0)
                continue;
            if (plan_add(&p, i, roles[i], 0, 0)) {
                errset(error, "dedicated route slot conflict");
                return -1;
            }
        }
    }
    /* Add every advertised/PAT/SI PID; never remove routes on natural disappearance. */
    for (unsigned i = 0; i < nn; ++i) {
        uint16_t pid = needed[i];
        if (pid == 0)
            continue;
        int existing = plan_find_pid(&p, pid);
        if (existing >= 0)
            continue;
        uint16_t binding = 0;
        for (unsigned j = 0; j < r->pmt_count; ++j)
            for (unsigned k = 0; k < r->pmts[j].streams; ++k)
                if (r->pmts[j].stream[k].pid == pid && r->pmts[j].stream[k].ecm_pid) {
                    uint16_t ep = r->pmts[j].stream[k].ecm_pid;
                    int es = plan_find_pid(&p, ep);
                    if (es < 0 || !p.routes[es].is_ecm) {
                        errset(error, "stream references an unregistered ECM PID");
                        return -1;
                    }
                    if (binding && binding != (uint16_t)es) {
                        errset(error, "shared PID has conflicting ECM bindings");
                        return -1;
                    }
                    binding = (uint16_t)es;
                }
        if (add_general(&p, pid, binding, 0)) {
            errset(error, "PID slot capacity exceeded or conflicting ECM binding");
            return -1;
        }
    }
    for (unsigned j = 0; j < r->pmt_count; ++j)
        for (unsigned k = 0; k < r->pmts[j].streams; ++k) {
            uint16_t pid = r->pmts[j].stream[k].pid;
            int s = plan_find_pid(&p, pid);
            if (s < 0)
                continue;
            uint16_t binding = 0;
            if (r->pmts[j].stream[k].ecm_pid)
                binding = (uint16_t)plan_find_pid(&p, r->pmts[j].stream[k].ecm_pid);
            dt295_route *existing = &p.routes[s];
            if (existing->is_ecm || (existing->ecm_slot && existing->ecm_slot != binding)) {
                errset(error, "live PID ECM rebind is unverified");
                return -1;
            }
            if (!existing->ecm_slot && binding)
                existing->ecm_slot = binding;
        }
    p.ready = r->have_cat && p.missing_pmts == 0;
    if (error)
        error[0] = 0;
    *desired = p;
    return 0;
}

static int plan_valid(const dt295_route_plan *p, char error[DT295_ROUTE_ERROR]) {
    unsigned count = 0;
    for (unsigned i = 0; i < DT295_SLOT_COUNT; ++i) {
        const dt295_route *r = &p->routes[i];
        if (!r->enabled)
            continue;
        count++;
        if (r->slot != i || r->pid >= 0x1fff || (i == 4 && r->pid != 0) || (i == 4 && r->is_ecm)) {
            errset(error, "invalid route slot or PID");
            return -1;
        }
        if (r->is_ecm && (i < 5 || r->ecm_slot != i || p->ecm_pid[i] != r->pid)) {
            errset(error, "invalid ECM slot ownership");
            return -1;
        }
        if (!r->is_ecm && r->ecm_slot) {
            if (r->ecm_slot >= 64 || !p->routes[r->ecm_slot].enabled ||
                !p->routes[r->ecm_slot].is_ecm) {
                errset(error, "unresolved ECM slot reference");
                return -1;
            }
        }
        for (unsigned j = i + 1; j < DT295_SLOT_COUNT; ++j)
            if (p->routes[j].enabled && p->routes[j].pid == r->pid) {
                errset(error, "duplicate PID registration");
                return -1;
            }
    }
    if (count != p->route_count) {
        errset(error, "route count does not match slots");
        return -1;
    }
    for (unsigned i = 0; i < DT295_SLOT_COUNT; ++i)
        if (p->ecm_pid[i] &&
            (!p->routes[i].enabled || !p->routes[i].is_ecm || p->routes[i].pid != p->ecm_pid[i])) {
            errset(error, "ECM inverse map mismatch");
            return -1;
        }
    return 0;
}

int dt295_routes_initial_ops(const dt295_route_plan *desired,
                             dt295_route_op ops[DT295_ROUTE_OP_LIMIT], size_t *count,
                             char error[DT295_ROUTE_ERROR]) {
    if (!desired || !ops || !count || plan_valid(desired, error))
        return -1;
    size_t n = 0;
    /* ECM routes must precede all routes that reference them. */
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned i = 0; i < DT295_SLOT_COUNT; ++i) {
            const dt295_route *r = &desired->routes[i];
            if (!r->enabled || i == 4 || (!!r->is_ecm != (pass == 0)))
                continue;
            if (n == DT295_ROUTE_OP_LIMIT) {
                errset(error, "route operation capacity exceeded");
                return -1;
            }
            ops[n++] = (dt295_route_op){(uint8_t)i, 1, r->pid, r->ecm_slot};
        }
    *count = n;
    if (error)
        error[0] = 0;
    return 0;
}

int dt295_routes_plan_ops(const dt295_route_plan *current, const dt295_route_plan *desired,
                          dt295_route_op ops[DT295_ROUTE_OP_LIMIT], size_t *count,
                          char error[DT295_ROUTE_ERROR]) {
    if (!current || !desired || !ops || !count || plan_valid(current, error) ||
        plan_valid(desired, error))
        return -1;
    size_t n = 0;
    for (unsigned i = 0; i < 64; ++i) {
        const dt295_route *old = &current->routes[i], *want = &desired->routes[i];
        if (old->enabled) {
            if (!want->enabled || old->pid != want->pid || old->ecm_slot != want->ecm_slot ||
                old->is_ecm != want->is_ecm) {
                errset(error, "desired plan removes or changes an active route");
                return -1;
            }
        }
    }
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned i = 0; i < 64; ++i) {
            const dt295_route *want = &desired->routes[i];
            if (!want->enabled || current->routes[i].enabled || i == 4 ||
                (!!want->is_ecm != (pass == 0)))
                continue;
            if (n == DT295_ROUTE_OP_LIMIT) {
                errset(error, "route operation capacity exceeded");
                return -1;
            }
            ops[n++] = (dt295_route_op){(uint8_t)i, 1, want->pid, want->ecm_slot};
        }
    *count = n;
    if (error)
        error[0] = 0;
    return 0;
}

int dt295_routes_guard(const dt295_route_plan *current, const dt295_route_op *ops, size_t n,
                       dt295_route_plan *candidate, char error[DT295_ROUTE_ERROR]) {
    if (!current || (!ops && n) || !candidate || n > DT295_ROUTE_OP_LIMIT ||
        plan_valid(current, error))
        return -1;
    dt295_route_plan c = *current;
    for (size_t k = 0; k < n; ++k) {
        const dt295_route_op *o = &ops[k];
        if (o->slot >= 64 || o->enabled > 1 || o->pid >= 0x1fff || o->ecm_slot >= 64 ||
            o->slot == 4) {
            errset(error, "route operation is outside supported ranges");
            return -1;
        }
        dt295_route *old = &c.routes[o->slot];
        if (!o->enabled) {
            if (!old->enabled)
                continue;
            if (old->pid != o->pid || old->ecm_slot != o->ecm_slot) {
                errset(error, "disable does not match the active route");
                return -1;
            }
            if (old->is_ecm)
                for (unsigned i = 0; i < 64; ++i)
                    if (i != o->slot && c.routes[i].enabled && c.routes[i].ecm_slot == o->slot) {
                        errset(error, "cannot remove an ECM slot while referenced");
                        return -1;
                    }
            memset(old, 0, sizeof(*old));
            c.ecm_pid[o->slot] = 0;
            c.route_count--;
            continue;
        }
        int is_ecm = o->slot >= 5 && o->ecm_slot == o->slot;
        for (unsigned i = 0; i < 64; ++i)
            if (i != o->slot && c.routes[i].enabled && c.routes[i].pid == o->pid) {
                errset(error, "duplicate PID or ECM registration prohibited");
                return -1;
            }
        if (old->enabled) {
            if (old->pid != o->pid) {
                errset(error, "cannot replace an active route slot");
                return -1;
            }
            if (old->ecm_slot != o->ecm_slot || old->is_ecm != !!is_ecm) {
                errset(error, "live ECM rebind or slot replacement is unverified");
                return -1;
            }
            continue;
        }
        if (!is_ecm && o->ecm_slot) {
            if (!c.routes[o->ecm_slot].enabled || !c.routes[o->ecm_slot].is_ecm) {
                errset(error, "route references an unregistered ECM slot");
                return -1;
            }
        }
        c.routes[o->slot] = (dt295_route){o->slot, 1, (uint8_t)is_ecm, 0, o->pid, o->ecm_slot};
        if (is_ecm)
            c.ecm_pid[o->slot] = o->pid;
        c.route_count++;
    }
    if (plan_valid(&c, error))
        return -1;
    *candidate = c;
    if (error)
        error[0] = 0;
    return 0;
}

void dt295_routes_commit(dt295_route_plan *live, const dt295_route_plan *candidate) {
    if (live && candidate)
        *live = *candidate;
}
