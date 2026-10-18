/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <libusb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "protocol.h"

#define LOOPBACK_TRANSFERS 64U

static pthread_mutex_t loop_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t loop_changed = PTHREAD_COND_INITIALIZER;
static struct libusb_transfer *transfers[LOOPBACK_TRANSFERS];
static bool pending[LOOPBACK_TRANSFERS];
static struct libusb_transfer *events[LOOPBACK_TRANSFERS * 2U];
static size_t event_head;
static size_t event_count;
static unsigned control_requests;
static uint16_t last_control_type;
int test_libusb_deliver(struct libusb_transfer*, const uint8_t*, size_t);

int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *handle,
                                      unsigned char endpoint, unsigned char *data,
                                      int length, int *transferred, unsigned int timeout)
{
    (void)handle; (void)timeout;
    if ((endpoint & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT && length >= (int)SMS_HEADER_SIZE) {
        const uint16_t type = sms_get_le16(data);
        ++control_requests;
        last_control_type = type;
        *transferred = length;
        uint16_t response_type = type == MSG_SMS_ADD_PID_FILTER_REQ
            ? MSG_SMS_ADD_PID_FILTER_RES : 0U;
        if (response_type) {
            uint8_t response[SMS_HEADER_SIZE];
            sms_pack_header(response, response_type, 0U, 0U, sizeof(response), 0U);
            for (size_t i = 0U; i < LOOPBACK_TRANSFERS; ++i) {
                pthread_mutex_lock(&loop_mutex);
                struct libusb_transfer *transfer = transfers[i];
                bool is_pending = pending[i];
                pthread_mutex_unlock(&loop_mutex);
                if (transfer && is_pending)
                    return test_libusb_deliver(transfer, response, sizeof(response));
            }
            return LIBUSB_ERROR_NOT_FOUND;
        }
        return 0;
    }
    *transferred = length;
    return 0;
}

unsigned test_libusb_control_requests(void) { return control_requests; }
uint16_t test_libusb_last_control_type(void) { return last_control_type; }

static size_t transfer_slot(struct libusb_transfer *transfer)
{
    for (size_t i = 0U; i < LOOPBACK_TRANSFERS; ++i) {
        if (transfers[i] == transfer) return i;
    }
    return LOOPBACK_TRANSFERS;
}

static int queue_event(struct libusb_transfer *transfer)
{
    if (event_count == LOOPBACK_TRANSFERS * 2U) return LIBUSB_ERROR_BUSY;
    events[(event_head + event_count) % (LOOPBACK_TRANSFERS * 2U)] = transfer;
    ++event_count;
    pthread_cond_broadcast(&loop_changed);
    return 0;
}

struct libusb_transfer * LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
    (void)iso_packets;
    struct libusb_transfer *transfer = calloc(1U, sizeof(*transfer));
    if (!transfer) return NULL;
    pthread_mutex_lock(&loop_mutex);
    size_t slot = transfer_slot(NULL);
    if (slot == LOOPBACK_TRANSFERS) {
        pthread_mutex_unlock(&loop_mutex);
        free(transfer);
        return NULL;
    }
    transfers[slot] = transfer;
    pthread_mutex_unlock(&loop_mutex);
    return transfer;
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *transfer)
{
    pthread_mutex_lock(&loop_mutex);
    size_t slot = transfer_slot(transfer);
    if (slot != LOOPBACK_TRANSFERS) {
        transfers[slot] = NULL;
        pending[slot] = false;
    }
    pthread_mutex_unlock(&loop_mutex);
    free(transfer);
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *transfer)
{
    pthread_mutex_lock(&loop_mutex);
    size_t slot = transfer_slot(transfer);
    if (slot == LOOPBACK_TRANSFERS || pending[slot]) {
        pthread_mutex_unlock(&loop_mutex);
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    pending[slot] = true;
    pthread_mutex_unlock(&loop_mutex);
    return 0;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *transfer)
{
    pthread_mutex_lock(&loop_mutex);
    size_t slot = transfer_slot(transfer);
    if (slot == LOOPBACK_TRANSFERS || !pending[slot]) {
        pthread_mutex_unlock(&loop_mutex);
        return LIBUSB_ERROR_NOT_FOUND;
    }
    pending[slot] = false;
    transfer->status = LIBUSB_TRANSFER_CANCELLED;
    transfer->actual_length = 0;
    int rc = queue_event(transfer);
    pthread_mutex_unlock(&loop_mutex);
    return rc;
}

int LIBUSB_CALL libusb_handle_events_timeout(libusb_context *context,
                                              struct timeval *timeout)
{
    (void)context;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout->tv_sec;
    deadline.tv_nsec += timeout->tv_usec * 1000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&loop_mutex);
    while (event_count == 0U)
        if (pthread_cond_timedwait(&loop_changed, &loop_mutex, &deadline) != 0) break;
    if (event_count == 0U) {
        pthread_mutex_unlock(&loop_mutex);
        return 0;
    }
    struct libusb_transfer *transfer = events[event_head];
    event_head = (event_head + 1U) % (LOOPBACK_TRANSFERS * 2U);
    --event_count;
    pthread_mutex_unlock(&loop_mutex);
    transfer->callback(transfer);
    return 0;
}

int test_libusb_deliver(struct libusb_transfer *transfer, const uint8_t *bytes,
                        size_t length)
{
    if (!transfer || !bytes || length > (size_t)transfer->length) return -1;
    pthread_mutex_lock(&loop_mutex);
    size_t slot = transfer_slot(transfer);
    if (slot == LOOPBACK_TRANSFERS || !pending[slot]) {
        pthread_mutex_unlock(&loop_mutex);
        return -1;
    }
    pending[slot] = false;
    memcpy(transfer->buffer, bytes, length);
    transfer->status = LIBUSB_TRANSFER_COMPLETED;
    transfer->actual_length = (int)length;
    int rc = queue_event(transfer);
    pthread_mutex_unlock(&loop_mutex);
    return rc;
}

int test_libusb_pending(struct libusb_transfer *transfer)
{
    pthread_mutex_lock(&loop_mutex);
    size_t slot = transfer_slot(transfer);
    int result = slot != LOOPBACK_TRANSFERS && pending[slot];
    pthread_mutex_unlock(&loop_mutex);
    return result;
}
