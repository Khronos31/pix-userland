#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
/* SPDX-License-Identifier: MIT */
#include "dt295_capture.h"
#include "dt295.h"
#include "dt295_routes.h"
#include "dt295_stream.h"
#include "os.h"
#include "pixy.h"
#include "transport.h"
#include "ts_output.h"
#include "usb_backend.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#if defined(__APPLE__) || defined(DT295_CAPTURE_TESTING)
typedef struct {
    usb_backend *usb;
    dt295_stream *stream;
    uint64_t last_sequence, last_completed_ns;
} capture_usb;
static uint64_t capture_now(void *p) {
    (void)p;
    return now_ms();
}
static void capture_sleep(void *p, unsigned ms) {
    (void)p;
    sleep_ms(ms);
}
static int capture_control(void *p, uint8_t rt, uint8_t req, uint16_t value, uint16_t index,
                           uint8_t *data, size_t length, unsigned ms, size_t *actual) {
    capture_usb *u = p;
    if (length > UINT16_MAX)
        return LIBUSB_ERROR_INVALID_PARAM;
    int r = libusb_control_transfer(usb_native_handle(u->usb), rt, req, value, index, data,
                                    (uint16_t)length, ms);
    *actual = r < 0 ? 0 : (size_t)r;
    return r < 0 ? r : 0;
}
static int capture_bulk(void *p, uint8_t ep, uint8_t *data, size_t length, unsigned ms,
                        size_t *actual) {
    capture_usb *u = p;
    *actual = 0;
    if (ep == 0x81 && u->stream) {
        int r = dt295_stream_read(u->stream, data, length, &u->last_sequence, &u->last_completed_ns,
                                  ms);
        if (r > 0) {
            *actual = (size_t)r;
            return 0;
        }
        return r == 0 ? LIBUSB_ERROR_TIMEOUT : r;
    }
    if (length > INT_MAX)
        return LIBUSB_ERROR_INVALID_PARAM;
    int n = 0;
    int r = libusb_bulk_transfer(usb_native_handle(u->usb), ep, data, (int)length, &n, ms);
    if (n > 0)
        *actual = (size_t)n;
    return r;
}
static int capture_interrupt(void *p, uint8_t ep, uint8_t *data, size_t length, unsigned ms,
                             size_t *actual) {
    capture_usb *u = p;
    if (length > INT_MAX)
        return LIBUSB_ERROR_INVALID_PARAM;
    int n = 0;
    int r = libusb_interrupt_transfer(usb_native_handle(u->usb), ep, data, (int)length, &n, ms);
    *actual = n > 0 ? (size_t)n : 0;
    return r;
}
static int output_exit(int error) {
    return error == ENOBUFS || error == EAGAIN || error == ETIMEDOUT ? 8 : 70;
}
/* Only a new required PID/binding needs a device mutation. Natural table removal
 * leaves registered routes in place until this recording ends. */
static int missing_route(const dt295_route_plan *current, const dt295_route_plan *desired) {
    for (unsigned i = 0; i < DT295_SLOT_COUNT; ++i) {
        const dt295_route *r = &desired->routes[i];
        if (!r->enabled)
            continue;
        int found = 0;
        for (unsigned j = 0; j < DT295_SLOT_COUNT; ++j) {
            const dt295_route *a = &current->routes[j];
            if (!a->enabled || a->pid != r->pid || a->is_ecm != r->is_ecm)
                continue;
            unsigned ae = a->ecm_slot ? current->ecm_pid[a->ecm_slot] : 0;
            unsigned re = r->ecm_slot ? desired->ecm_pid[r->ecm_slot] : 0;
            if (ae == re) {
                found = 1;
                break;
            }
        }
        if (!found)
            return 1;
    }
    return 0;
}
static int apply_plan(dt295 *d, dt295_route_plan *current, const dt295_route_plan *desired,
                      char *error, size_t error_size) {
    dt295_route_op ops[DT295_ROUTE_OP_LIMIT];
    size_t count = 0;
    char route_error[DT295_ROUTE_ERROR] = {0};
    for (unsigned i = 0; i < DT295_SLOT_COUNT; ++i) {
        const dt295_route *r = &desired->routes[i], *old = &current->routes[i];
        if (!r->enabled || i == 4)
            continue;
        if (old->enabled && old->pid == r->pid && old->ecm_slot == r->ecm_slot &&
            old->is_ecm == r->is_ecm)
            continue;
        /* ECMs precede their users even if their slot index is high. */
        if (r->is_ecm)
            ops[count++] = (dt295_route_op){r->slot, 1, r->pid, r->ecm_slot};
    }
    for (unsigned i = 0; i < DT295_SLOT_COUNT; ++i) {
        const dt295_route *r = &desired->routes[i], *old = &current->routes[i];
        if (!r->enabled || i == 4 || r->is_ecm)
            continue;
        if (old->enabled && old->pid == r->pid && old->ecm_slot == r->ecm_slot &&
            old->is_ecm == r->is_ecm)
            continue;
        ops[count++] = (dt295_route_op){r->slot, 1, r->pid, r->ecm_slot};
    }
    dt295_route_plan candidate;
    if (dt295_routes_guard(current, ops, count, &candidate, route_error)) {
        snprintf(error, error_size, "%s", route_error);
        return -1;
    }
    for (size_t i = 0; i < count; ++i)
        if (dt295_route_write(d, &ops[i], error, error_size))
            return -1;
    /* The hardware may be partially changed on failure; never commit that path. */
    candidate.ready = desired->ready;
    candidate.cat_received = desired->cat_received;
    candidate.missing_pmts = desired->missing_pmts;
    candidate.primary_program = desired->primary_program;
    candidate.has_primary = desired->has_primary;
    candidate.video_stream_type = desired->video_stream_type;
    candidate.audio_stream_type = desired->audio_stream_type;
    memcpy(candidate.dedicated_pid, desired->dedicated_pid, sizeof(candidate.dedicated_pid));
    dt295_routes_commit(current, &candidate);
    return 0;
}
static void capture_message(ts_output *diag, const char *text) {
    char line[1024];
    size_t used = 0;
    const char *prefix = "{\"event\":\"detail\",\"message\":\"";
    memcpy(line, prefix, strlen(prefix));
    used = strlen(prefix);
    for (const unsigned char *p = (const unsigned char *)text; *p && used + 8 < sizeof(line); p++) {
        if (*p == '"' || *p == '\\') {
            line[used++] = '\\';
            line[used++] = (char)*p;
        } else if (*p < 32)
            line[used++] = ' ';
        else
            line[used++] = (char)*p;
    }
    memcpy(line + used, "\"}\n", 3);
    used += 3;
    ts_output_submit(diag, line, used, NULL);
}
static int capture_status(ts_output *diag, unsigned phase, uint64_t packets, uint64_t elapsed,
                          uint64_t renewals, dt295_stream *stream, ts_output *output,
                          const dt295_stream_stats *final, const dt295_routes *tables) {
    dt295_stream_stats usb = {0};
    ts_output_stats writer = {0};
    dt295_routes_snapshot routes = {0};
    if (stream)
        dt295_stream_snapshot(stream, &usb);
    else if (final)
        usb = *final;
    ts_output_snapshot(output, &writer);
    if (tables)
        dt295_routes_snapshot_get(tables, &routes);
    char line[4096];
    int n = snprintf(
        line, sizeof(line),
        "{\"model\":\"DT295\",\"phase\":%u,\"packets\":%llu,\"elapsed_ms\":%llu,"
        "\"renewals\":%llu,\"usb_bytes\":%llu,\"usb_delivered_bytes\":%llu,"
        "\"usb_late_actual_bytes\":%llu,\"usb_pending_blocks\":%llu,\"usb_active_transfers\":%llu,"
        "\"usb_peak_pending_blocks\":%llu,\"usb_cancellations\":%llu,"
        "\"usb_last_error_status\":%llu,\"usb_last_error_actual\":%llu,"
        "\"usb_error\":%d,\"usb_timeouts\":%llu,\"confirmed_bytes\":%llu,"
        "\"pending_bytes\":%llu,\"rejected_bytes\":%llu,\"uncertain_bytes\":%llu,"
        "\"have_pat\":%u,\"have_cat\":%u,\"program_count\":%u,\"missing_pmts\":%u,"
        "\"tables_ready\":%u,\"invalid_sections\":%u,\"continuity_errors\":%u,"
        "\"unsupported_tables\":%u,"
        "\"usb_max_gap_ns\":%llu,\"queue_peak_bytes\":%llu,\"writer_child_pid\":%lld,"
        "\"writer_warning\":%d,\"writer_stalled\":%d,\"writer_error\":%d,"
        "\"writer_write_ns_total\":%llu,\"writer_write_ns_max\":%llu,"
        "\"writer_write_records\":%llu,\"max_pending_age_ns\":%llu,"
        "\"oldest_pending_age_ns\":%llu,\"max_parent_poll_gap_ns\":%llu,"
        "\"max_parent_ack_gap_ns\":%llu,\"send_eagain_count\":%llu,"
        "\"ack_eagain_count\":%llu,\"zero_poll_budget_exhaustions\":%llu}\n",
        phase, (unsigned long long)packets, (unsigned long long)elapsed,
        (unsigned long long)renewals, (unsigned long long)usb.completed_bytes,
        (unsigned long long)usb.delivered_bytes, (unsigned long long)usb.late_actual_bytes,
        (unsigned long long)usb.pending_blocks, (unsigned long long)usb.active_transfers,
        (unsigned long long)usb.peak_pending_blocks, (unsigned long long)usb.cancellations,
        (unsigned long long)usb.last_error_status, (unsigned long long)usb.last_error_actual,
        usb.first_error, (unsigned long long)usb.timeouts,
        (unsigned long long)writer.confirmed_bytes, (unsigned long long)writer.pending_bytes,
        (unsigned long long)writer.rejected_bytes, (unsigned long long)writer.uncertain_bytes,
        (unsigned)routes.have_pat, (unsigned)routes.have_cat, (unsigned)routes.program_count,
        (unsigned)routes.missing_pmts, (unsigned)routes.ready, (unsigned)routes.invalid_sections,
        (unsigned)routes.continuity_errors, (unsigned)routes.unsupported_tables,
        (unsigned long long)usb.max_completion_gap_ns,
        (unsigned long long)writer.peak_pending_bytes, (long long)writer.child_pid, writer.warning,
        writer.stalled, writer.first_error,
        (unsigned long long)writer.writer_write_ns_total,
        (unsigned long long)writer.writer_write_ns_max,
        (unsigned long long)writer.writer_write_records,
        (unsigned long long)writer.max_pending_age_ns,
        (unsigned long long)writer.oldest_pending_age_ns,
        (unsigned long long)writer.max_parent_poll_gap_ns,
        (unsigned long long)writer.max_parent_ack_gap_ns,
        (unsigned long long)writer.send_eagain_count,
        (unsigned long long)writer.ack_eagain_count,
        (unsigned long long)writer.zero_poll_budget_exhaustions);
    return n < 0 || (size_t)n >= sizeof(line) ? EOVERFLOW
                                              : ts_output_submit(diag, line, (size_t)n, NULL);
}

#endif
int dt295_capture_run(const dt295_capture_config *cfg) {
#if !defined(__APPLE__) && !defined(DT295_CAPTURE_TESTING)
    (void)cfg;
    fprintf(stderr, "DT295 capture currently supports macOS terrestrial target 0 only\n");
    return 2;
#else
    if (!cfg || !cfg->assets || !cfg->output || cfg->channel < 13 || cfg->channel > 62)
        return 2;
    char error[256] = {0}, stop_error[256] = {0}, shutdown_error[256] = {0},
         diag_path[4096];
    int rc = 0, recording = 0, native_abandoned = 0, lock_fd = -1;
    dt295_stream_stats final_usb = {0};
    uint64_t packets = 0, renewals = 0, record_start = 0, elapsed = 0;
    size_t remainder = 0, drained = 0;
    dt295_assets *assets = NULL;
    dt295 *d = NULL;
    dt295_routes *tables = NULL;
    ts_output *output = NULL, *diag = NULL;
    capture_usb u = {0};
    ts_check *check = calloc(1, sizeof(*check));
    if (!check)
        return 70;
    ts_check_init(check);
    char asset_path[4096];
    int path_size = snprintf(asset_path, sizeof(asset_path), "%s/dt295.assets", cfg->assets);
    if (path_size < 0 || (size_t)path_size >= sizeof(asset_path)) {
        rc = 10;
        goto done;
    }
    if (dt295_assets_load(asset_path, &assets, error, sizeof(error))) {
        rc = 10;
        goto done;
    }
    char lock_path[160];
    snprintf(lock_path, sizeof(lock_path), "/tmp/pix-userland-dt295-%lu.lock",
             (unsigned long)getuid());
    lock_fd = open(lock_path, O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
    struct stat lock_info;
    struct flock lock_request = {0};
    lock_request.l_type = F_WRLCK;
    lock_request.l_whence = SEEK_SET;
    if (lock_fd < 0 || fstat(lock_fd, &lock_info) || !S_ISREG(lock_info.st_mode) ||
        lock_info.st_uid != getuid() || fcntl(lock_fd, F_SETLK, &lock_request)) {
        rc = 4;
        snprintf(error, sizeof(error), "DT295 recording lock is busy or unavailable");
        goto done;
    }
    tables = dt295_routes_create();
    if (!tables) {
        rc = 70;
        goto done;
    }
    /* Both helpers must be created before libusb can create internal threads. */
    int e = ts_output_open(cfg->output, STDOUT_FILENO, NULL, &output);
    if (e) {
        rc = output_exit(e);
        snprintf(error, sizeof(error), "TS output: %s", strerror(e));
        goto done;
    }
    const char *diag_name = "-";
    if (cfg->diagnostic) {
        if (mkdir(cfg->diagnostic, 0700)) {
            rc = 70;
            snprintf(error, sizeof(error), "diagnostic directory: %s", strerror(errno));
            goto done;
        }
        int n = snprintf(diag_path, sizeof(diag_path), "%s/status.jsonl", cfg->diagnostic);
        if (n < 0 || (size_t)n >= sizeof(diag_path)) {
            rc = 70;
            goto done;
        }
        diag_name = diag_path;
    }
    ts_output_config diag_config = {0};
    diag_config.capacity_bytes = 8 * 1024 * 1024;
    e = ts_output_open(diag_name, STDERR_FILENO, &diag_config, &diag);
    if (e) {
        rc = 70;
        goto done;
    }
    rc = usb_open_model(&u.usb, &cfg->selector, cfg->fd, cfg->detach, USB_MODEL_DT295);
    if (rc)
        goto done;
    dt295_io io = {&u,          capture_control, capture_bulk, capture_interrupt,
                   capture_now, capture_sleep};
    if (dt295_create(&d, &io, assets, error, sizeof(error))) {
        rc = 70;
        goto done;
    }
    if (dt295_boot(d, error, sizeof(error)) || dt295_open_session(d, error, sizeof(error))) {
        rc = 7;
        goto done;
    }
    if (dt295_card_initialize(d, error, sizeof(error)) ||
        dt295_card_activate(d, 2000, error, sizeof(error))) {
        rc = 9;
        goto done;
    }
    if (dt295_tune_terrestrial(d, cfg->channel, error, sizeof(error))) {
        rc = 5;
        goto done;
    }
    dt295_route_plan live = {0}, bootstrap;
    char route_error[DT295_ROUTE_ERROR] = {0};
    dt295_route_op initial[DT295_ROUTE_OP_LIMIT];
    size_t initial_count = 0;
    if (dt295_routes_initial_plan(tables, &bootstrap, route_error) ||
        dt295_routes_initial_ops(&bootstrap, initial, &initial_count, route_error)) {
        snprintf(error, sizeof(error), "%s", route_error);
        rc = 8;
        goto done;
    }
    if (dt295_prepare_plan(d, &bootstrap, error, sizeof(error))) {
        rc = 7;
        goto done;
    }
    live = bootstrap;
    if (dt295_start(d, error, sizeof(error))) {
        rc = 7;
        goto done;
    }
    e = dt295_stream_start(usb_native_context(u.usb), usb_native_handle(u.usb), &u.stream);
    if (e) {
        rc = 7;
        snprintf(error, sizeof(error), "native USB start error %d", e);
        goto done;
    }
    uint64_t preparation_start = now_ms(), last_input = preparation_start,
             next_status = preparation_start;
    uint8_t data[65536 + 188], out[65536 + 188];
    while (!cfg->interrupted || !*cfg->interrupted) {
        uint64_t now = now_ms();
        if (recording) {
            elapsed = now - record_start;
            if (cfg->seconds && elapsed >= (uint64_t)cfg->seconds * 1000)
                break;
        } else if (now - preparation_start >= 30000) {
            rc = 5;
            snprintf(error, sizeof(error), "incomplete initial PAT/PMT/CAT after 30 seconds");
            break;
        }
        e = ts_output_poll(output, 0);
        if (!e)
            e = ts_output_poll(diag, 0);
        if (e) {
            rc = output_exit(e);
            snprintf(error, sizeof(error), "output progress: %s", strerror(e));
            break;
        }
        int renewed = 0;
        if (dt295_renew_if_due(d, &renewed, error, sizeof(error))) {
            rc = 7;
            break;
        }
        renewals += (unsigned)renewed;
        size_t actual = 0;
        e = capture_bulk(&u, 0x81, data + remainder, 65536, 100, &actual);
        if (e && !(e == LIBUSB_ERROR_TIMEOUT && actual)) {
            if (e == LIBUSB_ERROR_TIMEOUT && now_ms() - last_input < 10000)
                continue;
            rc = 7;
            snprintf(error, sizeof(error), "USB input error %d", e);
            break;
        }
        if (!actual)
            continue;
        last_input = now_ms();
        size_t total = remainder + actual, length = total / 188 * 188, emitted = 0;
        for (size_t at = 0; at < length; at += 188) {
            uint8_t *packet = data + at;
            if (dt295_decode_packet(d, packet) < 0) {
                rc = 8;
                snprintf(error, sizeof(error), "invalid DT295 TS packet");
                break;
            }
            dt295_routes_feed(tables, packet);
            if (recording) {
                ts_check_packet(check, packet);
                packets++;
                unsigned pid = ((packet[1] & 31u) << 8) | packet[2];
                if (!cfg->filter_pids || cfg->pid_mask[pid]) {
                    memcpy(out + emitted, packet, 188);
                    emitted += 188;
                }
            }
        }
        if (rc)
            break;
        remainder = total - length;
        memmove(data, data + length, remainder);
        if (emitted && (e = ts_output_submit(output, out, emitted, NULL))) {
            rc = output_exit(e);
            snprintf(error, sizeof(error), "TS enqueue: %s", strerror(e));
            break;
        }
        dt295_routes_snapshot snapshot;
        dt295_routes_snapshot_get(tables, &snapshot);
        if (snapshot.unsupported_tables) {
            rc = 8;
            snprintf(error, sizeof(error),
                     "unsupported or ambiguous PAT/PMT/CAT configuration; reacquisition required");
            break;
        }
        dt295_route_plan desired;
        int plan_rc = 0;
        if (recording)
            plan_rc = dt295_routes_build(tables, 0, &live, &desired, route_error);
        else if (!snapshot.ready)
            plan_rc = dt295_routes_bootstrap_plan(tables, &live, &desired, route_error);
        if (plan_rc) {
            rc = 8;
            snprintf(error, sizeof(error), "route plan: %s", route_error);
            break;
        }
        if (recording) {
            if (missing_route(&live, &desired)) {
                rc = 8;
                snprintf(error, sizeof(error),
                         "broadcast requires an unsupported live PID/ECM route change");
                break;
            }
        } else if (!snapshot.ready) {
            if (apply_plan(d, &live, &desired, error, sizeof(error))) {
                rc = 8;
                break;
            }
        } else {
            /* General configuration is acquired outside the recording interval.
             * Reloading uses the same transport key, as the tested rearm path. */
            dt295_route_plan full;
            if (dt295_routes_build(tables, 0, NULL, &full, route_error)) {
                rc = 8;
                snprintf(error, sizeof(error), "initial complete plan: %s", route_error);
                break;
            }
            unsigned ecms = 0;
            for (unsigned i = 0; i < 64; i++)
                if (full.routes[i].enabled && full.routes[i].is_ecm)
                    ecms++;
            if (ecms > 1) {
                rc = 8;
                snprintf(error, sizeof(error),
                         "multiple independent ECMs are outside the verified DT295 scope");
                break;
            }
            if (dt295_stop(d, &drained, error, sizeof(error))) {
                rc = 7;
                break;
            }
            if (dt295_stream_stop(u.stream, 3000) || dt295_stream_destroy(u.stream)) {
                native_abandoned = 1;
                rc = 7;
                snprintf(error, sizeof(error), "bootstrap USB cancellation did not quiesce");
                break;
            }
            u.stream = NULL;
            if (dt295_reprepare_plan(d, &full, error, sizeof(error)) ||
                dt295_start(d, error, sizeof(error))) {
                rc = 7;
                break;
            }
            e = dt295_stream_start(usb_native_context(u.usb), usb_native_handle(u.usb), &u.stream);
            if (e) {
                rc = 7;
                snprintf(error, sizeof(error), "recording USB start error %d", e);
                break;
            }
            live = full;
            remainder = 0;
            recording = 1;
            record_start = last_input = now_ms();
            ts_check_init(check);
        }
        now = now_ms();
        if (now >= next_status) {
            e = capture_status(diag, recording ? 2 : 1, packets, elapsed, renewals, u.stream,
                               output, NULL, tables);
            if (e) {
                rc = 70;
                snprintf(error, sizeof(error), "diagnostic enqueue: %s", strerror(e));
                break;
            }
            next_status = now + 10000;
        }
    }
done:
    if (recording)
        elapsed = now_ms() - record_start;
    if (d && dt295_has_session(d) && !native_abandoned &&
        dt295_stop(d, &drained, stop_error, sizeof(stop_error)) && !rc)
        rc = 7;
    if (u.stream && !native_abandoned) {
        int stop = dt295_stream_stop(u.stream, 3000);
        dt295_stream_snapshot(u.stream, &final_usb);
        if (stop || dt295_stream_destroy(u.stream)) {
            native_abandoned = 1;
            if (!rc)
                rc = 7;
        } else
            u.stream = NULL;
    }
    if (d && !native_abandoned && dt295_has_session(d) &&
        dt295_shutdown(d, shutdown_error, sizeof(shutdown_error)) && !rc)
        rc = 7;
    if (output) {
        e = ts_output_finish(output, TS_OUTPUT_DRAIN_MS);
        if (e && !rc)
            rc = output_exit(e);
    }
    if (recording && !rc &&
        (check->tei || check->scrambled || check->invalid || check->crc_errors ||
         check->continuity))
        rc = 8;
    if (diag && output) {
        capture_status(diag, rc ? 4 : 3, packets, elapsed, renewals, u.stream, output,
                       &final_usb, tables);
        char end[1024];
        int n =
            snprintf(end, sizeof(end),
                     "{\"event\":\"end\",\"exit\":%d,\"state\":%u,\"packets\":%llu,\"elapsed_ms\":%"
                     "llu,\"trailing_bytes\":%zu,\"stop_drained_bytes\":%zu,\"tei\":%llu,\"cc\":%"
                     "llu,\"crc\":%llu,\"native_abandoned\":%d}\n",
                     rc, d && !native_abandoned ? dt295_state(d) : 65535,
                     (unsigned long long)packets, (unsigned long long)elapsed, remainder, drained,
                     (unsigned long long)check->tei, (unsigned long long)check->continuity,
                     (unsigned long long)check->crc_errors, native_abandoned);
        uint64_t id;
        if (n > 0 && (size_t)n < sizeof(end))
            ts_output_submit(diag, end, (size_t)n, &id);
        if (rc && error[0])
            capture_message(diag, error);
        if (stop_error[0])
            capture_message(diag, stop_error);
        if (shutdown_error[0])
            capture_message(diag, shutdown_error);
        e = ts_output_finish(diag, TS_OUTPUT_DRAIN_MS);
        if (e && !rc)
            rc = 70;
    }
    if (!diag && error[0])
        fprintf(stderr, "DT295: %s\n", error);
    if (!diag && stop_error[0])
        fprintf(stderr, "DT295 stop cleanup: %s\n", stop_error);
    if (!diag && shutdown_error[0])
        fprintf(stderr, "DT295 shutdown cleanup: %s\n", shutdown_error);
    if (ts_output_destroy(output) && !rc)
        rc = 70;
    if (ts_output_destroy(diag) && !rc)
        rc = 70;
    dt295_routes_destroy(tables);
    if (!native_abandoned) {
        dt295_destroy(d);
        usb_close(u.usb);
    }
    if (lock_fd >= 0 && !native_abandoned)
        close(lock_fd);
    if (!native_abandoned)
        dt295_assets_free(assets);
    free(check);
    return rc;
#endif
}
