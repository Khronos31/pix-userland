/* SPDX-License-Identifier: MIT */
#ifndef PIX_DT295_ROUTES_H
#define PIX_DT295_ROUTES_H

#include <stddef.h>
#include <stdint.h>

#define DT295_PID_LIMIT 8192u
#define DT295_SLOT_COUNT 64u
#define DT295_ROUTE_OP_LIMIT 64u
#define DT295_ROUTE_ERROR 128u

typedef struct dt295_routes dt295_routes;

typedef struct {
    uint8_t slot;
    uint8_t enabled;
    uint8_t is_ecm;
    uint8_t reserved;
    uint16_t pid;
    uint16_t ecm_slot;
} dt295_route;

typedef struct {
    uint8_t slot;
    uint8_t enabled;
    uint16_t pid;
    uint16_t ecm_slot;
} dt295_route_op;

typedef struct {
    dt295_route routes[DT295_SLOT_COUNT];
    uint16_t ecm_pid[DT295_SLOT_COUNT]; /* zero means unused */
    uint8_t route_count;
    uint8_t ready; /* PAT and every advertised PMT complete */
    uint8_t cat_received;
    uint8_t has_primary;
    uint16_t dedicated_pid[4]; /* V, A, PMT, PCR logical roles; zero means disabled */
    uint8_t video_stream_type, audio_stream_type;
    uint16_t primary_program;
    uint16_t missing_pmts;
} dt295_route_plan;

typedef struct {
    uint16_t program;
    uint16_t pmt_pid;
} dt295_pat_program;

typedef struct {
    uint8_t have_pat, have_cat;
    uint16_t tsid;
    uint16_t program_count;
    dt295_pat_program programs[64];
    uint16_t missing_pmts;
    uint8_t ready;
    uint32_t invalid_sections, continuity_errors, unsupported_tables;
} dt295_routes_snapshot;

/* The watcher owns bounded memory; create returns NULL on allocation failure. */
dt295_routes *dt295_routes_create(void);
/* Satellite streams may omit CAT; terrestrial readiness keeps requiring it. */
void dt295_routes_set_satellite(dt295_routes *, int satellite);
void dt295_routes_destroy(dt295_routes *);
void dt295_routes_reset(dt295_routes *);
/* Feed one complete 188-byte TS packet. 0=accepted/ignored, -1=bad arguments. */
int dt295_routes_feed(dt295_routes *, const uint8_t packet[188]);
/* Version policy is fail-closed: accept forward modulo-32 steps 1..15 (including
 * 31->0), ignore the immediately superseded version, and count other backward or
 * half-cycle transitions as unsupported. A changed TSID resets table identity. */
void dt295_routes_snapshot_get(const dt295_routes *, dt295_routes_snapshot *);

/* Build the smallest bootstrap route plan, even while PAT-listed PMTs are missing. */
int dt295_routes_initial_plan(const dt295_routes *, dt295_route_plan *,
                              char error[DT295_ROUTE_ERROR]);
/* Recompute bootstrap requirements while retaining all previously assigned slots. */
int dt295_routes_bootstrap_plan(const dt295_routes *, const dt295_route_plan *current,
                                dt295_route_plan *desired, char error[DT295_ROUTE_ERROR]);
/* Build a complete desired plan. primary_program=0 selects the first decodable AV program. */
int dt295_routes_build(const dt295_routes *, uint16_t primary_program,
                       const dt295_route_plan *current, dt295_route_plan *desired,
                       char error[DT295_ROUTE_ERROR]);
/* Produce additions only; this is intended for initial acquisition/bootstrap. */
int dt295_routes_initial_ops(const dt295_route_plan *, dt295_route_op ops[DT295_ROUTE_OP_LIMIT],
                             size_t *count, char error[DT295_ROUTE_ERROR]);
int dt295_routes_plan_ops(const dt295_route_plan *current, const dt295_route_plan *desired,
                          dt295_route_op ops[DT295_ROUTE_OP_LIMIT], size_t *count,
                          char error[DT295_ROUTE_ERROR]);
/* Validate the entire operation list before returning a candidate state. */
int dt295_routes_guard(const dt295_route_plan *, const dt295_route_op *, size_t,
                       dt295_route_plan *candidate, char error[DT295_ROUTE_ERROR]);
/* Caller invokes only after every corresponding hardware operation succeeded. */
void dt295_routes_commit(dt295_route_plan *live, const dt295_route_plan *candidate);

#endif
