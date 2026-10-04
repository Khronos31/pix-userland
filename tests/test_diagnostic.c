#include "diagnostic.h"
#include "pixy.h"
#include "vectors.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#define rmdir _rmdir
#endif
struct output {
    uint8_t bytes[1024];
    size_t count;
};
static int collect(void *u, const uint8_t *p, size_t n) {
    struct output *o = u;
    assert(o->count + n <= sizeof(o->bytes));
    memcpy(o->bytes + o->count, p, n);
    o->count += n;
    return 0;
}
static int discard(void *u, const uint8_t *p, size_t n) {
    (void)u;
    (void)p;
    (void)n;
    return 0;
}
static void feed(diagnostic *g, ts_decoder *d, ts_check *c, const uint8_t *p, size_t n) {
    assert(!diag_before(g, d, c, p, n, now_ms(), 1, 1));
    assert(!ts_decoder_feed(d, p, n));
    assert(!diag_after(g, d, c, 0));
}
int main(void) {
    char dir[128], path[200];
    snprintf(dir, sizeof(dir), "tests/diag-test-%llu", (unsigned long long)now_ms());
    diagnostic *g = diag_open(dir);
    assert(g);
    assert(!diag_open(dir));
    ts_decoder *d = calloc(1, sizeof(*d)), *replayed = calloc(1, sizeof(*d));
    ts_check *c = calloc(1, sizeof(*c)), *checked = calloc(1, sizeof(*c));
    assert(d && replayed && c && checked);
    struct output original = {0}, restored = {0};
    ts_check_init(c);
    ts_decoder_init(d, context, collect, &original, c);
    /* Checkpoint while the key table/marker is fragmented, not at packet boundary. */
    assert(!ts_decoder_feed(d, wire, 37));
    feed(g, d, c, wire + 37, sizeof(wire) - 37);
    assert(!diag_close(g));
    snprintf(path, sizeof(path), "%s/ring-00.pixdiag", dir);
    assert(!diag_replay(path, replayed, checked, collect, &restored));
    assert(original.count == restored.count &&
           !memcmp(original.bytes, restored.bytes, original.count));
    assert(c->packets == checked->packets && d->iv_updates == replayed->iv_updates);
    /* A truncated frame must fail instead of silently reporting a complete replay. */
    FILE *broken = fopen(path, "ab");
    assert(broken);
    assert(fputc(1, broken) != EOF);
    assert(!fclose(broken));
    memset(replayed, 0, sizeof(*replayed));
    memset(checked, 0, sizeof(*checked));
    assert(diag_replay(path, replayed, checked, discard, NULL) < 0);
    assert(!remove(path));
    snprintf(path, sizeof(path), "%s/events.log", dir);
    assert(!remove(path));
    assert(!rmdir(dir));
    g = diag_open(dir);
    assert(g);
    ts_check_init(c);
    ts_decoder_init(d, context, discard, NULL, c);
    uint8_t data[188 * 174];
    for (size_t i = 0; i < sizeof(data); i += 188) {
        memset(data + i, 255, 188);
        data[i] = 0x47;
        data[i + 1] = 0x1f;
        data[i + 2] = 0xff;
        data[i + 3] = 0x10;
    }
    for (unsigned i = 0; i < DIAG_SLOTS * 3; i++)
        feed(g, d, c, data, sizeof(data));
    /* Inject two junk bytes: preserve pre-error ring, replay the error exactly. */
    uint8_t bad[190];
    memcpy(bad + 2, data, 188);
    bad[0] = 0x12;
    bad[1] = 0x34;
    feed(g, d, c, bad, sizeof(bad));
    feed(g, d, c, data, sizeof(data));
    assert(d->resync == 2);
    for (unsigned i = 0; i < 20; i++)
        feed(g, d, c, data, sizeof(data));
    assert(!diag_close(g));
    unsigned found_error = 0;
    size_t total = 0;
    for (unsigned i = 0; i < DIAG_SLOTS; i++) {
        snprintf(path, sizeof(path), "%s/ring-%02u.pixdiag", dir, i);
        struct stat st;
        assert(!stat(path, &st));
        total += (size_t)st.st_size;
        memset(replayed, 0, sizeof(*replayed));
        memset(checked, 0, sizeof(*checked));
        assert(!diag_replay(path, replayed, checked, discard, NULL));
        found_error += replayed->resync > 0;
        assert(!remove(path));
    }
    assert(found_error);
    snprintf(path, sizeof(path), "%s/post.pixdiag", dir);
    struct stat st;
    assert(!stat(path, &st));
    total += (size_t)st.st_size;
    assert(!diag_replay(path, replayed, checked, discard, NULL));
    assert(replayed->resync == 2);
    assert(!remove(path));
    assert(total < (DIAG_SLOTS + 1) * (sizeof(*d) + sizeof(*c) + 1024) +
                       DIAG_SLOTS * (DIAG_SEGMENT + 32768) + DIAG_POST + 32768);
    snprintf(path, sizeof(path), "%s/events.log", dir);
    assert(!remove(path));
    assert(!rmdir(dir));
    free(d);
    free(replayed);
    free(c);
    free(checked);
    puts("Diagnostic checkpoint, ring rotation, trigger, replay and capacity tests passed");
    return 0;
}
