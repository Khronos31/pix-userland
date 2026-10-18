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

#include "protocol.h"
#include "control-parse.h"
#include "device-selector.h"
#include "usb-location.h"

#ifdef _WIN32
#include "siano-os.h"
#endif

#include "stream-state.h"
#include "queue-policy.h"
#include "control-input.h"
#include "output-writer.h"
#include "write-policy.h"
#include "siano-device.h"

#include <errno.h>
#include <fcntl.h>
#include <libusb.h>
#include <limits.h>
/* These headers include <libusb.h>; keep them after siano-os.h so winsock2.h
 * stays ahead of windows.h in the MSVC include order. */
#include "detach-decision.h"
#include "exit-codes.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if !defined(_WIN32) && defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x01000107)
#define SIANO_HAVE_WRAP_SYS_DEVICE 1
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif
/* Upper bound on one pending output step; removed from any stop-event latency. */
#define OUTPUT_WAIT_MS 50
#define WRITER_THREAD_PRIORITY 10

struct options {
    bool list;
    bool control;
    bool detach_kernel_driver;
    bool fail_on_drop;
    bool verbose;
    struct siano_device_selector device_selector;
    const char *device_spec;
    int device_fd;
    int duration;
    bool have_duration;
    bool have_channel;
    unsigned channel;
    bool have_frequency;
    uint32_t frequency;
    char *firmware;
    char *output;
    uint16_t pids[64];
    size_t pid_count;
};

static void on_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage: %s [options]\n"
            "  -c, --channel N       ISDB-T physical channel (13..62 or T13..T62)\n"
            "  -f, --freq HZ         tune frequency in Hz\n"
            "  -t, --time SECONDS    stop after duration (default: until SIGINT)\n"
            "  -F, --firmware PATH   isdbt_rio.inp\n"
            "  -d, --device SPEC     index, USB port path (B-A.P...), or bus:address\n"
            "      --fd FD           use an already-open USB fd (Termux/Android)\n"
            "  -p, --pid PID         add PID filter (repeatable)\n"
            "  -o, --output PATH     write TS to PATH instead of stdout\n"
            "  -v, --verbose         log control message types\n"
            "  -l, --list            list devices (not filtered by --device)\n"
            "      --control         accept channel/tune/quit commands on stdin\n"
            "      --fail-on-drop    exit 8 on the first TS queue overflow\n"
            "      --detach-kernel-driver\n"
            "                        take the device from a bound kernel driver (e.g. smsusb)\n"
            "  -h, --help            show this help\n"
            "A leftover integer argument is treated as --fd (termux-usb -e).\n",
            program);
}

static int parse_unsigned(const char *text, unsigned long max, unsigned long *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno || end == text || *end != '\0' || parsed > max)
        return -EINVAL;
    *value = parsed;
    return 0;
}

/* mirakc-style terrestrial channel: uppercase T plus two decimal digits. */
static int parse_channel(const char *text, unsigned long *value)
{
    if (text[0] == 'T') {
        if (text[1] < '0' || text[1] > '9' ||
            text[2] < '0' || text[2] > '9' || text[3] != '\0')
            return -EINVAL;
        *value = (unsigned long)(text[1] - '0') * 10U +
                 (unsigned long)(text[2] - '0');
        return 0;
    }
    return parse_unsigned(text, 62, value);
}

static int apply_option(int option, const char *value_text, struct options *options,
                        const char *program)
{
    unsigned long value;

    switch (option) {
    case 'c':
        if (parse_channel(value_text, &value) < 0 || value < 13 || value > 62)
            return -EINVAL;
        options->channel = (unsigned)value;
        options->have_channel = true;
        return 0;
    case 'f':
        if (parse_unsigned(value_text, UINT32_MAX, &value) < 0 || value == 0)
            return -EINVAL;
        options->frequency = (uint32_t)value;
        options->have_frequency = true;
        return 0;
    case 't':
        if (parse_unsigned(value_text, INT_MAX, &value) < 0 || value == 0)
            return -EINVAL;
        options->duration = (int)value;
        options->have_duration = true;
        return 0;
    case 'F':
        options->firmware = (char *)value_text;
        return 0;
    case 'd':
        if (siano_parse_device_selector(value_text, &options->device_selector) < 0)
            return -EINVAL;
        options->device_spec = value_text;
        return 0;
    case 1:
        if (parse_unsigned(value_text, INT_MAX, &value) < 0)
            return -EINVAL;
        options->device_fd = (int)value;
        return 0;
    case 'p':
        if (options->pid_count == sizeof(options->pids) / sizeof(options->pids[0]) ||
            parse_unsigned(value_text, 0x2000, &value) < 0)
            return -EINVAL;
        options->pids[options->pid_count++] = (uint16_t)value;
        return 0;
    case 'o':
        options->output = (char *)value_text;
        return 0;
    case 'v':
        options->verbose = true;
        return 0;
    case 'l':
        options->list = true;
        return 0;
    case 2:
        options->control = true;
        return 0;
    case 3:
        options->detach_kernel_driver = true;
        return 0;
    case 4:
        options->fail_on_drop = true;
        return 0;
    case 'h':
        usage(stdout, program);
        fflush(stdout);
        exit(0);
    default:
        return -EINVAL;
    }
}

static int finish_options(int argc, char **argv, int leftover, struct options *options)
{
    if (leftover < argc) {
        unsigned long fd_value;

        if (leftover + 1 != argc ||
            parse_unsigned(argv[leftover], INT_MAX, &fd_value) < 0)
            return -EINVAL;
        if (options->device_fd >= 0 && options->device_fd != (int)fd_value)
            return -EINVAL;
        options->device_fd = (int)fd_value;
    }
    if (options->have_channel && options->have_frequency)
        return -EINVAL;
    if (options->control && (options->list || options->have_duration))
        return -EINVAL;
    if (options->list && options->device_fd >= 0)
        return -EINVAL;
    if (options->list && options->detach_kernel_driver)
        return -EINVAL;
    if (options->device_fd >= 0 &&
        (options->device_selector.kind != SIANO_DEVICE_SELECTOR_INDEX ||
         options->device_selector.index != 0))
        return -EINVAL;
    if (!options->list && !options->control &&
        !options->have_channel && !options->have_frequency)
        return -EINVAL;
    return 0;
}

static int parse_options(int argc, char **argv, struct options *options)
{
    static const struct option long_options[] = {
        {"channel", required_argument, NULL, 'c'},
        {"freq", required_argument, NULL, 'f'},
        {"time", required_argument, NULL, 't'},
        {"firmware", required_argument, NULL, 'F'},
        {"device", required_argument, NULL, 'd'},
        {"fd", required_argument, NULL, 1},
        {"pid", required_argument, NULL, 'p'},
        {"output", required_argument, NULL, 'o'},
        {"verbose", no_argument, NULL, 'v'},
        {"list", no_argument, NULL, 'l'},
        {"control", no_argument, NULL, 2},
        {"detach-kernel-driver", no_argument, NULL, 3},
        {"fail-on-drop", no_argument, NULL, 4},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    memset(options, 0, sizeof(*options));
    options->device_fd = -1;
    options->device_selector.kind = SIANO_DEVICE_SELECTOR_INDEX;
    options->device_selector.index = 0;
    options->device_spec = "0";
#ifdef _WIN32
    {
        int i = 1;

        while (i < argc) {
            const char *a = argv[i];
            const char *value = NULL;
            int option = 0;
            int needs_value = 0;
            size_t n;

            if (a[0] != '-')
                break;
            i++;
            if (a[1] == '-' && a[2] == '\0')
                break;
            if (a[1] == '-') {
                const char *name = a + 2;
                const char *eq = strchr(name, '=');
                size_t namelen = eq ? (size_t)(eq - name) : strlen(name);

                for (n = 0; long_options[n].name; n++) {
                    if (strncmp(long_options[n].name, name, namelen) != 0 ||
                        long_options[n].name[namelen] != '\0')
                        continue;
                    option = long_options[n].val;
                    needs_value = long_options[n].has_arg == required_argument;
                    break;
                }
                if (!option)
                    return -EINVAL;
                if (needs_value) {
                    if (eq)
                        value = eq + 1;
                    else if (i < argc)
                        value = argv[i++];
                    else
                        return -EINVAL;
                }
            } else {
                option = a[1];
                if (option == '\0' || strchr("cftFdpovlh", option) == NULL)
                    return -EINVAL;
                needs_value = (option == 'c' || option == 'f' || option == 't' ||
                               option == 'F' || option == 'd' || option == 'p' ||
                               option == 'o');
                if (a[2] != '\0' && needs_value)
                    value = a + 2;
                else if (needs_value) {
                    if (i < argc)
                        value = argv[i++];
                    else
                        return -EINVAL;
                }
            }
            if (apply_option(option, value, options, argv[0]) < 0)
                return -EINVAL;
        }
        return finish_options(argc, argv, i, options);
    }
#else
    {
        int option;

        while ((option = getopt_long(argc, argv, "c:f:t:F:d:p:o:vlh", long_options,
                                     NULL)) != -1) {
            if (apply_option(option, optarg, options, argv[0]) < 0)
                return -EINVAL;
        }
        return finish_options(argc, argv, optind, options);
    }
#endif
}

static int list_devices(libusb_context *usb)
{
    libusb_device **list;
    ssize_t count;

    count = libusb_get_device_list(usb, &list);
    if (count < 0) {
        fprintf(stderr, "libusb_get_device_list: %s\n", libusb_strerror((int)count));
        return siano_list_result((long long)count);
    }
    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor descriptor;
        bool supported;
        const char *name;

        if (libusb_get_device_descriptor(list[i], &descriptor) < 0)
            continue;
        name = device_name(descriptor.idVendor, descriptor.idProduct, &supported);
        if (descriptor.idVendor != 0x187f && descriptor.idVendor != 0x3275)
            continue;
        /* Hub depth is at most 7; a longer path reads as "port=-". */
        uint8_t ports[7];
        int port_count = libusb_get_port_numbers(list[i], ports, (int)sizeof(ports));
        char location[64];
        if (siano_format_usb_location(location, sizeof(location), libusb_get_bus_number(list[i]),
                                      libusb_get_device_address(list[i]), ports,
                                      port_count) < 0)
            location[0] = '\0';
        if (supported) {
            printf("model=%s usb=%04x:%04x%s status=ready receivers=1\n",
                   name, (unsigned)descriptor.idVendor,
                   (unsigned)descriptor.idProduct, location);
            printf("receiver=0 device=1 local=0 system=ISDB-T\n");
        } else {
            printf("rejected model=%s usb=%04x:%04x%s status=unsupported\n",
                   name, (unsigned)descriptor.idVendor,
                   (unsigned)descriptor.idProduct, location);
        }
    }
    libusb_free_device_list(list, 1);
    return 0;
}

static int resolve_firmware(const char *requested, char **path)
{
    static const char *const defaults[] = {
        "./firmware/isdbt_rio.inp",
        "isdbt_rio.inp",
#ifndef _WIN32
        "/lib/firmware/isdbt_rio.inp",
#endif
    };

    if (requested) {
        if (access(requested, R_OK) < 0) {
            fprintf(stderr, "firmware '%s': %s\n", requested, strerror(errno));
            return -ENOENT;
        }
        *path = strdup(requested);
        return *path ? 0 : -ENOMEM;
    }
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        if (access(defaults[i], R_OK) == 0) {
            *path = strdup(defaults[i]);
            return *path ? 0 : -ENOMEM;
        }
    }
    fprintf(stderr, "firmware 'isdbt_rio.inp' not found; use --firmware PATH\n");
    return -ENOENT;
}

static void try_lock_pages(void)
{
#ifdef MCL_CURRENT
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "mlockall: %s (continuing)\n", strerror(errno));
    else
        fprintf(stderr, "address space locked\n");
#endif
}

/* Consulted while output is pending so stop events are not deferred by a
 * stalled consumer. Uses the same error priority as the existing loops. */
static int stream_pending_check(struct siano_device *device)
{
    int error = siano_stream_state_error(&device->state);

    if (error < 0)
        return error;
    if (ts_queue_overflowed(&device->ts))
        return -ENOBUFS;
    return 0;
}

static int stream_ts(struct siano_device *device, struct siano_output_pump *pump,
                     int duration)
{
    struct timespec deadline;
    uint8_t data[USB_TRANSFER_SIZE];

    if (duration > 0) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += duration;
    }
    while (!stop_requested) {
        bool pending = false;

        if (!siano_output_pump_busy(pump)) {
            size_t length;
            int rc = ts_pop(&device->ts, data, &length);

            rc = siano_stream_state_stream_result(&device->state, rc);
            if (rc < 0 && rc != -EAGAIN)
                return rc;
            if (rc == 0)
                siano_output_pump_feed(pump, data, length);
        }
        if (siano_output_pump_busy(pump)) {
            int rc = siano_output_pump_step(pump);

            if (rc < 0) {
                if (siano_report_output_error(rc))
                    fprintf(stderr, "TS output: %s\n", strerror(-rc));
                return rc;
            }
            if (rc == SIANO_OUTPUT_PENDING) {
                int error = stream_pending_check(device);

                if (error < 0)
                    return error;
                pending = true;
            }
        }
        if (duration > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
                break;
        }
        if (pending)
            siano_output_wait(&pump->output, OUTPUT_WAIT_MS);
    }
    return 0;
}

/* A successful tune drops queued old chunks and unstarted bytes, but lets a
 * partially emitted packet finish before output from the new channel. A failed
 * tune retains the original queue and pump state. */
static bool stream_retune_finish(struct siano_device *device,
                                 struct siano_output_pump *pump, int tune_result)
{
    bool succeeded = ts_queue_retune_finish(&device->ts, &pump->hold_len,
                                            tune_result);

    if (succeeded)
        siano_output_pump_retune(pump);
    return succeeded;
}

static int handle_control_line(struct siano_device *device, const struct options *options,
                               bool *pid_filters_added, const char *line, bool *quit,
                               struct siano_output_pump *pump)
{
    uint32_t value = 0;
    uint32_t frequency;
    enum control_command_type command = parse_control_line(line, &value);
    int rc;

    if (command == CONTROL_EMPTY)
        return 0;
    if (command == CONTROL_INVALID) {
        fprintf(stderr, "control: invalid\n");
        return 0;
    }
    if (command == CONTROL_QUIT) {
        *quit = true;
        return 0;
    }
    if (command == CONTROL_CHANNEL) {
        rc = sms_channel_frequency(value, &frequency);
        if (rc < 0) {
            fprintf(stderr, "control: invalid\n");
            return 0;
        }
    } else {
        frequency = value;
    }
    ts_queue_retune_begin(&device->ts);
    rc = tune(device, frequency);
    (void)stream_retune_finish(device, pump, rc);
    if (rc < 0) {
        if (!stop_requested)
            fprintf(stderr, "control: tune failed: %s\n", strerror(-rc));
        return 0;
    }
    if (!*pid_filters_added) {
        rc = configure_pid_filters(device, options->pids, options->pid_count);
        if (rc < 0)
            return rc;
        *pid_filters_added = true;
    }
    fprintf(stderr, "tuned %u\n", frequency);
    return 0;
}

static int stream_ts_control(struct siano_device *device, const struct options *options,
                             bool *pid_filters_added, struct siano_output_pump *pump)
{
    struct siano_control_input control;
    uint8_t data[USB_TRANSFER_SIZE];
    char line[128];
    size_t line_len = 0;
    bool line_overflow = false;
    bool quit = false;
    int rc;

#ifdef _WIN32
    rc = siano_control_input_init(&control, GetStdHandle(STD_INPUT_HANDLE));
#else
    rc = siano_control_input_init(&control, STDIN_FILENO);
#endif
    if (rc < 0)
        return rc;

    while (!stop_requested && !quit) {
        bool pending = false;

        if (!siano_output_pump_busy(pump)) {
            size_t length;
            rc = ts_pop(&device->ts, data, &length);

            rc = siano_stream_state_stream_result(&device->state, rc);
            if (rc < 0 && rc != -EAGAIN)
                return rc;
            if (rc == 0)
                siano_output_pump_feed(pump, data, length);
        }
        if (siano_output_pump_busy(pump)) {
            rc = siano_output_pump_step(pump);
            if (rc < 0) {
                if (siano_report_output_error(rc))
                    fprintf(stderr, "TS output: %s\n", strerror(-rc));
                return rc;
            }
            if (rc == SIANO_OUTPUT_PENDING) {
                int error = stream_pending_check(device);

                if (error < 0)
                    return error;
                pending = true;
            }
        }
        rc = siano_control_input_ready(&control);
        if (rc < 0)
            return rc;
        if (rc > 0) {
            uint8_t input[256];
            int64_t count = siano_control_input_read(&control, input, sizeof(input));
            if (count == -EAGAIN)
                continue;
            if (count < 0)
                return (int)count;
            if (count == 0)
                break;
            pending = false;
            for (ssize_t i = 0; i < count && !quit; i++) {
                if (input[i] == '\n') {
                    if (line_overflow) {
                        fprintf(stderr, "control: invalid\n");
                    } else {
                        line[line_len] = '\0';
                        rc = handle_control_line(device, options, pid_filters_added,
                                                 line, &quit, pump);
                        if (rc < 0)
                            return rc;
                    }
                    line_len = 0;
                    line_overflow = false;
                } else if (line_len + 1 < sizeof(line)) {
                    line[line_len++] = (char)input[i];
                } else {
                    line_overflow = true;
                }
            }
        }
        if (pending && !quit)
            siano_output_wait(&pump->output, OUTPUT_WAIT_MS);
    }
    return 0;
}

#ifdef _WIN32
static BOOL WINAPI on_console(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        stop_requested = 1;
        return TRUE;
    }
    return FALSE;
}
#endif

int main(int argc, char **argv)
{
    struct options options;
    struct siano_device *device;
    struct siano_output_pump pump;
    libusb_context *usb = NULL;
    char *firmware_path = NULL;
    uint32_t frequency;
    int output_fd = STDOUT_FILENO;
    bool close_output = false;
    bool pid_filters_added = false;
    bool pump_initialized = false;
    int state_error;
    int rc;
    uint64_t drops = 0;

#ifdef _WIN32
    SetConsoleCtrlHandler(on_console, TRUE);
    _setmode(fileno(stdout), O_BINARY);
#else
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, NULL, _IOLBF, 0);
#endif
    rc = parse_options(argc, argv, &options);
    if (rc < 0) {
        usage(stderr, argv[0]);
        return 2;
    }
    if (options.have_channel) {
        rc = sms_channel_frequency(options.channel, &frequency);
        if (rc < 0) {
            usage(stderr, argv[0]);
            return 2;
        }
        fprintf(stderr, "channel %u -> %u Hz\n", options.channel, (unsigned)frequency);
    } else if (options.have_frequency) {
        frequency = options.frequency;
    } else {
        frequency = 0;
    }
    if (!options.list) {
        rc = resolve_firmware(options.firmware, &firmware_path);
        if (rc < 0)
            return siano_exit_code(rc);
    }
#ifdef SIANO_HAVE_WRAP_SYS_DEVICE
    if (options.device_fd >= 0) {
        /* Skip udev/usbfs scans so Android/Termux can pass a Host API fd. */
        rc = libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
        if (rc < 0)
            fprintf(stderr, "LIBUSB_OPTION_NO_DEVICE_DISCOVERY: %s\n",
                    libusb_error_name(rc));
    }
#endif
    rc = libusb_init(&usb);
    if (rc < 0) {
        fprintf(stderr, "libusb_init: %s\n", libusb_error_name(rc));
        free(firmware_path);
        return 1;
    }
    if (options.list) {
        rc = list_devices(usb);
        libusb_exit(usb);
        return siano_exit_code(rc);
    }
    device = calloc(1, sizeof(*device));
    if (!device) {
        free(firmware_path);
        libusb_exit(usb);
        return 70;
    }
    if (init_device_state(device, usb, options.verbose) < 0) {
        fprintf(stderr, "failed to initialize device state\n");
        free(device);
        free(firmware_path);
        libusb_exit(usb);
        return 70;
    }
    ts_queue_set_fail_on_drop(&device->ts, options.fail_on_drop);
    device->detach_kernel_driver = options.detach_kernel_driver;
    try_lock_pages();
    try_realtime(pthread_self(), WRITER_THREAD_PRIORITY, "writer");
    if (options.device_fd >= 0)
        rc = open_from_fd(device, options.device_fd);
    else
        rc = open_rio(device, &options.device_selector, options.device_spec);
    if (rc < 0)
        goto out;
    rc = start_streaming(device);
    if (rc < 0)
        goto out;
    rc = set_device_mode(device, firmware_path);
    if (rc < 0)
        goto out;
    /* smsdvb tunes first (set_frontend), then ADD_PID on start_feed. */
    if (options.have_channel || options.have_frequency) {
        rc = tune(device, frequency);
        if (rc < 0 && !options.control)
            goto out;
        if (rc == 0) {
            fprintf(stderr, "ISDB-T lock acquired\n");
            if (options.control)
                fprintf(stderr, "tuned %u\n", (unsigned)frequency);
            rc = configure_pid_filters(device, options.pids, options.pid_count);
            if (rc < 0)
                goto out;
            pid_filters_added = true;
        } else {
            if (!stop_requested)
                fprintf(stderr, "control: tune failed: %s\n", strerror(-rc));
        }
    }
    rc = 0;
    if (options.output) {
        output_fd = open(options.output, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
        if (output_fd < 0) {
            fprintf(stderr, "output '%s': %s\n", options.output, strerror(errno));
            rc = -errno;
            goto out;
        }
        close_output = true;
    }
    rc = siano_output_pump_init(&pump, output_fd);
    if (rc < 0) {
        fprintf(stderr, "output: %s\n", strerror(-rc));
        goto out;
    }
    pump_initialized = true;
    rc = options.control ? stream_ts_control(device, &options, &pid_filters_added, &pump) :
                           stream_ts(device, &pump, options.duration);
    siano_output_pump_destroy(&pump);
    pump_initialized = false;
out:
    if (pump_initialized)
        siano_output_pump_destroy(&pump);
    state_error = siano_stream_state_error(&device->state);
    close_device(device, &drops);
    if (drops != 0)
        fprintf(stderr, "TS queue dropped %llu chunk(s)\n",
                (unsigned long long)drops);
    if (close_output)
        close(output_fd);
    free(device);
    free(firmware_path);
    libusb_exit(usb);
    if (rc < 0 && !stop_requested && state_error == 0)
        fprintf(stderr, "siano-ts: %s\n", strerror(-rc));
    if (drops != 0)
        return 8;
    if (stop_requested)
        return 0;
    if (state_error == LIBUSB_ERROR_NO_DEVICE)
        return 7;
    return siano_exit_code(rc);
}
