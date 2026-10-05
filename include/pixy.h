#ifndef PIXY_H
#define PIXY_H
#include "crypto.h"
#include <stddef.h>
#include <stdint.h>
typedef struct {
    void *user;
    /* 0 success; IN updates length, OUT must transfer the complete buffer. */
    int (*bulk)(void *, uint8_t endpoint, uint8_t *, size_t *, unsigned timeout_ms);
} pixy_io;
typedef struct {
    pixy_io io;
    uint8_t fxseq, seq, key[16], tx[16], rx[16], token[4];
    int powered, session_ready, streaming;
    char error[192];
} pixy;
typedef enum { PIXY_TERRESTRIAL, PIXY_BS, PIXY_CS } pixy_system;
void pixy_init(pixy *, pixy_io);
int pixy_power(pixy *, const uint8_t *, size_t);
int pixy_session(pixy *, const uint8_t bootstrap[16], const uint8_t validation[64]);
int pixy_tuner_init(pixy *);
int pixy_tune(pixy *, pixy_system, uint32_t frequency, int lna, int *tsid);
int pixy_tune_slot(pixy *, pixy_system, uint32_t frequency, int lna, int *tsid, int slot);
int pixy_certify(pixy *);
int pixy_status(pixy *, uint8_t *, size_t *);
int pixy_fifo_clear(pixy *);
int pixy_start(pixy *, uint8_t context[32]);
int pixy_stop(pixy *);
int pixy_shutdown(pixy *);
int pixy_frequency(pixy_system, unsigned channel, uint32_t *);
uint64_t now_ms(void);
void sleep_ms(unsigned);
#endif
