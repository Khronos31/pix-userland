#ifndef PIX_TS_OUTPUT_H
#define PIX_TS_OUTPUT_H

#include <stddef.h>
#include <stdint.h>

typedef struct ts_output ts_output;

enum {
    TS_OUTPUT_CAPACITY_BYTES = 128 * 1024 * 1024,
    TS_OUTPUT_WARN_FILE_MS = 10000,
    TS_OUTPUT_FAIL_FILE_MS = 30000,
    TS_OUTPUT_FAIL_PIPE_MS = 3000,
    TS_OUTPUT_DRAIN_MS = 60000
};

typedef struct {
    unsigned file_warning_ms;
    unsigned file_stall_ms;
    unsigned pipe_stall_ms;
    unsigned drain_ms;
    size_t capacity_bytes; /* zero selects 128 MiB */
    /* Synthetic child fault controls for unit tests; leave zero in applications. */
    size_t test_write_chunk_bytes;
    uint64_t test_fail_after_bytes;
    int test_close_error;
    int test_crash_after_receive;
    int test_stall_close_ack;
    int test_stall_after_receive;
    unsigned test_ack_batch_records; /* child test hook: emit FIFO ACKs in batches */
    unsigned test_write_delay_ms;    /* child test hook: delay each record's write service */
} ts_output_config;

typedef struct {
    uint64_t submitted_bytes;
    uint64_t confirmed_bytes; /* child write(2) return values only */
    uint64_t pending_bytes;
    uint64_t peak_pending_bytes;
    uint64_t rejected_bytes;  /* known never written */
    uint64_t uncertain_bytes; /* child exited before acknowledging outcome */
    uint64_t completed_records, last_progress_ns;
    /* Monotonic durations/counts for locating output queue pressure. Child
       duration spans a record's write phase, including EAGAIN wait/retries. */
    uint64_t writer_write_ns_total, writer_write_ns_max, writer_write_records;
    uint64_t max_pending_age_ns, oldest_pending_age_ns;
    uint64_t max_parent_poll_gap_ns; /* includes submit-to-first-poll while pending */
    uint64_t max_parent_ack_gap_ns;  /* gap between ACKs observed by parent */
    uint64_t send_eagain_count, ack_eagain_count, zero_poll_budget_exhaustions;
    int first_error;   /* errno or internal error */
    int64_t child_pid; /* retained when the child has not been reaped */
    int child_status;  /* wait status, or -1 while running */
    int warning, stalled, finished;
} ts_output_stats;

/* Return values are errno values (EINVAL, EAGAIN, EPIPE, ETIMEDOUT, etc.).
 * Call before starting threads. A named path is opened/truncated synchronously
 * here; the child owns subsequent writes and close. stdout_fd
 * is borrowed and inherited when path is NULL or "-". */
int ts_output_open(const char *path, int stdout_fd, const ts_output_config *config,
                   ts_output **out);
/* Copies data into bounded parent memory. id is optional. Returns 0 or errno-style error. */
int ts_output_submit(ts_output *output, const void *data, size_t length, uint64_t *id);
/* Pumps IPC and observes the child. timeout_ms bounds this call. */
int ts_output_poll(ts_output *output, unsigned timeout_ms);
void ts_output_snapshot(ts_output *output, ts_output_stats *out);
/* Drains and waits up to drain_ms. Return 0 only after all accepted bytes are
 * acknowledged and the child closed successfully. */
int ts_output_finish(ts_output *output, unsigned drain_ms);
/* Stops accepting data and waits up to deadline_ms; outstanding bytes become
 * rejected or uncertain according to what crossed IPC and was acknowledged. */
int ts_output_abort(ts_output *output, unsigned deadline_ms);
/* Returns ETIMEDOUT and retains the object if the child is still unreaped. */
int ts_output_destroy(ts_output *output);

#endif
