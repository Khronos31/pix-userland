/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dt295_live_policy.h"

#include <stdio.h>
#include <string.h>

/* During a recording, allow only additions to genuinely free slots. Existing
 * PID/ECM registrations and the dedicated decoder roles must remain identical.
 * A new stream may reuse an ECM route that is already active, but may not add
 * or change any ECM route. */
static int live_pid_is_retained(const dt295_route_plan *current,
                                const dt295_route_plan *desired, unsigned pid) {
    for (unsigned i = 0; i < DT295_SLOT_COUNT; ++i) {
        const dt295_route *old = &current->routes[i];
        if (old->enabled && old->pid == pid)
            return desired->routes[i].enabled && desired->routes[i].pid == old->pid &&
                   desired->routes[i].ecm_slot == old->ecm_slot &&
                   desired->routes[i].is_ecm == old->is_ecm;
    }
    return 0;
}
int dt295_primary_has_no_ecm_dependency(const dt295_route_plan *plan) {
    if (!plan || !plan->has_primary)
        return 0;
    unsigned av_routes = 0;
    for (unsigned role = 0; role < 2; ++role) {
        uint16_t pid = plan->dedicated_pid[role];
        if (!pid)
            continue;
        int found = 0;
        for (unsigned i = 0; i < DT295_SLOT_COUNT; ++i) {
            const dt295_route *route = &plan->routes[i];
            if (route->enabled && route->pid == pid) {
                if (route->is_ecm || route->ecm_slot)
                    return 0;
                found = 1;
                break;
            }
        }
        if (!found)
            return 0;
        av_routes++;
    }
    return av_routes != 0;
}

int dt295_initial_ecm_gate_allowed(const dt295_route_plan *plan) {
    if (!plan)
        return 0;
    unsigned ecms = 0;
    for (unsigned i = 0; i < DT295_SLOT_COUNT; ++i)
        if (plan->routes[i].enabled && plan->routes[i].is_ecm)
            ecms++;
    return ecms <= 1 || dt295_primary_has_no_ecm_dependency(plan);
}

static int same_plan_route(const dt295_route *left, const dt295_route *right) {
    return left->slot == right->slot && left->enabled == right->enabled &&
           left->is_ecm == right->is_ecm && left->pid == right->pid &&
           left->ecm_slot == right->ecm_slot;
}

int live_initial_pmt_completion_only(const dt295_routes_snapshot *initial,
                                     const dt295_routes_snapshot *snapshot,
                                     const dt295_route_plan *current,
                                     dt295_route_plan *desired,
                                     const dt295_route_op *ops, size_t count,
                                     char *error, size_t error_size) {
#define COMPLETION_REJECT(condition, message) \
    do { \
        if (condition) { \
            snprintf(error, error_size, "%s", message); \
            return -1; \
        } \
    } while (0)
    COMPLETION_REJECT(!initial || !snapshot || !current || !desired || !ops,
                      "initial completion has missing state");
    COMPLETION_REJECT(!initial->have_pat || !snapshot->have_pat ||
                      initial->have_cat != snapshot->have_cat || initial->tsid != snapshot->tsid ||
                      initial->program_count != snapshot->program_count ||
                      memcmp(initial->programs, snapshot->programs,
                             initial->program_count * sizeof(initial->programs[0])),
                      "PAT/TSID changed during initial completion");
    COMPLETION_REJECT(initial->cat_version_valid != snapshot->cat_version_valid ||
                      initial->cat_version != snapshot->cat_version ||
                      snapshot->unsupported_tables || initial->invalid_sections ||
                      snapshot->invalid_sections != initial->invalid_sections ||
                      snapshot->continuity_errors != initial->continuity_errors,
                      "CAT or malformed tables changed during initial completion");
    COMPLETION_REJECT(snapshot->missing_pmts >= initial->missing_pmts,
                      "PMT completion made no progress");
    COMPLETION_REJECT(!count, "initial PMT completion has no route additions");
    COMPLETION_REJECT(current->ready,
                      "initial live plan is already complete");
    COMPLETION_REJECT(desired->ready != snapshot->ready ||
                      desired->missing_pmts != snapshot->missing_pmts,
                      "desired live plan does not match its PMT snapshot");
    COMPLETION_REJECT(current->primary_program != desired->primary_program ||
                      current->has_primary != desired->has_primary ||
                      current->video_stream_type != desired->video_stream_type ||
                      current->audio_stream_type != desired->audio_stream_type ||
                      memcmp(current->dedicated_pid, desired->dedicated_pid,
                             sizeof(current->dedicated_pid)) ||
                      !dt295_primary_has_no_ecm_dependency(current) ||
                      !dt295_primary_has_no_ecm_dependency(desired),
                      "primary roles or ECM binding changed during initial completion");
#undef COMPLETION_REJECT
    unsigned completed = 0;
    for (unsigned i = 0; i < initial->program_count; ++i) {
        if (initial->pmt_present[i]) {
            if (!snapshot->pmt_present[i] ||
                initial->pmt_version[i] != snapshot->pmt_version[i]) {
                snprintf(error, error_size, "known PMT changed during initial completion");
                return -1;
            }
        } else if (snapshot->pmt_present[i]) {
            completed++;
        }
    }
    if (!completed) {
        snprintf(error, error_size, "no initially missing PMT was completed");
        return -1;
    }
    for (unsigned i = 0; i < DT295_SLOT_COUNT; ++i) {
        if (current->routes[i].enabled &&
            (!desired->routes[i].enabled ||
             !same_plan_route(&current->routes[i], &desired->routes[i]) ||
             current->ecm_pid[i] != desired->ecm_pid[i])) {
            snprintf(error, error_size, "initial completion changes an existing route binding");
            return -1;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        const dt295_route_op *op = &ops[i];
        if (!op->enabled || op->slot >= DT295_SLOT_COUNT ||
            current->routes[op->slot].enabled || !desired->routes[op->slot].enabled ||
            desired->routes[op->slot].slot != op->slot ||
            desired->routes[op->slot].pid != op->pid ||
            desired->routes[op->slot].ecm_slot != op->ecm_slot) {
            snprintf(error, error_size, "initial completion requires a slot replacement");
            return -1;
        }
    }
    return 0;
}

int live_additions_only(const dt295_route_plan *current,
                        dt295_route_plan *desired,
                        const dt295_route_op *ops, size_t count,
                        char *error, size_t error_size) {
    if (current->has_primary && !desired->has_primary && !desired->primary_program &&
        !desired->video_stream_type && !desired->audio_stream_type &&
        !desired->dedicated_pid[0] && !desired->dedicated_pid[1] &&
        !desired->dedicated_pid[2] && !desired->dedicated_pid[3]) {
        int retained = 1;
        for (unsigned i = 0; i < 4; ++i)
            if (current->dedicated_pid[i] &&
                !live_pid_is_retained(current, desired, current->dedicated_pid[i]))
                retained = 0;
        if (retained) {
            desired->primary_program = current->primary_program;
            desired->has_primary = current->has_primary;
            desired->video_stream_type = current->video_stream_type;
            desired->audio_stream_type = current->audio_stream_type;
            memcpy(desired->dedicated_pid, current->dedicated_pid,
                   sizeof(desired->dedicated_pid));
        }
    }
    if (current->primary_program != desired->primary_program ||
        current->has_primary != desired->has_primary) {
        snprintf(error, error_size,
                 "broadcast changed primary or dedicated PID metadata during recording");
        return -1;
    }
    for (unsigned i = 0; i < 4; ++i) {
        uint16_t old_pid = current->dedicated_pid[i];
        uint16_t new_pid = desired->dedicated_pid[i];
        if (old_pid == new_pid)
            continue;
        if (old_pid && !new_pid && live_pid_is_retained(current, desired, old_pid)) {
            /* Keep the configured alias while its advertised PID is absent. */
            desired->dedicated_pid[i] = old_pid;
            continue;
        }
        snprintf(error, error_size,
                 "broadcast changed primary or dedicated PID metadata during recording");
        return -1;
    }
    if (current->video_stream_type != desired->video_stream_type) {
        if (current->video_stream_type && !desired->video_stream_type &&
            current->dedicated_pid[0] &&
            live_pid_is_retained(current, desired, current->dedicated_pid[0]))
            desired->video_stream_type = current->video_stream_type;
        else {
            snprintf(error, error_size,
                     "broadcast changed primary or dedicated PID metadata during recording");
            return -1;
        }
    }
    if (current->audio_stream_type != desired->audio_stream_type) {
        if (current->audio_stream_type && !desired->audio_stream_type &&
            current->dedicated_pid[1] &&
            live_pid_is_retained(current, desired, current->dedicated_pid[1]))
            desired->audio_stream_type = current->audio_stream_type;
        else {
            snprintf(error, error_size,
                     "broadcast changed primary or dedicated PID metadata during recording");
            return -1;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        const dt295_route_op *op = &ops[i];
        if (!op->enabled || op->slot >= DT295_SLOT_COUNT ||
            current->routes[op->slot].enabled || !desired->routes[op->slot].enabled) {
            snprintf(error, error_size, "live route addition does not target a free slot");
            return -1;
        }
        const dt295_route *route = &desired->routes[op->slot];
        if (route->slot != op->slot || route->pid != op->pid ||
            route->ecm_slot != op->ecm_slot || route->is_ecm) {
            snprintf(error, error_size,
                     "live route change requires an unsupported ECM or slot replacement");
            return -1;
        }
        if (route->ecm_slot) {
            unsigned ecm_slot = route->ecm_slot;
            if (ecm_slot >= DT295_SLOT_COUNT || !current->routes[ecm_slot].enabled ||
                !current->routes[ecm_slot].is_ecm ||
                !desired->routes[ecm_slot].enabled || !desired->routes[ecm_slot].is_ecm ||
                current->ecm_pid[ecm_slot] != desired->ecm_pid[ecm_slot] ||
                current->routes[ecm_slot].pid != desired->routes[ecm_slot].pid) {
                snprintf(error, error_size,
                         "live PID requires a new or changed ECM route");
                return -1;
            }
        }
    }
    return 0;
}
