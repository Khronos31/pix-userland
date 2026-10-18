#include "dt295.h"
#include "dt295_crypto.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int random_bytes(void *p, size_t n) {
    memset(p, 0, n);
    return 0;
}

static void from_hex(const char *s, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned x = 0;
        assert(sscanf(s + 2 * i, "%2x", &x) == 1);
        out[i] = (uint8_t)x;
    }
}

int main(void) {
    uint8_t digest[32], want[32];
    dt295_sha256((const uint8_t *)"abc", 3, digest);
    from_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
    assert(memcmp(digest, want, 32) == 0);

    uint8_t key[16], mac[16], want_mac[16];
    from_hex("2b7e151628aed2a6abf7158809cf4f3c", key, 16);
    dt295_cmac(key, NULL, 0, mac);
    from_hex("bb1d6929e95937287fa37d129b756746", want_mac, 16);
    assert(memcmp(mac, want_mac, 16) == 0);

    uint16_t divider = 0;
    assert(dt295_terrestrial_rf_divider(13, &divider) == 0 && divider == 30281);
    assert(dt295_terrestrial_rf_divider(27, &divider) == 0 && divider == 35657);
    assert(dt295_terrestrial_rf_divider(62, &divider) == 0 && divider == 49097);
    assert(dt295_terrestrial_rf_divider(12, &divider) < 0);

    uint8_t iv[16] = {0}, packet[188] = {0x47, 0, 0, 0x10}, original[188];
    assert(dt295_packet_crypto(key, iv, packet) == 0);
    packet[3] = 0x90;
    memcpy(original, packet, sizeof(packet));
    assert(dt295_packet_crypto(key, iv, packet) == -2);
    assert(memcmp(packet, original, sizeof(packet)) == 0);
    packet[3] = 0x30;
    packet[4] = 184;
    memcpy(original, packet, sizeof(packet));
    assert(dt295_packet_crypto(key, iv, packet) == -1);
    assert(memcmp(packet, original, sizeof(packet)) == 0);
    packet[3] = 0x10;
    packet[4] = 0;
    assert(dt295_packet_crypto(key, iv, packet) == 0);
    packet[0] = 0;
    memcpy(original, packet, sizeof(packet));
    assert(dt295_packet_crypto(key, iv, packet) == -1);
    assert(memcmp(packet, original, sizeof(packet)) == 0);
    return 0;
}
