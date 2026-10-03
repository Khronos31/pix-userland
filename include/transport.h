#ifndef PIX_TRANSPORT_H
#define PIX_TRANSPORT_H
#include "crypto.h"
#define TS_SIZE 188
#define KEY_LIMIT 256
#define FRAGMENT_LIMIT 64
typedef int (*ts_sink)(void *, const uint8_t *, size_t);
typedef struct {
    uint64_t packets, tei, scrambled, continuity, invalid, crc_errors, pat, pmt;
    uint8_t cc[8192], seen[8192], last[8192][188];
    struct {
        uint16_t pid;
        size_t used;
        uint8_t data[4096];
    } psi[65];
    unsigned psi_count;
} ts_check;
typedef struct {
    uint64_t input, markers, iv_updates, output, resync, waiting, trailing;
    uint64_t key_tables, previous_key_uses;
    uint8_t encrypted_kk[16], key_ids[KEY_LIMIT][16], keys[KEY_LIMIT][16];
    unsigned key_count;
    uint8_t previous_key_ids[KEY_LIMIT][16], previous_keys[KEY_LIMIT][16];
    unsigned previous_key_count;
    const char *error;
    uint8_t table[FRAGMENT_LIMIT * 158];
    unsigned fragment, next_fragment;
    uint8_t buffer[32768 + 564];
    size_t used;
    int active, failed;
    aes_ctr ctr;
    ts_sink sink;
    void *user;
    ts_check *check;
} ts_decoder;
void ts_check_init(ts_check *);
void ts_check_packet(ts_check *, const uint8_t packet[188]);
uint32_t ts_crc(const uint8_t *, size_t);
void ts_decoder_init(ts_decoder *, const uint8_t context[32], ts_sink, void *, ts_check *);
int ts_decoder_feed(ts_decoder *, const uint8_t *, size_t);
#endif
