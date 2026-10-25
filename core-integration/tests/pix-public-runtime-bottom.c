/* SPDX-License-Identifier: MIT */
#include <libusb.h>
#include "pixy.h"
#include "pix-runtime-vectors.h"
#include "usb_backend.h"

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

#ifdef PIX_TEST_DT295_FIXTURE
int dt295_actual_fixture_control(uint8_t, uint8_t, uint16_t, uint16_t,
                                 uint8_t *, uint16_t);
int dt295_actual_fixture_bulk(uint8_t, unsigned char *, int, int *);
int dt295_actual_fixture_interrupt(uint8_t, unsigned char *, int, int *);
int __real_usb_open_model(usb_backend **, const usb_selector *, int, int, int);

int __wrap_usb_open_model(usb_backend **output, const usb_selector *selector, int fd,
                          int detach, int model)
{
    if (output == NULL) return -1;
    if (model != USB_MODEL_DT295)
        return __real_usb_open_model(output, selector, fd, detach, model);
    *output = (usb_backend *)(uintptr_t)0x295U;
    return 0;
}
void __real_usb_close(usb_backend *backend);
void __wrap_usb_close(usb_backend *backend)
{ if (backend != (usb_backend *)(uintptr_t)0x295U) __real_usb_close(backend); }
void *__real_usb_native_context(usb_backend *backend);
void *__wrap_usb_native_context(usb_backend *backend)
{ return backend == (usb_backend *)(uintptr_t)0x295U
      ? (void *)(uintptr_t)0x29501U : __real_usb_native_context(backend); }
void *__real_usb_native_handle(usb_backend *backend);
void *__wrap_usb_native_handle(usb_backend *backend)
{ return backend == (usb_backend *)(uintptr_t)0x295U
      ? (void *)(uintptr_t)0x29502U : __real_usb_native_handle(backend); }
int LIBUSB_CALL __real_libusb_control_transfer(libusb_device_handle *, uint8_t,
    uint8_t, uint16_t, uint16_t, unsigned char *, uint16_t, unsigned int);
int __wrap_libusb_control_transfer(libusb_device_handle *handle, uint8_t type,
    uint8_t request, uint16_t value, uint16_t index, unsigned char *data,
    uint16_t length, unsigned int timeout)
{
#ifdef PIX_EMBEDDED_ASSETS
    (void)timeout;
#endif
    if (handle != (libusb_device_handle *)(uintptr_t)0x29502U)
#ifdef PIX_EMBEDDED_ASSETS
        return LIBUSB_ERROR_NO_DEVICE;
#else
        return __real_libusb_control_transfer(handle, type, request, value, index,
                                              data, length, timeout);
#endif
    return dt295_actual_fixture_control(type, request, value, index, data, length);
}
int LIBUSB_CALL __real_libusb_bulk_transfer(libusb_device_handle *, unsigned char,
    unsigned char *, int, int *, unsigned int);
int __wrap_libusb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint,
    unsigned char *data, int length, int *actual, unsigned int timeout)
{
#ifdef PIX_EMBEDDED_ASSETS
    (void)timeout;
#endif
    if (handle != (libusb_device_handle *)(uintptr_t)0x29502U)
#ifdef PIX_EMBEDDED_ASSETS
        return LIBUSB_ERROR_NO_DEVICE;
#else
        return __real_libusb_bulk_transfer(handle, endpoint, data, length, actual, timeout);
#endif
    return dt295_actual_fixture_bulk(endpoint, data, length, actual);
}
int LIBUSB_CALL __real_libusb_interrupt_transfer(libusb_device_handle *, unsigned char,
    unsigned char *, int, int *, unsigned int);
int __wrap_libusb_interrupt_transfer(libusb_device_handle *handle, unsigned char endpoint,
    unsigned char *data, int length, int *actual, unsigned int timeout)
{
#ifdef PIX_EMBEDDED_ASSETS
    (void)timeout;
#endif
    if (handle != (libusb_device_handle *)(uintptr_t)0x29502U)
#ifdef PIX_EMBEDDED_ASSETS
        return LIBUSB_ERROR_NO_DEVICE;
#else
        return __real_libusb_interrupt_transfer(handle, endpoint, data, length, actual,
                                               timeout);
#endif
    return dt295_actual_fixture_interrupt(endpoint, data, length, actual);
}
int __wrap_random_bytes(void *output, size_t length)
{ if (output != NULL) memset(output, 0, length); return 0; }
#endif
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

#ifndef PIX_EMBEDDED_ASSETS
int __real_libusb_release_interface(libusb_device_handle *, int);
void __real_libusb_close(libusb_device_handle *);
#endif
int __wrap_libusb_release_interface(libusb_device_handle *handle, int interface)
{
    ++release_count;
    fprintf(stderr, "PIX_CLEANUP release-interface=%u\n", release_count);
#ifdef PIX_EMBEDDED_ASSETS
    (void)handle;
    (void)interface;
    return 0;
#else
    return __real_libusb_release_interface(handle, interface);
#endif
}
void __wrap_libusb_close(libusb_device_handle *handle)
{
    ++close_count;
    fprintf(stderr, "PIX_CLEANUP close-handle=%u\n", close_count);
#ifdef PIX_EMBEDDED_ASSETS
    (void)handle;
#else
    __real_libusb_close(handle);
#endif
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
int pix_test_libusb_handle_events_timeout(libusb_context *, struct timeval *);
int __wrap_libusb_handle_events_timeout(libusb_context *context, struct timeval *timeout)
{
    const unsigned before = pix_test_usb_completions();
    const int result = pix_test_libusb_handle_events_timeout(context, timeout);
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
