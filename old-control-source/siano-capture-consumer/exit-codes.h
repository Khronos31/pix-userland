/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIANO_EXIT_CODES_H
#define SIANO_EXIT_CODES_H

#include <errno.h>
#include <libusb.h>

int siano_exit_code(int rc);
int siano_list_result(long long device_count);
int siano_report_output_error(int error);

#endif
