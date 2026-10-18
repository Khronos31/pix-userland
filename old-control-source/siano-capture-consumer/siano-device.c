/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * User-space RIO transport, based on the framing and control flow in
 * smsusb_onresponse(), smscore_load_firmware_family2(), and
 * smsdvb_isdbt_set_frontend() from the Linux Siano driver.
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "siano-device.h"
#include "protocol.h"
#include "device-selector.h"
#include "usb-location.h"
#include "detach-decision.h"
#include "exit-codes.h"
#include "queue-policy.h"
#include "output-writer.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if !defined(_WIN32) && defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x01000107)
#define SIANO_HAVE_WRAP_SYS_DEVICE 1
#endif
#define CONTROL_TIMEOUT_MS 10000U
#define LOCK_TIMEOUT_MS 10000U
#define EVENT_THREAD_PRIORITY 20
/* Darwin condvars are CLOCK_REALTIME and lack pthread_condattr_setclock. */
#ifndef SIANO_COND_CLOCK
#if defined(__APPLE__)
#define SIANO_COND_CLOCK CLOCK_REALTIME
#else
#define SIANO_COND_CLOCK CLOCK_MONOTONIC
#endif
#endif
#ifdef _WIN32
volatile int stop_requested;
#else
volatile sig_atomic_t stop_requested;
#endif
_Static_assert(USB_TRANSFER_SIZE <= SIANO_OUTPUT_FEED_MAX,
               "output pump feed buffer must hold one USB transfer");

bool is_rio_id(uint16_t vendor, uint16_t product)
{
    return (vendor == 0x3275 && product == 0x0080) ||
           (vendor == 0x187f && product == 0x0600) ||
           (vendor == 0x187f && product == 0x0302);
}
const char *device_name(uint16_t vendor, uint16_t product, bool *supported)
{
    *supported = is_rio_id(vendor, product);
    if (*supported) {
        if (vendor == 0x3275 && product == 0x0080)
            return "PX-S1UD";
        if (vendor == 0x187f && product == 0x0600)
            return "Siano-Rio-0600";
        return "Siano-Rio-0302";
    }
    if (vendor == 0x187f) {
        switch (product) {
        case 0x0010: return "Siano-Stellar-ROM";
        case 0x0100: return "Siano-Stellar";
        case 0x0200: return "Siano-Nova-A";
        case 0x0201: return "Siano-Nova-B";
        case 0x0300: return "Siano-Vega";
        case 0x0301: return "Siano-Venice";
        case 0x0310: return "Siano-Ming";
        case 0x0500: return "Siano-Pele";
        case 0x0700: return "Siano-Denver-2160";
        case 0x0800: return "Siano-Denver-1530";
        default: return "Siano-device";
        }
    }
    return "USB-device";
}
static int cond_init_waitable(pthread_cond_t *cond)
{
#ifdef _WIN32
    return pthread_cond_init(cond, NULL) == 0 ? 0 : -1;
#else
    pthread_condattr_t attr;

    if (pthread_condattr_init(&attr) != 0)
        return -1;
#if !defined(__APPLE__)
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0) {
        pthread_condattr_destroy(&attr);
        return -1;
    }
#endif
    if (pthread_cond_init(cond, &attr) != 0) {
        pthread_condattr_destroy(&attr);
        return -1;
    }
    pthread_condattr_destroy(&attr);
    return 0;
#endif
}
static void cond_deadline_from_now(struct timespec *deadline, unsigned extra_ms)
{
    clock_gettime(SIANO_COND_CLOCK, deadline);
    deadline->tv_sec += extra_ms / 1000U;
    deadline->tv_nsec += (long)(extra_ms % 1000U) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}
static int ts_queue_init(struct ts_queue *queue)
{
    if (pthread_mutex_init(&queue->mutex, NULL) != 0)
        return -1;
    if (cond_init_waitable(&queue->available) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        return -1;
    }
    return 0;
}
static void ts_queue_close(struct ts_queue *queue)
{
    pthread_mutex_lock(&queue->mutex);
    queue->closed = true;
    pthread_cond_broadcast(&queue->available);
    pthread_mutex_unlock(&queue->mutex);
}
void ts_queue_set_fail_on_drop(struct ts_queue *queue, bool enabled)
{
    pthread_mutex_lock(&queue->mutex);
    queue->fail_on_drop = enabled;
    pthread_mutex_unlock(&queue->mutex);
}
void ts_queue_retune_begin(struct ts_queue *queue)
{
    pthread_mutex_lock(&queue->mutex);
    siano_queue_retune_begin(&queue->epoch, &queue->discarding);
    pthread_mutex_unlock(&queue->mutex);
}
bool ts_queue_retune_finish(struct ts_queue *queue, size_t *hold_len,
                                   int tune_result)
{
    bool succeeded;

    pthread_mutex_lock(&queue->mutex);
    succeeded = siano_queue_retune_finish(tune_result, &queue->epoch,
                                          &queue->discarding, &queue->head,
                                          &queue->tail, &queue->count, hold_len);
    pthread_mutex_unlock(&queue->mutex);
    return succeeded;
}
static void ts_queue_transfer_state(struct ts_queue *queue, uint64_t *epoch,
                                    uint64_t *capture_generation)
{
    pthread_mutex_lock(&queue->mutex);
    *epoch = queue->epoch;
    *capture_generation = queue->capture_generation;
    pthread_mutex_unlock(&queue->mutex);
}
static void ts_queue_destroy(struct ts_queue *queue)
{
    pthread_cond_destroy(&queue->available);
    pthread_mutex_destroy(&queue->mutex);
}
void try_realtime(pthread_t thread, int priority, const char *name)
{
#ifdef SCHED_FIFO
    struct sched_param param;
    int return_code;

    memset(&param, 0, sizeof(param));
    param.sched_priority = priority;
    return_code = pthread_setschedparam(thread, SCHED_FIFO, &param);
    if (return_code != 0)
        fprintf(stderr, "%s SCHED_FIFO %d: %s (continuing)\n",
                name, priority, strerror(return_code));
    else
        fprintf(stderr, "%s running SCHED_FIFO %d\n", name, priority);
#else
    (void)thread;
    (void)priority;
    (void)name;
#endif
}
static void ts_enqueue_generation(struct ts_queue *queue,
                                  uint64_t transfer_epoch,
                                  uint64_t capture_generation,
                                  const uint8_t *data, size_t length)
{
    pthread_mutex_lock(&queue->mutex);
    if (!queue->capture_enabled ||
        queue->capture_generation != capture_generation ||
        !siano_queue_epoch_accepts(queue->epoch, queue->discarding,
                                   transfer_epoch) || queue->closed) {
        pthread_mutex_unlock(&queue->mutex);
        return;
    }
    if (queue->count == TS_QUEUE_SLOTS) {
        queue->drops++;
        if (siano_queue_note_drop(queue->fail_on_drop, &queue->closed,
                                  &queue->overflowed))
            pthread_cond_broadcast(&queue->available);
        pthread_mutex_unlock(&queue->mutex);
        return;
    }
    if (length > sizeof(queue->chunks[queue->tail].data))
        length = sizeof(queue->chunks[queue->tail].data);
    memcpy(queue->chunks[queue->tail].data, data, length);
    queue->chunks[queue->tail].length = length;
    queue->tail = (queue->tail + 1) % TS_QUEUE_SLOTS;
    queue->count++;
    pthread_cond_signal(&queue->available);
    pthread_mutex_unlock(&queue->mutex);
}

void ts_enqueue(struct ts_queue *queue, uint64_t transfer_epoch,
                const uint8_t *data, size_t length)
{
    uint64_t capture_generation;

    pthread_mutex_lock(&queue->mutex);
    capture_generation = queue->capture_generation;
    pthread_mutex_unlock(&queue->mutex);
    ts_enqueue_generation(queue, transfer_epoch, capture_generation, data,
                          length);
}
int ts_pop(struct ts_queue *queue, uint8_t *data, size_t *length)
{
    struct timespec deadline;
    int rc = 0;

    cond_deadline_from_now(&deadline, 100);
    pthread_mutex_lock(&queue->mutex);
    while (queue->count == 0 && !queue->closed && !stop_requested) {
        rc = pthread_cond_timedwait(&queue->available, &queue->mutex, &deadline);
        if (rc == ETIMEDOUT)
            break;
    }
    if (queue->overflowed) {
        rc = siano_queue_overflow_result(queue->overflowed);
    } else if (queue->count != 0) {
        struct ts_chunk *chunk = &queue->chunks[queue->head];
        memcpy(data, chunk->data, chunk->length);
        *length = chunk->length;
        queue->head = (queue->head + 1) % TS_QUEUE_SLOTS;
        queue->count--;
        rc = 0;
    } else {
        rc = -EAGAIN;
    }
    pthread_mutex_unlock(&queue->mutex);
    return rc;
}
int ts_queue_capture_start(struct ts_queue *queue, uint64_t *generation)
{
    int rc = 0;

    pthread_mutex_lock(&queue->mutex);
    if (queue->closed || queue->overflowed) {
        rc = -ESHUTDOWN;
    } else if (queue->capture_enabled) {
        rc = -EALREADY;
    } else if (queue->capture_generation == UINT64_MAX) {
        rc = -EOVERFLOW;
    } else {
        queue->capture_generation++;
        queue->head = 0;
        queue->tail = 0;
        queue->count = 0;
        queue->capture_enabled = true;
        *generation = queue->capture_generation;
        pthread_cond_broadcast(&queue->available);
    }
    pthread_mutex_unlock(&queue->mutex);
    return rc;
}

void ts_queue_capture_stop(struct ts_queue *queue)
{
    pthread_mutex_lock(&queue->mutex);
    if (queue->capture_enabled) {
        queue->capture_enabled = false;
        if (queue->capture_generation != UINT64_MAX)
            queue->capture_generation++;
        queue->head = 0;
        queue->tail = 0;
        queue->count = 0;
    }
    pthread_cond_broadcast(&queue->available);
    pthread_mutex_unlock(&queue->mutex);
}

int ts_pop_capture(struct ts_queue *queue, uint64_t generation,
                   uint8_t *data, size_t *length)
{
    struct timespec deadline;
    int rc = 0;

    cond_deadline_from_now(&deadline, 100);
    pthread_mutex_lock(&queue->mutex);
    while (queue->count == 0 && !queue->closed && !stop_requested &&
           queue->capture_enabled &&
           queue->capture_generation == generation) {
        rc = pthread_cond_timedwait(&queue->available, &queue->mutex,
                                    &deadline);
        if (rc == ETIMEDOUT)
            break;
    }
    if (queue->overflowed) {
        rc = siano_queue_overflow_result(queue->overflowed);
    } else if (!queue->capture_enabled ||
               queue->capture_generation != generation) {
        rc = -ECANCELED;
    } else if (queue->count != 0) {
        struct ts_chunk *chunk = &queue->chunks[queue->head];
        memcpy(data, chunk->data, chunk->length);
        *length = chunk->length;
        queue->head = (queue->head + 1) % TS_QUEUE_SLOTS;
        queue->count--;
        rc = 0;
    } else {
        rc = -EAGAIN;
    }
    pthread_mutex_unlock(&queue->mutex);
    return rc;
}
static void response_note(struct siano_device *device, uint16_t type,
                          const uint8_t *payload, size_t payload_length)
{
    pthread_mutex_lock(&device->response_mutex);
    if (type < sizeof(device->response_count) / sizeof(device->response_count[0]))
        device->response_count[type]++;

    if (type == MSG_SMS_GET_VERSION_EX_RES && payload_length >= 12) {
        /* smscore_onresponse() uses rom_ver_major/minor as fw_version. */
        device->version.firmware_id = payload[4];
        device->version.supported_protocols = payload[5];
        device->version.firmware_version = ((uint16_t)payload[10] << 8) | payload[11];
        if (device->verbose) {
            fprintf(stderr,
                    "version payload: chip=%04x fw_id=%u proto=0x%02x app=%u.%u.%u rom=%u.%u\n",
                    (unsigned)sms_get_le16(payload), (unsigned)payload[4],
                    (unsigned)payload[5], (unsigned)payload[6],
                    (unsigned)payload[7], (unsigned)payload[8],
                    (unsigned)payload[10], (unsigned)payload[11]);
        }
    } else if ((type == MSG_SMS_GET_STATISTICS_EX_RES && payload_length >= 20) ||
               (type == MSG_SMS_GET_STATISTICS_RES && payload_length >= 16)) {
        size_t demod_offset = type == MSG_SMS_GET_STATISTICS_EX_RES ? 16 : 12;
        if (sms_get_le32(payload + demod_offset) != 0)
            device->locked = true;
    } else if (type == MSG_SMS_SIGNAL_DETECTED_IND) {
        device->locked = true;
    } else if (type == MSG_SMS_NO_SIGNAL_IND) {
        device->locked = false;
    }
    pthread_cond_broadcast(&device->response_changed);
    pthread_mutex_unlock(&device->response_mutex);
}
static void handle_received_buffer(struct siano_device *device, uint64_t transfer_epoch,
                                   uint64_t capture_generation,
                                   const uint8_t *buffer, size_t actual_length)
{
    struct sms_frame frame;
    int rc;

    rc = sms_frame_message(buffer, actual_length, (size_t)device->response_alignment,
                           &frame);
    if (rc < 0) {
        if (device->verbose)
            fprintf(stderr, "invalid RX frame: %s (%zu bytes)\n",
                    strerror(-rc), actual_length);
        return;
    }
    if (device->verbose && frame.type != MSG_SMS_DVBT_BDA_DATA)
        fprintf(stderr, "RX %s(%u) length=%u\n", sms_message_type_name(frame.type),
                (unsigned)frame.type, (unsigned)frame.length);
    if (frame.type == MSG_SMS_DVBT_BDA_DATA) {
        ts_enqueue_generation(&device->ts, transfer_epoch, capture_generation,
                              buffer + frame.payload_offset,
                              frame.payload_length);
    } else {
        response_note(device, frame.type, buffer + frame.payload_offset,
                      frame.payload_length);
    }
}
static void cancel_transfers(struct siano_device *device)
{
    struct libusb_transfer *transfers[MAX_URBS];

    pthread_mutex_lock(&device->state.mutex);
    memcpy(transfers, device->transfers, sizeof(transfers));
    pthread_mutex_unlock(&device->state.mutex);
    for (size_t i = 0; i < MAX_URBS; i++) {
        if (transfers[i])
            (void)libusb_cancel_transfer(transfers[i]);
    }
}
static void fail_streaming(struct siano_device *device, int error)
{
    if (!siano_stream_state_fail(&device->state, error))
        return;

    fprintf(stderr, "USB streaming stopped: %s\n", libusb_error_name(error));
    /* Wake stream_ts before waiting for the event thread to drain peers. */
    ts_queue_close(&device->ts);
    pthread_mutex_lock(&device->response_mutex);
    pthread_cond_broadcast(&device->response_changed);
    pthread_mutex_unlock(&device->response_mutex);
    cancel_transfers(device);
}
static void transfer_callback(struct libusb_transfer *transfer)
{
    struct siano_transfer_context *context = transfer->user_data;
    struct siano_device *device = context->device;
    uint64_t transfer_epoch = context->epoch;
    uint64_t capture_generation = context->capture_generation;
    bool stopping;
    enum siano_transfer_action action;

    pthread_mutex_lock(&device->state.mutex);
    if (device->active_transfers > 0)
        device->active_transfers--;
    stopping = device->state.stopping;
    pthread_mutex_unlock(&device->state.mutex);

    action = siano_transfer_classify_status(transfer->status, stopping);
    if (action == SIANO_TRANSFER_FATAL) {
        fail_streaming(device, siano_transfer_status_error(transfer->status));
        return;
    }
    if (action == SIANO_TRANSFER_DRAIN)
        return;
    if (action == SIANO_TRANSFER_COMPLETE && transfer->actual_length > 0)
        handle_received_buffer(device, transfer_epoch, capture_generation,
                               transfer->buffer,
                               (size_t)transfer->actual_length);

    pthread_mutex_lock(&device->state.mutex);
    if (!device->state.stopping) {
        ts_queue_transfer_state(&device->ts, &context->epoch,
                                &context->capture_generation);
        int rc = libusb_submit_transfer(transfer);
        if (rc == 0) {
            device->active_transfers++;
        } else {
            pthread_mutex_unlock(&device->state.mutex);
            fail_streaming(device, rc);
            return;
        }
    }
    pthread_mutex_unlock(&device->state.mutex);
}
static void *event_thread_main(void *arg)
{
    struct siano_device *device = arg;

    for (;;) {
        bool done;
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
        int rc;

        pthread_mutex_lock(&device->state.mutex);
        done = device->state.stopping && device->active_transfers == 0;
        pthread_mutex_unlock(&device->state.mutex);
        if (done)
            break;

        rc = libusb_handle_events_timeout(device->usb, &timeout);
        if (rc < 0 && rc != LIBUSB_ERROR_INTERRUPTED) {
            fail_streaming(device, rc);
        }
    }
    return NULL;
}
int start_streaming(struct siano_device *device)
{
    int rc;

    rc = pthread_create(&device->event_thread, NULL, event_thread_main, device);
    if (rc != 0)
        return -rc;
    device->event_thread_started = true;
    try_realtime(device->event_thread, EVENT_THREAD_PRIORITY, "usb-event");
    fprintf(stderr, "USB ring %u x %u bytes\n", MAX_URBS, USB_TRANSFER_SIZE);
    for (size_t i = 0; i < MAX_URBS; i++) {
        uint8_t *buffer = malloc(USB_TRANSFER_SIZE);
        struct libusb_transfer *transfer = libusb_alloc_transfer(0);

        if (!buffer || !transfer) {
            free(buffer);
            libusb_free_transfer(transfer);
            return -ENOMEM;
        }
        libusb_fill_bulk_transfer(transfer, device->handle, device->in_ep,
                                  buffer, USB_TRANSFER_SIZE,
                                  transfer_callback, &device->transfer_contexts[i], 0);
        pthread_mutex_lock(&device->state.mutex);
        if (device->state.stopping) {
            rc = device->state.error != 0 ? device->state.error : -ECANCELED;
            pthread_mutex_unlock(&device->state.mutex);
            free(buffer);
            libusb_free_transfer(transfer);
            return rc;
        }
        device->transfer_buffers[i] = buffer;
        device->transfers[i] = transfer;
        device->transfer_contexts[i].device = device;
        ts_queue_transfer_state(&device->ts,
                                &device->transfer_contexts[i].epoch,
                                &device->transfer_contexts[i].capture_generation);
        rc = libusb_submit_transfer(transfer);
        if (rc == 0)
            device->active_transfers++;
        pthread_mutex_unlock(&device->state.mutex);
        if (rc < 0) {
            fprintf(stderr, "libusb_submit_transfer: %s\n", libusb_error_name(rc));
            return rc;
        }
    }
    return 0;
}
static void stop_streaming(struct siano_device *device)
{
    siano_stream_state_stop(&device->state);
    cancel_transfers(device);

    if (device->event_thread_started)
        pthread_join(device->event_thread, NULL);
    for (size_t i = 0; i < MAX_URBS; i++) {
        libusb_free_transfer(device->transfers[i]);
        free(device->transfer_buffers[i]);
        device->transfers[i] = NULL;
        device->transfer_buffers[i] = NULL;
    }
}
static int wait_response(struct siano_device *device, uint16_t type,
                         unsigned timeout_ms)
{
    struct timespec deadline;
    int event_error;
    int rc = 0;

    cond_deadline_from_now(&deadline, timeout_ms);
    pthread_mutex_lock(&device->response_mutex);
    while (device->response_count[type] == 0 && !stop_requested &&
           siano_stream_state_error(&device->state) == 0) {
        rc = pthread_cond_timedwait(&device->response_changed,
                                    &device->response_mutex, &deadline);
        if (rc == ETIMEDOUT)
            break;
    }
    if (device->response_count[type] != 0) {
        device->response_count[type]--;
        rc = 0;
    } else if (stop_requested) {
        rc = EINTR;
    } else if ((event_error = siano_stream_state_error(&device->state)) < 0) {
        rc = -event_error;
    } else if (rc == ETIMEDOUT) {
        rc = ETIMEDOUT;
    } else {
        rc = EIO;
    }
    pthread_mutex_unlock(&device->response_mutex);
    return rc == 0 ? 0 : -rc;
}
static void clear_response(struct siano_device *device, uint16_t type)
{
    pthread_mutex_lock(&device->response_mutex);
    device->response_count[type] = 0;
    pthread_mutex_unlock(&device->response_mutex);
}
static int send_message(struct siano_device *device, const uint8_t *message,
                        size_t length)
{
    int transferred = 0;
    int rc;
    uint16_t type = sms_get_le16(message);

    if (device->verbose)
        fprintf(stderr, "TX %s(%u) length=%zu\n", sms_message_type_name(type),
                (unsigned)type, length);
    rc = libusb_bulk_transfer(device->handle, device->tx_ep,
                              (unsigned char *)message, (int)length,
                              &transferred, 1000);
    if (rc < 0)
        fprintf(stderr, "bulk OUT: %s\n", libusb_error_name(rc));
    else if ((size_t)transferred != length)
        rc = LIBUSB_ERROR_IO;
    return rc;
}
static int send_and_wait(struct siano_device *device, const uint8_t *message,
                         size_t length, uint16_t response_type)
{
    int rc;

    clear_response(device, response_type);
    rc = send_message(device, message, length);
    if (rc < 0)
        return rc;
    rc = wait_response(device, response_type, CONTROL_TIMEOUT_MS);
    if (rc < 0) {
        if (siano_stream_state_error(&device->state) == rc)
            fprintf(stderr, "waiting for %s: %s\n",
                    sms_message_type_name(response_type), libusb_error_name(rc));
        else
            fprintf(stderr, "waiting for %s: %s\n",
                    sms_message_type_name(response_type), strerror(-rc));
    }
    return rc;
}
static int get_version(struct siano_device *device)
{
    uint8_t message[SMS_HEADER_SIZE];

    sms_pack_header(message, MSG_SMS_GET_VERSION_EX_REQ, 0, SMS_HIF_TASK,
                    SMS_HEADER_SIZE, 0);
    return send_and_wait(device, message, sizeof(message),
                         MSG_SMS_GET_VERSION_EX_RES);
}
static int read_file(const char *path, uint8_t **data, size_t *size)
{
    FILE *file;
    long file_size;
    uint8_t *buffer;

    file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "firmware '%s': %s\n", path, strerror(errno));
        return -errno;
    }
    if (fseek(file, 0, SEEK_END) < 0 || (file_size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) < 0 || (uintmax_t)file_size > SIZE_MAX) {
        fclose(file);
        return -EIO;
    }
    buffer = malloc((size_t)file_size);
    if (!buffer) {
        fclose(file);
        return -ENOMEM;
    }
    if (fread(buffer, 1, (size_t)file_size, file) != (size_t)file_size) {
        free(buffer);
        fclose(file);
        return -EIO;
    }
    fclose(file);
    *data = buffer;
    *size = (size_t)file_size;
    return 0;
}
static uint32_t firmware_checksum(const uint8_t *payload, size_t length)
{
    uint32_t checksum = 0;

    for (size_t i = 0; i + 4 <= length; i += 4)
        checksum += sms_get_le32(payload + i);
    return checksum;
}
static int load_family2_firmware(struct siano_device *device, const char *path,
                                 int current_mode)
{
    uint8_t *file_data = NULL;
    uint8_t *wire_payload = NULL;
    size_t file_size = 0;
    struct sms_firmware_header header;
    uint32_t address;
    int rc;

    rc = read_file(path, &file_data, &file_size);
    if (rc < 0)
        return rc;
    rc = sms_parse_firmware_header(file_data, file_size, &header);
    if (rc < 0) {
        fprintf(stderr, "invalid firmware header in '%s': %s\n", path,
                strerror(-rc));
        free(file_data);
        return -EBADMSG;
    }
    fprintf(stderr, "firmware: checksum=0x%08x length=%u start=0x%08x\n",
            (unsigned)header.check_sum, (unsigned)header.length,
            (unsigned)header.start_address);

    /* smscore_load_firmware_family2() passes the complete file size as size.
     * Preserve that wire length; the final 12 bytes are outside the payload
     * described by the header and are zero-padded here for deterministic I/O. */
    wire_payload = calloc(1, file_size);
    if (!wire_payload) {
        free(file_data);
        return -ENOMEM;
    }
    memcpy(wire_payload, file_data + 12, header.length);
    address = header.start_address;

    if (current_mode != DEVICE_MODE_NONE) {
        uint8_t message[SMS_HEADER_SIZE];

        sms_pack_header(message, MSG_SW_RELOAD_START_REQ, 0, SMS_HIF_TASK,
                        SMS_HEADER_SIZE, 0);
        rc = send_and_wait(device, message, sizeof(message), MSG_SW_RELOAD_START_RES);
        if (rc < 0)
            goto out;
        /* smscore_load_firmware_family2() deliberately takes payload[20]
         * from the firmware image when reloading an already-running mode. */
        if (header.length < 24) {
            rc = -EBADMSG;
            goto out;
        }
        address = sms_get_le32(wire_payload + 20);
    }

    fprintf(stderr, "firmware: checksum(sum32)=0x%08x, downloading %zu bytes\n",
            firmware_checksum(wire_payload, header.length), file_size);
    for (size_t offset = 0; offset < file_size; ) {
        uint8_t message[SMS_MAX_MESSAGE_SIZE];
        size_t chunk = file_size - offset;
        if (chunk > SMS_MAX_PAYLOAD_SIZE)
            chunk = SMS_MAX_PAYLOAD_SIZE;
        sms_pack_header(message, MSG_SMS_DATA_DOWNLOAD_REQ, 0, SMS_HIF_TASK,
                        (uint16_t)(SMS_HEADER_SIZE + 4U + chunk), 0);
        sms_put_le32(message + SMS_HEADER_SIZE, address);
        memcpy(message + SMS_HEADER_SIZE + 4U, wire_payload + offset, chunk);
        rc = send_and_wait(device, message, SMS_HEADER_SIZE + 4U + chunk,
                           MSG_SMS_DATA_DOWNLOAD_RES);
        if (rc < 0)
            goto out;
        offset += chunk;
        address += (uint32_t)chunk;
    }

    {
        uint8_t message[SMS_HEADER_SIZE + 12U];

        sms_pack_header(message, MSG_SMS_DATA_VALIDITY_REQ, 0, SMS_HIF_TASK,
                        sizeof(message), 0);
        sms_put_le32(message + SMS_HEADER_SIZE + 0, header.start_address);
        sms_put_le32(message + SMS_HEADER_SIZE + 4, header.length);
        sms_put_le32(message + SMS_HEADER_SIZE + 8, 0); /* regular checksum */
        rc = send_and_wait(device, message, sizeof(message),
                           MSG_SMS_DATA_VALIDITY_RES);
        if (rc < 0)
            goto out;
    }

    if (current_mode == DEVICE_MODE_NONE) {
        uint8_t message[SMS_HEADER_SIZE + 20U];

        sms_pack_header(message, MSG_SMS_SWDOWNLOAD_TRIGGER_REQ, 0, SMS_HIF_TASK,
                        sizeof(message), 0);
        sms_put_le32(message + SMS_HEADER_SIZE + 0, header.start_address);
        sms_put_le32(message + SMS_HEADER_SIZE + 4, 6);
        sms_put_le32(message + SMS_HEADER_SIZE + 8, 0x200);
        sms_put_le32(message + SMS_HEADER_SIZE + 12, 0);
        sms_put_le32(message + SMS_HEADER_SIZE + 16, 4);
        rc = send_and_wait(device, message, sizeof(message),
                           MSG_SMS_SWDOWNLOAD_TRIGGER_RES);
    } else {
        uint8_t message[SMS_HEADER_SIZE];

        sms_pack_header(message, MSG_SW_RELOAD_EXEC_REQ, 0, SMS_HIF_TASK,
                        sizeof(message), 0);
        rc = send_message(device, message, sizeof(message));
    }
    if (rc < 0)
        goto out;
    {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 400000000L};
        nanosleep(&delay, NULL);
    }
out:
    free(wire_payload);
    free(file_data);
    return rc;
}
int set_device_mode(struct siano_device *device, const char *firmware)
{
    int current_mode;
    int rc;

    rc = get_version(device);
    if (rc < 0)
        return rc;
    current_mode = device->version.firmware_id == 255 ? DEVICE_MODE_NONE :
                   device->version.firmware_id;
    fprintf(stderr, "firmware mode=%d supported=0x%02x fw=%u.%u\n",
            current_mode, (unsigned)device->version.supported_protocols,
            (unsigned)(device->version.firmware_version >> 8),
            (unsigned)(device->version.firmware_version & 0xff));
    /* smscore_set_device_mode() returns immediately when the running mode
     * already matches. Re-sending INIT_DEVICE on a warm RIO leaves HIF
     * unresponsive to the next control message (observed on PX-S1UD). */
    if (current_mode == DEVICE_MODE_ISDBT_BDA)
        return 0;
    if (!(device->version.supported_protocols & (1U << DEVICE_MODE_ISDBT_BDA))) {
        rc = load_family2_firmware(device, firmware, current_mode);
        if (rc < 0)
            return rc;
        rc = get_version(device);
        if (rc < 0)
            return rc;
        current_mode = device->version.firmware_id == 255 ? DEVICE_MODE_NONE :
                       device->version.firmware_id;
        if (current_mode == DEVICE_MODE_ISDBT_BDA)
            return 0;
    }
    if (device->version.firmware_version >= 0x800) {
        uint8_t message[SMS_HEADER_SIZE + 4U];

        sms_pack_header(message, MSG_SMS_INIT_DEVICE_REQ, 0, SMS_HIF_TASK,
                        sizeof(message), 0);
        sms_put_le32(message + SMS_HEADER_SIZE, DEVICE_MODE_ISDBT_BDA);
        rc = send_and_wait(device, message, sizeof(message), MSG_SMS_INIT_DEVICE_RES);
        if (rc < 0)
            return rc;
    }
    return 0;
}
static int add_pid(struct siano_device *device, uint16_t pid)
{
    uint8_t message[SMS_HEADER_SIZE + 4U];

    sms_pack_header(message, MSG_SMS_ADD_PID_FILTER_REQ,
                    SMS_DVBT_BDA_CONTROL_MSG_ID, SMS_HIF_TASK, sizeof(message), 0);
    sms_put_le32(message + SMS_HEADER_SIZE, pid);
    return send_and_wait(device, message, sizeof(message), MSG_SMS_ADD_PID_FILTER_RES);
}
static int send_pid_filter(struct siano_device *device, uint16_t pid)
{
    uint8_t message[SMS_HEADER_SIZE + 4U];

    sms_pack_header(message, MSG_SMS_ADD_PID_FILTER_REQ,
                    SMS_DVBT_BDA_CONTROL_MSG_ID, SMS_HIF_TASK, sizeof(message), 0);
    sms_put_le32(message + SMS_HEADER_SIZE, pid);
    return send_message(device, message, sizeof(message));
}
int tune(struct siano_device *device, uint32_t frequency)
{
    uint8_t message[SMS_HEADER_SIZE + 16U];
    struct timespec deadline;
    int rc;

    pthread_mutex_lock(&device->response_mutex);
    device->locked = false;
    pthread_mutex_unlock(&device->response_mutex);
    sms_pack_header(message, MSG_SMS_ISDBT_TUNE_REQ,
                    SMS_DVBT_BDA_CONTROL_MSG_ID, SMS_HIF_TASK, sizeof(message), 0);
    sms_put_le32(message + SMS_HEADER_SIZE + 0, frequency);
    sms_put_le32(message + SMS_HEADER_SIZE + 4, BW_ISDBT_13SEG);
    sms_put_le32(message + SMS_HEADER_SIZE + 8, 12000000U);
    sms_put_le32(message + SMS_HEADER_SIZE + 12, 0);
    rc = send_and_wait(device, message, sizeof(message), MSG_SMS_ISDBT_TUNE_RES);
    if (rc < 0)
        return rc;

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += LOCK_TIMEOUT_MS / 1000U;
    deadline.tv_nsec += (long)(LOCK_TIMEOUT_MS % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    while (!stop_requested) {
        bool locked;
        struct timespec now;
        uint8_t stats[SMS_HEADER_SIZE];
        uint16_t stats_request = device->version.firmware_version >= 0x800 ?
                                 MSG_SMS_GET_STATISTICS_EX_REQ : MSG_SMS_GET_STATISTICS_REQ;

        pthread_mutex_lock(&device->response_mutex);
        locked = device->locked;
        pthread_mutex_unlock(&device->response_mutex);
        if (locked)
            return 0;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
            break;

        sms_pack_header(stats, stats_request, SMS_DVBT_BDA_CONTROL_MSG_ID,
                        SMS_HIF_TASK, sizeof(stats), 0);
        (void)send_message(device, stats, sizeof(stats));
        /* Discard a bounded amount of TS during lock wait. An unbounded
         * drain never returns once the mux is flowing. */
        {
            uint8_t discard[USB_TRANSFER_SIZE];
            size_t discard_length;
            for (int n = 0; n < 32; n++) {
                if (ts_pop(&device->ts, discard, &discard_length) != 0)
                    break;
            }
        }
        pthread_mutex_lock(&device->response_mutex);
        struct timespec short_deadline;
        cond_deadline_from_now(&short_deadline, 100);
        (void)pthread_cond_timedwait(&device->response_changed,
                                     &device->response_mutex, &short_deadline);
        pthread_mutex_unlock(&device->response_mutex);
    }
    if (!stop_requested)
        fprintf(stderr, "ISDB-T tune response received but no demod lock\n");
    return -ETIMEDOUT;
}
bool ts_queue_overflowed(struct ts_queue *queue)
{
    bool overflowed;

    pthread_mutex_lock(&queue->mutex);
    overflowed = queue->overflowed;
    pthread_mutex_unlock(&queue->mutex);
    return overflowed;
}
int configure_pid_filters(struct siano_device *device, const uint16_t *pids,
                          size_t pid_count)
{
    if (pid_count == 0) {
        /* 0x2000 is the DVB catch-all. This firmware does not ACK it;
         * the mux still flows, so do not block recording on the response. */
        return send_pid_filter(device, 0x2000);
    }
    for (size_t i = 0; i < pid_count; i++) {
        int rc;

        if (siano_pid_filter_requires_ack(pids[i])) {
            rc = add_pid(device, pids[i]);
        } else {
            rc = send_pid_filter(device, pids[i]);
        }
        if (rc < 0)
            return rc;
    }
    return 0;
}
static int inspect_and_claim(struct siano_device *device)
{
    libusb_device *usb_device;
    struct libusb_config_descriptor *config = NULL;
    struct libusb_device_descriptor descriptor;
    bool supported;
    const char *name;
    int rc;

    usb_device = libusb_get_device(device->handle);
    rc = libusb_get_device_descriptor(usb_device, &descriptor);
    if (rc < 0) {
        fprintf(stderr, "device descriptor: %s\n", libusb_error_name(rc));
        return rc;
    }
    name = device_name(descriptor.idVendor, descriptor.idProduct, &supported);
    if (!supported) {
        fprintf(stderr, "%04x:%04x is %s\n", descriptor.idVendor,
                descriptor.idProduct, name);
        return -ENODEV;
    }
    rc = libusb_get_active_config_descriptor(usb_device, &config);
    if (rc < 0) {
        fprintf(stderr, "active USB config: %s\n", libusb_error_name(rc));
        return rc;
    }
    device->interface_number = -1;
    for (int i = 0; i < config->bNumInterfaces && device->interface_number < 0; i++) {
        const struct libusb_interface *interface = &config->interface[i];
        for (int a = 0; a < interface->num_altsetting; a++) {
            const struct libusb_interface_descriptor *alt = &interface->altsetting[a];
            uint8_t in_ep = 0;
            uint8_t descriptor_out_ep = 0;
            int max_packet = 0;

            for (int e = 0; e < alt->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *endpoint = &alt->endpoint[e];
                if ((endpoint->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
                    LIBUSB_TRANSFER_TYPE_BULK)
                    continue;
                if (endpoint->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                    in_ep = endpoint->bEndpointAddress;
                    max_packet = endpoint->wMaxPacketSize & 0x7ff;
                } else {
                    descriptor_out_ep = endpoint->bEndpointAddress;
                }
            }
            if (!in_ep)
                continue;
            device->interface_number = alt->bInterfaceNumber;
            device->in_ep = in_ep;
            device->response_alignment = max_packet > (int)SMS_HEADER_SIZE ?
                                          max_packet - SMS_HEADER_SIZE : 0;
            if (descriptor_out_ep != 2)
                fprintf(stderr, "warning: descriptor OUT endpoint is 0x%02x; kernel protocol uses fixed endpoint 2\n",
                        descriptor_out_ep);
            break;
        }
    }
    libusb_free_config_descriptor(config);
    if (device->interface_number < 0) {
        fprintf(stderr, "no bulk IN endpoint found\n");
        return -ENODEV;
    }
    device->tx_ep = 2; /* smsusb_sendrequest() uses usb_sndbulkpipe(..., 2). */
    /*
     * Do not take the device from a bound kernel driver unless asked.  Handing
     * a running tuner over from smsusb is not judged safe (README), and a
     * program that did not mean to would silently break the DVB user of it.
     * Only the interface claimed below matters: that is where smsusb binds
     * (smsusb_probe() accepts only the board's intf_num, 0 for the supported
     * IDs, and only on one-interface configs), and auto-detach below applies
     * to exactly that interface.  libusb reports LIBUSB_ERROR_NOT_SUPPORTED
     * where it cannot tell (e.g. Windows); go on there, as before.
     */
    if (!device->detach_kernel_driver) {
        const int bound = libusb_kernel_driver_active(device->handle,
                                                     device->interface_number);
        enum detach_decision decision = classify_kernel_driver_query(bound);

        if (decision == REFUSE_BOUND) {
            fprintf(stderr, "interface %d is bound to a kernel driver (e.g. smsusb); not taking it over.\n"
                            "If it is smsusb, blacklist smsusb, smsdvb and smsmdtv and reboot.\n"
                            "To take it anyway, pass --detach-kernel-driver.\n",
                    device->interface_number);
            (void)siano_stream_state_fail(&device->state, EBUSY);
            return -EBUSY;
        }
        if (decision == REFUSE_UNKNOWN) {
            fprintf(stderr, "cannot query kernel driver state on interface %d: %s\n",
                    device->interface_number, libusb_error_name(bound));
            (void)siano_stream_state_fail(&device->state, EIO);
            return -EIO;
        }
    }
    if (device->detach_kernel_driver)
        (void)libusb_set_auto_detach_kernel_driver(device->handle, 1);
    rc = libusb_claim_interface(device->handle, device->interface_number);
    if (rc < 0) {
        fprintf(stderr, "libusb_claim_interface(%d): %s\n", device->interface_number,
                libusb_error_name(rc));
        return rc;
    }
    /* smsusb_probe() clears halt on both bulk endpoints before talking. */
    (void)libusb_clear_halt(device->handle, device->in_ep);
    (void)libusb_clear_halt(device->handle, device->tx_ep | LIBUSB_ENDPOINT_OUT);
    fprintf(stderr, "opened RIO %04x:%04x interface %d, IN endpoint 0x%02x, response alignment %d\n",
            descriptor.idVendor, descriptor.idProduct, device->interface_number,
            device->in_ep, device->response_alignment);
    return 0;
}
static bool selector_matches(const struct siano_device_selector *selector,
                             libusb_device *candidate)
{
    if (selector->kind == SIANO_DEVICE_SELECTOR_BUS_ADDRESS)
        return libusb_get_bus_number(candidate) == selector->bus &&
               libusb_get_device_address(candidate) == selector->address;
    if (selector->kind == SIANO_DEVICE_SELECTOR_PORT) {
        uint8_t ports[8];
        int count = libusb_get_port_numbers(candidate, ports, (int)sizeof(ports));

        return libusb_get_bus_number(candidate) == selector->bus &&
               count == (int)selector->port_count &&
               memcmp(ports, selector->ports, selector->port_count) == 0;
    }
    return false;
}
int open_rio(struct siano_device *device,
                    const struct siano_device_selector *selector,
                    const char *selector_text)
{
    libusb_device **list;
    ssize_t count;
    unsigned found = 0;
    unsigned matched = 0;
    libusb_device *selected = NULL;
    int rc;

    count = libusb_get_device_list(device->usb, &list);
    if (count < 0)
        return (int)count;
    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor descriptor;

        if (libusb_get_device_descriptor(list[i], &descriptor) < 0)
            continue;
        if (!is_rio_id(descriptor.idVendor, descriptor.idProduct))
            continue;
        if (selector->kind == SIANO_DEVICE_SELECTOR_INDEX) {
            if (found++ == selector->index) {
                selected = list[i];
                break;
            }
        } else if (selector_matches(selector, list[i])) {
            selected = list[i];
            matched++;
        }
    }
    if (matched > 1) {
        fprintf(stderr, "RIO device %s is ambiguous\n", selector_text);
        libusb_free_device_list(list, 1);
        return -EINVAL;
    }
    if (!selected) {
        fprintf(stderr, "RIO device %s not found\n", selector_text);
        libusb_free_device_list(list, 1);
        return -ENODEV;
    }
    rc = libusb_open(selected, &device->handle);
    libusb_free_device_list(list, 1);
    if (rc < 0) {
        fprintf(stderr, "libusb_open: %s\n", libusb_error_name(rc));
        return rc;
    }
    return inspect_and_claim(device);
}
int open_from_fd(struct siano_device *device, int fd)
{
#if defined(_WIN32)
    (void)device;
    (void)fd;
    fprintf(stderr, "--fd is not supported on Windows\n");
    return -ENOTSUP;
#elif defined(SIANO_HAVE_WRAP_SYS_DEVICE)
    int rc;

    if (fd < 0 || fcntl(fd, F_GETFD) < 0) {
        fprintf(stderr, "USB fd %d: %s\n", fd, strerror(errno));
        return -EBADF;
    }
    rc = libusb_wrap_sys_device(device->usb, (intptr_t)fd, &device->handle);
    if (rc < 0) {
        fprintf(stderr, "libusb_wrap_sys_device(%d): %s\n", fd,
                libusb_error_name(rc));
        return rc;
    }
    fprintf(stderr, "wrapped USB fd %d\n", fd);
    return inspect_and_claim(device);
#else
    (void)device;
    fprintf(stderr, "--fd requires libusb >= 1.0.23\n");
    return -ENOTSUP;
#endif
}
void destroy_device_sync_state(struct siano_device *device)
{
    if (device->response_cond_initialized) {
        pthread_cond_destroy(&device->response_changed);
        device->response_cond_initialized = false;
    }
    if (device->response_mutex_initialized) {
        pthread_mutex_destroy(&device->response_mutex);
        device->response_mutex_initialized = false;
    }
    if (device->state_initialized) {
        siano_stream_state_destroy(&device->state);
        device->state_initialized = false;
    }
    if (device->ts_initialized) {
        ts_queue_destroy(&device->ts);
        device->ts_initialized = false;
    }
}
void close_device(struct siano_device *device, uint64_t *drops)
{
    if (device->event_thread_started)
        stop_streaming(device);
    if (device->ts_initialized) {
        ts_queue_close(&device->ts);
        pthread_mutex_lock(&device->ts.mutex);
        *drops = device->ts.drops;
        pthread_mutex_unlock(&device->ts.mutex);
    }
    if (device->handle) {
        if (device->interface_number >= 0)
            libusb_release_interface(device->handle, device->interface_number);
        libusb_close(device->handle);
        device->handle = NULL;
    }
    destroy_device_sync_state(device);
}
int init_device_state_capture(struct siano_device *device, libusb_context *usb,
                              bool verbose, bool capture_enabled)
{
    memset(device, 0, sizeof(*device));
    device->usb = usb;
    device->verbose = verbose;
    device->interface_number = -1;
    if (siano_stream_state_init(&device->state) != 0)
        return -1;
    device->state_initialized = true;
    if (pthread_mutex_init(&device->response_mutex, NULL) != 0)
        goto fail;
    device->response_mutex_initialized = true;
    if (cond_init_waitable(&device->response_changed) != 0)
        goto fail;
    device->response_cond_initialized = true;
    if (ts_queue_init(&device->ts) < 0)
        goto fail;
    device->ts.capture_enabled = capture_enabled;
    device->ts_initialized = true;
    return 0;

fail:
    destroy_device_sync_state(device);
    return -1;
}

int init_device_state(struct siano_device *device, libusb_context *usb,
                      bool verbose)
{
    return init_device_state_capture(device, usb, verbose, true);
}
