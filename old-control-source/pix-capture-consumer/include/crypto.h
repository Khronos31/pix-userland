#ifndef PIXY_CRYPTO_H
#define PIXY_CRYPTO_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint8_t round[176];
} aes128;
typedef struct {
    aes128 aes;
    uint8_t iv[16], block[16];
    unsigned used, counter_bytes;
} aes_ctr;
void aes_init(aes128 *, const uint8_t key[16]);
void aes_encrypt(const aes128 *, const uint8_t in[16], uint8_t out[16]);
void aes_decrypt(const aes128 *, const uint8_t in[16], uint8_t out[16]);
void ctr_init(aes_ctr *, const uint8_t key[16], const uint8_t iv[16], unsigned counter_bytes);
void ctr_xor(aes_ctr *, uint8_t *, size_t);
void secure_clear(void *, size_t);
int random_bytes(void *, size_t);
int rsa_generate(uint8_t modulus[128], uint8_t private_exponent[128]);
int rsa_apply(const uint8_t input[128], const uint8_t exponent[128], const uint8_t modulus[128],
              uint8_t output[128]);
#endif
