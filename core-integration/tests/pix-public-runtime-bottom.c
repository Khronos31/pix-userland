/* SPDX-License-Identifier: MIT */
#include <libusb.h>
#include "pixy.h"
#include "pix-runtime-vectors.h"

#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int device_storage[2];
static int handle_storage;
static libusb_device *device_list[3];
static unsigned opened_device;
static unsigned release_count;
static unsigned close_count;
static unsigned shutdown_count;
static int fail_thread_create;
void pix_test_usb_queue(const uint8_t *bytes, size_t length);
void pix_test_fail_thread_create(void) { fail_thread_create = 1; }
unsigned pix_test_release_count(void) { return release_count; }
unsigned pix_test_close_count(void) { return close_count; }
unsigned pix_test_shutdown_count(void) { return shutdown_count; }
unsigned pix_test_usb_completions(void);
unsigned pix_test_usb_device_index(libusb_device *device)
{ return device == (libusb_device *)&device_storage[1] ? 1U : 0U; }
void pix_test_usb_set_opened_device(libusb_device *device)
{ opened_device = pix_test_usb_device_index(device); }
unsigned pix_test_usb_opened_device(void) { return opened_device; }

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context *context, libusb_device ***list)
{
    (void)context;
    device_list[0] = (libusb_device *)&device_storage[0];
    device_list[1] = (libusb_device *)&device_storage[1];
    device_list[2] = NULL;
    *list = device_list;
    return 2;
}

void LIBUSB_CALL libusb_free_device_list(libusb_device **list, int unref_devices)
{ (void)list; (void)unref_devices; }

uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *device)
{ (void)device; return 1U; }

uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device *device)
{ (void)device; return 1U; }

int LIBUSB_CALL libusb_get_port_numbers(libusb_device *device, uint8_t *ports, int length)
{
    if (length < 1) return LIBUSB_ERROR_OVERFLOW;
    ports[0] = pix_test_usb_device_index(device) == 0U ? 2U : 3U;
    return 1;
}

int LIBUSB_CALL libusb_open(libusb_device *device, libusb_device_handle **handle)
{
    pix_test_usb_set_opened_device(device);
    *handle = (libusb_device_handle *)&handle_storage;
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_get_string_descriptor_ascii(libusb_device_handle *handle,
                                                    uint8_t descriptor_index,
                                                    unsigned char *data, int length)
{
    static const unsigned char serials[2][10] = {
        "PIXDT18101", "PIXDT29501"
    };
    (void)handle;
    if (descriptor_index != 1U || length < 10) return LIBUSB_ERROR_INVALID_PARAM;
    memcpy(data, serials[opened_device], 10U);
    return 10;
}

int __wrap_pixy_power(pixy *device, const uint8_t *firmware, size_t length)
{ (void)device; (void)firmware; (void)length; return 0; }
int __wrap_pixy_session(pixy *device, const uint8_t bootstrap[16], const uint8_t validation[64])
{ (void)device; (void)bootstrap; (void)validation; return 0; }
int __wrap_pixy_tuner_init(pixy *device) { (void)device; return 0; }
int __wrap_pixy_tune_slot(pixy *device, pixy_system system, uint32_t hz,
                          int lna, int *tsid, int slot)
{
    (void)device; (void)lna;
    if (system != PIXY_TERRESTRIAL && slot >= 0 && *tsid == -1) *tsid = 0x1234;
    const char *path = getenv("PIX_TEST_TUNE_LOG");
    if (path != NULL) {
        FILE *file = fopen(path, "a");
        if (file != NULL) {
            fprintf(file, "system=%d hz=%u slot=%d tsid=%d\n",
                    (int)system, hz, slot, *tsid);
            fclose(file);
        }
    }
    return 0;
}
int __wrap_pixy_status(pixy *device, uint8_t *status, size_t *length)
{
    (void)device;
    if (*length < 64U) return -1;
    memset(status, 0, *length);
    status[3] = 1U;
    *length = 64U;
    return 0;
}
int __wrap_pixy_fifo_clear(pixy *device) { (void)device; return 0; }
int __wrap_pixy_start(pixy *device, uint8_t output[32])
{
    (void)device;
    if (getenv("PIX_NO_TS") == NULL) pix_test_usb_queue(wire, sizeof(wire));
    memcpy(output, context, sizeof(context));
    return 0;
}
int __wrap_pixy_stop(pixy *device) { (void)device; return 0; }
int __wrap_pixy_certify(pixy *device) { (void)device; return 0; }
int __wrap_pixy_shutdown(pixy *device)
{ (void)device; ++shutdown_count; fprintf(stderr, "PIX_CLEANUP shutdown=%u\n", shutdown_count); return 0; }

int __real_libusb_release_interface(libusb_device_handle *, int);
void __real_libusb_close(libusb_device_handle *);
int __wrap_libusb_release_interface(libusb_device_handle *handle, int interface)
{
    ++release_count;
    fprintf(stderr, "PIX_CLEANUP release-interface=%u\n", release_count);
    return __real_libusb_release_interface(handle, interface);
}
void __wrap_libusb_close(libusb_device_handle *handle)
{
    ++close_count;
    fprintf(stderr, "PIX_CLEANUP close-handle=%u\n", close_count);
    __real_libusb_close(handle);
}

#ifdef _WIN32
HANDLE WINAPI __real_CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T,
                                  LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
HANDLE WINAPI __wrap_CreateThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stack,
    LPTHREAD_START_ROUTINE entry, LPVOID argument, DWORD flags, LPDWORD thread_id)
{
    if (fail_thread_create) {
        fail_thread_create = 0;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return __real_CreateThread(attributes, stack, entry, argument, flags, thread_id);
}
#else
int __real_pthread_create(pthread_t *, const pthread_attr_t *,
                          void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                          void *(*entry)(void *), void *argument)
{
    if (fail_thread_create) {
        fail_thread_create = 0;
        return EAGAIN;
    }
    return __real_pthread_create(thread, attributes, entry, argument);
}
#endif

#ifdef PIX_TEST_CALLBACK_GATE
int __real_libusb_handle_events_timeout(libusb_context *, struct timeval *);
int __wrap_libusb_handle_events_timeout(libusb_context *context, struct timeval *timeout)
{
    const unsigned before = pix_test_usb_completions();
    const int result = __real_libusb_handle_events_timeout(context, timeout);
    static int held;
    const char *gate = getenv("PIX_HOLD_CALLBACK_FILE");
    const char *release = getenv("PIX_RELEASE_CALLBACK_FILE");
    if (!held && gate != NULL && release != NULL && pix_test_usb_completions() > before) {
        held = 1;
        FILE *marker = fopen(gate, "w");
        if (marker != NULL) { fputs("held\n", marker); fclose(marker); }
        const struct timespec pause = {0, 1000000L};
        while (access(release, F_OK) != 0) (void)nanosleep(&pause, NULL);
    }
    return result;
}
#endif
