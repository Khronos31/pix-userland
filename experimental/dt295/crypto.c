/* Experimental DT295 transport protection. No device/session secrets here. */
#include "crypto.h"
#include <string.h>

int dt295_cbc_encrypt(const unsigned char key[16], const unsigned char iv[16],
                       unsigned char *p, size_t n)
{
    if (n % 16) return -1;
    aes128 a;
    unsigned char chain[16], block[16];
    aes_init(&a, key); memcpy(chain, iv, 16);
    while (n) {
        for (unsigned i = 0; i < 16; i++) block[i] = p[i] ^ chain[i];
        aes_encrypt(&a, block, p); memcpy(chain, p, 16);
        p += 16; n -= 16;
    }
    secure_clear(&a, sizeof(a)); secure_clear(chain, 16); secure_clear(block, 16);
    return 0;
}

/* CBC full blocks followed by CFB-style residual, independently for each TS. */
int dt295_payload(const unsigned char key[16], const unsigned char iv[16],
                  unsigned char *p, size_t n)
{
    aes128 a;
    unsigned char chain[16], cipher[16], plain[16];
    aes_init(&a, key);
    memcpy(chain, iv, 16);
    while (n >= 16) {
        memcpy(cipher, p, 16);
        aes_decrypt(&a, cipher, plain);
        for (unsigned i = 0; i < 16; i++) p[i] = plain[i] ^ chain[i];
        memcpy(chain, cipher, 16);
        p += 16; n -= 16;
    }
    if (n) {
        aes_encrypt(&a, chain, plain);
        for (size_t i = 0; i < n; i++) p[i] ^= plain[i];
    }
    secure_clear(&a, sizeof(a)); secure_clear(chain, 16);
    secure_clear(cipher, 16); secure_clear(plain, 16);
    return 0;
}

/* Return 1 decrypted, 0 clear, -1 malformed/TEI, -2 other protection mode.
 * Leave all bytes unchanged on error; never label unsupported ciphertext clear. */
int dt295_packet(const unsigned char key[16], const unsigned char iv[16],
                 unsigned char p[188])
{
    unsigned afc = (p[3] >> 4) & 3, tsc = p[3] >> 6;
    size_t off = 4;
    if (p[0] != 0x47 || (p[1] & 0x80) || !afc) return -1;
    if (afc & 2) {
        off = 5u + p[4];
        if (off > 188 || (afc == 2 && off != 188) || (afc == 3 && off >= 188))
            return -1;
    }
    if (!tsc) return 0;
    if (tsc != 1) return -2;
    if (!(afc & 1)) return -1;
    dt295_payload(key, iv, p + off, 188 - off);
    p[3] &= 0x3f;
    return 1;
}

static void double_block(unsigned char p[16])
{
    unsigned carry = p[0] >> 7;
    for (unsigned i = 0; i < 15; i++) p[i] = (p[i] << 1) | (p[i+1] >> 7);
    p[15] = (p[15] << 1) ^ (carry ? 0x87 : 0);
}

void dt295_cmac(const unsigned char key[16], const unsigned char *p,
                 size_t n, unsigned char out[16])
{
    aes128 a;
    unsigned char sub[16] = {0}, chain[16] = {0}, block[16] = {0};
    aes_init(&a, key); aes_encrypt(&a, sub, block);
    memcpy(sub, block, 16); double_block(sub);
    while (n > 16) {
        for (unsigned i = 0; i < 16; i++) block[i] = p[i] ^ chain[i];
        aes_encrypt(&a, block, chain); p += 16; n -= 16;
    }
    memset(block, 0, 16);
    if (n) memcpy(block, p, n);
    if (n < 16) { block[n] = 0x80; double_block(sub); }
    for (unsigned i = 0; i < 16; i++) block[i] ^= sub[i] ^ chain[i];
    aes_encrypt(&a, block, out);
    secure_clear(&a, sizeof(a)); secure_clear(sub, 16);
    secure_clear(chain, 16); secure_clear(block, 16);
}
