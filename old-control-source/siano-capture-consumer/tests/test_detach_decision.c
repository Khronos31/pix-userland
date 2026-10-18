/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../detach-decision.h"

#include <assert.h>

int main(void)
{
    assert(classify_kernel_driver_query(1) == REFUSE_BOUND);
    assert(classify_kernel_driver_query(0) == ALLOW);
    assert(classify_kernel_driver_query(LIBUSB_ERROR_NOT_SUPPORTED) == ALLOW);
    assert(classify_kernel_driver_query(LIBUSB_ERROR_IO) == REFUSE_UNKNOWN);
    return 0;
}
