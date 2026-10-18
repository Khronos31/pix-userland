/* SPDX-License-Identifier: GPL-2.0-or-later */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "control-input.h"

#include <errno.h>
#include <stdbool.h>

#ifdef _WIN32

static int win32_error_to_errno(DWORD error)
{
    switch (error) {
    case ERROR_INVALID_HANDLE:
        return EBADF;
    case ERROR_ACCESS_DENIED:
        return EACCES;
    case ERROR_INVALID_PARAMETER:
        return EINVAL;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return ENOMEM;
    default:
        return EIO;
    }
}

int siano_control_input_init(struct siano_control_input *input,
                             siano_control_input_handle handle)
{
    DWORD type;
    DWORD error;

    if (input == NULL || handle == NULL || handle == INVALID_HANDLE_VALUE)
        return -EBADF;
    SetLastError(NO_ERROR);
    type = GetFileType(handle);
    if (type == FILE_TYPE_UNKNOWN) {
        error = GetLastError();
        return error == NO_ERROR ? -EINVAL : -win32_error_to_errno(error);
    }
    if (type != FILE_TYPE_PIPE && type != FILE_TYPE_CHAR && type != FILE_TYPE_DISK)
        return -EINVAL;
    input->handle = handle;
    input->type = type;
    input->available = 0;
    input->eof = false;
    return 0;
}

int siano_control_input_ready(struct siano_control_input *input)
{
    if (input->eof)
        return 1;
    if (input->type == FILE_TYPE_PIPE) {
        DWORD available = 0;

        if (!PeekNamedPipe(input->handle, NULL, 0, NULL, &available, NULL)) {
            DWORD error = GetLastError();

            if (error == ERROR_BROKEN_PIPE) {
                input->eof = true;
                input->available = 0;
                return 1;
            }
            return -win32_error_to_errno(error);
        }
        input->available = available;
        return available > 0 ? 1 : 0;
    }
    if (input->type == FILE_TYPE_DISK)
        return 1;
    if (input->type == FILE_TYPE_CHAR) {
        DWORD result = WaitForSingleObject(input->handle, 0);

        if (result == WAIT_OBJECT_0)
            return 1;
        if (result == WAIT_TIMEOUT)
            return 0;
        if (result == WAIT_FAILED)
            return -win32_error_to_errno(GetLastError());
        return -EIO;
    }
    return -EINVAL;
}

int64_t siano_control_input_read(struct siano_control_input *input,
                                 uint8_t *buffer, size_t capacity)
{
    DWORD count = 0;

    if (input->eof)
        return 0;
    if (capacity == 0)
        return 0;
    if (input->type == FILE_TYPE_PIPE) {
        if (input->available == 0)
            return -EAGAIN;
        if (capacity > input->available)
            capacity = input->available;
        input->available = 0;
    }
    if (capacity > UINT32_MAX)
        capacity = UINT32_MAX;
    if (!ReadFile(input->handle, buffer, (DWORD)capacity, &count, NULL)) {
        DWORD error = GetLastError();

        if (error == ERROR_BROKEN_PIPE) {
            input->eof = true;
            input->available = 0;
            return 0;
        }
        return -win32_error_to_errno(error);
    }
    if (input->type == FILE_TYPE_PIPE && count == 0)
        input->eof = true;
    return (int64_t)count;
}

#else

#include <poll.h>
#include <unistd.h>

int siano_control_input_init(struct siano_control_input *input,
                             siano_control_input_handle handle)
{
    if (input == NULL || handle < 0)
        return -EBADF;
    input->handle = handle;
    input->eof = false;
    return 0;
}

int siano_control_input_ready(struct siano_control_input *input)
{
    struct pollfd descriptor = { input->handle, POLLIN, 0 };
    int result = poll(&descriptor, 1, 0);

    if (result < 0 && errno == EINTR)
        return 0;
    if (result < 0)
        return -errno;
    return result > 0 ? 1 : 0;
}

int64_t siano_control_input_read(struct siano_control_input *input,
                                 uint8_t *buffer, size_t capacity)
{
    ssize_t count = read(input->handle, buffer, capacity);

    return count < 0 ? -errno : (int64_t)count;
}

#endif
