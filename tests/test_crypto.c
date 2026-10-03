#include "crypto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    uint8_t key[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    uint8_t plain[16] = {0,    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                         0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    uint8_t expected[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                            0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a},
            out[16];
    aes128 a;
    aes_init(&a, key);
    aes_encrypt(&a, plain, out);
    assert(!memcmp(out, expected, 16));
    aes_decrypt(&a, out, out);
    assert(!memcmp(out, plain, 16));
    uint8_t n[128], d[128], e[128] = {0}, x[128] = {0}, y[128], z[128];
    e[125] = 1;
    e[127] = 1;
    x[127] = 42;
    assert(!rsa_generate(n, d));
    assert(!rsa_apply(x, e, n, y));
    assert(!rsa_apply(y, d, n, z));
    assert(!memcmp(x, z, 128));
    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        assert(f);
        assert(fwrite(n, 1, 128, f) == 128);
        assert(fwrite(d, 1, 128, f) == 128);
        fclose(f);
    }
    puts("AES FIPS-197 and RSA round-trip passed");
    return 0;
}
