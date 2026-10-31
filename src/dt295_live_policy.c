/* SPDX-License-Identifier: MIT */
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
