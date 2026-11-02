/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PIX_DT295_LIVE_POLICY_H
#define PIX_DT295_LIVE_POLICY_H

#include "dt295_routes.h"

/* Validate a route plan update observed while a recording is active.
 * Returns 0 when the effective plan only adds new streams to genuinely free
 * slots, and -1 with a message in error otherwise. The caller may adjust
 * desired (metadata carried over from current) before applying the plan. */
int live_additions_only(const dt295_route_plan *current, dt295_route_plan *desired,
                        const dt295_route_op *ops, size_t count, char *error,
                        size_t error_size);
int dt295_primary_has_no_ecm_dependency(const dt295_route_plan *plan);
int dt295_initial_ecm_gate_allowed(const dt295_route_plan *plan,
                                   uint32_t invalid_sections);
int live_initial_pmt_completion_only(const dt295_routes_snapshot *initial,
                                     const dt295_routes_snapshot *current_snapshot,
                                     const dt295_route_plan *current,
                                     dt295_route_plan *desired,
                                     const dt295_route_op *ops, size_t count,
                                     char *error, size_t error_size);

#endif
