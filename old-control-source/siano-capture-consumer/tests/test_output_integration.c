/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Offline integration tests for the bounded output path (#15). The production
 * translation unit is included so the real stream_ts / stream_ts_control loops,
 * the real queue/stream-state policy and the real output pump run against
 * stalled pipes/sockets. No USB device is opened.
 */
#ifndef _WIN32

#define main siano_ts_program_main
#include "../siano-ts.c"
#undef main

#include <assert.h>
#include <signal.h>
#include <sys/socket.h>

#define EVENT_BOUND_MS 2000.0

static struct siano_device device;

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static void fill_packet(uint8_t *buffer, size_t length)
{
    memset(buffer, 0, length);
    for (size_t offset = 0; offset + SIANO_OUTPUT_PACKET <= length;
         offset += SIANO_OUTPUT_PACKET)
        buffer[offset] = 0x47;
}

static void make_device(struct siano_device *device)
{
    assert(init_device_state(device, NULL, false) == 0);
}

/* Leaves the read end open and unread, so the next write would block. */
static void full_pipe(int fds[2])
{
    uint8_t filler[4096];
    int flags;

    memset(filler, 0, sizeof(filler));
    assert(pipe(fds) == 0);
    flags = fcntl(fds[1], F_GETFL);
    assert(flags >= 0);
    assert(fcntl(fds[1], F_SETFL, flags | O_NONBLOCK) == 0);
    while (write(fds[1], filler, sizeof(filler)) > 0)
        ;
    assert(fcntl(fds[1], F_SETFL, flags) == 0);
}

static void *raise_after(void *arg)
{
    int *spec = arg;
    struct timespec delay;

    delay.tv_sec = spec[1] / 1000;
    delay.tv_nsec = (long)(spec[1] % 1000) * 1000000L;
    nanosleep(&delay, NULL);
    kill(getpid(), spec[0]);
    return NULL;
}

struct fail_spec {
    struct siano_device *device;
    int error;
    int delay_ms;
};

static void *fail_after(void *arg)
{
    struct fail_spec *spec = arg;
    struct timespec delay;

    delay.tv_sec = spec->delay_ms / 1000;
    delay.tv_nsec = (long)(spec->delay_ms % 1000) * 1000000L;
    nanosleep(&delay, NULL);
    siano_stream_state_fail(&spec->device->state, spec->error);
    return NULL;
}

struct overflow_spec {
    struct ts_queue *queue;
    int delay_ms;
    int count;
};

static void *overflow_after(void *arg)
{
    struct overflow_spec *spec = arg;
    uint8_t chunk[SIANO_OUTPUT_PACKET];
    struct timespec delay;

    delay.tv_sec = spec->delay_ms / 1000;
    delay.tv_nsec = (long)(spec->delay_ms % 1000) * 1000000L;
    nanosleep(&delay, NULL);
    fill_packet(chunk, sizeof(chunk));
    for (int i = 0; i < spec->count; i++)
        ts_enqueue(spec->queue, 0, chunk, sizeof(chunk));
    return NULL;
}

struct write_spec {
    int fd;
    const char *text;
    int delay_ms;
};

static void *write_after(void *arg)
{
    struct write_spec *spec = arg;
    struct timespec delay;

    delay.tv_sec = spec->delay_ms / 1000;
    delay.tv_nsec = (long)(spec->delay_ms % 1000) * 1000000L;
    nanosleep(&delay, NULL);
    assert(write(spec->fd, spec->text, strlen(spec->text)) > 0);
    return NULL;
}

static void test_signal_event(int signo)
{
    struct siano_output_pump pump;
    uint8_t chunk[SIANO_OUTPUT_PACKET * 4];
    pthread_t thread;
    int spec[2] = { signo, 100 };
    int fds[2];
    double elapsed;
    int rc;

    full_pipe(fds);
    make_device(&device);
    signal(signo, on_signal);
    stop_requested = 0;
    fill_packet(chunk, sizeof(chunk));
    ts_enqueue(&device.ts, 0, chunk, sizeof(chunk));
    assert(siano_output_pump_init(&pump, fds[1]) == 0);
    assert(pthread_create(&thread, NULL, raise_after, spec) == 0);
    double start = now_ms();
    rc = stream_ts(&device, &pump, 0);
    elapsed = now_ms() - start;
    pthread_join(thread, NULL);
    siano_output_pump_destroy(&pump);
    close(fds[1]);
    close(fds[0]);
    destroy_device_sync_state(&device);
    assert(rc == 0);
    assert(stop_requested == 1);
    assert(elapsed < EVENT_BOUND_MS);
    stop_requested = 0;
    printf("signal %d: bounded %.1f ms\n", signo, elapsed);
}

static void test_duration_event(void)
{
    struct siano_output_pump pump;
    uint8_t chunk[SIANO_OUTPUT_PACKET * 4];
    int fds[2];
    double elapsed;
    int rc;

    full_pipe(fds);
    make_device(&device);
    stop_requested = 0;
    fill_packet(chunk, sizeof(chunk));
    ts_enqueue(&device.ts, 0, chunk, sizeof(chunk));
    assert(siano_output_pump_init(&pump, fds[1]) == 0);
    double start = now_ms();
    rc = stream_ts(&device, &pump, 1);
    elapsed = now_ms() - start;
    siano_output_pump_destroy(&pump);
    close(fds[1]);
    close(fds[0]);
    destroy_device_sync_state(&device);
    assert(rc == 0);
    assert(elapsed >= 900.0);
    assert(elapsed < EVENT_BOUND_MS);
    printf("duration expiry: bounded %.1f ms\n", elapsed);
}

static void test_usb_error(int injected, int expected)
{
    struct siano_output_pump pump;
    struct fail_spec spec = { &device, injected, 100 };
    uint8_t chunk[SIANO_OUTPUT_PACKET * 4];
    pthread_t thread;
    int fds[2];
    double elapsed;
    int rc;

    full_pipe(fds);
    make_device(&device);
    stop_requested = 0;
    fill_packet(chunk, sizeof(chunk));
    ts_enqueue(&device.ts, 0, chunk, sizeof(chunk));
    assert(siano_output_pump_init(&pump, fds[1]) == 0);
    assert(pthread_create(&thread, NULL, fail_after, &spec) == 0);
    double start = now_ms();
    rc = stream_ts(&device, &pump, 0);
    elapsed = now_ms() - start;
    pthread_join(thread, NULL);
    siano_output_pump_destroy(&pump);
    close(fds[1]);
    close(fds[0]);
    destroy_device_sync_state(&device);
    assert(rc == expected);
    assert(elapsed < EVENT_BOUND_MS);
    printf("usb error %d -> %d: bounded %.1f ms\n", injected, rc, elapsed);
}

static void test_overflow_event(void)
{
    struct siano_output_pump pump;
    struct overflow_spec spec = { &device.ts, 100, (int)TS_QUEUE_SLOTS + 4 };
    uint8_t seed[SIANO_OUTPUT_PACKET * 2];
    pthread_t thread;
    uint64_t drops;
    int fds[2];
    double elapsed;
    int rc;

    full_pipe(fds);
    make_device(&device);
    stop_requested = 0;
    ts_queue_set_fail_on_drop(&device.ts, true);
    fill_packet(seed, sizeof(seed));
    ts_enqueue(&device.ts, 0, seed, sizeof(seed));
    assert(siano_output_pump_init(&pump, fds[1]) == 0);
    assert(pthread_create(&thread, NULL, overflow_after, &spec) == 0);
    double start = now_ms();
    rc = stream_ts(&device, &pump, 0);
    elapsed = now_ms() - start;
    pthread_join(thread, NULL);
    drops = device.ts.drops;
    siano_output_pump_destroy(&pump);
    close(fds[1]);
    close(fds[0]);
    destroy_device_sync_state(&device);
    assert(rc == -ENOBUFS);
    assert(drops != 0);
    assert(elapsed < EVENT_BOUND_MS);
    printf("drop overflow: bounded %.1f ms\n", elapsed);
}

static void test_control_quit_event(void)
{
    struct siano_output_pump pump;
    struct options options;
    struct write_spec spec;
    uint8_t chunk[SIANO_OUTPUT_PACKET * 4];
    bool pid_filters_added = false;
    pthread_t thread;
    int input[2];
    int output[2];
    int saved_stdin;
    double elapsed;
    int rc;

    memset(&options, 0, sizeof(options));
    full_pipe(output);
    assert(pipe(input) == 0);
    saved_stdin = dup(STDIN_FILENO);
    assert(saved_stdin >= 0);
    assert(dup2(input[0], STDIN_FILENO) == STDIN_FILENO);
    make_device(&device);
    stop_requested = 0;
    fill_packet(chunk, sizeof(chunk));
    ts_enqueue(&device.ts, 0, chunk, sizeof(chunk));
    assert(siano_output_pump_init(&pump, output[1]) == 0);
    spec.fd = input[1];
    spec.text = "quit\n";
    spec.delay_ms = 100;
    assert(pthread_create(&thread, NULL, write_after, &spec) == 0);
    double start = now_ms();
    rc = stream_ts_control(&device, &options, &pid_filters_added, &pump);
    elapsed = now_ms() - start;
    pthread_join(thread, NULL);
    assert(dup2(saved_stdin, STDIN_FILENO) == STDIN_FILENO);
    close(saved_stdin);
    close(input[0]);
    close(input[1]);
    siano_output_pump_destroy(&pump);
    close(output[1]);
    close(output[0]);
    destroy_device_sync_state(&device);
    assert(rc == 0);
    assert(elapsed < EVENT_BOUND_MS);
    printf("control quit: bounded %.1f ms\n", elapsed);
}

static void test_epipe(void)
{
    struct siano_device device;
    struct siano_output_pump pump;
    uint8_t packet[SIANO_OUTPUT_PACKET];
    size_t written = 0;
    int fds[2];

    signal(SIGPIPE, SIG_IGN);
    assert(pipe(fds) == 0);
    close(fds[0]);
    assert(siano_output_pump_init(&pump, fds[1]) == 0);
    fill_packet(packet, sizeof(packet));
    assert(siano_output_write(&pump.output, packet, sizeof(packet), &written) == -EPIPE);
    assert(siano_report_output_error(-EPIPE) == 0);
    siano_output_pump_destroy(&pump);
    close(fds[1]);

    assert(pipe(fds) == 0);
    close(fds[0]);
    make_device(&device);
    stop_requested = 0;
    fill_packet(packet, sizeof(packet));
    ts_enqueue(&device.ts, 0, packet, sizeof(packet));
    assert(siano_output_pump_init(&pump, fds[1]) == 0);
    assert(stream_ts(&device, &pump, 0) == -EPIPE);
    siano_output_pump_destroy(&pump);
    close(fds[1]);
    destroy_device_sync_state(&device);
    printf("EPIPE: preserved\n");
}

/* Complete packets are emitted in order; a sub-packet tail is carried. */
static void test_alignment_order(void)
{
    struct siano_output_pump pump;
    uint8_t first[SIANO_OUTPUT_PACKET + 94];
    uint8_t second[94 + SIANO_OUTPUT_PACKET];
    uint8_t output[1024];
    size_t total = 0;
    int fds[2];

    assert(pipe(fds) == 0);
    assert(siano_output_pump_init(&pump, fds[1]) == 0);
    memset(first, 0, sizeof(first));
    first[0] = 0x47;
    first[SIANO_OUTPUT_PACKET] = 0x47;
    siano_output_pump_feed(&pump, first, sizeof(first));
    while (siano_output_pump_busy(&pump))
        assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    assert(siano_output_pump_pending(&pump) == 94);
    memset(second, 0, sizeof(second));
    second[94] = 0x47;
    siano_output_pump_feed(&pump, second, sizeof(second));
    while (siano_output_pump_busy(&pump))
        assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    siano_output_pump_destroy(&pump);
    close(fds[1]);
    for (;;) {
        ssize_t count = read(fds[0], output + total, sizeof(output) - total);
        if (count <= 0)
            break;
        total += (size_t)count;
    }
    close(fds[0]);
    assert(total == SIANO_OUTPUT_PACKET * 3);
    for (size_t offset = 0; offset + SIANO_OUTPUT_PACKET <= total;
         offset += SIANO_OUTPUT_PACKET)
        assert(output[offset] == 0x47);
    printf("alignment/order: %zu bytes\n", total);
}

static size_t read_available(int fd, uint8_t *buffer, size_t capacity)
{
    size_t got = 0;

    while (got < capacity) {
        ssize_t count = read(fd, buffer + got, capacity - got);

        if (count <= 0)
            break;
        got += (size_t)count;
    }
    return got;
}

/* Preserve the old per-chunk two-packet lookahead: a false sync candidate is
 * rejected when its successor is present and invalid, while a final complete
 * packet at a chunk boundary has no successor to check. */
static void test_corrupt_sync_lookahead(void)
{
    struct siano_output_pump pump;
    uint8_t packets[SIANO_OUTPUT_PACKET * 3];
    uint8_t output[sizeof(packets)];
    size_t total;
    int fds[2];

    memset(packets, 0x11, sizeof(packets));
    packets[0] = 0x47;
    packets[SIANO_OUTPUT_PACKET] = 0x47;
    packets[2U * SIANO_OUTPUT_PACKET] = 0x00;
    packets[1] = 0x21;
    packets[SIANO_OUTPUT_PACKET + 1] = 0x32;

    assert(pipe(fds) == 0);
    assert(siano_output_pump_init(&pump, fds[1]) == 0);
    siano_output_pump_feed(&pump, packets, sizeof(packets));
    while (siano_output_pump_busy(&pump))
        assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    siano_output_pump_destroy(&pump);
    close(fds[1]);
    total = read_available(fds[0], output, sizeof(output));
    close(fds[0]);
    assert(total == SIANO_OUTPUT_PACKET);
    assert(memcmp(output, packets, SIANO_OUTPUT_PACKET) == 0);

    assert(pipe(fds) == 0);
    assert(siano_output_pump_init(&pump, fds[1]) == 0);
    siano_output_pump_feed(&pump, packets, SIANO_OUTPUT_PACKET * 2U);
    while (siano_output_pump_busy(&pump))
        assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    siano_output_pump_feed(&pump, packets + SIANO_OUTPUT_PACKET * 2U,
                           SIANO_OUTPUT_PACKET);
    while (siano_output_pump_busy(&pump))
        assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    siano_output_pump_destroy(&pump);
    close(fds[1]);
    total = read_available(fds[0], output, sizeof(output));
    close(fds[0]);
    assert(total == SIANO_OUTPUT_PACKET * 2U);
    assert(memcmp(output, packets, total) == 0);
    printf("corrupt sync lookahead: chunk boundary behavior preserved\n");
}

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);

    assert(flags >= 0);
    assert(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

static uint8_t hook_capture[4096];
static size_t hook_capture_len;
static size_t hook_chunk;

static int64_t inject_write(const uint8_t *data, size_t length)
{
    size_t count = length < hook_chunk ? length : hook_chunk;

    memcpy(hook_capture + hook_capture_len, data, count);
    hook_capture_len += count;
    return (int64_t)count;
}

/* Deterministic 1..187 partial results through the production pump. */
static void test_partial_resume(void)
{
    struct siano_output_pump pump;
    uint8_t packet[SIANO_OUTPUT_PACKET];
    int fds[2];

    assert(pipe(fds) == 0);
    assert(siano_output_pump_init(&pump, fds[1]) == 0);
    fill_packet(packet, sizeof(packet));
    hook_capture_len = 0;
    hook_chunk = 100;
    siano_output_test_set_write_hook(inject_write);
    siano_output_pump_feed(&pump, packet, sizeof(packet));
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_PENDING);
    assert(hook_capture_len == 100);
    assert(siano_output_pump_pending(&pump) == SIANO_OUTPUT_PACKET);
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    assert(hook_capture_len == sizeof(packet));
    assert(memcmp(hook_capture, packet, sizeof(packet)) == 0);
    siano_output_test_set_write_hook(NULL);
    siano_output_pump_destroy(&pump);
    close(fds[1]);
    close(fds[0]);
    printf("partial resume: 100 + %zu bytes\n",
           sizeof(packet) - (size_t)100);
}

/* A 0-byte result is PENDING, not an error or a busy loop, and resumes. */
static void test_zero_resume(void)
{
    struct siano_output_pump pump;
    uint8_t packet[SIANO_OUTPUT_PACKET];
    int fds[2];

    assert(pipe(fds) == 0);
    assert(siano_output_pump_init(&pump, fds[1]) == 0);
    fill_packet(packet, sizeof(packet));
    hook_capture_len = 0;
    hook_chunk = 0;
    siano_output_test_set_write_hook(inject_write);
    siano_output_pump_feed(&pump, packet, sizeof(packet));
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_PENDING);
    assert(hook_capture_len == 0);
    assert(siano_output_pump_pending(&pump) == SIANO_OUTPUT_PACKET);
    hook_chunk = sizeof(packet);
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    assert(hook_capture_len == sizeof(packet));
    assert(memcmp(hook_capture, packet, sizeof(packet)) == 0);
    siano_output_test_set_write_hook(NULL);
    siano_output_pump_destroy(&pump);
    close(fds[1]);
    close(fds[0]);
    printf("zero resume: 0 + %zu bytes\n", sizeof(packet));
}

/* Real full pipe: the packet is emitted only after the reader drains, with the
 * production pump and the real write path (no hook). */
static void test_real_pipe_resume(void)
{
    struct siano_output_pump pump;
    uint8_t filler[4096];
    uint8_t packet[SIANO_OUTPUT_PACKET];
    uint8_t got[SIANO_OUTPUT_PACKET];
    size_t spilled;
    ssize_t count;
    int fds[2];

    assert(pipe(fds) == 0);
    set_nonblocking(fds[1]);
    memset(filler, 0x55, sizeof(filler));
    while ((count = write(fds[1], filler, sizeof(filler))) > 0)
        ;
    assert(siano_output_pump_init(&pump, fds[1]) == 0);
    fill_packet(packet, sizeof(packet));
    siano_output_pump_feed(&pump, packet, sizeof(packet));
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_PENDING);
    assert(siano_output_pump_pending(&pump) == SIANO_OUTPUT_PACKET);
    set_nonblocking(fds[0]);
    do {
        spilled = read_available(fds[0], filler, sizeof(filler));
    } while (spilled != 0);
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    assert(read_available(fds[0], got, sizeof(got)) == SIANO_OUTPUT_PACKET);
    assert(memcmp(got, packet, sizeof(packet)) == 0);
    siano_output_pump_destroy(&pump);
    close(fds[1]);
    close(fds[0]);
    printf("real pipe resume: %zu bytes\n", sizeof(packet));
}

/* O_NONBLOCK is restored on the shared open file description, other bits and a
 * pre-existing O_NONBLOCK are preserved. */
static void test_flags_restored(void)
{
    struct siano_output_pump pump;
    int duplicate;
    int before;
    int during;
    int after;
    int after_duplicate;
    int fds[2];

    assert(pipe(fds) == 0);
    duplicate = dup(fds[1]);
    assert(duplicate >= 0);
    before = fcntl(fds[1], F_GETFL);
    assert(before >= 0);
    assert(siano_output_pump_init(&pump, fds[1]) == 0);
    during = fcntl(fds[1], F_GETFL);
    assert((during & O_NONBLOCK) != 0);
    siano_output_pump_destroy(&pump);
    after = fcntl(fds[1], F_GETFL);
    after_duplicate = fcntl(duplicate, F_GETFL);
    assert((after & O_NONBLOCK) == (before & O_NONBLOCK));
    assert((after_duplicate & O_NONBLOCK) == (before & O_NONBLOCK));
    assert(fcntl(fds[1], F_SETFL, before | O_NONBLOCK) == 0);
    assert(siano_output_pump_init(&pump, fds[1]) == 0);
    siano_output_pump_destroy(&pump);
    assert((fcntl(fds[1], F_GETFL) & O_NONBLOCK) != 0);
    close(duplicate);
    close(fds[1]);
    close(fds[0]);
    printf("fd flags: restored\n");
}

/* Success discards pending and hold; failure keeps both with the queue. */
static void test_retune_transition(void)
{
    struct siano_output_pump pump;
    uint8_t chunk[SIANO_OUTPUT_PACKET * 2];
    uint8_t tail[100];
    int fds[2];

    full_pipe(fds);
    make_device(&device);
    stop_requested = 0;
    assert(siano_output_pump_init(&pump, fds[1]) == 0);
    fill_packet(chunk, sizeof(chunk));
    siano_output_pump_feed(&pump, chunk, sizeof(chunk));
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_PENDING);
    assert(siano_output_pump_busy(&pump));

    ts_enqueue(&device.ts, 0, chunk, sizeof(chunk));
    ts_queue_retune_begin(&device.ts);
    assert(!stream_retune_finish(&device, &pump, -EIO));
    assert(siano_output_pump_busy(&pump));
    assert(siano_output_pump_pending(&pump) != 0);
    assert(device.ts.count != 0);

    ts_queue_retune_begin(&device.ts);
    assert(stream_retune_finish(&device, &pump, 0));
    assert(!siano_output_pump_busy(&pump));
    assert(siano_output_pump_pending(&pump) == 0);
    assert(device.ts.count == 0);

    memset(tail, 0x11, sizeof(tail));
    siano_output_pump_feed(&pump, tail, sizeof(tail));
    while (siano_output_pump_busy(&pump))
        assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    assert(siano_output_pump_pending(&pump) == sizeof(tail));
    ts_queue_retune_begin(&device.ts);
    assert(stream_retune_finish(&device, &pump, 0));
    assert(siano_output_pump_pending(&pump) == 0);

    siano_output_pump_destroy(&pump);
    close(fds[1]);
    close(fds[0]);
    destroy_device_sync_state(&device);
    printf("retune transition: success clears, failure retains\n");
}

static void test_retune_partial_packet(void)
{
    struct siano_output_pump pump;
    uint8_t old_packets[SIANO_OUTPUT_PACKET * 3];
    uint8_t new_packet[SIANO_OUTPUT_PACKET];
    int fds[2];

    assert(pipe(fds) == 0);
    make_device(&device);
    assert(siano_output_pump_init(&pump, fds[1]) == 0);
    fill_packet(old_packets, sizeof(old_packets));
    old_packets[1] = 0x11;
    old_packets[SIANO_OUTPUT_PACKET + 1] = 0x22;
    old_packets[2 * SIANO_OUTPUT_PACKET + 1] = 0x33;
    fill_packet(new_packet, sizeof(new_packet));
    new_packet[1] = 0xa4;
    hook_capture_len = 0;
    hook_chunk = 100;
    siano_output_test_set_write_hook(inject_write);
    ts_enqueue(&device.ts, device.ts.epoch, old_packets, sizeof(old_packets));
    siano_output_pump_feed(&pump, old_packets, sizeof(old_packets));
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_PENDING);
    assert(hook_capture_len == 100);
    assert(pump.packet_off == 100);
    hook_chunk = 0;
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_PENDING);
    assert(hook_capture_len == 100);

    ts_queue_retune_begin(&device.ts);
    assert(!stream_retune_finish(&device, &pump, -EIO));
    assert(siano_output_pump_busy(&pump));
    assert(pump.packet_off == 100);
    assert(pump.scratch_len == sizeof(old_packets));
    assert(pump.send_off == 0);
    assert(pump.packet_resume_off == 0);
    assert(device.ts.count == 1);

    ts_queue_retune_begin(&device.ts);
    assert(stream_retune_finish(&device, &pump, 0));
    assert(siano_output_pump_busy(&pump));
    assert(siano_output_pump_pending(&pump) == SIANO_OUTPUT_PACKET - 100);
    assert(pump.packet_off == 100);
    assert(pump.packet_resume_off == 100);
    assert(device.ts.count == 0);

    ts_enqueue(&device.ts, device.ts.epoch, old_packets, sizeof(old_packets));
    ts_queue_retune_begin(&device.ts);
    assert(stream_retune_finish(&device, &pump, 0));
    assert(siano_output_pump_pending(&pump) == SIANO_OUTPUT_PACKET - 100);
    assert(pump.packet_off == 100);
    assert(device.ts.count == 0);

    hook_chunk = 20;
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_PENDING);
    assert(hook_capture_len == 120);
    assert(pump.packet_off == 120);
    assert(siano_output_pump_pending(&pump) == SIANO_OUTPUT_PACKET - 120);
    ts_enqueue(&device.ts, device.ts.epoch, old_packets, sizeof(old_packets));
    ts_queue_retune_begin(&device.ts);
    assert(stream_retune_finish(&device, &pump, 0));
    assert(siano_output_pump_pending(&pump) == SIANO_OUTPUT_PACKET - 120);
    assert(pump.packet_off == 120);
    assert(pump.packet_resume_off == 120);
    assert(device.ts.count == 0);

    hook_chunk = SIANO_OUTPUT_PACKET - 120;
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    assert(hook_capture_len == SIANO_OUTPUT_PACKET);
    assert(memcmp(hook_capture, old_packets, SIANO_OUTPUT_PACKET) == 0);
    hook_chunk = SIANO_OUTPUT_PACKET;
    siano_output_pump_feed(&pump, new_packet, sizeof(new_packet));
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    assert(hook_capture_len == SIANO_OUTPUT_PACKET * 2);
    assert(memcmp(hook_capture + SIANO_OUTPUT_PACKET, new_packet,
                  SIANO_OUTPUT_PACKET) == 0);
    assert(hook_capture[0] == 0x47);
    assert(hook_capture[SIANO_OUTPUT_PACKET] == 0x47);
    assert(hook_capture[SIANO_OUTPUT_PACKET + 1] == 0xa4);

    siano_output_test_set_write_hook(NULL);
    siano_output_pump_destroy(&pump);
    close(fds[1]);
    close(fds[0]);
    destroy_device_sync_state(&device);
    printf("retune partial packet: completed old remainder, discarded other old bytes\n");
}

static int run_parse(const char *const *args, size_t count, struct options *options)
{
    char *argv[8];

    assert(count < sizeof(argv) / sizeof(argv[0]));
    for (size_t i = 0; i < count; i++)
        argv[i] = (char *)args[i];
    optind = 1;
    return parse_options((int)count, argv, options);
}

static void expect_channel_ok(const char *const *args, size_t count,
                              unsigned expected)
{
    struct options options;

    assert(run_parse(args, count, &options) == 0);
    assert(options.have_channel);
    assert(options.channel == expected);
}

static void expect_channel_bad(const char *text)
{
    const char *args[] = { "siano-ts", "--channel", text };
    struct options options;

    assert(run_parse(args, 3, &options) < 0);
}

static void test_parse_options(void)
{
    const char *t13[] = { "siano-ts", "--channel", "T13" };
    const char *t27[] = { "siano-ts", "--channel", "T27" };
    const char *t62[] = { "siano-ts", "--channel", "T62" };
    const char *n13[] = { "siano-ts", "--channel", "13" };
    const char *n27[] = { "siano-ts", "--channel", "27" };
    const char *nhex[] = { "siano-ts", "--channel", "0x1b" };
    const char *short_sep[] = { "siano-ts", "-c", "T27" };
    const char *short_join[] = { "siano-ts", "-cT27" };
    const char *long_eq[] = { "siano-ts", "--channel=T27" };

    expect_channel_ok(t13, 3, 13);
    expect_channel_ok(t27, 3, 27);
    expect_channel_ok(t62, 3, 62);
    expect_channel_ok(n13, 3, 13);
    expect_channel_ok(n27, 3, 27);
    expect_channel_ok(nhex, 3, 27);
    expect_channel_ok(short_sep, 3, 27);
    expect_channel_ok(short_join, 2, 27);
    expect_channel_ok(long_eq, 2, 27);
    expect_channel_bad("T12");
    expect_channel_bad("T63");
    expect_channel_bad("T027");
    expect_channel_bad("T0x1b");
    expect_channel_bad("T+27");
    expect_channel_bad("t27");
    expect_channel_bad(" T27");
    expect_channel_bad("T27 ");
    expect_channel_bad("");
    expect_channel_bad("T");
    expect_channel_bad("T7");
    expect_channel_bad("T277");
    expect_channel_bad("T2a");
    printf("parse_options: accepted/rejected matrix OK\n");
}

static void watchdog(int signo)
{
    (void)signo;
    _exit(99);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, watchdog);
    alarm(60);
    test_parse_options();
    test_signal_event(SIGINT);
    test_signal_event(SIGTERM);
    test_duration_event();
    test_usb_error(LIBUSB_ERROR_NO_DEVICE, LIBUSB_ERROR_NO_DEVICE);
    test_usb_error(LIBUSB_ERROR_IO, LIBUSB_ERROR_NO_DEVICE);
    test_overflow_event();
    test_control_quit_event();
    test_epipe();
    test_alignment_order();
    test_corrupt_sync_lookahead();
    test_partial_resume();
    test_zero_resume();
    test_real_pipe_resume();
    test_flags_restored();
    test_retune_transition();
    test_retune_partial_packet();
    alarm(0);
    printf("output integration tests: PASS\n");
    return 0;
}

#endif /* !_WIN32 */
