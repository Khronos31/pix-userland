/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIANO_DETACH_DECISION_H
#define SIANO_DETACH_DECISION_H

#include <libusb.h>

enum detach_decision {
    ALLOW,
    REFUSE_BOUND,
    REFUSE_UNKNOWN,
};

static inline enum detach_decision classify_kernel_driver_query(int query_result)
{
    if (query_result == 1)
        return REFUSE_BOUND;
    if (query_result < 0 && query_result != LIBUSB_ERROR_NOT_SUPPORTED)
        return REFUSE_UNKNOWN;
    return ALLOW;
}

#endif
