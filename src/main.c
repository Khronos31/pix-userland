#include "os.h"
#include "pixy.h"
#include "transport.h"
#include "usb_backend.h"
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include "version.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define QUEUE_SLOTS 128
static volatile sig_atomic_t interrupted;
static void on_signal(int s) {
    (void)s;
    interrupted = 1;
}
typedef struct {
    uint8_t bytes[QUEUE_SLOTS][32768];
    size_t lengths[QUEUE_SLOTS];
    unsigned head, count;
    int done, error, output;
    uint64_t write_started;
    uint64_t usb_bytes;
    os_mutex mutex;
    os_cond cond;
    ts_decoder decoder;
    ts_check check;
} stream;
static int write_ts(void *u, const uint8_t *p, size_t n) {
    stream *s = u;
    uint64_t deadline = now_ms() + 3000;
    while (n) {
#ifdef _WIN32
        os_mutex_lock(&s->mutex);
        s->write_started = now_ms();
        os_mutex_unlock(&s->mutex);
#endif
        ssize_t r = write(s->output, p, n);
#ifdef _WIN32
        os_mutex_lock(&s->mutex);
        s->write_started = 0;
        os_mutex_unlock(&s->mutex);
#endif
        if (r > 0) {
            p += r;
            n -= (size_t)r;
            deadline = now_ms() + 3000;
            continue;
        }
#ifndef _WIN32
        if (r < 0 && errno == EINTR)
            continue;
#endif
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (now_ms() >= deadline) {
                fprintf(stderr, "TS output stalled for 3 seconds\n");
                return -1;
            }
#ifdef _WIN32
            sleep_ms(50);
#else
            struct pollfd fd = {s->output, POLLOUT, 0};
            if (poll(&fd, 1, 50) < 0 && errno != EINTR)
                return -1;
#endif
            continue;
        }
        fprintf(stderr, "TS write: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}
static int enqueue(void *u, const uint8_t *p, size_t n) {
    stream *s = u;
    os_mutex_lock(&s->mutex);
    if (n > 32768 || s->count == QUEUE_SLOTS || s->error) {
        s->error = 1;
        os_cond_signal(&s->cond);
        os_mutex_unlock(&s->mutex);
        return -1;
    }
    unsigned at = (s->head + s->count) % QUEUE_SLOTS;
    memcpy(s->bytes[at], p, n);
    s->lengths[at] = n;
    s->count++;
    s->usb_bytes += n;
    os_cond_signal(&s->cond);
    os_mutex_unlock(&s->mutex);
    return 0;
}
static void *worker(void *u) {
    stream *s = u;
    uint8_t data[32768];
    for (;;) {
        os_mutex_lock(&s->mutex);
        while (!s->count && !s->done && !s->error)
            os_cond_wait(&s->cond, &s->mutex);
        if (s->error || (!s->count && s->done)) {
            os_mutex_unlock(&s->mutex);
            break;
        }
        size_t n = s->lengths[s->head];
        memcpy(data, s->bytes[s->head], n);
        s->head = (s->head + 1) % QUEUE_SLOTS;
        s->count--;
        os_mutex_unlock(&s->mutex);
        if (ts_decoder_feed(&s->decoder, data, n)) {
            os_mutex_lock(&s->mutex);
            s->error = 1;
            os_mutex_unlock(&s->mutex);
            break;
        }
    }
    return NULL;
}
static int number(const char *p, unsigned long max, unsigned long *out) {
    char *end;
    errno = 0;
    if (!p || !*p || *p == '-' || *p == '+')
        return -1;
    unsigned long v = strtoul(p, &end, 0);
    if (errno || *end || v > max)
        return -1;
    *out = v;
    return 0;
}
static void usage(FILE *f) {
    fprintf(
        f,
        "pix-ts " VERSION " — PIX-DT181-PU0 libusb TS receiver\n"
        "Usage: pix-ts --system terrestrial|bs|cs --channel N [options]\n"
        "  -c, --channel N       physical channel (terrestrial 13..52, BS odd 1..23, CS even "
        "2..24)\n"
        "  -s, --system TYPE     terrestrial (default), bs, cs\n"
        "      --tsid N          satellite TSID, decimal or 0xhex; default first detected\n"
        "  -t, --time SECONDS    receive for N seconds; default until SIGINT/SIGTERM\n"
        "  -o, --output PATH     new file (never overwrite); default stdout, '-' also stdout\n"
        "  -a, --assets DIR      fpga.bin, bootstrap.bin, validation.bin (default ./assets)\n"
        "  -d, --device INDEX    zero-based matching USB device index (default 0)\n"
        "      --fd FD          borrowed USB fd from Android/Termux (or final integer argument)\n"
        "      --lna on|off     internal receive amplifier; default terrestrial off, BS/CS on\n"
        "      --detach-kernel-driver   explicitly detach a bound interface\n"
        "      --list           list supported USB devices without opening\n"
        "      --inspect FILE   validate an existing 188-byte TS (no USB/assets required)\n"
        "      --decode RAW --context FILE  offline USB-transfer decode; use -o for output\n"
        "      --version        show version\n"
        "  -h, --help           show help\n"
        "TS goes to stdout; all diagnostics and final JSON statistics go to stderr.\n"
        "This model does not provide LNB power. LNA is a separate internal amplifier.\n");
}
static uint8_t *load_file(const char *dir, const char *name, size_t expected) {
    char path[4096];
    if (snprintf(path, sizeof(path), "%s%s%s",
                 (name[0] == '/' || (strlen(name) > 2 && name[1] == ':')) ? "" : dir,
                 (name[0] == '/' || (strlen(name) > 2 && name[1] == ':')) ? "" : "/",
                 name) >= (int)sizeof(path))
        return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return NULL;
    }
    uint8_t *p = malloc(expected);
    if (!p) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(p, 1, expected, f);
    int extra = fgetc(f), bad = ferror(f);
    fclose(f);
    if (n != expected || extra != EOF || bad) {
        fprintf(stderr, "%s: expected %zu bytes\n", path, expected);
        free(p);
        return NULL;
    }
    return p;
}
static int check_bad(const ts_check *c) {
    return c->tei || c->scrambled || c->continuity || c->invalid || c->crc_errors || !c->pat ||
           !c->pmt;
}
static void report(const stream *s) {
    const ts_decoder *d = &s->decoder;
    const ts_check *c = &s->check;
    fprintf(stderr,
            "{\"packets\":%llu,\"usb_bytes\":%llu,\"markers\":%llu,\"iv_updates\":%llu,\"resync_"
            "bytes\":%llu,\"waiting_for_iv\":%llu,\"trailing_bytes\":%zu,\"tei\":%llu,"
            "\"scrambled\":%llu,\"continuity_errors\":%llu,\"invalid_packets\":%llu,\"psi_crc_"
            "errors\":%llu,\"pat\":%llu,\"pmt\":%llu,\"stream_error\":%d}\n",
            (unsigned long long)c->packets, (unsigned long long)s->usb_bytes,
            (unsigned long long)d->markers, (unsigned long long)d->iv_updates,
            (unsigned long long)d->resync, (unsigned long long)d->waiting, d->used,
            (unsigned long long)c->tei, (unsigned long long)c->scrambled,
            (unsigned long long)c->continuity, (unsigned long long)c->invalid,
            (unsigned long long)c->crc_errors, (unsigned long long)c->pat,
            (unsigned long long)c->pmt, s->error);
}
static int offline(const char *path, const char *context, stream *s) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 3;
    }
    uint8_t block[32768];
    size_t n;
    if (context) {
        uint8_t *ctx = load_file(".", context, 32);
        if (!ctx) {
            fclose(f);
            return 3;
        }
        ts_decoder_init(&s->decoder, ctx, write_ts, s, &s->check);
        secure_clear(ctx, 32);
        free(ctx);
        while ((n = fread(block, 1, sizeof(block), f)) > 0) {
            s->usb_bytes += n;
            if (ts_decoder_feed(&s->decoder, block, n)) {
                s->error = 1;
                break;
            }
        }
    } else {
        uint8_t p[188];
        while ((n = fread(p, 1, 188, f)) == 188)
            ts_check_packet(&s->check, p);
        s->decoder.used = n;
    }
    if (ferror(f))
        s->error = 1;
    fclose(f);
    report(s);
    return s->error || check_bad(&s->check) || s->decoder.resync || s->decoder.waiting ||
                   (!context && s->decoder.used)
               ? 8
               : 0;
}
int main(int argc, char **argv) {
    pixy_system system = PIXY_TERRESTRIAL;
    unsigned channel = 0, index = 0, seconds = 0;
    int fd = -1, tsid = -1, lna = -1, detach = 0, list = 0, rc = 2;
    const char *assets = "assets", *output = "-", *inspect = NULL, *raw = NULL, *context = NULL;
    static const struct option opts[] = {{"channel", 1, 0, 'c'},
                                         {"system", 1, 0, 's'},
                                         {"time", 1, 0, 't'},
                                         {"output", 1, 0, 'o'},
                                         {"assets", 1, 0, 'a'},
                                         {"device", 1, 0, 'd'},
                                         {"tsid", 1, 0, 256},
                                         {"fd", 1, 0, 257},
                                         {"lna", 1, 0, 258},
                                         {"detach-kernel-driver", 0, 0, 259},
                                         {"list", 0, 0, 260},
                                         {"inspect", 1, 0, 261},
                                         {"decode", 1, 0, 262},
                                         {"context", 1, 0, 263},
                                         {"version", 0, 0, 264},
                                         {"help", 0, 0, 'h'},
                                         {0, 0, 0, 0}};
    int c;
    unsigned long v;
    while ((c = getopt_long(argc, argv, "c:s:t:o:a:d:h", opts, NULL)) != -1) {
        switch (c) {
        case 'c':
            if (number(optarg, 52, &v) || !v)
                goto bad;
            channel = (unsigned)v;
            break;
        case 's':
            if (!strcmp(optarg, "terrestrial"))
                system = PIXY_TERRESTRIAL;
            else if (!strcmp(optarg, "bs"))
                system = PIXY_BS;
            else if (!strcmp(optarg, "cs"))
                system = PIXY_CS;
            else
                goto bad;
            break;
        case 't':
            if (number(optarg, 86400 * 365u, &v) || !v)
                goto bad;
            seconds = (unsigned)v;
            break;
        case 'o':
            output = optarg;
            break;
        case 'a':
            assets = optarg;
            break;
        case 'd':
            if (number(optarg, UINT_MAX, &v))
                goto bad;
            index = (unsigned)v;
            break;
        case 256:
            if (number(optarg, 65535, &v))
                goto bad;
            tsid = (int)v;
            break;
        case 257:
            if (number(optarg, INT_MAX, &v))
                goto bad;
            fd = (int)v;
            break;
        case 258:
            if (!strcmp(optarg, "on"))
                lna = 1;
            else if (!strcmp(optarg, "off"))
                lna = 0;
            else
                goto bad;
            break;
        case 259:
            detach = 1;
            break;
        case 260:
            list = 1;
            break;
        case 261:
            inspect = optarg;
            break;
        case 262:
            raw = optarg;
            break;
        case 263:
            context = optarg;
            break;
        case 264:
            puts("pix-ts " VERSION);
            return 0;
        case 'h':
            usage(stdout);
            return 0;
        default:
            goto bad;
        }
    }
    if (optind < argc) {
        if (optind + 1 != argc || fd >= 0 || number(argv[optind], INT_MAX, &v))
            goto bad;
        fd = (int)v;
    }
#ifdef _WIN32
    if (fd >= 0) {
        fprintf(stderr, "--fd is only available on Linux/Android\n");
        goto bad;
    }
#endif
    if (fd >= 0 && (list || index))
        goto bad;
    if (list) {
        if (channel || inspect || raw || context)
            goto bad;
        return usb_list() ? 4 : 0;
    }
    if ((inspect && raw) || (raw && !context) || (context && !raw) ||
        ((inspect || raw) && (channel || fd >= 0)))
        goto bad;
    uint32_t hz = 0;
    if (!inspect && !raw) {
        if (!channel || pixy_frequency(system, channel, &hz))
            goto bad;
        if (system == PIXY_TERRESTRIAL && tsid >= 0)
            goto bad;
    }
#ifdef _WIN32
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
#endif
    stream *s = calloc(1, sizeof(*s));
    if (!s)
        return 3;
    s->output = STDOUT_FILENO;
    ts_check_init(&s->check);
    int flags = -1;
    if (!inspect) {
        if (strcmp(output, "-")) {
            s->output = open(output, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0666);
            if (s->output < 0) {
                perror(output);
                free(s);
                return 3;
            }
        } else if (isatty(STDOUT_FILENO)) {
            fprintf(stderr, "Refusing binary TS output to a terminal; use -o FILE or a pipe\n");
            free(s);
            return 2;
        }
        flags = os_output_prepare(s->output);
        if (flags < 0) {
            perror("output flags");
            rc = 3;
            goto finish;
        }
    }
    if (inspect || raw) {
        if (os_mutex_init(&s->mutex, NULL)) {
            rc = 3;
            goto finish;
        }
        rc = offline(inspect ? inspect : raw, context, s);
        os_mutex_destroy(&s->mutex);
        goto finish;
    }
    uint8_t *fw = load_file(assets, "fpga.bin", 699673),
            *bootstrap = load_file(assets, "bootstrap.bin", 16),
            *validation = load_file(assets, "validation.bin", 64);
    if (!fw || !bootstrap || !validation) {
        free(fw);
        free(bootstrap);
        free(validation);
        rc = 3;
        goto finish;
    }
    usb_backend *usb = NULL;
    pixy device;
    int opened = 0, queue = 0, started_thread = 0;
    os_thread thread;
    rc = 4;
    if (usb_open(&usb, index, fd, detach))
        goto cleanup;
    opened = 1;
    pixy_init(&device, usb_io(usb));
    fprintf(stderr, "Initializing PIX-DT181-PU0...\n");
    if (pixy_power(&device, fw, 699673) || pixy_session(&device, bootstrap, validation) ||
        pixy_tuner_init(&device))
        goto cleanup;
    if (interrupted) {
        rc = 130;
        goto cleanup;
    }
    if (pixy_tune(&device, system, hz, lna < 0 ? system != PIXY_TERRESTRIAL : lna, &tsid))
        goto cleanup;
    uint8_t status[64];
    size_t statusn = 64;
    if (pixy_status(&device, status, &statusn))
        goto cleanup;
    fprintf(stderr, "locked system=%s channel=%u frequency=%u tsid=%d bcas_inserted=%d\n",
            system == PIXY_TERRESTRIAL ? "terrestrial"
            : system == PIXY_BS        ? "bs"
                                       : "cs",
            channel, hz, tsid, statusn > 3 ? !!status[3] : -1);
    if (os_mutex_init(&s->mutex, NULL)) {
        rc = 3;
        goto cleanup;
    }
    if (os_cond_init(&s->cond, NULL)) {
        os_mutex_destroy(&s->mutex);
        rc = 3;
        goto cleanup;
    }
    queue = 1;
    if (pixy_fifo_clear(&device) || usb_stream_start(usb, enqueue, s))
        goto cleanup;
    uint8_t stream_context[32];
    if (pixy_start(&device, stream_context))
        goto cleanup;
    ts_decoder_init(&s->decoder, stream_context, write_ts, s, &s->check);
    secure_clear(stream_context, 32);
    if (os_thread_create(&thread, NULL, worker, s)) {
        rc = 3;
        goto cleanup;
    }
    started_thread = 1;
    uint64_t start = now_ms(), next = start + 1000;
    rc = 0;
    while (!interrupted && (!seconds || now_ms() - start < (uint64_t)seconds * 1000)) {
        if (usb_poll(usb, 50)) {
            rc = 8;
            break;
        }
        os_mutex_lock(&s->mutex);
        int err = s->error;
#ifdef _WIN32
        if (s->write_started && now_ms() - s->write_started > 3000) {
            s->error = 1;
            err = 1;
        }
#endif
        os_mutex_unlock(&s->mutex);
        if (err) {
            rc = 8;
            break;
        }
        if (now_ms() >= next) {
            if (pixy_certify(&device)) {
                rc = 4;
                break;
            }
            next = now_ms() + 1000;
        }
    }
cleanup:
    if (opened) {
        if (rc)
            fprintf(stderr, "%s\n", device.error[0] ? device.error : "Capture did not complete");
        if (device.streaming && pixy_stop(&device) && !rc)
            rc = 4;
        if (usb_stream_stop(usb) && !rc)
            rc = 8;
        if (started_thread) {
            os_mutex_lock(&s->mutex);
            s->done = 1;
            os_cond_signal(&s->cond);
            os_mutex_unlock(&s->mutex);
            os_thread_join(thread, rc || interrupted);
            report(s);
            if (!rc &&
                (s->error || check_bad(&s->check) || s->decoder.resync || s->decoder.waiting))
                rc = 8;
        }
        if (pixy_shutdown(&device) && !rc)
            rc = 4;
        usb_close(usb);
        secure_clear(&device, sizeof(device));
    }
    if (queue) {
        os_cond_destroy(&s->cond);
        os_mutex_destroy(&s->mutex);
    }
    free(fw);
    secure_clear(bootstrap, 16);
    free(bootstrap);
    secure_clear(validation, 64);
    free(validation);
finish:
    if (flags >= 0 && s->output == STDOUT_FILENO)
        os_output_restore(s->output, flags);
    if (s->output != STDOUT_FILENO && close(s->output) && !rc)
        rc = 3;
    secure_clear(s, sizeof(*s));
    free(s);
    return rc;
bad:
    usage(stderr);
    return 2;
}
