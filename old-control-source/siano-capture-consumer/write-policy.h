/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIANO_WRITE_POLICY_H
#define SIANO_WRITE_POLICY_H

#include <stddef.h>

static inline size_t siano_write_request_size(size_t remaining,
                                              size_t platform_limit)
{
    return remaining < platform_limit ? remaining : platform_limit;
}

#endif
