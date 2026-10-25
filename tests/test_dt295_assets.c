#define _POSIX_C_SOURCE 200809L
#include "dt295.h"
#include "dt295_crypto.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HEADER_SIZE (8 + 6 * 36)
#define MATERIAL_SIZE 0x1280

int random_bytes(void *p, size_t n) {
    memset(p, 0, n);
    return 0;
}

static void put32(uint8_t *p, uint32_t x) {
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16);
    p[3] = (uint8_t)(x >> 24);
}

static void put16(uint8_t *p, uint16_t x) {
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)(x >> 8);
}

static uint8_t *make_bundle(size_t *length) {
    const uint32_t lens[6] = {1, 1, MATERIAL_SIZE, 8, 12, 16};
    size_t total = HEADER_SIZE;
    for (size_t i = 0; i < 6; i++)
        total += lens[i];
    uint8_t *bundle = calloc(1, total);
    assert(bundle);
    memcpy(bundle, "D295A1\0\0", 8);
    size_t offset = HEADER_SIZE;
    for (size_t i = 0; i < 6; i++) {
        uint8_t *row = bundle + 8 + i * 36;
        put32(row, lens[i]);
        if (i == 0)
            bundle[offset] = 0x11;
        if (i == 1)
            bundle[offset] = 0x22;
        if (i == 2)
            memset(bundle + offset, 0x33, lens[i]);
        if (i == 3) {
            put16(bundle + offset, 0x1000);
            bundle[offset + 3] = 1;
        }
        dt295_sha256(bundle + offset, lens[i], row + 4);
        offset += lens[i];
    }
    *length = total;
    return bundle;
}

static uint8_t *read_file(const char *path, size_t *length) {
    FILE *f = fopen(path, "rb");
    assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long n = ftell(f);
    assert(n > 0 && fseek(f, 0, SEEK_SET) == 0);
    uint8_t *data = malloc((size_t)n);
    assert(data && fread(data, 1, (size_t)n, f) == (size_t)n);
    assert(fgetc(f) == EOF && !ferror(f));
    fclose(f);
    *length = (size_t)n;
    return data;
}

static void expect_bytes_rejected(const uint8_t *data, size_t length) {
    dt295_assets *assets = NULL;
    char error[128] = {0};
    assert(dt295_assets_load_bytes(data, length, &assets, error, sizeof(error)) != 0);
    assert(!assets && error[0]);
}

int main(int argc, char **argv) {
    size_t length = 0;
    uint8_t *bundle = make_bundle(&length);
    uint8_t *original = malloc(length);
    assert(original);
    memcpy(original, bundle, length);

    dt295_assets *from_bytes = NULL, *from_path = NULL;
    char error[128] = {0};
    assert(dt295_assets_load_bytes(bundle, length, &from_bytes, error, sizeof(error)) == 0);
    assert(from_bytes && !error[0]);
    char path[] = "/tmp/pix-dt295-assets-test-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "wb");
    assert(f && fwrite(bundle, 1, length, f) == length && fclose(f) == 0);
    assert(dt295_assets_load(path, &from_path, error, sizeof(error)) == 0);
    assert(from_path && !error[0]);
    assert(unlink(path) == 0);
    assert(memcmp(bundle, original, length) == 0);
    free(bundle);
    dt295_assets_free(from_bytes);
    dt295_assets_free(from_path);
    free(original);

    size_t bad_length = 0;
    bundle = make_bundle(&bad_length);
    expect_bytes_rejected(bundle, bad_length - 1);
    bundle[HEADER_SIZE] ^= 1;
    expect_bytes_rejected(bundle, bad_length);
    free(bundle);

    bundle = make_bundle(&bad_length);
    size_t field_offset = HEADER_SIZE + 1U + 1U + MATERIAL_SIZE;
    put16(bundle + field_offset, 0x0ffe);
    dt295_sha256(bundle + field_offset, 8U, bundle + 8U + 3U * 36U + 4U);
    expect_bytes_rejected(bundle, bad_length);
    free(bundle);

    bundle = make_bundle(&bad_length);
    put32(bundle + 8U, UINT32_MAX);
    expect_bytes_rejected(bundle, bad_length);
    free(bundle);

    if (argc == 2) {
        uint8_t *real = read_file(argv[1], &length);
        dt295_assets *real_bytes = NULL, *real_path = NULL;
        assert(dt295_assets_load_bytes(real, length, &real_bytes, error, sizeof(error)) == 0);
        assert(dt295_assets_load(argv[1], &real_path, error, sizeof(error)) == 0);
        free(real);
        dt295_assets_free(real_bytes);
        dt295_assets_free(real_path);
        puts("DT295 private file/bytes asset validation: PASS");
    } else {
        assert(argc == 1);
    }
    return 0;
}
