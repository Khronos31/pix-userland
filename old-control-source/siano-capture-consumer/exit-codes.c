/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "exit-codes.h"

int siano_list_result(long long device_count)
{
    return device_count < 0 ? (int)device_count : 0;
}

int siano_report_output_error(int error)
{
    return error != -EPIPE;
}

int siano_exit_code(int rc)
{
    if (rc >= 0)
        return 0;
    if (rc == -ENODEV)
        return 3;
    if (rc == -EBUSY || rc == LIBUSB_ERROR_BUSY)
        return 4;
    if (rc == -ETIMEDOUT)
        return 5;
    if (rc == LIBUSB_ERROR_NO_DEVICE)
        return 7;
    if (rc == -ENOENT || rc == -EBADMSG)
        return 10;
    if (rc == -ENOMEM || rc == -EAGAIN)
        return 70;
    return 1;
}
