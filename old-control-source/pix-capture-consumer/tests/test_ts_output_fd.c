#define _POSIX_C_SOURCE 200809L
#include "ts_output.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    FD_CLEANUP_NORMAL,
    FD_CLEANUP_FALLBACK,
    FD_CLEANUP_FAIL,
    FD_CLEANUP_PARTIAL,
    FD_CLEANUP_FAST_ONLY,
    FD_CLEANUP_BAD_DIRENT
};

extern void ts_output_test_set_fd_cleanup_mode(int mode);
extern int ts_output_test_close_unrelated_fds(int channel, int output_fd);
extern int ts_output_test_parse_dirents(const char *buffer, size_t length);

static void temp_path(char path[64]) {
    strcpy(path, "/tmp/pix-ts-output-fd-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
}

static int fd_open(int fd) {
    return fcntl(fd, F_GETFD) >= 0;
}

static void wait_child(pid_t pid, int expected) {
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == expected);
}

static void test_cleanup_pair(int mode, int reverse, int expect_error) {
    int pipe_a[2], pipe_b[2];
    assert(pipe(pipe_a) == 0 && pipe(pipe_b) == 0);
    int output_fd = pipe_a[0], channel = pipe_b[1];
    if (reverse) {
        int temp = output_fd;
        output_fd = channel;
        channel = temp;
    }
    int close_a = pipe_a[1], close_b = pipe_b[0];
    ts_output_test_set_fd_cleanup_mode(mode);
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        int rc = ts_output_test_close_unrelated_fds(channel, output_fd);
        if (expect_error) {
            if (rc < 0 && errno == EACCES)
                _exit(0);
            _exit(1);
        }
        if (rc || !fd_open(channel) || !fd_open(output_fd) || !fd_open(0) || !fd_open(1) ||
            !fd_open(2) || fd_open(close_a) || fd_open(close_b))
            _exit(1);
        _exit(0);
    }
    wait_child(pid, 0);
    close(pipe_a[0]);
    close(pipe_a[1]);
    close(pipe_b[0]);
    close(pipe_b[1]);
}

static void test_low_fds(void) {
    int pipe_a[2], pipe_b[2];
    assert(pipe(pipe_a) == 0 && pipe(pipe_b) == 0);
    ts_output_test_set_fd_cleanup_mode(FD_CLEANUP_FAST_ONLY);
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        if (dup2(pipe_a[0], STDIN_FILENO) < 0 || dup2(pipe_b[1], STDOUT_FILENO) < 0)
            _exit(1);
        close(pipe_a[0]);
        close(pipe_a[1]);
        close(pipe_b[0]);
        close(pipe_b[1]);
        if (ts_output_test_close_unrelated_fds(STDOUT_FILENO, STDIN_FILENO) ||
            !fd_open(STDIN_FILENO) || !fd_open(STDOUT_FILENO) || !fd_open(STDERR_FILENO))
            _exit(1);
        _exit(0);
    }
    wait_child(pid, 0);
    close(pipe_a[0]);
    close(pipe_a[1]);
    close(pipe_b[0]);
    close(pipe_b[1]);
}

static void assert_accounted(const ts_output_stats *stats) {
    assert(!stats->pending_bytes);
    assert(stats->submitted_bytes == stats->confirmed_bytes + stats->rejected_bytes +
                                          stats->uncertain_bytes);
}

static void test_high_fd(int mode) {
    int pipe_fd[2];
    assert(pipe(pipe_fd) == 0);
    struct rlimit original, raised, lowered;
    assert(getrlimit(RLIMIT_NOFILE, &original) == 0);
    if (original.rlim_max <= 70000) {
        puts("SKIP high-FD-unwanted fixture: RLIMIT_NOFILE hard limit does not permit FD 70000");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return;
    }
    raised = original;
    raised.rlim_cur = 70001;
    assert(setrlimit(RLIMIT_NOFILE, &raised) == 0);
    int high_fd = fcntl(pipe_fd[1], F_DUPFD_CLOEXEC, 70000);
    assert(high_fd >= 70000);
    close(pipe_fd[1]);
    lowered = raised;
    lowered.rlim_cur = 1024;
    assert(setrlimit(RLIMIT_NOFILE, &lowered) == 0);

    int output_fd = open("/dev/null", O_WRONLY);
    assert(output_fd >= 0);
    ts_output_test_set_fd_cleanup_mode(mode);
    ts_output *output = NULL;
    assert(ts_output_open("-", output_fd, NULL, &output) == 0);
    close(output_fd);
    close(high_fd);
    struct pollfd wait_eof = {pipe_fd[0], POLLIN | POLLHUP, 0};
    assert(poll(&wait_eof, 1, 1000) == 1);
    unsigned char byte;
    assert(read(pipe_fd[0], &byte, 1) == 0);
    close(pipe_fd[0]);

    unsigned char payload[257];
    memset(payload, 0x5a, sizeof(payload));
    assert(ts_output_submit(output, payload, sizeof(payload), NULL) == 0);
    assert(ts_output_finish(output, 3000) == 0);
    ts_output_stats stats;
    ts_output_snapshot(output, &stats);
    assert(stats.confirmed_bytes == sizeof(payload) && !stats.pending_bytes &&
           stats.child_status != -1);
    assert(ts_output_destroy(output) == 0);
    assert(setrlimit(RLIMIT_NOFILE, &original) == 0);
}

static void test_high_retained_fds(int mode) {
    struct rlimit original, raised;
    assert(getrlimit(RLIMIT_NOFILE, &original) == 0);
    if (original.rlim_max < 70002) {
        puts("SKIP high-FD-retained fixture: RLIMIT_NOFILE hard limit does not permit FDs 70000-70001");
        return;
    }
    int channel_pipe[2], output_pipe[2];
    assert(pipe(channel_pipe) == 0 && pipe(output_pipe) == 0);
    raised = original;
    raised.rlim_cur = 70002;
    assert(setrlimit(RLIMIT_NOFILE, &raised) == 0);
    int channel = fcntl(channel_pipe[1], F_DUPFD_CLOEXEC, 70000);
    int output_fd = fcntl(output_pipe[1], F_DUPFD_CLOEXEC, 70001);
    assert(channel >= 70000 && output_fd >= 70001);
    close(channel_pipe[1]);
    close(output_pipe[1]);
    ts_output_test_set_fd_cleanup_mode(mode);
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        if (ts_output_test_close_unrelated_fds(channel, output_fd) ||
            write(channel, "c", 1) != 1 || write(output_fd, "o", 1) != 1)
            _exit(1);
        _exit(0);
    }
    wait_child(pid, 0);
    char byte = 0;
    assert(read(channel_pipe[0], &byte, 1) == 1 && byte == 'c');
    assert(read(output_pipe[0], &byte, 1) == 1 && byte == 'o');
    close(channel);
    close(output_fd);
    close(channel_pipe[0]);
    close(output_pipe[0]);
    assert(setrlimit(RLIMIT_NOFILE, &original) == 0);
}

static size_t put_dirent(char *buffer, const char *name, unsigned short record_length) {
    size_t header_length = 19;
    memcpy(buffer + 16, &record_length, sizeof(record_length));
    if (name)
        memcpy(buffer + header_length, name, strlen(name) + 1);
    return record_length;
}

static void test_dirent_parser(void) {
    char bytes[128] = {0};
    size_t length = put_dirent(bytes + 1, "0", 21);
    assert(ts_output_test_parse_dirents(bytes + 1, length) == 0);

    memset(bytes, 0, sizeof(bytes));
    size_t first = put_dirent(bytes, "0", 21);
    size_t second = put_dirent(bytes + first, "1", 21);
    assert((first % 2) == 1 && ts_output_test_parse_dirents(bytes, first + second) == 0);

    memset(bytes, 0, sizeof(bytes));
    assert(ts_output_test_parse_dirents(bytes, 17) < 0);
    memset(bytes, 0, sizeof(bytes));
    assert(put_dirent(bytes, "0", 0) == 0 && ts_output_test_parse_dirents(bytes, 21) < 0);
    memset(bytes, 0, sizeof(bytes));
    assert(put_dirent(bytes, "0", 19) == 19 && ts_output_test_parse_dirents(bytes, 21) < 0);
    memset(bytes, 0, sizeof(bytes));
    assert(put_dirent(bytes, "0", 22) == 22 && ts_output_test_parse_dirents(bytes, 21) < 0);
    memset(bytes, '0', sizeof(bytes));
    unsigned short record_length = 21;
    memcpy(bytes + 16, &record_length, sizeof(record_length));
    assert(ts_output_test_parse_dirents(bytes, record_length) < 0);

    const char *bad_names[] = {"", "x", "4294967296", "2147483648"};
    for (size_t i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); ++i) {
        memset(bytes, 0, sizeof(bytes));
        unsigned short size = (unsigned short)(19 + strlen(bad_names[i]) + 1);
        put_dirent(bytes, bad_names[i], size);
        assert(ts_output_test_parse_dirents(bytes, size) < 0);
    }

    memset(bytes, 0, sizeof(bytes));
    first = put_dirent(bytes, "0", 21);
    bytes[first] = 1;
    assert(ts_output_test_parse_dirents(bytes, first + 1) < 0);
}

static void test_writer_failure(void) {
    char path[64];
    temp_path(path);
    ts_output_test_set_fd_cleanup_mode(FD_CLEANUP_FAIL);
    ts_output *output = NULL;
    assert(ts_output_open(path, -1, NULL, &output) == 0);
    ts_output_test_set_fd_cleanup_mode(FD_CLEANUP_NORMAL);
    unsigned char payload[211];
    memset(payload, 0x41, sizeof(payload));
    assert(ts_output_submit(output, payload, sizeof(payload), NULL) == 0);
    int rc = 0;
    for (unsigned i = 0; i < 100 && !rc; ++i) {
        rc = ts_output_poll(output, 5);
        if (!rc) {
            struct timespec pause = {0, 1000000L};
            nanosleep(&pause, NULL);
        }
    }
    assert(rc != 0);
    assert(ts_output_abort(output, 1000) == 0);
    ts_output_stats stats;
    ts_output_snapshot(output, &stats);
    assert_accounted(&stats);
    assert(stats.confirmed_bytes == 0 && stats.submitted_bytes == sizeof(payload) &&
           stats.child_status != -1 && WIFEXITED(stats.child_status) &&
           WEXITSTATUS(stats.child_status) == 87);
    struct stat st;
    assert(stat(path, &st) == 0 && st.st_size == 0);
    assert(ts_output_destroy(output) == 0);
    unlink(path);
}

static void test_malformed_writer_failure(void) {
    char path[64];
    temp_path(path);
    ts_output_test_set_fd_cleanup_mode(FD_CLEANUP_BAD_DIRENT);
    ts_output *output = NULL;
    assert(ts_output_open(path, -1, NULL, &output) == 0);
    ts_output_test_set_fd_cleanup_mode(FD_CLEANUP_NORMAL);
    unsigned char payload[211];
    memset(payload, 0x41, sizeof(payload));
    assert(ts_output_submit(output, payload, sizeof(payload), NULL) == 0);
    int rc = 0;
    for (unsigned i = 0; i < 100 && !rc; ++i) {
        rc = ts_output_poll(output, 5);
        if (!rc) {
            struct timespec pause = {0, 1000000L};
            nanosleep(&pause, NULL);
        }
    }
    assert(rc != 0);
    assert(ts_output_abort(output, 1000) == 0);
    ts_output_stats stats;
    ts_output_snapshot(output, &stats);
    assert_accounted(&stats);
    assert(stats.confirmed_bytes == 0 && stats.submitted_bytes == sizeof(payload) &&
           stats.child_status != -1 && WIFEXITED(stats.child_status) &&
           WEXITSTATUS(stats.child_status) == 87);
    struct stat st;
    assert(stat(path, &st) == 0 && st.st_size == 0);
    assert(ts_output_destroy(output) == 0);
    unlink(path);
}

static void test_independent_writer_eof(void) {
    char path_a[64], path_b[64];
    temp_path(path_a);
    temp_path(path_b);
    ts_output_config fail = {0}, stall = {0};
    fail.test_crash_after_receive = 1;
    stall.test_stall_after_receive = 1;
    ts_output *a = NULL, *b = NULL;
    ts_output_test_set_fd_cleanup_mode(FD_CLEANUP_NORMAL);
    assert(ts_output_open(path_a, -1, &fail, &a) == 0);
    assert(ts_output_open(path_b, -1, &stall, &b) == 0);
    unsigned char payload[211];
    memset(payload, 0x6b, sizeof(payload));
    assert(ts_output_submit(a, payload, sizeof(payload), NULL) == 0);
    int rc = 0;
    for (unsigned i = 0; i < 100 && !rc; ++i) {
        rc = ts_output_poll(a, 5);
        if (!rc) {
            struct timespec pause = {0, 1000000L};
            nanosleep(&pause, NULL);
        }
    }
    assert(rc != 0);
    assert(ts_output_abort(a, 1000) == 0);
    ts_output_stats stats;
    ts_output_snapshot(a, &stats);
    assert_accounted(&stats);
    assert(stats.uncertain_bytes == sizeof(payload) && stats.child_status != -1 &&
           WIFEXITED(stats.child_status) && WEXITSTATUS(stats.child_status) == 86);
    assert(ts_output_abort(b, 1000) == 0);
    assert(ts_output_destroy(a) == 0 && ts_output_destroy(b) == 0);
    unlink(path_a);
    unlink(path_b);
}

int main(void) {
    /* Both keep orders, forced proc fallback, partial fast-path fallback, and fail-closed. */
    test_cleanup_pair(FD_CLEANUP_FAST_ONLY, 0, 0);
    test_cleanup_pair(FD_CLEANUP_FALLBACK, 1, 0);
    test_cleanup_pair(FD_CLEANUP_PARTIAL, 0, 0);
    test_cleanup_pair(FD_CLEANUP_FAIL, 0, 1);

    test_low_fds();
    test_dirent_parser();
    test_high_fd(FD_CLEANUP_NORMAL);
    test_high_fd(FD_CLEANUP_FALLBACK);
    test_high_retained_fds(FD_CLEANUP_NORMAL);
    test_high_retained_fds(FD_CLEANUP_FALLBACK);
    test_writer_failure();
    test_malformed_writer_failure();
    test_independent_writer_eof();
    puts("TS output Linux FD cleanup tests passed: fast path, fallback, partial fallback, fail-closed, retained low/high descriptors, independent writer EOF, reap and accounting");
    return 0;
}
