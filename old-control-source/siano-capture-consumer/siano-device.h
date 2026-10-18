/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIANO_DEVICE_H
#define SIANO_DEVICE_H

#include "siano-os.h"
#include "stream-state.h"
#include "device-selector.h"

#include <libusb.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef _WIN32
#include <signal.h>
#endif

/* Use a deeper ring on macOS to absorb host-side callback stalls. */
#if defined(__APPLE__)
#define MAX_URBS 128U
#else
#define MAX_URBS 32U
#endif
#define USB_TRANSFER_SIZE 16384U
#define TS_QUEUE_SLOTS 256U

struct ts_chunk {
    size_t length;
    uint8_t data[USB_TRANSFER_SIZE];
};

struct ts_queue {
    pthread_mutex_t mutex;
    pthread_cond_t available;
    struct ts_chunk chunks[TS_QUEUE_SLOTS];
    size_t head;
    size_t tail;
    size_t count;
    uint64_t drops;
    bool closed;
    bool fail_on_drop;
    bool overflowed;
    bool discarding;
    uint64_t epoch;
    bool capture_enabled;
    uint64_t capture_generation;
};

struct siano_device;

struct siano_transfer_context {
    struct siano_device *device;
    /* Captured at submit time so delayed callbacks retain their old generation. */
    uint64_t epoch;
    uint64_t capture_generation;
};

struct version_info {
    uint8_t firmware_id;
    uint8_t supported_protocols;
    uint16_t firmware_version;
};

struct siano_device {
    libusb_context *usb;
    libusb_device_handle *handle;
    /* --detach-kernel-driver: take the interface from a bound kernel driver. */
    bool detach_kernel_driver;
    int interface_number;
    uint8_t in_ep;
    uint8_t tx_ep;
    int response_alignment;
    int verbose;

    pthread_t event_thread;
    bool event_thread_started;
    struct siano_stream_state state;
    bool state_initialized;
    int active_transfers;
    struct libusb_transfer *transfers[MAX_URBS];
    uint8_t *transfer_buffers[MAX_URBS];
    struct siano_transfer_context transfer_contexts[MAX_URBS];

    pthread_mutex_t response_mutex;
    pthread_cond_t response_changed;
    bool response_mutex_initialized;
    bool response_cond_initialized;
    unsigned response_count[1024];
    struct version_info version;
    bool locked;

    struct ts_queue ts;
    bool ts_initialized;
};

#ifdef _WIN32
extern volatile int stop_requested;
#else
extern volatile sig_atomic_t stop_requested;
#endif

bool is_rio_id(uint16_t vendor, uint16_t product);
const char *device_name(uint16_t vendor, uint16_t product, bool *supported);
void try_realtime(pthread_t thread, int priority, const char *name);
void ts_queue_set_fail_on_drop(struct ts_queue *queue, bool enabled);
void ts_queue_retune_begin(struct ts_queue *queue);
bool ts_queue_retune_finish(struct ts_queue *queue, size_t *hold_len,
                            int tune_result);
void ts_enqueue(struct ts_queue *queue, uint64_t transfer_epoch,
                const uint8_t *data, size_t length);
int ts_pop(struct ts_queue *queue, uint8_t *data, size_t *length);
int ts_queue_capture_start(struct ts_queue *queue, uint64_t *generation);
void ts_queue_capture_stop(struct ts_queue *queue);
int ts_pop_capture(struct ts_queue *queue, uint64_t generation,
                   uint8_t *data, size_t *length);
bool ts_queue_overflowed(struct ts_queue *queue);
int init_device_state(struct siano_device *device, libusb_context *usb,
                      bool verbose);
int init_device_state_capture(struct siano_device *device, libusb_context *usb,
                              bool verbose, bool capture_enabled);
int open_rio(struct siano_device *device,
             const struct siano_device_selector *selector,
             const char *selector_text);
int open_from_fd(struct siano_device *device, int fd);
int start_streaming(struct siano_device *device);
int set_device_mode(struct siano_device *device, const char *firmware);
int tune(struct siano_device *device, uint32_t frequency);
int configure_pid_filters(struct siano_device *device, const uint16_t *pids,
                          size_t pid_count);
void close_device(struct siano_device *device, uint64_t *drops);
void destroy_device_sync_state(struct siano_device *device);

#endif
