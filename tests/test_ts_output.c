#ifdef _WIN32
#include <stdio.h>
int main(void) {
    puts("DT295 output helper: skipped (POSIX implementation only)");
    return 0;
}
#else
#include "ts_output.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void temp_path(char path[64]) {
    strcpy(path, "/tmp/pix-ts-output-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
}
static void parent_sigterm_handler(int signal_number) {
    (void)signal_number;
}
static void verify_file(const char *path, const unsigned char *expected, size_t n) {
    unsigned char *actual = malloc(n ? n : 1);
    assert(actual);
    int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    size_t have = 0;
    while (have < n) {
        ssize_t r = read(fd, actual + have, n - have);
        assert(r > 0);
        have += (size_t)r;
    }
    unsigned char extra;
    assert(read(fd, &extra, 1) == 0);
    close(fd);
    assert(!memcmp(actual, expected, n));
    free(actual);
}
int main(void) {
    unsigned char payload[4096];
    for (size_t i = 0; i < sizeof(payload); ++i)
        payload[i] = (unsigned char)(i * 29u);
    char path[64];
    ts_output *a = NULL, *b = NULL;
    uint64_t id;
    ts_output_stats stats;

    /* Two independent children must not inherit each other's IPC endpoints. */
    temp_path(path);
    assert(ts_output_open(path, -1, NULL, &a) == 0);
    char path2[64];
    temp_path(path2);
    assert(ts_output_open(path2, -1, NULL, &b) == 0);
    assert(ts_output_submit(a, payload, sizeof(payload), &id) == 0 && id == 1);
    assert(ts_output_submit(b, payload + 4, 3, NULL) == 0);
    assert(ts_output_submit(b, payload + 17, 701, &id) == 0 && id == 2);
    assert(ts_output_finish(a, 3000) == 0);
    assert(ts_output_finish(b, 3000) == 0);
    ts_output_snapshot(a, &stats);
    assert(stats.finished && stats.confirmed_bytes == sizeof(payload) && !stats.pending_bytes);
    verify_file(path, payload, sizeof(payload));
    unsigned char expected_b[704];
    memcpy(expected_b, payload + 4, 3);
    memcpy(expected_b + 3, payload + 17, 701);
    verify_file(path2, expected_b, sizeof(expected_b));
    assert(ts_output_destroy(a) == 0 && ts_output_destroy(b) == 0);
    unlink(path);
    unlink(path2);

    /* Capacity includes the job resident in the child and refuses overflow visibly. */
    temp_path(path);
    ts_output_config cfg = {0};
    cfg.capacity_bytes = sizeof(payload);
    assert(ts_output_open(path, -1, &cfg, &a) == 0);
    assert(ts_output_submit(a, payload, sizeof(payload), &id) == 0);
    assert(ts_output_submit(a, payload, 1, &id) == EAGAIN);
    ts_output_snapshot(a, &stats);
    assert(stats.submitted_bytes == sizeof(payload) + 1 && stats.rejected_bytes == 1 &&
           stats.peak_pending_bytes == sizeof(payload));
    assert(ts_output_finish(a, 3000) == 0);
    verify_file(path, payload, sizeof(payload));
    assert(ts_output_destroy(a) == 0);
    unlink(path);

    /* The child resets the caller's SIGTERM handler so abort can reap it promptly. */
    temp_path(path);
    void (*old_handler)(int) = signal(SIGTERM, parent_sigterm_handler);
    memset(&cfg, 0, sizeof(cfg));
    cfg.test_stall_after_receive = 1;
    assert(ts_output_open(path, -1, &cfg, &a) == 0);
    assert(ts_output_submit(a, payload, 211, NULL) == 0);
    for (unsigned i = 0; i < 100; ++i) {
        (void)ts_output_poll(a, 2);
        ts_output_snapshot(a, &stats);
        struct timespec pause = {0, 2000000L};
        nanosleep(&pause, NULL);
    }
    struct timespec abort_start, abort_end;
    assert(clock_gettime(CLOCK_MONOTONIC, &abort_start) == 0);
    assert(ts_output_abort(a, 500) == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &abort_end) == 0);
    int64_t abort_elapsed_ns = (int64_t)(abort_end.tv_sec - abort_start.tv_sec) * 1000000000LL +
                               (int64_t)abort_end.tv_nsec - (int64_t)abort_start.tv_nsec;
    assert(abort_elapsed_ns < 300000000LL);
    ts_output_snapshot(a, &stats);
    assert(stats.child_status != -1 && stats.pending_bytes == 0 && stats.uncertain_bytes == 211 &&
           stats.confirmed_bytes == 0);
    assert(ts_output_destroy(a) == 0);
    unlink(path);
    (void)signal(SIGTERM, old_handler);

    /* Missing close acknowledgement fails at the drain deadline even with no pending payload. */
    temp_path(path);
    memset(&cfg, 0, sizeof(cfg));
    cfg.test_stall_close_ack = 1;
    assert(ts_output_open(path, -1, &cfg, &a) == 0);
    assert(ts_output_submit(a, payload, 29, NULL) == 0);
    struct timespec start_time, end_time;
    assert(clock_gettime(CLOCK_MONOTONIC, &start_time) == 0);
    assert(ts_output_finish(a, 120) == ETIMEDOUT);
    assert(clock_gettime(CLOCK_MONOTONIC, &end_time) == 0);
    int64_t elapsed_ns = (int64_t)(end_time.tv_sec - start_time.tv_sec) * 1000000000LL +
                         (int64_t)end_time.tv_nsec - (int64_t)start_time.tv_nsec;
    assert(elapsed_ns >= 100000000LL && elapsed_ns < 1000000000LL);
    ts_output_snapshot(a, &stats);
    assert(stats.confirmed_bytes == 29 && stats.pending_bytes == 0 &&
           stats.first_error == ETIMEDOUT);
    assert(ts_output_abort(a, 500) == 0);
    assert(ts_output_destroy(a) == 0);
    unlink(path);

    /* Simulated short writes followed by an error count only positive write returns. */
    temp_path(path);
    memset(&cfg, 0, sizeof(cfg));
    cfg.test_write_chunk_bytes = 7;
    cfg.test_fail_after_bytes = 37;
    assert(ts_output_open(path, -1, &cfg, &a) == 0);
    assert(ts_output_submit(a, payload, 100, &id) == 0);
    assert(ts_output_finish(a, 3000) == EIO);
    ts_output_snapshot(a, &stats);
    assert(stats.confirmed_bytes == 37 && stats.rejected_bytes == 63 && !stats.pending_bytes &&
           stats.first_error == EIO);
    verify_file(path, payload, 37);
    assert(ts_output_destroy(a) == 0);
    unlink(path);

    /* Close errors keep successfully written bytes confirmed and fail completion. */
    temp_path(path);
    memset(&cfg, 0, sizeof(cfg));
    cfg.test_close_error = 1;
    assert(ts_output_open(path, -1, &cfg, &a) == 0);
    assert(ts_output_submit(a, payload, 73, &id) == 0);
    assert(ts_output_finish(a, 3000) == EIO);
    ts_output_snapshot(a, &stats);
    assert(stats.confirmed_bytes == 73 && stats.rejected_bytes == 0 && stats.first_error == EIO);
    verify_file(path, payload, 73);
    assert(ts_output_destroy(a) == 0);
    unlink(path);

    /* A child exit after receiving a full frame leaves the write result unknown. */
    temp_path(path);
    memset(&cfg, 0, sizeof(cfg));
    cfg.test_crash_after_receive = 1;
    assert(ts_output_open(path, -1, &cfg, &a) == 0);
    assert(ts_output_submit(a, payload, 211, &id) == 0);
    ts_output_snapshot(a, &stats);
    assert(stats.child_pid > 0 && stats.child_status == -1);
    int crash_rc = 0;
    for (unsigned i = 0; i < 500 && !crash_rc; ++i)
        crash_rc = ts_output_poll(a, 5);
    assert(crash_rc == EPIPE);
    assert(ts_output_abort(a, 1000) == 0);
    ts_output_snapshot(a, &stats);
    assert(stats.pending_bytes == 0 && stats.uncertain_bytes == 211 && stats.confirmed_bytes == 0 &&
           stats.rejected_bytes == 0);
    assert(ts_output_destroy(a) == 0);
    unlink(path);

    /* A blocked pipe is bounded by its shorter stall deadline and remains uncertain. */
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    memset(&cfg, 0, sizeof(cfg));
    cfg.capacity_bytes = 1024 * 1024;
    cfg.pipe_stall_ms = 100;
    assert(ts_output_open("-", pipefd[1], &cfg, &a) == 0);
    close(pipefd[1]);
    unsigned char *large = malloc(512 * 1024);
    assert(large);
    memset(large, 0x7a, 512 * 1024);
    assert(ts_output_submit(a, large, 512 * 1024, &id) == 0);
    int poll_rc = 0;
    for (unsigned i = 0; i < 500 && !poll_rc; ++i)
        poll_rc = ts_output_poll(a, 5);
    assert(poll_rc == ETIMEDOUT);
    (void)ts_output_abort(a, 300);
    ts_output_snapshot(a, &stats);
    assert(stats.stalled && stats.first_error == ETIMEDOUT &&
           stats.pending_bytes + stats.confirmed_bytes + stats.rejected_bytes +
                   stats.uncertain_bytes ==
               stats.submitted_bytes);
    assert(ts_output_destroy(a) == 0);
    close(pipefd[0]);
    free(large);

    /* Writer timing is measured independently from parent polling. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.test_write_delay_ms = 40;
    int telemetry_fd = open("/dev/null", O_WRONLY);
    assert(telemetry_fd >= 0);
    assert(ts_output_open("-", telemetry_fd, &cfg, &a) == 0);
    close(telemetry_fd);
    assert(ts_output_submit(a, payload, sizeof(payload), NULL) == 0);
    assert(ts_output_finish(a, 3000) == 0);
    ts_output_snapshot(a, &stats);
    assert(stats.writer_write_records == 1 && stats.writer_write_ns_total >= 30000000ULL &&
           stats.writer_write_ns_max >= 30000000ULL && stats.max_pending_age_ns >= 30000000ULL);
    assert(ts_output_destroy(a) == 0);

    /* A deliberate delay in parent servicing is visible even when the child
       writer itself is quick. */
    memset(&cfg, 0, sizeof(cfg));
    telemetry_fd = open("/dev/null", O_WRONLY);
    assert(telemetry_fd >= 0);
    assert(ts_output_open("-", telemetry_fd, &cfg, &a) == 0);
    close(telemetry_fd);
    assert(ts_output_submit(a, payload, sizeof(payload), NULL) == 0);
    struct timespec parent_service_pause = {0, 80000000L};
    nanosleep(&parent_service_pause, NULL);
    assert(ts_output_poll(a, 0) == 0);
    assert(ts_output_finish(a, 3000) == 0);
    ts_output_snapshot(a, &stats);
    assert(stats.max_parent_poll_gap_ns >= 60000000ULL &&
           stats.max_pending_age_ns >= stats.max_parent_poll_gap_ns &&
           stats.writer_write_ns_max < stats.max_parent_poll_gap_ns);
    assert(ts_output_destroy(a) == 0);

    /* A receive-like producer polls once per 64 KiB tick. poll(0) must drain
       ready frame/ACK work instead of accumulating one record per tick. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.capacity_bytes = 16 * 1024 * 1024;
    int paced_fd = open("/dev/null", O_WRONLY);
    assert(paced_fd >= 0);
    assert(ts_output_open("-", paced_fd, &cfg, &a) == 0);
    close(paced_fd);
    unsigned char *paced = malloc(64 * 1024);
    assert(paced);
    memset(paced, 0x4d, 64 * 1024);
    const unsigned paced_ticks = 160;
    uint64_t paced_peak = 0;
    for (unsigned tick = 0; tick < paced_ticks; ++tick) {
        assert(ts_output_poll(a, 0) == 0);
        assert(ts_output_submit(a, paced, 64 * 1024, NULL) == 0);
        ts_output_snapshot(a, &stats);
        if (stats.peak_pending_bytes > paced_peak)
            paced_peak = stats.peak_pending_bytes;
        struct timespec pace = {0, 5000000L};
        nanosleep(&pace, NULL);
    }
    assert(ts_output_finish(a, 10000) == 0);
    ts_output_snapshot(a, &stats);
    assert(stats.confirmed_bytes == (uint64_t)paced_ticks * 64 * 1024 &&
           !stats.pending_bytes);
    assert(paced_peak < 8 * 64 * 1024);
    assert(ts_output_destroy(a) == 0);

    /* Receive callbacks arrive in short bursts with variable gaps. The writer
       gets an idle interval after each burst; output must recover to zero
       pending rather than carry a residual queue into the next interval. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.capacity_bytes = 16 * 1024 * 1024;
    paced_fd = open("/dev/null", O_WRONLY);
    assert(paced_fd >= 0);
    assert(ts_output_open("-", paced_fd, &cfg, &a) == 0);
    close(paced_fd);
    const unsigned burst_count = 16;
    const unsigned records_per_burst = 8;
    uint64_t burst_peak = 0;
    for (unsigned burst = 0; burst < burst_count; ++burst) {
        for (unsigned tick = 0; tick < records_per_burst; ++tick) {
            assert(ts_output_poll(a, 0) == 0);
            assert(ts_output_submit(a, paced, 64 * 1024, NULL) == 0);
            ts_output_snapshot(a, &stats);
            if (stats.peak_pending_bytes > burst_peak)
                burst_peak = stats.peak_pending_bytes;
            if ((burst + tick) % 3 == 0) {
                struct timespec variable_gap = {0, 500000L};
                nanosleep(&variable_gap, NULL);
            }
        }
        struct timespec idle_gap = {0, 25000000L};
        nanosleep(&idle_gap, NULL);
        for (unsigned drain = 0; drain < 200; ++drain) {
            assert(ts_output_poll(a, 0) == 0);
            ts_output_snapshot(a, &stats);
            if (!stats.pending_bytes)
                break;
            struct timespec yield = {0, 500000L};
            nanosleep(&yield, NULL);
        }
        assert(stats.pending_bytes == 0);
    }
    assert(burst_peak < 2 * records_per_burst * 64 * 1024);
    assert(ts_output_finish(a, 5000) == 0);
    ts_output_snapshot(a, &stats);
    assert(stats.confirmed_bytes ==
               (uint64_t)burst_count * records_per_burst * 64 * 1024 &&
           !stats.pending_bytes);
    assert(ts_output_destroy(a) == 0);
    free(paced);

    /* The isolated child deliberately retains FIFO ACKs until eight records
       have been written. A sender with only one outstanding record deadlocks;
       the byte-window pipeline fills the child and then validates all ACKs. */
    temp_path(path);
    memset(&cfg, 0, sizeof(cfg));
    cfg.capacity_bytes = 1024 * 1024;
    cfg.test_ack_batch_records = 8;
    assert(ts_output_open(path, -1, &cfg, &a) == 0);
    size_t lengths[8], expected_length = 0;
    unsigned char *records[8];
    for (unsigned i = 0; i < 8; ++i) {
        lengths[i] = 4096 + i * 509;
        records[i] = malloc(lengths[i]);
        assert(records[i]);
        memset(records[i], (int)(0x30 + i), lengths[i]);
        expected_length += lengths[i];
    }
    unsigned char *expected = malloc(expected_length);
    assert(expected);
    size_t expected_offset = 0;
    uint64_t batch_peak = 0;
    for (unsigned i = 0; i < 8; ++i) {
        assert(ts_output_poll(a, 0) == 0);
        assert(ts_output_submit(a, records[i], lengths[i], NULL) == 0);
        memcpy(expected + expected_offset, records[i], lengths[i]);
        expected_offset += lengths[i];
        ts_output_snapshot(a, &stats);
        if (stats.peak_pending_bytes > batch_peak)
            batch_peak = stats.peak_pending_bytes;
    }
    assert(ts_output_finish(a, 5000) == 0);
    ts_output_snapshot(a, &stats);
    assert(stats.completed_records == 8 && stats.confirmed_bytes == expected_length &&
           !stats.pending_bytes && batch_peak == expected_length);
    verify_file(path, expected, expected_length);
    assert(ts_output_destroy(a) == 0);
    unlink(path);
    free(expected);
    for (unsigned i = 0; i < 8; ++i)
        free(records[i]);

    /* A record larger than the in-flight byte window is admitted when it is
       the only record in flight, then drains without window deadlock. */
    temp_path(path);
    memset(&cfg, 0, sizeof(cfg));
    assert(ts_output_open(path, -1, &cfg, &a) == 0);
    size_t oversized_length = 2 * 1024 * 1024;
    unsigned char *oversized = malloc(oversized_length);
    assert(oversized);
    memset(oversized, 0xa7, oversized_length);
    assert(ts_output_submit(a, oversized, oversized_length, NULL) == 0);
    assert(ts_output_finish(a, 10000) == 0);
    ts_output_snapshot(a, &stats);
    assert(stats.completed_records == 1 && stats.confirmed_bytes == oversized_length &&
           !stats.pending_bytes);
    verify_file(path, oversized, oversized_length);
    assert(ts_output_destroy(a) == 0);
    unlink(path);
    free(oversized);

    /* If the child stops after receiving one oversized record, that completely
       sent record is uncertain on abort; later records blocked by the byte
       window are known never sent and therefore rejected. */
    temp_path(path);
    memset(&cfg, 0, sizeof(cfg));
    cfg.capacity_bytes = 8 * 1024 * 1024;
    cfg.test_stall_after_receive = 1;
    assert(ts_output_open(path, -1, &cfg, &a) == 0);
    size_t stalled_record_length = 2 * 1024 * 1024;
    unsigned char *stalled_record = malloc(stalled_record_length);
    assert(stalled_record);
    memset(stalled_record, 0x62, stalled_record_length);
    for (unsigned i = 0; i < 3; ++i)
        assert(ts_output_submit(a, stalled_record, stalled_record_length, NULL) == 0);
    for (unsigned i = 0; i < 20; ++i)
        (void)ts_output_poll(a, 10);
    assert(ts_output_abort(a, 500) == 0);
    ts_output_snapshot(a, &stats);
    assert(stats.uncertain_bytes == stalled_record_length &&
           stats.rejected_bytes == 2 * stalled_record_length && !stats.pending_bytes &&
           stats.submitted_bytes == stats.confirmed_bytes + stats.rejected_bytes +
                                        stats.uncertain_bytes);
    assert(ts_output_destroy(a) == 0);
    unlink(path);
    free(stalled_record);

    puts("TS output synthetic process tests passed: independent helpers, optional ids, bounded "
         "queue, short writes, partial failure, close error/ACK stall, child crash, stalled pipe, "
         "writer and parent-service telemetry, paced and burst zero-time polling, bounded FIFO "
         "ACK pipeline");
    return 0;
}
#endif
