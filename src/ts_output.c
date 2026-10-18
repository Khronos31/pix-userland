#ifdef __linux__
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include "ts_output.h"

#include <errno.h>
#ifdef _WIN32
#include <string.h>
#ifndef ENOTSUP
#define ENOTSUP EINVAL
#endif
int ts_output_open(const char *p, int fd, const ts_output_config *c, ts_output **o) {
    (void)p;
    (void)fd;
    (void)c;
    if (o)
        *o = NULL;
    return ENOTSUP;
}
int ts_output_submit(ts_output *o, const void *d, size_t n, uint64_t *id) {
    (void)o;
    (void)d;
    (void)n;
    (void)id;
    return ENOTSUP;
}
int ts_output_poll(ts_output *o, unsigned ms) {
    (void)o;
    (void)ms;
    return ENOTSUP;
}
void ts_output_snapshot(ts_output *o, ts_output_stats *s) {
    (void)o;
    if (s)
        memset(s, 0, sizeof(*s));
}
int ts_output_finish(ts_output *o, unsigned ms) {
    (void)o;
    (void)ms;
    return ENOTSUP;
}
int ts_output_abort(ts_output *o, unsigned ms) {
    (void)o;
    (void)ms;
    return ENOTSUP;
}
int ts_output_destroy(ts_output *o) {
    (void)o;
    return ENOTSUP;
}
#else
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

#define TS_CHILD_END UINT64_MAX
#define TS_OUTPUT_IPC_WINDOW_BYTES (1024U * 1024U)
#define TS_CHILD_FD_CLEANUP_FAILED 87
struct ts_frame {
    uint64_t id;
    uint64_t length;
};
struct ts_ack {
    uint64_t id;
    uint64_t written;
    int32_t error;
    int32_t close_error;
    uint64_t writer_write_ns;
};
struct ts_job {
    struct ts_job *next;
    uint64_t id;
    size_t length, sent;
    uint64_t submitted_ns;
    int ipc_complete;
    unsigned char *data;
};
struct ts_output {
    pid_t child;
    int socket_fd, pipe_output;
    struct ts_job *head, *tail, *current;
    struct ts_frame frame;
    size_t frame_sent;
    unsigned char ack_buffer[sizeof(struct ts_ack)];
    size_t ack_have;
    size_t queued, ipc_inflight_bytes;
    size_t capacity;
    uint64_t next_id, last_progress_ns, idle_since_ns, last_pending_poll_ns, last_ack_ns;
    unsigned warn_ms, file_stall_ms, pipe_stall_ms, drain_ms;
    int first_error, accepting, ending, end_sent, ended, reaped, child_status;
    int protocol_failed;
    size_t test_write_chunk;
    uint64_t test_fail_after;
    int test_close_error;
    ts_output_stats stats;
};

static uint64_t mono_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return 0;
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}
static void add_counter(uint64_t *counter, uint64_t amount) {
    if (UINT64_MAX - *counter < amount)
        *counter = UINT64_MAX;
    else
        *counter += amount;
}
static void remember_error(ts_output *o, int error) {
    if (!o->first_error)
        o->first_error = error ? error : EIO;
    o->stats.first_error = o->first_error;
    o->stats.child_pid = (int64_t)o->child;
}
static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}
static int write_full(int fd, const void *data, size_t length) {
    const unsigned char *p = data;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n > 0) {
            p += (size_t)n;
            length -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd f = {fd, POLLOUT, 0};
            int r;
            do {
                r = poll(&f, 1, -1);
            } while (r < 0 && errno == EINTR);
            if (r > 0)
                continue;
        }
        return errno ? errno : EIO;
    }
    return 0;
}
static int read_full(int fd, void *data, size_t length) {
    unsigned char *p = data;
    while (length) {
        ssize_t n = read(fd, p, length);
        if (n > 0) {
            p += (size_t)n;
            length -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return n == 0 ? EPIPE : (errno ? errno : EIO);
    }
    return 0;
}
#ifdef __linux__
struct ts_linux_dirent64 {
    uint64_t ino;
    int64_t offset;
    unsigned short record_length;
    unsigned char type;
    char name[];
};
static int ts_close_unrelated_fds(int channel, int output_fd);
static int ts_parse_proc_fd_entries(const char *buffer, size_t length, const int *keep,
                                    unsigned count, int enum_fd);

#ifdef TS_OUTPUT_TESTING_FD_CLEANUP
enum {
    TS_TEST_FD_CLEANUP_NORMAL,
    TS_TEST_FD_CLEANUP_FALLBACK,
    TS_TEST_FD_CLEANUP_FAIL,
    TS_TEST_FD_CLEANUP_PARTIAL,
    TS_TEST_FD_CLEANUP_FAST_ONLY,
    TS_TEST_FD_CLEANUP_BAD_DIRENT
};
static int test_fd_cleanup_mode;
static unsigned test_close_range_calls;

void ts_output_test_set_fd_cleanup_mode(int mode) {
    test_fd_cleanup_mode = mode;
    test_close_range_calls = 0;
}

int ts_output_test_close_unrelated_fds(int channel, int output_fd) {
    return ts_close_unrelated_fds(channel, output_fd);
}

int ts_output_test_parse_dirents(const char *buffer, size_t length) {
    const int keep[] = {0, 1, 2};
    return ts_parse_proc_fd_entries(buffer, length, keep, 3, -1);
}
#endif

static int ts_fd_is_kept(unsigned int fd, const int *keep, unsigned count, int enum_fd) {
    if (fd == (unsigned int)enum_fd)
        return 1;
    for (unsigned i = 0; i < count; ++i)
        if (fd == (unsigned int)keep[i])
            return 1;
    return 0;
}

static int ts_name_is_dot(const char *name, size_t length) {
    return length == 1 && name[0] == '.';
}

static int ts_name_is_dotdot(const char *name, size_t length) {
    return length == 2 && name[0] == '.' && name[1] == '.';
}

static int ts_close_fd_once(int fd) {
    int rc = close(fd);
    if (!rc || errno == EINTR)
        return 0;
    return -1;
}

static int ts_parse_proc_fd_entries(const char *buffer, size_t length, const int *keep,
                                   unsigned count, int enum_fd) {
    size_t header_length = offsetof(struct ts_linux_dirent64, name);
    const char *entry = buffer;
    size_t remaining = length;
    while (remaining) {
        size_t record_length_offset = offsetof(struct ts_linux_dirent64, record_length);
        if (remaining < record_length_offset + sizeof(unsigned short)) {
            errno = EIO;
            return -1;
        }
        unsigned short raw_record_length;
        memcpy(&raw_record_length, entry + record_length_offset, sizeof(raw_record_length));
        size_t record_length = raw_record_length;
        if (remaining < header_length + 1 || record_length < header_length + 1 ||
            record_length > remaining) {
            errno = EIO;
            return -1;
        }
        const char *name = entry + header_length;
        size_t name_capacity = record_length - header_length;
        size_t name_length = 0;
        while (name_length < name_capacity && name[name_length])
            ++name_length;
        if (name_length == name_capacity) {
            errno = EIO;
            return -1;
        }
        if (!ts_name_is_dot(name, name_length) && !ts_name_is_dotdot(name, name_length)) {
            if (!name_length) {
                errno = EIO;
                return -1;
            }
            unsigned int fd = 0;
            for (size_t i = 0; i < name_length; ++i) {
                char digit = name[i];
                if (digit < '0' || digit > '9' ||
                    fd > (UINT_MAX - (unsigned int)(digit - '0')) / 10U) {
                    errno = EIO;
                    return -1;
                }
                fd = fd * 10U + (unsigned int)(digit - '0');
            }
            if (fd > INT_MAX) {
                errno = EIO;
                return -1;
            }
            if (!ts_fd_is_kept(fd, keep, count, enum_fd) && ts_close_fd_once((int)fd))
                return -1;
        }
        entry += record_length;
        remaining -= record_length;
    }
    return 0;
}

static int ts_close_range(unsigned int first, unsigned int last) {
    if (first > last)
        return 0;
#ifdef TS_OUTPUT_TESTING_FD_CLEANUP
    if (test_fd_cleanup_mode == TS_TEST_FD_CLEANUP_FAIL) {
        errno = EPERM;
        return -1;
    }
    if (test_fd_cleanup_mode == TS_TEST_FD_CLEANUP_PARTIAL && test_close_range_calls++) {
        errno = EPERM;
        return -1;
    }
#endif
#if defined(SYS_close_range)
    return (int)syscall(SYS_close_range, first, last, 0U);
#elif defined(__NR_close_range)
    return (int)syscall(__NR_close_range, first, last, 0U);
#else
    (void)first;
    (void)last;
    errno = ENOSYS;
    return -1;
#endif
}

static int ts_close_fds_by_proc(const int *keep, unsigned count) {
#if defined(SYS_openat) && defined(SYS_getdents64)
#ifdef TS_OUTPUT_TESTING_FD_CLEANUP
    if (test_fd_cleanup_mode == TS_TEST_FD_CLEANUP_FAIL) {
        errno = EACCES;
        return -1;
    }
#endif
    int dir_fd;
    do {
        dir_fd = (int)syscall(SYS_openat, AT_FDCWD, "/proc/self/fd",
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    } while (dir_fd < 0 && errno == EINTR);
    if (dir_fd < 0)
        return -1;

    char buffer[4096];
    int failed = 0;
#ifdef TS_OUTPUT_TESTING_FD_CLEANUP
    if (test_fd_cleanup_mode == TS_TEST_FD_CLEANUP_BAD_DIRENT) {
        char malformed[64] = {0};
        size_t header_length = offsetof(struct ts_linux_dirent64, name);
        size_t prefix_length = header_length + 2;
        unsigned short record_length = (unsigned short)prefix_length;
        memcpy(malformed + offsetof(struct ts_linux_dirent64, record_length),
               &record_length, sizeof(record_length));
        malformed[header_length] = '0';
        malformed[header_length + 1] = '\0';
        int rc = ts_parse_proc_fd_entries(malformed, prefix_length + 1, keep, count, dir_fd);
        int saved_errno = errno;
        (void)ts_close_fd_once(dir_fd);
        errno = saved_errno;
        return rc;
    }
#endif
    for (;;) {
        long length;
        do {
            length = syscall(SYS_getdents64, dir_fd, buffer, sizeof(buffer));
        } while (length < 0 && errno == EINTR);
        if (length < 0) {
            failed = 1;
            break;
        }
        if (!length)
            break;
        if ((size_t)length > sizeof(buffer)) {
            failed = 1;
            break;
        }
        if (ts_parse_proc_fd_entries(buffer, (size_t)length, keep, count, dir_fd)) {
            failed = 1;
            break;
        }
    }
    if (ts_close_fd_once(dir_fd))
        failed = 1;
    if (failed) {
        if (!errno)
            errno = EIO;
        return -1;
    }
    return 0;
#else
    (void)keep;
    (void)count;
    errno = ENOSYS;
    return -1;
#endif
}

static int ts_close_unrelated_fds(int channel, int output_fd) {
    int keep[5] = {0, 1, 2, channel, output_fd};
    unsigned count = 0;
    for (unsigned i = 0; i < sizeof(keep) / sizeof(keep[0]); ++i) {
        if (keep[i] < 0)
            continue;
        int value = keep[i];
        unsigned insert = count;
        while (insert && keep[insert - 1] > value) {
            keep[insert] = keep[insert - 1];
            --insert;
        }
        keep[insert] = value;
        ++count;
    }
    unsigned unique = 0;
    for (unsigned i = 0; i < count; ++i)
        if (!unique || keep[i] != keep[unique - 1])
            keep[unique++] = keep[i];
    count = unique;

#ifdef TS_OUTPUT_TESTING_FD_CLEANUP
    if (test_fd_cleanup_mode == TS_TEST_FD_CLEANUP_FALLBACK ||
        test_fd_cleanup_mode == TS_TEST_FD_CLEANUP_BAD_DIRENT)
        return ts_close_fds_by_proc(keep, count);
#endif

    unsigned int first = 3;
    int finished = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (keep[i] < 3)
            continue;
        unsigned int fd = (unsigned int)keep[i];
        if (fd > first && ts_close_range(first, fd - 1U)) {
#ifdef TS_OUTPUT_TESTING_FD_CLEANUP
            if (test_fd_cleanup_mode == TS_TEST_FD_CLEANUP_FAST_ONLY)
                return -1;
#endif
            return ts_close_fds_by_proc(keep, count);
        }
        if (fd == UINT_MAX) {
            finished = 1;
            break;
        }
        if (fd >= first)
            first = fd + 1U;
    }
    if (!finished && ts_close_range(first, UINT_MAX)) {
#ifdef TS_OUTPUT_TESTING_FD_CLEANUP
        if (test_fd_cleanup_mode == TS_TEST_FD_CLEANUP_FAST_ONLY)
            return -1;
#endif
        return ts_close_fds_by_proc(keep, count);
    }
    return 0;
}
#endif

static void child_loop(int channel, int output_fd, int is_pipe, int max_fd, size_t test_chunk,
                       uint64_t test_fail_after, int test_close_error, int test_crash_after_receive,
                       int test_stall_close_ack, int test_stall_after_receive,
                       unsigned test_ack_batch_records, unsigned test_write_delay_ms) {
    (void)signal(SIGPIPE, SIG_IGN);
    (void)signal(SIGTERM, SIG_DFL);
    (void)signal(SIGINT, SIG_IGN);
#ifdef __linux__
    (void)max_fd;
    if (ts_close_unrelated_fds(channel, output_fd))
        _exit(TS_CHILD_FD_CLEANUP_FAILED);
#else
    for (int fd = 3; fd < max_fd; ++fd)
        if (fd != channel && fd != output_fd)
            (void)close(fd);
#endif
    uint64_t total_written = 0;
    struct ts_ack pending_acks[64];
    unsigned pending_ack_count = 0;
    if (test_ack_batch_records > 64)
        test_ack_batch_records = 64;
    for (;;) {
        struct ts_frame frame;
        int e = read_full(channel, &frame, sizeof(frame));
        if (e)
            _exit(80);
        if (frame.id == TS_CHILD_END) {
            for (unsigned i = 0; i < pending_ack_count; ++i)
                if (write_full(channel, &pending_acks[i], sizeof(pending_acks[i])))
                    _exit(84);
            int close_error = close(output_fd) ? errno : 0;
            if (!close_error && test_close_error)
                close_error = EIO;
            if (test_stall_close_ack) {
                for (;;) {
                    struct timespec pause = {3600, 0};
                    nanosleep(&pause, NULL);
                }
            }
            struct ts_ack ack = {TS_CHILD_END, 0, 0, close_error, 0};
            (void)write_full(channel, &ack, sizeof(ack));
            close(channel);
            _exit(close_error ? 3 : 0);
        }
        if (!frame.length || frame.length > TS_OUTPUT_CAPACITY_BYTES)
            _exit(81);
        size_t length = (size_t)frame.length;
        unsigned char *buffer = malloc(length);
        if (!buffer)
            _exit(82);
        e = read_full(channel, buffer, length);
        if (e) {
            free(buffer);
            _exit(83);
        }
        if (test_crash_after_receive)
            _exit(86);
        if (test_stall_after_receive) {
            for (;;) {
                struct timespec pause = {3600, 0};
                nanosleep(&pause, NULL);
            }
        }
        uint64_t write_start_ns = mono_ns();
        if (test_write_delay_ms) {
            struct timespec delay = {(time_t)(test_write_delay_ms / 1000),
                                     (long)(test_write_delay_ms % 1000) * 1000000L};
            while (nanosleep(&delay, &delay) && errno == EINTR) {
            }
        }
        size_t off = 0;
        int write_error = 0;
        while (off < length) {
            size_t amount = length - off;
            if (is_pipe && amount > 4096)
                amount = 4096;
            if (test_chunk && amount > test_chunk)
                amount = test_chunk;
            if (test_fail_after && total_written >= test_fail_after) {
                write_error = EIO;
                break;
            }
            if (test_fail_after && amount > test_fail_after - total_written)
                amount = (size_t)(test_fail_after - total_written);
            if (!amount) {
                write_error = EIO;
                break;
            }
            ssize_t n = write(output_fd, buffer + off, amount);
            if (n > 0) {
                off += (size_t)n;
                total_written += (uint64_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                struct pollfd f = {output_fd, POLLOUT, 0};
                int r;
                do {
                    r = poll(&f, 1, -1);
                } while (r < 0 && errno == EINTR);
                if (r > 0)
                    continue;
            }
            write_error = errno ? errno : EIO;
            break;
        }
        free(buffer);
        uint64_t write_duration_ns = mono_ns() - write_start_ns;
        struct ts_ack ack = {frame.id, off, write_error, 0, write_duration_ns};
        if (test_ack_batch_records) {
            pending_acks[pending_ack_count++] = ack;
            if (pending_ack_count >= test_ack_batch_records) {
                for (unsigned i = 0; i < pending_ack_count; ++i)
                    if (write_full(channel, &pending_acks[i], sizeof(pending_acks[i])))
                        _exit(84);
                pending_ack_count = 0;
            }
        } else {
            e = write_full(channel, &ack, sizeof(ack));
            if (e)
                _exit(84);
        }
        if (write_error) {
            (void)close(output_fd);
            close(channel);
            _exit(4);
        }
    }
}
static void classify_unresolved(ts_output *o) {
    for (struct ts_job *j = o->head; j; j = j->next) {
        if (j->ipc_complete)
            o->stats.uncertain_bytes += j->length;
        else
            o->stats.rejected_bytes += j->length;
    }
    o->stats.pending_bytes = 0;
    o->queued = 0;
    o->last_pending_poll_ns = 0;
    struct ts_job *j = o->head;
    while (j) {
        struct ts_job *next = j->next;
        free(j->data);
        free(j);
        j = next;
    }
    o->head = o->tail = o->current = NULL;
}
static void observe_child(ts_output *o) {
    if (o->reaped || o->child <= 0)
        return;
    int status = 0;
    pid_t r = waitpid(o->child, &status, WNOHANG);
    if (r == o->child) {
        o->reaped = 1;
        o->child_status = status;
        o->stats.child_status = status;
        if (!o->ended) {
            remember_error(o, EPIPE);
            classify_unresolved(o);
        } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            remember_error(o, EIO);
        }
    } else if (r < 0 && errno != EINTR) {
        remember_error(o, errno);
    }
}
static void remove_head(ts_output *o) {
    struct ts_job *j = o->head;
    if (!j)
        return;
    o->head = j->next;
    if (!o->head)
        o->tail = NULL;
    if (j->ipc_complete) {
        if (o->ipc_inflight_bytes >= j->length)
            o->ipc_inflight_bytes -= j->length;
        else
            o->ipc_inflight_bytes = 0;
    }
    o->queued -= j->length;
    free(j->data);
    free(j);
}
static int pump_send(ts_output *o) {
    if (o->reaped || o->first_error)
        return 0;
    if (!o->current) {
        struct ts_job *j = o->head;
        while (j && j->ipc_complete)
            j = j->next;
        if (j) {
            /* Permit one oversized record when the window is empty. */
            if (o->ipc_inflight_bytes &&
                (o->ipc_inflight_bytes >= TS_OUTPUT_IPC_WINDOW_BYTES ||
                 j->length > TS_OUTPUT_IPC_WINDOW_BYTES - o->ipc_inflight_bytes))
                return 0;
            o->current = j;
            o->frame.id = o->current->id;
            o->frame.length = o->current->length;
            o->frame_sent = 0;
            o->current->sent = 0;
        }
    }
    if (o->current) {
        if (o->frame_sent < sizeof(o->frame)) {
            const unsigned char *p = (const unsigned char *)&o->frame + o->frame_sent;
            ssize_t n = send(o->socket_fd, p, sizeof(o->frame) - o->frame_sent, MSG_NOSIGNAL);
            if (n > 0) {
                o->frame_sent += (size_t)n;
                return 1;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    add_counter(&o->stats.send_eagain_count, 1);
                return 0;
            }
            remember_error(o, errno ? errno : EPIPE);
            return -1;
        }
        if (o->current->sent < o->current->length) {
            ssize_t n = send(o->socket_fd, o->current->data + o->current->sent,
                             o->current->length - o->current->sent, MSG_NOSIGNAL);
            if (n > 0) {
                o->current->sent += (size_t)n;
                if (o->current->sent == o->current->length) {
                    o->current->ipc_complete = 1;
                    o->ipc_inflight_bytes += o->current->length;
                    o->current = NULL;
                    o->frame_sent = 0;
                }
                return 1;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    add_counter(&o->stats.send_eagain_count, 1);
                return 0;
            }
            remember_error(o, errno ? errno : EPIPE);
            return -1;
        }
    } else if (o->ending && !o->end_sent && !o->head) {
        struct ts_frame end = {TS_CHILD_END, 0};
        ssize_t n = send(o->socket_fd, (const unsigned char *)&end, sizeof(end), MSG_NOSIGNAL);
        if (n == (ssize_t)sizeof(end)) {
            o->end_sent = 1;
            o->last_progress_ns = mono_ns();
            return 1;
        }
        if (n > 0) { /* small atomic frame; continue through the general path next poll */
            o->frame = end;
            o->frame_sent = (size_t)n;
            o->end_sent = -1;
            return 1;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                add_counter(&o->stats.send_eagain_count, 1);
            return 0;
        }
        remember_error(o, errno ? errno : EPIPE);
        return -1;
    } else if (o->ending && o->end_sent == -1) {
        ssize_t n = send(o->socket_fd, (const unsigned char *)&o->frame + o->frame_sent,
                         sizeof(o->frame) - o->frame_sent, MSG_NOSIGNAL);
        if (n > 0) {
            o->frame_sent += (size_t)n;
            if (o->frame_sent == sizeof(o->frame))
                o->end_sent = 1;
            return 1;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                add_counter(&o->stats.send_eagain_count, 1);
            return 0;
        }
        remember_error(o, errno ? errno : EPIPE);
        return -1;
    }
    return 0;
}
static int pump_ack(ts_output *o) {
    ssize_t n =
        recv(o->socket_fd, o->ack_buffer + o->ack_have, sizeof(struct ts_ack) - o->ack_have, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            add_counter(&o->stats.ack_eagain_count, 1);
        return 0;
    }
    if (n == 0) {
        observe_child(o);
        if (!o->reaped)
            remember_error(o, EPIPE);
        return -1;
    }
    if (n < 0) {
        remember_error(o, errno ? errno : EIO);
        return -1;
    }
    o->ack_have += (size_t)n;
    if (o->ack_have != sizeof(struct ts_ack))
        return 1;
    struct ts_ack ack;
    memcpy(&ack, o->ack_buffer, sizeof(ack));
    o->ack_have = 0;
    o->last_progress_ns = mono_ns();
    o->stats.last_progress_ns = o->last_progress_ns;
    if (ack.id == TS_CHILD_END) {
        if (!o->end_sent || ack.close_error)
            remember_error(o, ack.close_error ? ack.close_error : EPROTO);
        else
            o->ended = 1;
        return 1;
    }
    if (!o->head || !o->head->ipc_complete || o->ipc_inflight_bytes < o->head->length ||
        ack.id != o->head->id || ack.written > o->head->length) {
        remember_error(o, EPROTO);
        o->protocol_failed = 1;
        return -1;
    }
    uint64_t ack_now_ns = o->last_progress_ns;
    if (o->last_ack_ns && ack_now_ns >= o->last_ack_ns &&
        ack_now_ns - o->last_ack_ns > o->stats.max_parent_ack_gap_ns)
        o->stats.max_parent_ack_gap_ns = ack_now_ns - o->last_ack_ns;
    o->last_ack_ns = ack_now_ns;
    add_counter(&o->stats.writer_write_ns_total, ack.writer_write_ns);
    add_counter(&o->stats.writer_write_records, 1);
    if (ack.writer_write_ns > o->stats.writer_write_ns_max)
        o->stats.writer_write_ns_max = ack.writer_write_ns;
    uint64_t pending_age_ns = ack_now_ns >= o->head->submitted_ns
                                  ? ack_now_ns - o->head->submitted_ns
                                  : 0;
    if (pending_age_ns > o->stats.max_pending_age_ns)
        o->stats.max_pending_age_ns = pending_age_ns;
    o->stats.confirmed_bytes += ack.written;
    if (ack.error) {
        o->stats.rejected_bytes += o->head->length - ack.written;
        remember_error(o, ack.error);
    } else if (ack.written != o->head->length) {
        o->stats.rejected_bytes += o->head->length - ack.written;
        remember_error(o, EIO);
    } else {
        ++o->stats.completed_records;
    }
    remove_head(o);
    o->stats.pending_bytes = o->stats.submitted_bytes - o->stats.confirmed_bytes -
                             o->stats.rejected_bytes - o->stats.uncertain_bytes;
    if (!o->stats.pending_bytes)
        o->last_pending_poll_ns = 0;
    return 1;
}
static int has_unsent_job(const ts_output *o) {
    for (const struct ts_job *j = o->head; j; j = j->next)
        if (!j->ipc_complete)
            return 1;
    return 0;
}
static int can_send_next_job(const ts_output *o) {
    if (o->current)
        return 1;
    for (const struct ts_job *j = o->head; j; j = j->next) {
        if (j->ipc_complete)
            continue;
        return !o->ipc_inflight_bytes ||
               (o->ipc_inflight_bytes < TS_OUTPUT_IPC_WINDOW_BYTES &&
                j->length <= TS_OUTPUT_IPC_WINDOW_BYTES - o->ipc_inflight_bytes);
    }
    return o->ending && !o->head && o->end_sent != 1;
}
static void check_stall(ts_output *o) {
    if (!o->stats.pending_bytes || o->first_error)
        return;
    uint64_t now = mono_ns();
    uint64_t elapsed = now - o->last_progress_ns;
    unsigned warn = o->pipe_output ? o->pipe_stall_ms : o->warn_ms;
    unsigned fail = o->pipe_output ? o->pipe_stall_ms : o->file_stall_ms;
    if (elapsed >= (uint64_t)warn * 1000000ULL)
        o->stats.warning = 1;
    if (elapsed >= (uint64_t)fail * 1000000ULL) {
        o->stats.stalled = 1;
        remember_error(o, ETIMEDOUT);
    }
}
int ts_output_open(const char *path, int stdout_fd, const ts_output_config *config,
                   ts_output **out) {
    if (!out)
        return EINVAL;
    *out = NULL;
    int target = -1, owned = 0;
    if (!path || !strcmp(path, "-")) {
        if (stdout_fd < 0)
            return EBADF;
        target = dup(stdout_fd);
        if (target < 0)
            return errno;
        owned = 1;
    } else {
        target = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
        if (target < 0)
            return errno;
        owned = 1;
    }
    struct stat st;
    int is_pipe = !fstat(target, &st) && (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode));
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) {
        int e = errno;
        close(target);
        return e;
    }
    /* A receive tick can submit a 64 KiB block. Keep enough IPC send space for
       one such block so the nonblocking pump does not strand every frame on
       repeated per-tick EAGAINs while the isolated writer drains it. */
    int ipc_send_bytes = 256 * 1024;
    if (setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &ipc_send_bytes, sizeof(ipc_send_bytes))) {
        int e = errno;
        close(pair[0]);
        close(pair[1]);
        close(target);
        return e;
    }
    if (set_nonblock(pair[0])) {
        int e = errno;
        close(pair[0]);
        close(pair[1]);
        close(target);
        return e;
    }
    struct rlimit limit;
    int max_fd = 65536;
    if (!getrlimit(RLIMIT_NOFILE, &limit) && limit.rlim_cur != RLIM_INFINITY &&
        limit.rlim_cur < (rlim_t)max_fd)
        max_fd = (int)limit.rlim_cur;
    size_t test_chunk = config ? config->test_write_chunk_bytes : 0;
    uint64_t test_fail_after = config ? config->test_fail_after_bytes : 0;
    int test_close_error = config ? config->test_close_error : 0;
    int test_crash = config ? config->test_crash_after_receive : 0;
    int test_stall_close_ack = config ? config->test_stall_close_ack : 0;
    int test_stall_after_receive = config ? config->test_stall_after_receive : 0;
    unsigned test_ack_batch_records = config ? config->test_ack_batch_records : 0;
    unsigned test_write_delay_ms = config ? config->test_write_delay_ms : 0;
    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        close(pair[0]);
        close(pair[1]);
        close(target);
        return e;
    }
    if (pid == 0) {
        close(pair[0]);
        child_loop(pair[1], target, is_pipe, max_fd, test_chunk, test_fail_after, test_close_error,
                   test_crash, test_stall_close_ack, test_stall_after_receive,
                   test_ack_batch_records, test_write_delay_ms);
        _exit(85);
    }
    close(pair[1]);
    if (owned)
        close(target);
    ts_output *o = calloc(1, sizeof(*o));
    if (!o) {
        close(pair[0]);
        kill(pid, SIGTERM);
        (void)waitpid(pid, NULL, 0);
        return ENOMEM;
    }
    o->child = pid;
    o->socket_fd = pair[0];
    o->pipe_output = is_pipe;
    o->capacity =
        config && config->capacity_bytes ? config->capacity_bytes : TS_OUTPUT_CAPACITY_BYTES;
    if (!o->capacity || o->capacity > TS_OUTPUT_CAPACITY_BYTES)
        o->capacity = TS_OUTPUT_CAPACITY_BYTES;
    o->warn_ms =
        config && config->file_warning_ms ? config->file_warning_ms : TS_OUTPUT_WARN_FILE_MS;
    o->file_stall_ms =
        config && config->file_stall_ms ? config->file_stall_ms : TS_OUTPUT_FAIL_FILE_MS;
    o->pipe_stall_ms =
        config && config->pipe_stall_ms ? config->pipe_stall_ms : TS_OUTPUT_FAIL_PIPE_MS;
    o->drain_ms = config && config->drain_ms ? config->drain_ms : TS_OUTPUT_DRAIN_MS;
    o->accepting = 1;
    o->child_status = -1;
    o->stats.child_status = -1;
    o->stats.child_pid = (int64_t)pid;
    o->test_write_chunk = test_chunk;
    o->test_fail_after = test_fail_after;
    o->test_close_error = test_close_error;
    o->last_progress_ns = o->idle_since_ns = mono_ns();
    *out = o;
    return 0;
}
int ts_output_submit(ts_output *o, const void *data, size_t length, uint64_t *id) {
    if (!o || (!data && length) || !length)
        return EINVAL;
    if (!o->accepting || o->first_error)
        return o->first_error ? o->first_error : EPIPE;
    if (length > o->capacity - o->queued) {
        o->stats.submitted_bytes += length;
        o->stats.rejected_bytes += length;
        return EAGAIN;
    }
    struct ts_job *j = calloc(1, sizeof(*j));
    if (!j) {
        o->stats.submitted_bytes += length;
        o->stats.rejected_bytes += length;
        remember_error(o, ENOMEM);
        return ENOMEM;
    }
    j->data = malloc(length);
    if (!j->data) {
        free(j);
        o->stats.submitted_bytes += length;
        o->stats.rejected_bytes += length;
        remember_error(o, ENOMEM);
        return ENOMEM;
    }
    int was_empty = o->stats.pending_bytes == 0;
    j->submitted_ns = mono_ns();
    memcpy(j->data, data, length);
    j->length = length;
    j->id = ++o->next_id;
    if (o->tail)
        o->tail->next = j;
    else
        o->head = j;
    o->tail = j;
    o->queued += length;
    o->stats.submitted_bytes += length;
    o->stats.pending_bytes += length;
    if (o->stats.pending_bytes > o->stats.peak_pending_bytes)
        o->stats.peak_pending_bytes = o->stats.pending_bytes;
    if (id)
        *id = j->id;
    if (was_empty) {
        o->last_progress_ns = j->submitted_ns;
        o->last_pending_poll_ns = j->submitted_ns;
    }
    return 0;
}
int ts_output_poll(ts_output *o, unsigned timeout_ms) {
    if (!o)
        return EINVAL;
    uint64_t poll_start_ns = mono_ns();
    if (o->stats.pending_bytes) {
        if (o->last_pending_poll_ns && poll_start_ns >= o->last_pending_poll_ns &&
            poll_start_ns - o->last_pending_poll_ns > o->stats.max_parent_poll_gap_ns)
            o->stats.max_parent_poll_gap_ns = poll_start_ns - o->last_pending_poll_ns;
        o->last_pending_poll_ns = poll_start_ns;
    } else
        o->last_pending_poll_ns = 0;
    uint64_t until = poll_start_ns + (uint64_t)timeout_ms * 1000000ULL;
    /* A zero-time poll is used on the USB receive path. Drain all work that is
       already runnable, but cap the work so a busy child cannot starve capture. */
    const unsigned zero_poll_step_limit = 128;
    const uint64_t zero_poll_time_limit_ns = 2000000ULL;
    uint64_t zero_poll_start = mono_ns();
    unsigned zero_poll_steps = 0;
    do {
        int send_progress = pump_send(o);
        int ack_progress = pump_ack(o);
        observe_child(o);
        check_stall(o);
        if (o->first_error)
            return o->first_error;
        if (timeout_ms == 0) {
            if (!send_progress && !ack_progress)
                return 0;
            if (++zero_poll_steps >= zero_poll_step_limit ||
                mono_ns() - zero_poll_start >= zero_poll_time_limit_ns) {
                add_counter(&o->stats.zero_poll_budget_exhaustions, 1);
                return 0;
            }
            continue;
        }
        if (ack_progress || mono_ns() >= until)
            return 0;
        int need_write = can_send_next_job(o) &&
                         (o->current || has_unsent_job(o) || (o->ending && !o->head));
        struct pollfd f = {o->socket_fd, (short)(POLLIN | (need_write ? POLLOUT : 0)), 0};
        int wait = 10;
        uint64_t remain = (until - mono_ns()) / 1000000ULL;
        if (remain < (uint64_t)wait)
            wait = (int)remain;
        int rc;
        do {
            rc = poll(&f, 1, wait);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0) {
            remember_error(o, errno);
            return o->first_error;
        }
    } while (mono_ns() < until || timeout_ms == 0);
    return 0;
}
void ts_output_snapshot(ts_output *o, ts_output_stats *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!o)
        return;
    observe_child(o);
    o->stats.first_error = o->first_error;
    o->stats.pending_bytes = o->stats.submitted_bytes - o->stats.confirmed_bytes -
                             o->stats.rejected_bytes - o->stats.uncertain_bytes;
    o->stats.last_progress_ns = o->last_progress_ns;
    uint64_t now = mono_ns();
    o->stats.oldest_pending_age_ns =
        o->head && now >= o->head->submitted_ns ? now - o->head->submitted_ns : 0;
    o->stats.finished = o->ended && o->reaped && !o->stats.pending_bytes && !o->first_error;
    *out = o->stats;
}
static int wait_child(ts_output *o, unsigned timeout_ms) {
    uint64_t end = mono_ns() + (uint64_t)timeout_ms * 1000000ULL;
    for (;;) {
        observe_child(o);
        if (o->reaped)
            return 0;
        if (mono_ns() >= end)
            return ETIMEDOUT;
        struct timespec pause = {0, 10000000L};
        nanosleep(&pause, NULL);
    }
}
int ts_output_finish(ts_output *o, unsigned drain_ms) {
    if (!o)
        return EINVAL;
    o->accepting = 0;
    o->ending = 1;
    if (!drain_ms)
        drain_ms = o->drain_ms;
    uint64_t end = mono_ns() + (uint64_t)drain_ms * 1000000ULL;
    while (o->stats.pending_bytes && !o->first_error && mono_ns() < end)
        (void)ts_output_poll(o, 10);
    if (!o->stats.pending_bytes && !o->first_error) {
        while (!o->ended && !o->first_error && mono_ns() < end)
            (void)ts_output_poll(o, 10);
        if (!o->first_error && o->ended) {
            int rc =
                wait_child(o, (unsigned)((end > mono_ns()) ? (end - mono_ns()) / 1000000ULL : 0));
            if (rc)
                remember_error(o, rc);
        }
        if (!o->first_error && (!o->ended || !o->reaped))
            remember_error(o, ETIMEDOUT);
    } else if (!o->first_error)
        remember_error(o, ETIMEDOUT);
    if (o->first_error)
        (void)ts_output_abort(o,
                              (unsigned)((end > mono_ns()) ? (end - mono_ns()) / 1000000ULL : 0));
    return o->first_error;
}
int ts_output_abort(ts_output *o, unsigned deadline_ms) {
    if (!o)
        return EINVAL;
    o->accepting = 0;
    if (!o->first_error)
        remember_error(o, ECANCELED);
    (void)shutdown(o->socket_fd, SHUT_RDWR);
    uint64_t end = mono_ns() + (uint64_t)deadline_ms * 1000000ULL;
    if (!o->reaped)
        (void)kill(o->child, SIGTERM);
    while (!o->reaped && mono_ns() < end) {
        observe_child(o);
        if (!o->reaped) {
            struct timespec pause = {0, 10000000L};
            nanosleep(&pause, NULL);
        }
    }
    if (!o->reaped) {
        (void)kill(o->child, SIGKILL);
        observe_child(o);
    }
    if (o->reaped)
        classify_unresolved(o);
    return o->reaped ? 0 : ETIMEDOUT;
}
int ts_output_destroy(ts_output *o) {
    if (!o)
        return 0;
    if (!o->reaped)
        (void)ts_output_abort(o, 250);
    if (!o->reaped)
        return ETIMEDOUT;
    close(o->socket_fd);
    struct ts_job *j = o->head;
    while (j) {
        struct ts_job *next = j->next;
        free(j->data);
        free(j);
        j = next;
    }
    free(o);
    return 0;
}
#endif
