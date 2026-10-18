/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIANO_OUTPUT_WRITER_H
#define SIANO_OUTPUT_WRITER_H

/*
 * Bounded, resumable TS output.
 *
 * The consumer loop owns a siano_output_pump. It feeds one 16 KiB chunk at a
 * time and calls the step function repeatedly. A step never blocks longer than
 * the bounded wait between retries, so the loop can observe SIGINT/SIGTERM, the
 * --time deadline, USB state errors, queue overflow and --control input while a
 * stalled consumer is not draining the pipe.
 *
 * POSIX: pollable fds (pipe/FIFO/socket/character device) are switched to
 * O_NONBLOCK for the lifetime of the pump and restored afterwards. Regular
 * files keep blocking writes; kernel/filesystem stalls on regular files are
 * out of scope.
 *
 * Windows: anonymous/message pipes are switched to PIPE_NOWAIT with
 * SetNamedPipeHandleState() and restored afterwards. Console and regular files
 * keep blocking writes. A byte-mode pipe may accept 0..n bytes of a 188-byte
 * request; the pump keeps the unwritten offset and resumes it. Successful
 * retunes discard unstarted bytes but retain and finish a packet already
 * partially emitted, preserving the consumer's 188-byte framing.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIANO_OUTPUT_PACKET 188U
#define SIANO_OUTPUT_FEED_MAX 16384U
#define SIANO_OUTPUT_SCRATCH (SIANO_OUTPUT_FEED_MAX + SIANO_OUTPUT_PACKET)

struct siano_output {
    int fd;
#ifndef _WIN32
    int saved_flags;
    bool armed;
    bool pollable;
#else
    void *handle;
    unsigned long saved_mode;
    bool nowait;
#endif
};

enum {
    /* All requested bytes were written. */
    SIANO_OUTPUT_DONE = 0,
    /* Wrote *written bytes (possibly 0); the fd is not ready for more. */
    SIANO_OUTPUT_PENDING = 1,
};

int siano_output_begin(struct siano_output *output, int fd);
int siano_output_write(struct siano_output *output, const uint8_t *data,
                       size_t length, size_t *written);
void siano_output_wait(struct siano_output *output, int timeout_ms);
void siano_output_end(struct siano_output *output);

/* Deterministic test seam for partial/zero write results. The hook returns the
 * number of bytes accepted (>= 0) or a negative errno; NULL uses the real
 * platform write. Never set in production. */
typedef int64_t (*siano_output_write_hook)(const uint8_t *data, size_t length);
void siano_output_test_set_write_hook(siano_output_write_hook hook);

struct siano_output_pump {
    struct siano_output output;
    uint8_t hold[SIANO_OUTPUT_PACKET];
    size_t hold_len;
    uint8_t scratch[SIANO_OUTPUT_SCRATCH];
    size_t scratch_len;
    size_t send_off;
    size_t packet_off;
    /* Prefix already emitted before the current packet remainder in scratch. */
    size_t packet_resume_off;
    bool writing;
    bool busy;
    int (*packet_sink)(void *, const uint8_t[SIANO_OUTPUT_PACKET]);
    void *packet_sink_user;
};

int siano_output_pump_init(struct siano_output_pump *pump, int fd);
int siano_output_pump_init_sink(
    struct siano_output_pump *pump,
    int (*sink)(void *, const uint8_t[SIANO_OUTPUT_PACKET]), void *user);
void siano_output_pump_destroy(struct siano_output_pump *pump);
void siano_output_pump_reset(struct siano_output_pump *pump);
void siano_output_pump_retune(struct siano_output_pump *pump);
bool siano_output_pump_busy(const struct siano_output_pump *pump);
size_t siano_output_pump_pending(const struct siano_output_pump *pump);
void siano_output_pump_feed(struct siano_output_pump *pump, const uint8_t *data,
                            size_t length);
int siano_output_pump_step(struct siano_output_pump *pump);

#endif
