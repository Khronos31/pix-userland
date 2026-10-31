/* SPDX-License-Identifier: MIT */
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

#endif
