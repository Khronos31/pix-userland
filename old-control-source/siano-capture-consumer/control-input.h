/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIANO_CONTROL_INPUT_H
#define SIANO_CONTROL_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
typedef HANDLE siano_control_input_handle;
#else
typedef int siano_control_input_handle;
#endif

struct siano_control_input {
    siano_control_input_handle handle;
#ifdef _WIN32
    DWORD type;
    DWORD available;
#endif
    bool eof;
};

int siano_control_input_init(struct siano_control_input *input,
                             siano_control_input_handle handle);
/* Returns 1 when a read will not wait, 0 when not ready, or a negative errno. */
int siano_control_input_ready(struct siano_control_input *input);
/* Returns bytes read, zero at EOF, or a negative errno. */
int64_t siano_control_input_read(struct siano_control_input *input,
                                 uint8_t *buffer, size_t capacity);

#endif
