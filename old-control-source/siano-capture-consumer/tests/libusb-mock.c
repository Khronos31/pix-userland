/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test-only libusb preload for Linux's dynamically linked CLI test. Linux
 * `make test` injects this so the existing test_cli.sh `--list` check runs
 * against an empty device list. Darwin runs that check without interposition;
 * it performs read-only enumeration, and static libusb is not interposed.
 * This mock implements only the entry points the --list path uses.
 */
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trace_call(const char *name)
{
    const char *trace = getenv("SIANO_TEST_USB_MOCK_TRACE");

    if (trace && strcmp(trace, "1") == 0)
        fprintf(stderr, "siano-test-usb-mock: %s\n", name);
}

static int mock_libusb_init(libusb_context **ctx)
{
    trace_call("libusb_init");
    if (ctx)
        *ctx = NULL;
    return 0;
}

static ssize_t mock_libusb_get_device_list(libusb_context *ctx,
                                           libusb_device ***list)
{
    trace_call("libusb_get_device_list");
    (void)ctx;
    if (list)
        *list = NULL;
    return 0;
}

static void mock_libusb_free_device_list(libusb_device **list,
                                         int unref_devices)
{
    trace_call("libusb_free_device_list");
    (void)list;
    (void)unref_devices;
}

static void mock_libusb_exit(libusb_context *ctx)
{
    trace_call("libusb_exit");
    (void)ctx;
}

#if defined(__APPLE__)
struct libusb_interpose_entry {
    const void *replacement;
    const void *replacee;
};

__attribute__((used)) static struct libusb_interpose_entry
libusb_interpose[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void *)mock_libusb_init, (const void *)libusb_init},
    {(const void *)mock_libusb_get_device_list,
     (const void *)libusb_get_device_list},
    {(const void *)mock_libusb_free_device_list,
     (const void *)libusb_free_device_list},
    {(const void *)mock_libusb_exit, (const void *)libusb_exit},
};
#else
int libusb_init(libusb_context **ctx)
{
    return mock_libusb_init(ctx);
}

ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
    return mock_libusb_get_device_list(ctx, list);
}

void libusb_free_device_list(libusb_device **list, int unref_devices)
{
    mock_libusb_free_device_list(list, unref_devices);
}

void libusb_exit(libusb_context *ctx)
{
    mock_libusb_exit(ctx);
}
#endif
