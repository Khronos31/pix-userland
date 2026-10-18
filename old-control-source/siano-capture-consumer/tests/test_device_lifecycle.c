/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <pthread.h>

#define main siano_ts_program_main
#include "../siano-ts.c"
#undef main

#include <assert.h>
#include <stdatomic.h>

static pthread_mutex_t mock_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t trace_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct libusb_transfer *mock_transfers[MAX_URBS * 4U];
static libusb_device *mock_usb_devices[2];
static struct libusb_device_descriptor mock_usb_descriptor;
static struct libusb_endpoint_descriptor mock_usb_endpoints[2];
static struct libusb_interface_descriptor mock_usb_altsetting;
static struct libusb_interface mock_usb_interface;
static struct libusb_interface mock_usb_interfaces[1];
static struct libusb_config_descriptor mock_usb_config;
static size_t mock_transfer_count;
static int mock_submit_count;
static int mock_fail_submit_at = -1;
static int mock_alloc_count;
static int mock_fail_alloc_at = -1;
static int mock_buffer_alloc_count;
static int mock_fail_buffer_alloc_at = -1;
static bool mock_fail_buffer_alloc;
static int mock_cancel_count;
static int mock_expected_cancels;
static int mock_callback_count;
static int mock_free_count;
static int mock_buffer_free_count;
static int mock_release_count;
static int mock_close_count;
static struct libusb_transfer *mock_fault_transfer;
static struct siano_device *mock_device;
static int mock_fault_status;
static bool mock_fault_pending;
static bool mock_main_inject_exit_state;
static uint64_t mock_main_injected_drops;
static atomic_bool mock_fault_seen;
static atomic_bool mock_hold_callbacks;
static atomic_bool mock_callback_entered;
static atomic_bool mock_release_callbacks;
static char mock_trace[32768];
static size_t mock_trace_length;
static void trace_event(const char *event, size_t index);

int __real_pthread_join(pthread_t thread, void **result);

int __wrap_pthread_join(pthread_t thread, void **result)
{
    int rc = __real_pthread_join(thread, result);
    trace_event("join", 0);
    return rc;
}

static void trace_event(const char *event, size_t index)
{
    pthread_mutex_lock(&trace_mutex);
    int n = snprintf(mock_trace + mock_trace_length,
                     sizeof(mock_trace) - mock_trace_length,
                     "%s%zu ", event, index);
    assert(n > 0 && (size_t)n < sizeof(mock_trace) - mock_trace_length);
    mock_trace_length += (size_t)n;
    pthread_mutex_unlock(&trace_mutex);
}

static void reset_mock(void)
{
    pthread_mutex_lock(&mock_mutex);
    memset(mock_transfers, 0, sizeof(mock_transfers));
    mock_transfer_count = 0;
    mock_submit_count = 0;
    mock_fail_submit_at = -1;
    mock_alloc_count = 0;
    mock_fail_alloc_at = -1;
    mock_buffer_alloc_count = 0;
    mock_fail_buffer_alloc_at = -1;
    mock_fail_buffer_alloc = false;
    mock_cancel_count = 0;
    mock_expected_cancels = 0;
    mock_callback_count = 0;
    mock_free_count = 0;
    mock_buffer_free_count = 0;
    mock_release_count = 0;
    mock_close_count = 0;
    mock_fault_transfer = NULL;
    mock_fault_status = LIBUSB_TRANSFER_COMPLETED;
    mock_fault_pending = false;
    mock_main_inject_exit_state = false;
    mock_main_injected_drops = 0;
    atomic_store(&mock_fault_seen, false);
    atomic_store(&mock_hold_callbacks, false);
    atomic_store(&mock_callback_entered, false);
    atomic_store(&mock_release_callbacks, false);
    pthread_mutex_lock(&trace_mutex);
    mock_trace_length = 0;
    mock_trace[0] = '\0';
    pthread_mutex_unlock(&trace_mutex);
    stop_requested = 0;
    pthread_mutex_unlock(&mock_mutex);
}

int LIBUSB_CALL libusb_init(libusb_context **ctx)
{
    if (ctx)
        *ctx = (libusb_context *)(uintptr_t)1;
    return 0;
}

void LIBUSB_CALL libusb_exit(libusb_context *ctx)
{
    (void)ctx;
}

int LIBUSB_CALL libusb_set_option(libusb_context *ctx,
                                  enum libusb_option option, ...)
{
    (void)ctx;
    (void)option;
    return 0;
}

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context *ctx,
                                            libusb_device ***list)
{
    (void)ctx;
    mock_usb_devices[0] = (libusb_device *)(uintptr_t)1;
    mock_usb_devices[1] = NULL;
    *list = mock_usb_devices;
    return 1;
}

void LIBUSB_CALL libusb_free_device_list(libusb_device **list,
                                         int unref_devices)
{
    (void)list;
    (void)unref_devices;
}

int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *device,
                                              struct libusb_device_descriptor *desc)
{
    (void)device;
    *desc = mock_usb_descriptor;
    return 0;
}

uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *device)
{
    (void)device;
    return 1;
}

uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device *device)
{
    (void)device;
    return 2;
}

int LIBUSB_CALL libusb_get_port_numbers(libusb_device *device, uint8_t *ports,
                                        int port_count)
{
    (void)device;
    (void)ports;
    (void)port_count;
    return 0;
}

int LIBUSB_CALL libusb_open(libusb_device *device,
                            libusb_device_handle **handle)
{
    (void)device;
    *handle = (libusb_device_handle *)(uintptr_t)1;
    return 0;
}

libusb_device * LIBUSB_CALL libusb_get_device(libusb_device_handle *handle)
{
    (void)handle;
    return mock_usb_devices[0];
}

int LIBUSB_CALL libusb_get_active_config_descriptor(
    libusb_device *device, struct libusb_config_descriptor **config)
{
    (void)device;
    *config = &mock_usb_config;
    return 0;
}

void LIBUSB_CALL libusb_free_config_descriptor(
    struct libusb_config_descriptor *config)
{
    (void)config;
}

int LIBUSB_CALL libusb_kernel_driver_active(libusb_device_handle *handle,
                                            int interface_number)
{
    (void)handle;
    (void)interface_number;
    return 0;
}

int LIBUSB_CALL libusb_set_auto_detach_kernel_driver(
    libusb_device_handle *handle, int enable)
{
    (void)handle;
    (void)enable;
    return 0;
}

int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *handle,
                                       int interface_number)
{
    (void)handle;
    (void)interface_number;
    return 0;
}

int LIBUSB_CALL libusb_clear_halt(libusb_device_handle *handle,
                                  unsigned char endpoint)
{
    (void)handle;
    (void)endpoint;
    return 0;
}

const char * LIBUSB_CALL libusb_error_name(int error_code)
{
    switch (error_code) {
    case LIBUSB_ERROR_NO_DEVICE: return "LIBUSB_ERROR_NO_DEVICE";
    case LIBUSB_ERROR_NO_MEM: return "LIBUSB_ERROR_NO_MEM";
    default: return "LIBUSB_ERROR_OTHER";
    }
}

const char * LIBUSB_CALL libusb_strerror(enum libusb_error error_code)
{
    (void)error_code;
    return "fake libusb error";
}

struct libusb_transfer * LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
    (void)iso_packets;
    if (mock_alloc_count++ == mock_fail_alloc_at)
        return NULL;
    return calloc(1, sizeof(struct libusb_transfer));
}

void *__real_malloc(size_t size);

void *__wrap_malloc(size_t size)
{
    if (mock_fail_buffer_alloc && size == USB_TRANSFER_SIZE &&
        mock_buffer_alloc_count++ == mock_fail_buffer_alloc_at)
        return NULL;
    return __real_malloc(size);
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *transfer)
{
    if (!transfer)
        return;
    trace_event("free", (size_t)mock_free_count++);
    free(transfer);
}

void __real_free(void *ptr);

void __wrap_free(void *ptr)
{
    if (ptr && mock_device) {
        for (size_t i = 0; i < MAX_URBS; i++) {
            if (mock_device->transfer_buffers[i] == ptr) {
                trace_event("buffer-free", (size_t)mock_buffer_free_count++);
                break;
            }
        }
    }
    __real_free(ptr);
}

int __wrap_mlockall(int flags)
{
    (void)flags;
    errno = EPERM;
    return -1;
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *transfer)
{
    if (!mock_device && transfer->user_data)
        mock_device = ((struct siano_transfer_context *)transfer->user_data)->device;
    pthread_mutex_lock(&mock_mutex);
    int index = mock_submit_count++;
    if (index == mock_fail_submit_at) {
        trace_event("submit-fail", (size_t)index);
        pthread_mutex_unlock(&mock_mutex);
        return LIBUSB_ERROR_NO_MEM;
    }
    if (mock_transfer_count < sizeof(mock_transfers) / sizeof(mock_transfers[0]))
        mock_transfers[mock_transfer_count++] = transfer;
    trace_event("submit", (size_t)index);
    pthread_mutex_unlock(&mock_mutex);
    return 0;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *transfer)
{
    pthread_mutex_lock(&mock_mutex);
    transfer->status = LIBUSB_TRANSFER_CANCELLED;
    transfer->user_data = transfer->user_data;
    mock_cancel_count++;
    trace_event("cancel", (size_t)mock_cancel_count - 1);
    pthread_mutex_unlock(&mock_mutex);
    return 0;
}

int LIBUSB_CALL libusb_handle_events_timeout(libusb_context *ctx,
                                             struct timeval *tv)
{
    (void)ctx;
    (void)tv;
    struct libusb_transfer *transfer = NULL;
    bool canceled = false;
    pthread_mutex_lock(&mock_mutex);
    for (size_t i = 0; i < mock_transfer_count; i++) {
        if (mock_transfers[i] && mock_transfers[i]->status == LIBUSB_TRANSFER_CANCELLED) {
            transfer = mock_transfers[i];
            mock_transfers[i] = NULL;
            canceled = true;
            break;
        }
    }
    if (!transfer && mock_fault_pending) {
        for (size_t i = 0; i < mock_transfer_count; i++) {
            if (mock_transfers[i] == mock_fault_transfer) {
                transfer = mock_transfers[i];
                mock_transfers[i] = NULL;
                transfer->status = (enum libusb_transfer_status)mock_fault_status;
                mock_fault_pending = false;
                break;
            }
        }
    }
    pthread_mutex_unlock(&mock_mutex);
    if (canceled) {
        for (;;) {
            pthread_mutex_lock(&mock_mutex);
            bool all_cancels_issued = mock_cancel_count >= mock_expected_cancels;
            pthread_mutex_unlock(&mock_mutex);
            if (all_cancels_issued)
                break;
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
            nanosleep(&delay, NULL);
        }
    }
    if (!transfer) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
        nanosleep(&delay, NULL);
        return 0;
    }
    if (atomic_load(&mock_hold_callbacks)) {
        atomic_store(&mock_callback_entered, true);
        while (!atomic_load(&mock_release_callbacks)) {
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
            nanosleep(&delay, NULL);
        }
    }
    trace_event("callback", (size_t)mock_callback_count++);
    transfer->callback(transfer);
    if (mock_fault_transfer == transfer)
        atomic_store(&mock_fault_seen, true);
    return 0;
}

int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *handle,
                                     unsigned char endpoint,
                                     unsigned char *data, int length,
                                     int *transferred, unsigned int timeout)
{
    uint16_t request = sms_get_le16(data);
    uint16_t response = 0;
    size_t payload_length = 0;
    (void)handle;
    (void)endpoint;
    (void)timeout;
    assert(mock_device != NULL);
    assert(mock_submit_count >= (int)MAX_URBS);
    if (request == MSG_SMS_GET_VERSION_EX_REQ) {
        response = MSG_SMS_GET_VERSION_EX_RES;
        payload_length = 12;
    } else if (request == MSG_SMS_ISDBT_TUNE_REQ) {
        response = MSG_SMS_ISDBT_TUNE_RES;
    } else if (request == MSG_SMS_GET_STATISTICS_EX_REQ ||
               request == MSG_SMS_GET_STATISTICS_REQ) {
        response = MSG_SMS_SIGNAL_DETECTED_IND;
    } else if (request == MSG_SMS_ADD_PID_FILTER_REQ &&
               sms_get_le32(data + SMS_HEADER_SIZE) != 0x2000U) {
        response = MSG_SMS_ADD_PID_FILTER_RES;
    }
    pthread_mutex_lock(&mock_mutex);
    if (response != 0) {
        struct libusb_transfer *transfer = mock_device->transfers[0];
        assert(transfer != NULL);
        memset(transfer->buffer, 0, USB_TRANSFER_SIZE);
        sms_pack_header(transfer->buffer, response, 0, SMS_HIF_TASK,
                        (uint16_t)(SMS_HEADER_SIZE + payload_length), 0);
        if (request == MSG_SMS_GET_VERSION_EX_REQ) {
            transfer->buffer[SMS_HEADER_SIZE + 4] = DEVICE_MODE_ISDBT_BDA;
            transfer->buffer[SMS_HEADER_SIZE + 5] = 0;
            transfer->buffer[SMS_HEADER_SIZE + 10] = 8;
        }
        transfer->actual_length = (int)(SMS_HEADER_SIZE + payload_length);
        transfer->status = LIBUSB_TRANSFER_COMPLETED;
        mock_fault_transfer = transfer;
        mock_fault_status = LIBUSB_TRANSFER_COMPLETED;
        mock_fault_pending = true;
    }
    if (mock_main_inject_exit_state &&
        request == MSG_SMS_ADD_PID_FILTER_REQ) {
        uint8_t packet[188] = {0x47};
        for (size_t i = 0; i < TS_QUEUE_SLOTS + 1; i++)
            ts_enqueue(&mock_device->ts, 0, packet, sizeof(packet));
        mock_main_injected_drops = mock_device->ts.drops;
        assert(siano_stream_state_fail(&mock_device->state,
                                      LIBUSB_ERROR_NO_DEVICE));
        stop_requested = 1;
        mock_main_inject_exit_state = false;
    }
    if (transferred)
        *transferred = length;
    pthread_mutex_unlock(&mock_mutex);
    return 0;
}

int LIBUSB_CALL libusb_release_interface(libusb_device_handle *handle, int iface)
{
    (void)handle;
    mock_release_count++;
    trace_event("release", (size_t)iface);
    return 0;
}

void LIBUSB_CALL libusb_close(libusb_device_handle *handle)
{
    (void)handle;
    mock_close_count++;
    trace_event("close", 0);
}

static void test_submit_failure_cleanup(int fail_index)
{
    struct siano_device *device = calloc(1, sizeof(*device));
    uint64_t drops = 0;

    assert(device != NULL);
    reset_mock();
    assert(init_device_state(device, NULL, false) == 0);
    device->handle = (libusb_device_handle *)(uintptr_t)1;
    device->interface_number = 0;
    mock_device = device;
    mock_expected_cancels = fail_index + 1;
    mock_fail_submit_at = fail_index;
    int rc = start_streaming(device);
    assert(rc < 0);
    close_device(device, &drops);
    mock_device = NULL;
    assert(mock_callback_count == fail_index);
    assert(mock_free_count == (fail_index + 1));
    assert(mock_buffer_free_count == (fail_index + 1));
    assert(mock_cancel_count == fail_index + 1);
    assert(strstr(mock_trace, "cancel") < strstr(mock_trace, "join"));
    if (fail_index > 0) {
        assert(strstr(mock_trace, "cancel") < strstr(mock_trace, "callback"));
        assert(strstr(mock_trace, "callback") < strstr(mock_trace, "join"));
    }
    assert(strstr(mock_trace, "join") < strstr(mock_trace, "free"));
    assert(strstr(mock_trace, "free") < strstr(mock_trace, "release"));
    assert(strstr(mock_trace, "free") < strstr(mock_trace, "buffer-free"));
    assert(strstr(mock_trace, "buffer-free") < strstr(mock_trace, "release"));
    assert(strstr(mock_trace, "release") < strstr(mock_trace, "close"));
    assert(mock_release_count == 1 && mock_close_count == 1);
    if (getenv("SIANO_TEST_LIFECYCLE_TRACE"))
        printf("submit-failure index=%d rc=%d trace=%s\n", fail_index, rc,
               mock_trace);
    free(device);
}

static void test_stop_cleanup(void)
{
    struct siano_device *device = calloc(1, sizeof(*device));
    uint64_t drops = 0;

    assert(device != NULL);
    reset_mock();
    assert(init_device_state(device, NULL, false) == 0);
    device->handle = (libusb_device_handle *)(uintptr_t)1;
    device->interface_number = 0;
    mock_device = device;
    mock_expected_cancels = MAX_URBS;
    assert(start_streaming(device) == 0);
    assert(mock_submit_count == (int)MAX_URBS);
    close_device(device, &drops);
    mock_device = NULL;
    assert(mock_callback_count == (int)MAX_URBS);
    assert(mock_cancel_count == (int)MAX_URBS);
    assert(mock_free_count == (int)MAX_URBS);
    assert(mock_buffer_free_count == (int)MAX_URBS);
    assert(strstr(mock_trace, "cancel") < strstr(mock_trace, "callback"));
    assert(strstr(mock_trace, "callback") < strstr(mock_trace, "join"));
    assert(strstr(mock_trace, "join") < strstr(mock_trace, "free"));
    assert(strstr(mock_trace, "free") < strstr(mock_trace, "buffer-free"));
    assert(strstr(mock_trace, "buffer-free") < strstr(mock_trace, "release"));
    assert(strstr(mock_trace, "free") < strstr(mock_trace, "release"));
    assert(strstr(mock_trace, "release") < strstr(mock_trace, "close"));
    assert(mock_release_count == 1 && mock_close_count == 1);
    if (getenv("SIANO_TEST_LIFECYCLE_TRACE"))
        printf("stop-cleanup transfers=%u trace=%s\n", MAX_URBS, mock_trace);
    free(device);
}

static void test_allocation_failure_cleanup(int fail_index)
{
    struct siano_device *device = calloc(1, sizeof(*device));
    uint64_t drops = 0;

    assert(device != NULL);
    reset_mock();
    assert(init_device_state(device, NULL, false) == 0);
    device->handle = (libusb_device_handle *)(uintptr_t)1;
    device->interface_number = 0;
    mock_device = device;
    mock_expected_cancels = fail_index;
    mock_fail_alloc_at = fail_index;
    assert(start_streaming(device) == -ENOMEM);
    close_device(device, &drops);
    mock_device = NULL;
    assert(mock_alloc_count == fail_index + 1);
    assert(mock_submit_count == fail_index);
    assert(mock_cancel_count == fail_index);
    assert(mock_callback_count == fail_index);
    assert(mock_free_count == fail_index);
    assert(mock_buffer_free_count == fail_index);
    assert(strstr(mock_trace, "join") != NULL);
    if (fail_index > 0) {
        assert(strstr(mock_trace, "cancel") < strstr(mock_trace, "callback"));
        assert(strstr(mock_trace, "callback") < strstr(mock_trace, "join"));
        assert(strstr(mock_trace, "join") < strstr(mock_trace, "free"));
        assert(strstr(mock_trace, "free") < strstr(mock_trace, "buffer-free"));
        assert(strstr(mock_trace, "buffer-free") < strstr(mock_trace, "release"));
        assert(strstr(mock_trace, "free") < strstr(mock_trace, "release"));
    }
    assert(strstr(mock_trace, "release") < strstr(mock_trace, "close"));
    assert(mock_release_count == 1 && mock_close_count == 1);
    if (getenv("SIANO_TEST_LIFECYCLE_TRACE"))
        printf("allocation-failure index=%d trace=%s\n", fail_index, mock_trace);
    free(device);
}

static void test_buffer_allocation_failure_cleanup(int fail_index)
{
    struct siano_device *device = calloc(1, sizeof(*device));
    uint64_t drops = 0;

    assert(device != NULL);
    reset_mock();
    assert(init_device_state(device, NULL, false) == 0);
    device->handle = (libusb_device_handle *)(uintptr_t)1;
    device->interface_number = 0;
    mock_device = device;
    mock_expected_cancels = fail_index;
    mock_fail_buffer_alloc_at = fail_index;
    mock_fail_buffer_alloc = true;
    assert(start_streaming(device) == -ENOMEM);
    mock_fail_buffer_alloc = false;
    close_device(device, &drops);
    mock_device = NULL;
    assert(mock_buffer_alloc_count == fail_index + 1);
    assert(mock_alloc_count == fail_index + 1);
    assert(mock_submit_count == fail_index);
    assert(mock_cancel_count == fail_index);
    assert(mock_callback_count == fail_index);
    assert(mock_free_count == fail_index + 1);
    assert(mock_buffer_free_count == fail_index);
    assert(strstr(mock_trace, "join") != NULL);
    if (fail_index > 0) {
        assert(strstr(mock_trace, "cancel") < strstr(mock_trace, "callback"));
        assert(strstr(mock_trace, "callback") < strstr(mock_trace, "join"));
        assert(strstr(mock_trace, "join") < strstr(mock_trace, "free1 "));
        assert(strstr(mock_trace, "join") < strstr(mock_trace, "buffer-free"));
        assert(strstr(mock_trace, "buffer-free") < strstr(mock_trace, "release"));
    }
    assert(strstr(mock_trace, "free") < strstr(mock_trace, "release"));
    assert(strstr(mock_trace, "release") < strstr(mock_trace, "close"));
    assert(mock_release_count == 1 && mock_close_count == 1);
    if (getenv("SIANO_TEST_LIFECYCLE_TRACE"))
        printf("buffer-allocation-failure index=%d trace=%s\n", fail_index,
               mock_trace);
    free(device);
}

static void test_async_failure(int status)
{
    struct siano_device *device = calloc(1, sizeof(*device));
    uint64_t drops = 0;

    assert(device != NULL);
    reset_mock();
    assert(init_device_state(device, NULL, false) == 0);
    device->handle = (libusb_device_handle *)(uintptr_t)1;
    device->interface_number = 0;
    mock_device = device;
    mock_expected_cancels = MAX_URBS * 2;
    assert(start_streaming(device) == 0);
    pthread_mutex_lock(&mock_mutex);
    mock_fault_transfer = device->transfers[0];
    mock_fault_status = status;
    mock_fault_pending = true;
    if (status == LIBUSB_TRANSFER_COMPLETED)
        mock_fail_submit_at = MAX_URBS;
    pthread_mutex_unlock(&mock_mutex);

    for (int i = 0; i < 1000 && !atomic_load(&mock_fault_seen); i++) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
        nanosleep(&delay, NULL);
    }
    assert(atomic_load(&mock_fault_seen));
    close_device(device, &drops);
    mock_device = NULL;
    assert(mock_callback_count == (int)MAX_URBS);
    assert(mock_cancel_count == (int)(MAX_URBS * 2));
    assert(mock_free_count == (int)MAX_URBS);
    assert(mock_buffer_free_count == (int)MAX_URBS);
    assert(strstr(mock_trace, "callback") < strstr(mock_trace, "cancel"));
    assert(strstr(mock_trace, "callback") < strstr(mock_trace, "join"));
    assert(strstr(mock_trace, "join") < strstr(mock_trace, "free"));
    assert(strstr(mock_trace, "free") < strstr(mock_trace, "buffer-free"));
    assert(strstr(mock_trace, "buffer-free") < strstr(mock_trace, "release"));
    assert(strstr(mock_trace, "free") < strstr(mock_trace, "release"));
    assert(strstr(mock_trace, "release") < strstr(mock_trace, "close"));
    assert(mock_release_count == 1 && mock_close_count == 1);
    printf("device lifecycle async-failure status=%d: PASS\n", status);
    free(device);
}

static void test_control_response_order(void)
{
    struct siano_device *device = calloc(1, sizeof(*device));
    uint64_t drops = 0;
#ifdef SIANO_DEVICE_H
    const uint16_t pids[] = {0x20, 0x2000};
#endif

    assert(device != NULL);
    reset_mock();
    assert(init_device_state(device, NULL, false) == 0);
    device->handle = (libusb_device_handle *)(uintptr_t)1;
    device->interface_number = 0;
    device->response_alignment = 0;
    mock_device = device;
    mock_expected_cancels = MAX_URBS;
    assert(start_streaming(device) == 0);
    assert(set_device_mode(device, NULL) == 0);
    assert(device->version.firmware_id == DEVICE_MODE_ISDBT_BDA);
    assert(tune(device, 473142857U) == 0);
#ifdef SIANO_DEVICE_H
    assert(configure_pid_filters(device, pids, sizeof(pids) / sizeof(pids[0])) == 0);
#else
    struct options options = {.pids = {0x20, 0x2000}, .pid_count = 2};
    assert(configure_pid_filters(device, &options) == 0);
#endif
    pthread_mutex_lock(&device->ts.mutex);
    assert(device->ts.count == 0);
    pthread_mutex_unlock(&device->ts.mutex);
    close_device(device, &drops);
    mock_device = NULL;
    assert(mock_buffer_free_count == (int)MAX_URBS);
    assert(mock_release_count == 1 && mock_close_count == 1);
    puts("device lifecycle RX-before-version/tune/PID; zero-TS responses: PASS");
    free(device);
}

struct close_args {
    struct siano_device *device;
    uint64_t drops;
};

static void *close_device_thread(void *arg)
{
    struct close_args *args = arg;

    close_device(args->device, &args->drops);
    return NULL;
}

static void test_delayed_cancellation(void)
{
    struct siano_device *device = calloc(1, sizeof(*device));
    struct close_args args = {.device = device, .drops = 0};
    pthread_t closer;

    assert(device != NULL);
    reset_mock();
    assert(init_device_state(device, NULL, false) == 0);
    device->handle = (libusb_device_handle *)(uintptr_t)1;
    device->interface_number = 0;
    mock_device = device;
    mock_expected_cancels = MAX_URBS;
    assert(start_streaming(device) == 0);
    atomic_store(&mock_hold_callbacks, true);
    assert(pthread_create(&closer, NULL, close_device_thread, &args) == 0);
    for (int i = 0; i < 1000 && !atomic_load(&mock_callback_entered); i++) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
        nanosleep(&delay, NULL);
    }
    assert(atomic_load(&mock_callback_entered));
    assert(mock_free_count == 0);
    assert(strstr(mock_trace, "release") == NULL);
    atomic_store(&mock_release_callbacks, true);
    assert(__real_pthread_join(closer, NULL) == 0);
    assert(mock_callback_count == (int)MAX_URBS);
    assert(mock_free_count == (int)MAX_URBS);
    assert(mock_buffer_free_count == (int)MAX_URBS);
    assert(strstr(mock_trace, "callback") < strstr(mock_trace, "join"));
    assert(strstr(mock_trace, "join") < strstr(mock_trace, "free"));
    assert(strstr(mock_trace, "free") < strstr(mock_trace, "buffer-free"));
    assert(strstr(mock_trace, "buffer-free") < strstr(mock_trace, "release"));
    assert(strstr(mock_trace, "free") < strstr(mock_trace, "release"));
    assert(strstr(mock_trace, "release") < strstr(mock_trace, "close"));
    assert(mock_release_count == 1 && mock_close_count == 1);
    mock_device = NULL;
    free(device);
    puts("device lifecycle delayed cancellation drain: PASS");
}

static int test_main_drop_exit(bool fail_on_drop)
{
    char output_path[] = "tests/.lifecycle-output-XXXXXX";
    char *arguments[10] = {"siano-ts", "--firmware", "Makefile", "--channel",
                           "22", "--output", output_path, NULL, NULL, NULL};
    int argc;
    int output_fd;
    int result;

    output_fd = mkstemp(output_path);
    assert(output_fd >= 0);
    assert(close(output_fd) == 0);
    argc = 7;
    if (fail_on_drop) {
        arguments[argc++] = "--fail-on-drop";
    }
    arguments[argc] = NULL;
    reset_mock();
    memset(&mock_usb_descriptor, 0, sizeof(mock_usb_descriptor));
    mock_usb_descriptor.idVendor = 0x187f;
    mock_usb_descriptor.idProduct = 0x0600;
    memset(mock_usb_endpoints, 0, sizeof(mock_usb_endpoints));
    mock_usb_endpoints[0].bEndpointAddress = LIBUSB_ENDPOINT_IN | 1;
    mock_usb_endpoints[0].bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;
    mock_usb_endpoints[0].wMaxPacketSize = 512;
    mock_usb_endpoints[1].bEndpointAddress = 2;
    mock_usb_endpoints[1].bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;
    mock_usb_endpoints[1].wMaxPacketSize = 512;
    memset(&mock_usb_altsetting, 0, sizeof(mock_usb_altsetting));
    mock_usb_altsetting.bInterfaceNumber = 0;
    mock_usb_altsetting.bNumEndpoints = 2;
    mock_usb_altsetting.endpoint = mock_usb_endpoints;
    mock_usb_interface.altsetting = &mock_usb_altsetting;
    mock_usb_interface.num_altsetting = 1;
    mock_usb_interfaces[0] = mock_usb_interface;
    memset(&mock_usb_config, 0, sizeof(mock_usb_config));
    mock_usb_config.bNumInterfaces = 1;
    mock_usb_config.interface = mock_usb_interfaces;
    mock_main_inject_exit_state = true;
    mock_expected_cancels = MAX_URBS;
    optind = 1;
    optopt = 0;
    result = siano_ts_program_main(argc, arguments);
    assert(unlink(output_path) == 0);
    assert(result == 8);
    assert(mock_cancel_count == (int)MAX_URBS);
    assert(mock_callback_count >= (int)MAX_URBS);
    assert(mock_main_injected_drops == 1);
    assert(mock_free_count == (int)MAX_URBS);
    assert(mock_buffer_free_count == (int)MAX_URBS);
    assert(mock_release_count == 1 && mock_close_count == 1);
    if (getenv("SIANO_TEST_LIFECYCLE_TRACE"))
        printf("main-drop-exit fail-on-drop=%d rc=%d trace=%s\n",
               fail_on_drop, result, mock_trace);
    mock_device = NULL;
    return result;
}

int main(void)
{
    for (int i = 0; i < (int)MAX_URBS; i++)
        test_buffer_allocation_failure_cleanup(i);
    for (int i = 0; i < (int)MAX_URBS; i++)
        test_allocation_failure_cleanup(i);
    for (int i = 0; i < (int)MAX_URBS; i++)
        test_submit_failure_cleanup(i);
    test_stop_cleanup();
    test_async_failure(LIBUSB_TRANSFER_COMPLETED);
    test_async_failure(LIBUSB_TRANSFER_NO_DEVICE);
    test_control_response_order();
    test_delayed_cancellation();
    test_main_drop_exit(false);
    test_main_drop_exit(true);
    puts("device lifecycle cleanup tests: PASS");
    return 0;
}
