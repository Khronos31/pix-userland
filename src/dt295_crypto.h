#ifndef PIXY_DT295_CRYPTO_INTERNAL_H
#define PIXY_DT295_CRYPTO_INTERNAL_H
#include <stddef.h>
#include <stdint.h>

int dt295_cbc_encrypt(const uint8_t key[16], const uint8_t iv[16], uint8_t *data, size_t length);
int dt295_payload(const uint8_t key[16], const uint8_t iv[16], uint8_t *data, size_t length);
void dt295_cmac(const uint8_t key[16], const uint8_t *data, size_t length, uint8_t out[16]);
int dt295_packet_crypto(const uint8_t key[16], const uint8_t iv[16], uint8_t packet[188]);
void dt295_sha256(const uint8_t *data, size_t length, uint8_t out[32]);

#endif
