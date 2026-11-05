/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int __wrap_random_bytes(void *output, size_t length)
{
    if (output == NULL && length != 0U) return -1;
    uint8_t *bytes = output;
    for (size_t i = 0U; i < length; ++i) bytes[i] = (uint8_t)(i * 37U + 11U);
    return 0;
}
