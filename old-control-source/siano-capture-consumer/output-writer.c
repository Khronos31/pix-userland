/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "output-writer.h"
#include "write-policy.h"

#include <errno.h>
#include <string.h>

#ifdef _WIN32
#include "siano-os.h"
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

static siano_output_write_hook test_write_hook;

void siano_output_test_set_write_hook(siano_output_write_hook hook)
{
    test_write_hook = hook;
}

static int siano_output_result(size_t length, int64_t count, size_t *written)
{
    if (count < 0)
        return count == -EAGAIN || count == -EINTR
                   ? SIANO_OUTPUT_PENDING
                   : (int)count;
    *written = (size_t)count;
    if (count == 0 || (size_t)count < length)
        return SIANO_OUTPUT_PENDING;
    return SIANO_OUTPUT_DONE;
}

#ifndef _WIN32

int siano_output_begin(struct siano_output *output, int fd)
{
    struct stat st;
    int flags;

    output->fd = fd;
    output->saved_flags = 0;
    output->armed = false;
    output->pollable = false;
    if (fd < 0)
        return -EBADF;
    if (fstat(fd, &st) != 0)
        return -errno;
    if (!(S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode) || S_ISCHR(st.st_mode)))
        return 0;
    flags = fcntl(fd, F_GETFL);
    if (flags < 0)
        return -errno;
    output->saved_flags = flags;
    output->pollable = true;
    if (flags & O_NONBLOCK)
        return 0;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -errno;
    output->armed = true;
    return 0;
}

int siano_output_write(struct siano_output *output, const uint8_t *data,
                       size_t length, size_t *written)
{
    ssize_t count;

    *written = 0;
    if (length == 0)
        return SIANO_OUTPUT_DONE;
    if (test_write_hook != NULL)
        return siano_output_result(length, test_write_hook(data, length), written);
    count = write(output->fd, data, length);
    if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return SIANO_OUTPUT_PENDING;
        return -errno;
    }
    *written = (size_t)count;
    if (count == 0 || (size_t)count < length)
        return SIANO_OUTPUT_PENDING;
    return SIANO_OUTPUT_DONE;
}

void siano_output_wait(struct siano_output *output, int timeout_ms)
{
    struct pollfd descriptor;

    if (timeout_ms <= 0)
        return;
    if (!output->pollable) {
        struct timespec delay;

        delay.tv_sec = timeout_ms / 1000;
        delay.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        nanosleep(&delay, NULL);
        return;
    }
    descriptor.fd = output->fd;
    descriptor.events = POLLOUT;
    descriptor.revents = 0;
    (void)poll(&descriptor, 1, timeout_ms);
}

void siano_output_end(struct siano_output *output)
{
    int current;

    if (output->fd < 0 || !output->armed)
        return;
    current = fcntl(output->fd, F_GETFL);
    if (current >= 0) {
        if (output->saved_flags & O_NONBLOCK)
            current |= O_NONBLOCK;
        else
            current &= ~O_NONBLOCK;
        (void)fcntl(output->fd, F_SETFL, current);
    }
    output->armed = false;
}

#else /* _WIN32 */

int siano_output_begin(struct siano_output *output, int fd)
{
    HANDLE handle;
    HANDLE query_handle;
    DWORD mode = 0;

    output->fd = fd;
    output->handle = NULL;
    output->saved_mode = 0;
    output->nowait = false;
    if (fd < 0)
        return -EBADF;
    handle = (HANDLE)_get_osfhandle(fd);
    if (handle == INVALID_HANDLE_VALUE)
        return -EBADF;
    output->handle = handle;
    if (GetFileType(handle) != FILE_TYPE_PIPE)
        return 0;
    if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(),
                         &query_handle, GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                         FALSE, 0))
        return GetLastError() == ERROR_ACCESS_DENIED ? -EACCES : -EIO;
    if (!GetNamedPipeHandleState(query_handle, &mode, NULL, NULL, NULL, NULL, 0)) {
        DWORD error = GetLastError();

        CloseHandle(query_handle);
        return error == ERROR_ACCESS_DENIED ? -EACCES : -EIO;
    }
    CloseHandle(query_handle);
    output->saved_mode = mode;
    if (!(mode & PIPE_NOWAIT)) {
        mode |= PIPE_NOWAIT;
        if (!SetNamedPipeHandleState(handle, &mode, NULL, NULL))
            return -EIO;
        output->nowait = true;
    }
    return 0;
}

int siano_output_write(struct siano_output *output, const uint8_t *data,
                       size_t length, size_t *written)
{
    DWORD count = 0;
    DWORD request;

    *written = 0;
    if (length == 0)
        return SIANO_OUTPUT_DONE;
    if (test_write_hook != NULL)
        return siano_output_result(length, test_write_hook(data, length), written);
    request = (DWORD)siano_write_request_size(length, (size_t)UINT32_MAX);
    if (!WriteFile((HANDLE)output->handle, data, request, &count, NULL)) {
        DWORD error = GetLastError();

        if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
            error == ERROR_NO_DATA)
            return -EPIPE;
        return -EIO;
    }
    *written = (size_t)count;
    if (count == 0 || (size_t)count < length)
        return SIANO_OUTPUT_PENDING;
    return SIANO_OUTPUT_DONE;
}

void siano_output_wait(struct siano_output *output, int timeout_ms)
{
    (void)output;
    if (timeout_ms > 0)
        Sleep((DWORD)timeout_ms);
}

void siano_output_end(struct siano_output *output)
{
    DWORD mode;

    if (output->handle == NULL || !output->nowait)
        return;
    mode = (DWORD)output->saved_mode;
    (void)SetNamedPipeHandleState((HANDLE)output->handle, &mode, NULL, NULL);
    output->nowait = false;
}

#endif

int siano_output_pump_init(struct siano_output_pump *pump, int fd)
{
    memset(pump, 0, sizeof(*pump));
    return siano_output_begin(&pump->output, fd);
}

int siano_output_pump_init_sink(
    struct siano_output_pump *pump,
    int (*sink)(void *, const uint8_t[SIANO_OUTPUT_PACKET]), void *user)
{
    if (!sink)
        return -EINVAL;
    memset(pump, 0, sizeof(*pump));
    pump->packet_sink = sink;
    pump->packet_sink_user = user;
    return 0;
}

void siano_output_pump_destroy(struct siano_output_pump *pump)
{
    siano_output_end(&pump->output);
}

void siano_output_pump_reset(struct siano_output_pump *pump)
{
    pump->hold_len = 0;
    pump->scratch_len = 0;
    pump->send_off = 0;
    pump->packet_off = 0;
    pump->packet_resume_off = 0;
    pump->writing = false;
    pump->busy = false;
}

void siano_output_pump_retune(struct siano_output_pump *pump)
{
    size_t remaining;
    size_t data_offset;

    if (!pump->writing || pump->packet_off == 0 ||
        pump->packet_off >= SIANO_OUTPUT_PACKET ||
        pump->packet_resume_off > pump->packet_off) {
        siano_output_pump_reset(pump);
        return;
    }
    remaining = SIANO_OUTPUT_PACKET - pump->packet_off;
    data_offset = pump->packet_off - pump->packet_resume_off;
    memmove(pump->scratch,
            pump->scratch + pump->send_off + data_offset, remaining);
    pump->hold_len = 0;
    pump->scratch_len = remaining;
    pump->send_off = 0;
    pump->packet_resume_off = pump->packet_off;
    pump->writing = true;
    pump->busy = true;
}

bool siano_output_pump_busy(const struct siano_output_pump *pump)
{
    return pump->busy;
}

size_t siano_output_pump_pending(const struct siano_output_pump *pump)
{
    if (pump->busy && pump->packet_resume_off != 0)
        return pump->scratch_len - pump->send_off -
               (pump->packet_off - pump->packet_resume_off);
    if (pump->busy)
        return pump->scratch_len - pump->send_off;
    return pump->hold_len;
}

void siano_output_pump_feed(struct siano_output_pump *pump, const uint8_t *data,
                            size_t length)
{
    memcpy(pump->scratch, pump->hold, pump->hold_len);
    memcpy(pump->scratch + pump->hold_len, data, length);
    pump->scratch_len = pump->hold_len + length;
    pump->hold_len = 0;
    pump->send_off = 0;
    pump->packet_off = 0;
    pump->packet_resume_off = 0;
    pump->writing = false;
    pump->busy = true;
}

int siano_output_pump_step(struct siano_output_pump *pump)
{
    while (pump->busy) {
        if (pump->writing) {
            if (pump->packet_sink) {
                int rc = pump->packet_sink(pump->packet_sink_user,
                                           pump->scratch + pump->send_off);

                if (rc != 0)
                    return rc < 0 ? rc : -EIO;
                pump->send_off += SIANO_OUTPUT_PACKET;
                pump->packet_off = 0;
                pump->writing = false;
                continue;
            }
            size_t written = 0;
            size_t request = SIANO_OUTPUT_PACKET - pump->packet_off;
            size_t data_offset = pump->packet_off - pump->packet_resume_off;
            int rc = siano_output_write(&pump->output,
                                        pump->scratch + pump->send_off + data_offset,
                                        request, &written);

            if (rc < 0)
                return rc;
            pump->packet_off += written;
            if (rc == SIANO_OUTPUT_PENDING)
                return SIANO_OUTPUT_PENDING;
            if (pump->packet_resume_off != 0) {
                pump->scratch_len = 0;
                pump->send_off = 0;
                pump->packet_off = 0;
                pump->packet_resume_off = 0;
                pump->writing = false;
                pump->busy = false;
                return SIANO_OUTPUT_DONE;
            }
            pump->send_off += SIANO_OUTPUT_PACKET;
            pump->packet_off = 0;
            pump->writing = false;
            continue;
        }
        while (pump->scratch_len - pump->send_off >= SIANO_OUTPUT_PACKET &&
               !(pump->scratch[pump->send_off] == 0x47 &&
                 (pump->scratch_len - pump->send_off < 2U * SIANO_OUTPUT_PACKET ||
                  pump->scratch[pump->send_off + SIANO_OUTPUT_PACKET] == 0x47)))
            pump->send_off++;
        if (pump->scratch_len - pump->send_off >= SIANO_OUTPUT_PACKET) {
            pump->writing = true;
            pump->packet_off = 0;
            continue;
        }
        if (pump->send_off != 0) {
            memmove(pump->scratch, pump->scratch + pump->send_off,
                    pump->scratch_len - pump->send_off);
            pump->scratch_len -= pump->send_off;
            pump->send_off = 0;
        }
        memcpy(pump->hold, pump->scratch, pump->scratch_len);
        pump->hold_len = pump->scratch_len;
        pump->scratch_len = 0;
        pump->busy = false;
    }
    return SIANO_OUTPUT_DONE;
}
