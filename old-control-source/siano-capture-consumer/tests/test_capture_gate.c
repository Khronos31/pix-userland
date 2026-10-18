/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include "../siano-device.c"
#include "siano-device.h"
#include "protocol.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t mock_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct libusb_transfer *pending[MAX_URBS * 2U];
static size_t pending_count;
static struct siano_device *mock_device;
static pthread_mutex_t observe_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t observe_changed = PTHREAD_COND_INITIALIZER;
static struct siano_device *observed_device;
static int capture_wait_entered;
static int capture_wait_result = -1;
static unsigned response_wait_entries;
static unsigned control_requests;
static uint16_t last_control_request;

int __real_pthread_cond_timedwait(pthread_cond_t *cond,
                                 pthread_mutex_t *mutex,
                                 const struct timespec *deadline);

int __wrap_pthread_cond_timedwait(pthread_cond_t *cond,
                                  pthread_mutex_t *mutex,
                                  const struct timespec *deadline)
{
    int capture_wait = observed_device && cond == &observed_device->ts.available;
    int response_wait = observed_device && cond == &observed_device->response_changed;
    if (capture_wait) {
        pthread_mutex_lock(&observe_mutex);
        capture_wait_entered = 1;
        pthread_cond_broadcast(&observe_changed);
        pthread_mutex_unlock(&observe_mutex);
    }
    if (response_wait) {
        pthread_mutex_lock(&observe_mutex);
        response_wait_entries++;
        pthread_cond_broadcast(&observe_changed);
        pthread_mutex_unlock(&observe_mutex);
    }
    int rc = __real_pthread_cond_timedwait(cond, mutex, deadline);
    if (capture_wait) {
        pthread_mutex_lock(&observe_mutex);
        capture_wait_result = rc;
        pthread_cond_broadcast(&observe_changed);
        pthread_mutex_unlock(&observe_mutex);
    }
    return rc;
}

struct libusb_transfer *LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
    (void)iso_packets;
    return calloc(1, sizeof(struct libusb_transfer));
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *transfer)
{
    free(transfer);
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *transfer)
{
    pthread_mutex_lock(&mock_mutex);
    if (!mock_device)
        mock_device = ((struct siano_transfer_context *)transfer->user_data)->device;
    for (size_t i = 0; i < pending_count; i++) {
        if (pending[i] == transfer) {
            pthread_mutex_unlock(&mock_mutex);
            return 0;
        }
    }
    assert(pending_count < sizeof(pending) / sizeof(pending[0]));
    pending[pending_count++] = transfer;
    pthread_mutex_unlock(&mock_mutex);
    return 0;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *transfer)
{
    pthread_mutex_lock(&mock_mutex);
    transfer->status = LIBUSB_TRANSFER_CANCELLED;
    pthread_mutex_unlock(&mock_mutex);
    return 0;
}

int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *handle,
                                     unsigned char endpoint,
                                     unsigned char *data, int length,
                                     int *transferred, unsigned int timeout)
{
    (void)handle;
    (void)endpoint;
    (void)timeout;
    assert(data != NULL && length > 0);
    pthread_mutex_lock(&observe_mutex);
    control_requests++;
    last_control_request = sms_get_le16(data);
    pthread_cond_broadcast(&observe_changed);
    pthread_mutex_unlock(&observe_mutex);
    *transferred = length;
    return 0;
}

int LIBUSB_CALL libusb_handle_events_timeout(libusb_context *ctx,
                                             struct timeval *tv)
{
    (void)ctx;
    (void)tv;
    struct libusb_transfer *transfer = NULL;
    pthread_mutex_lock(&mock_mutex);
    for (size_t i = 0; i < pending_count; i++) {
        if (pending[i] && pending[i]->status == LIBUSB_TRANSFER_CANCELLED) {
            transfer = pending[i];
            pending[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&mock_mutex);
    if (transfer)
        transfer->callback(transfer);
    else {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
        nanosleep(&delay, NULL);
    }
    return 0;
}

const char *LIBUSB_CALL libusb_error_name(int error_code)
{
    (void)error_code;
    return "fake libusb error";
}

const char *LIBUSB_CALL libusb_strerror(enum libusb_error error_code)
{
    (void)error_code;
    return "fake libusb error";
}

int LIBUSB_CALL libusb_release_interface(libusb_device_handle *handle,
                                         int interface_number)
{
    (void)handle;
    (void)interface_number;
    return 0;
}

void LIBUSB_CALL libusb_close(libusb_device_handle *handle)
{
    (void)handle;
}

static struct libusb_transfer *get_pending(size_t index)
{
    struct libusb_transfer *transfer = NULL;
    pthread_mutex_lock(&mock_mutex);
    for (size_t i = 0, found = 0; i < pending_count; i++) {
        if (!pending[i])
            continue;
        if (found++ == index) {
            transfer = pending[i];
            break;
        }
    }
    pthread_mutex_unlock(&mock_mutex);
    assert(transfer != NULL);
    return transfer;
}

static void deliver_sms(struct libusb_transfer *transfer, uint16_t type,
                        const uint8_t *payload, size_t payload_length)
{
    uint8_t frame[SMS_HEADER_SIZE + USB_TRANSFER_SIZE];
    size_t frame_length = SMS_HEADER_SIZE + payload_length;
    pthread_mutex_lock(&mock_mutex);
    for (size_t i = 0; i < pending_count; i++) {
        if (pending[i] == transfer) {
            pending[i] = NULL;
            break;
        }
    }
    sms_pack_header(frame, type, SMS_HIF_TASK, 0, (uint16_t)frame_length, 0);
    memcpy(frame + SMS_HEADER_SIZE, payload, payload_length);
    memcpy(transfer->buffer, frame, frame_length);
    transfer->actual_length = (int)frame_length;
    transfer->status = LIBUSB_TRANSFER_COMPLETED;
    pthread_mutex_unlock(&mock_mutex);
    transfer->callback(transfer);
}

struct pop_result {
    struct ts_queue *queue;
    uint64_t generation;
    int result;
};

struct version_result {
    struct siano_device *device;
    int result;
};

static void *capture_pop_thread(void *arg)
{
    struct pop_result *result = arg;
    uint8_t data[USB_TRANSFER_SIZE];
    size_t length = 0;
    result->result = ts_pop_capture(result->queue, result->generation,
                                    data, &length);
    return NULL;
}

static void *get_version_thread(void *arg)
{
    struct version_result *result = arg;
    result->result = get_version(result->device);
    return NULL;
}

static int wait_for_observation(unsigned *value, unsigned target,
                                unsigned timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&observe_mutex);
    while (*value < target) {
        int rc = pthread_cond_timedwait(&observe_changed, &observe_mutex,
                                        &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&observe_mutex);
            return -ETIMEDOUT;
        }
    }
    pthread_mutex_unlock(&observe_mutex);
    return 0;
}

static int wait_for_capture_wait(unsigned timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&observe_mutex);
    while (!capture_wait_entered) {
        int rc = pthread_cond_timedwait(&observe_changed, &observe_mutex,
                                        &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&observe_mutex);
            return -ETIMEDOUT;
        }
    }
    pthread_mutex_unlock(&observe_mutex);
    return 0;
}

static int join_bounded(pthread_t thread, unsigned timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return pthread_timedjoin_np(thread, NULL, &deadline);
}

static void test_capture_gate_callbacks(void)
{
    struct siano_device *device = calloc(1, sizeof(*device));
    uint8_t packet[188], replacement[188], response[12] = {0};
    uint8_t output[USB_TRANSFER_SIZE];
    size_t output_length = 0;
    uint64_t generation = 0, drops = 0;
    struct libusb_transfer *transfer;
    pthread_t pop_thread, control_thread;
    struct pop_result pop = {0};
    struct version_result version = {0};
    uint64_t old_generation;

    assert(device != NULL);
    memset(packet, 0x5a, sizeof(packet));
    packet[0] = 0x47;
    pending_count = 0;
    mock_device = NULL;
    assert(init_device_state_capture(device, NULL, false, false) == 0);
    device->handle = (libusb_device_handle *)(uintptr_t)1;
    device->interface_number = 0;
    device->response_alignment = 0;
    assert(start_streaming(device) == 0);
    observed_device = device;

    transfer = get_pending(0);
    deliver_sms(transfer, MSG_SMS_DVBT_BDA_DATA, packet, sizeof(packet));
    assert(ts_queue_capture_start(&device->ts, &generation) == 0);
    old_generation = generation;
    deliver_sms(transfer, MSG_SMS_DVBT_BDA_DATA, packet, sizeof(packet));
    pthread_mutex_lock(&device->ts.mutex);
    assert(device->ts.count == 0);
    uint64_t retune_epoch = device->ts.epoch;
    assert(device->ts.capture_generation == generation);
    pthread_mutex_unlock(&device->ts.mutex);

    deliver_sms(transfer, MSG_SMS_DVBT_BDA_DATA, packet, sizeof(packet));
    assert(ts_pop_capture(&device->ts, generation, output,
                          &output_length) == 0);
    assert(output_length == sizeof(packet));
    assert(memcmp(output, packet, sizeof(packet)) == 0);

    size_t hold_len = 0;
    ts_queue_retune_begin(&device->ts);
    assert(ts_queue_retune_finish(&device->ts, &hold_len, 0));
    pthread_mutex_lock(&device->ts.mutex);
    assert(device->ts.capture_generation == generation);
    assert(device->ts.epoch != retune_epoch);
    pthread_mutex_unlock(&device->ts.mutex);

    ts_queue_capture_stop(&device->ts);
    assert(ts_queue_capture_start(&device->ts, &generation) == 0);
    memset(replacement, 0x2d, sizeof(replacement));
    replacement[0] = 0x47;
    replacement[1] = 0xa7;
    transfer = get_pending(0);
    deliver_sms(transfer, MSG_SMS_DVBT_BDA_DATA, packet, sizeof(packet));
    deliver_sms(transfer, MSG_SMS_DVBT_BDA_DATA, replacement,
                sizeof(replacement));
    assert(ts_pop_capture(&device->ts, old_generation, output,
                          &output_length) == -ECANCELED);
    pthread_mutex_lock(&device->ts.mutex);
    assert(device->ts.count == 1);
    pthread_mutex_unlock(&device->ts.mutex);
    assert(ts_pop_capture(&device->ts, generation, output,
                          &output_length) == 0);
    assert(output_length == sizeof(replacement));
    assert(memcmp(output, replacement, sizeof(replacement)) == 0);
    ts_queue_capture_stop(&device->ts);

    pthread_mutex_lock(&device->ts.mutex);
    assert(!device->ts.capture_enabled);
    assert(device->ts.capture_generation != generation);
    pthread_mutex_unlock(&device->ts.mutex);

    response[4] = 3;
    response[5] = 0x21;
    response[10] = 0x12;
    response[11] = 0x34;
    version.device = device;
    assert(pthread_create(&control_thread, NULL, get_version_thread,
                          &version) == 0);
    int first_wait = wait_for_observation(&response_wait_entries, 1, 1000);
    transfer = get_pending(0);
    deliver_sms(transfer, MSG_SMS_GET_VERSION_EX_RES, response,
                sizeof(response));
    int first_join = join_bounded(control_thread, 4000);
    if (first_join != 0) {
        stop_requested = 1;
        pthread_mutex_lock(&device->response_mutex);
        pthread_cond_broadcast(&device->response_changed);
        pthread_mutex_unlock(&device->response_mutex);
        first_join = join_bounded(control_thread, 1000);
        stop_requested = 0;
    }
    assert(first_wait == 0);
    assert(first_join == 0);
    assert(version.result == 0);
    assert(device->version.firmware_id == 3);
    assert(device->version.firmware_version == 0x1234);

    response[4] = 4;
    response[10] = 0x56;
    response[11] = 0x78;
    assert(pthread_create(&control_thread, NULL, get_version_thread,
                          &version) == 0);
    int second_wait = wait_for_observation(&response_wait_entries, 2, 1000);
    transfer = get_pending(0);
    deliver_sms(transfer, MSG_SMS_GET_VERSION_EX_RES, response,
                sizeof(response));
    int second_join = join_bounded(control_thread, 4000);
    if (second_join != 0) {
        stop_requested = 1;
        pthread_mutex_lock(&device->response_mutex);
        pthread_cond_broadcast(&device->response_changed);
        pthread_mutex_unlock(&device->response_mutex);
        second_join = join_bounded(control_thread, 1000);
        stop_requested = 0;
    }
    assert(second_wait == 0);
    assert(second_join == 0);
    assert(version.result == 0);
    assert(device->version.firmware_id == 4);
    assert(device->version.firmware_version == 0x5678);
    pthread_mutex_lock(&observe_mutex);
    assert(control_requests == 2);
    assert(last_control_request == MSG_SMS_GET_VERSION_EX_REQ);
    pthread_mutex_unlock(&observe_mutex);

    assert(ts_queue_capture_start(&device->ts, &generation) == 0);
    pop.queue = &device->ts;
    pop.generation = generation;
    pop.result = 0;
    pthread_mutex_lock(&observe_mutex);
    capture_wait_entered = 0;
    capture_wait_result = -1;
    pthread_mutex_unlock(&observe_mutex);
    assert(pthread_create(&pop_thread, NULL, capture_pop_thread, &pop) == 0);
    int entered = wait_for_capture_wait(1000) == 0;
    ts_queue_capture_stop(&device->ts);
    int pop_join = join_bounded(pop_thread, 50);
    if (pop_join != 0) {
        ts_queue_capture_stop(&device->ts);
        assert(join_bounded(pop_thread, 500) == 0);
    }
    pthread_mutex_lock(&observe_mutex);
    int wait_result = capture_wait_result;
    pthread_mutex_unlock(&observe_mutex);
    assert(entered);
    assert(pop_join == 0);
    assert(wait_result == 0);
    assert(pop.result == -ECANCELED);

    pthread_mutex_lock(&device->ts.mutex);
    device->ts.capture_generation = UINT64_MAX;
    pthread_mutex_unlock(&device->ts.mutex);
    assert(ts_queue_capture_start(&device->ts, &generation) == -EOVERFLOW);
    pthread_mutex_lock(&device->ts.mutex);
    device->ts.closed = true;
    pthread_mutex_unlock(&device->ts.mutex);
    assert(ts_queue_capture_start(&device->ts, &generation) == -ESHUTDOWN);

    close_device(device, &drops);
    observed_device = NULL;
    for (size_t i = 0; i < pending_count; i++)
        free(pending[i]);
    mock_device = NULL;
    free(device);
    puts("capture gate: closed startup, stale callback, control response, retune separation, stop wake and terminal state: PASS");
}

int main(void)
{
    test_capture_gate_callbacks();
    return 0;
}
