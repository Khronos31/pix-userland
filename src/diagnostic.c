#include "diagnostic.h"
#include "os.h"
#include "pixy.h"
#include "version.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#endif
/* Private, same-build/ABI checkpoints. Never consume files from an untrusted source. */
typedef struct {
    char magic[16], version[32];
    uint64_t decoder_size, check_size, generation, start_ms;
} header;
typedef struct {
    uint64_t received_ms, processed_ms;
    uint32_t length, queued;
} frame;
struct diagnostic {
    char directory[3500];
    FILE *raw, *events;
    uint64_t generation, segment_bytes, post_bytes, log_bytes, last_sample;
    uint64_t resync, waiting, cc, tei, crc, invalid, scrambled;
    int triggered, stopped, failed;
};
static FILE *private_file(const char *path, int truncate) {
    int fd = open(path, O_WRONLY | O_CREAT | O_BINARY | (truncate ? O_TRUNC : O_EXCL), 0600);
    if (fd < 0)
        return NULL;
    FILE *f = fdopen(fd, "wb");
    if (!f)
        close(fd);
    return f;
}
static void event(diagnostic *g, const char *format, ...) {
    if (!g || !g->events || g->log_bytes >= 8u * 1024u * 1024u)
        return;
    int n = fprintf(g->events, "unix_s=%lld mono_ms=%llu ", (long long)time(NULL),
                    (unsigned long long)now_ms());
    va_list ap;
    va_start(ap, format);
    int m = vfprintf(g->events, format, ap);
    va_end(ap);
    if (n < 0 || m < 0 || fflush(g->events))
        g->failed = 1;
    else
        g->log_bytes += (unsigned)(n + m);
}
void diag_issue(void *u, const char *kind, unsigned pid, uint64_t packet, unsigned expected,
                unsigned actual) {
    event(u, "issue=%s pid=%u packet=%llu expected=%u actual=%u\n", kind, pid,
          (unsigned long long)packet, expected, actual);
}
diagnostic *diag_open(const char *dir) {
    diagnostic *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    if (strlen(dir) >= sizeof(g->directory)) {
        free(g);
        return NULL;
    }
#ifdef _WIN32
    int r = _mkdir(dir);
#else
    int r = mkdir(dir, 0700);
#endif
    if (r) {
        free(g);
        return NULL;
    }
    strcpy(g->directory, dir);
    char path[4096];
    snprintf(path, sizeof(path), "%s/events.log", dir);
    g->events = private_file(path, 0);
    if (!g->events) {
        free(g);
        return NULL;
    }
    event(g, "start version=%s slots=%u segment_bytes=%u post_bytes=%u\n", VERSION, DIAG_SLOTS,
          DIAG_SEGMENT, DIAG_POST);
    return g;
}
static int segment(diagnostic *g, const ts_decoder *d, const ts_check *c) {
    if (g->raw) {
        int r = fclose(g->raw);
        g->raw = NULL;
        if (r)
            return -1;
    }
    char path[4096];
    unsigned slot = (unsigned)(g->generation % DIAG_SLOTS);
    if (g->triggered)
        snprintf(path, sizeof(path), "%s/post.pixdiag", g->directory);
    else
        snprintf(path, sizeof(path), "%s/ring-%02u.pixdiag", g->directory, slot);
    g->raw = private_file(path, !g->triggered && g->generation >= DIAG_SLOTS);
    if (!g->raw)
        return -1;
    header h = {0};
    memcpy(h.magic, "PIXDIAG1", 8);
    snprintf(h.version, sizeof(h.version), "%s", VERSION);
    h.decoder_size = sizeof(*d);
    h.check_size = sizeof(*c);
    h.generation = g->generation++;
    h.start_ms = now_ms();
    ts_decoder copy = *d;
    copy.error = NULL;
    copy.sink = NULL;
    copy.user = NULL;
    copy.check = NULL;
    ts_check *check = malloc(sizeof(*check));
    if (!check)
        return -1;
    *check = *c;
    check->issue = NULL;
    check->issue_user = NULL;
    int bad = fwrite(&h, sizeof(h), 1, g->raw) != 1 ||
              fwrite(&copy, sizeof(copy), 1, g->raw) != 1 ||
              fwrite(check, sizeof(*check), 1, g->raw) != 1;
    secure_clear(&copy, sizeof(copy));
    secure_clear(check, sizeof(*check));
    free(check);
    g->segment_bytes = 0;
    event(g, "segment generation=%llu slot=%u post=%d\n", (unsigned long long)h.generation, slot,
          g->triggered);
    return bad ? -1 : 0;
}
int diag_before(diagnostic *g, const ts_decoder *d, const ts_check *c, const uint8_t *p, size_t n,
                uint64_t received, unsigned queued, unsigned high_water) {
    if (!g)
        return 0;
    uint64_t now = now_ms();
    if (now - g->last_sample >= 1000) {
        event(g,
              "sample queued=%u high_water=%u lag_ms=%llu packets=%llu key_tables=%llu "
              "previous=%llu\n",
              queued, high_water, (unsigned long long)(now - received),
              (unsigned long long)c->packets, (unsigned long long)d->key_tables,
              (unsigned long long)d->previous_key_uses);
        g->last_sample = now;
        if (g->raw && fflush(g->raw))
            g->failed = 1;
    }
    if (g->failed)
        return -1;
    if (g->stopped)
        return 0;
    if (!g->raw || (!g->triggered && g->segment_bytes >= DIAG_SEGMENT))
        if (segment(g, d, c)) {
            g->failed = 1;
            return -1;
        }
    frame f = {received, now, (uint32_t)n, queued};
    if (n > 32768 || fwrite(&f, sizeof(f), 1, g->raw) != 1 || fwrite(p, 1, n, g->raw) != n) {
        g->failed = 1;
        return -1;
    }
    g->segment_bytes += n;
    if (g->triggered)
        g->post_bytes += n;
    return 0;
}
int diag_after(diagnostic *g, const ts_decoder *d, const ts_check *c, int failed) {
    if (!g)
        return 0;
    int changed = d->resync != g->resync || d->waiting != g->waiting || c->continuity != g->cc ||
                  c->tei != g->tei || c->crc_errors != g->crc || c->invalid != g->invalid ||
                  c->scrambled != g->scrambled;
    if (changed || failed) {
        event(g,
              "anomaly resync=%llu waiting=%llu cc=%llu tei=%llu crc=%llu invalid=%llu "
              "scrambled=%llu failed=%d\n",
              (unsigned long long)d->resync, (unsigned long long)d->waiting,
              (unsigned long long)c->continuity, (unsigned long long)c->tei,
              (unsigned long long)c->crc_errors, (unsigned long long)c->invalid,
              (unsigned long long)c->scrambled, failed);
        if (!g->triggered) {
            g->triggered = 1;
            event(g, "trigger first-anomaly; preserving ring; collecting post=%u bytes\n",
                  DIAG_POST);
            if (segment(g, d, c))
                g->failed = 1;
        }
    }
    g->resync = d->resync;
    g->waiting = d->waiting;
    g->cc = c->continuity;
    g->tei = c->tei;
    g->crc = c->crc_errors;
    g->invalid = c->invalid;
    g->scrambled = c->scrambled;
    if (g->triggered && !g->stopped && g->post_bytes >= DIAG_POST) {
        g->stopped = 1;
        if (g->raw && fclose(g->raw))
            g->failed = 1;
        g->raw = NULL;
        event(g, "saved first incident; raw capture stopped at bound\n");
    }
    return g->failed ? -1 : 0;
}
int diag_close(diagnostic *g) {
    if (!g)
        return 0;
    event(g, "close triggered=%d stopped=%d failed=%d\n", g->triggered, g->stopped, g->failed);
    if (g->raw && fclose(g->raw))
        g->failed = 1;
    if (g->events && fclose(g->events))
        g->failed = 1;
    int r = g->failed ? -1 : 0;
    free(g);
    return r;
}
int diag_replay(const char *path, ts_decoder *d, ts_check *c, ts_sink sink, void *user) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    header h;
    int rc = -1;
    if (fread(&h, sizeof(h), 1, f) != 1 || memcmp(h.magic, "PIXDIAG1", 8) ||
        strncmp(h.version, VERSION, sizeof(h.version)) || h.decoder_size != sizeof(*d) ||
        h.check_size != sizeof(*c) || fread(d, sizeof(*d), 1, f) != 1 ||
        fread(c, sizeof(*c), 1, f) != 1)
        goto done;
    d->sink = sink;
    d->user = user;
    d->check = c;
    d->error = NULL;
    c->issue = NULL;
    c->issue_user = NULL;
    if (d->used >= 564 || d->key_count > KEY_LIMIT || d->previous_key_count > KEY_LIMIT ||
        d->next_fragment > FRAGMENT_LIMIT || d->fragment >= FRAGMENT_LIMIT || c->psi_count > 65 ||
        d->ctr.used > 16 || d->ctr.counter_bytes > 16 || (d->active && !d->ctr.counter_bytes) ||
        d->failed)
        goto done;
    for (unsigned i = 0; i < c->psi_count; i++)
        if (c->psi[i].used > 4096)
            goto done;
    fprintf(stderr,
            "diagnostic replay generation=%llu checkpoint_packets=%llu resync=%llu waiting=%llu "
            "cc=%llu\n",
            (unsigned long long)h.generation, (unsigned long long)c->packets,
            (unsigned long long)d->resync, (unsigned long long)d->waiting,
            (unsigned long long)c->continuity);
    frame block;
    uint8_t data[32768];
    size_t n;
    rc = 0;
    while ((n = fread(&block, 1, sizeof(block), f)) != 0) {
        if (n != sizeof(block) || block.length > sizeof(data) || !block.length ||
            fread(data, 1, block.length, f) != block.length ||
            ts_decoder_feed(d, data, block.length)) {
            rc = -1;
            break;
        }
    }
    if (ferror(f))
        rc = -1;
done:
    fclose(f);
    d->sink = sink;
    d->user = user;
    d->check = c;
    d->error = NULL;
    c->issue = NULL;
    c->issue_user = NULL;
    return rc;
}
