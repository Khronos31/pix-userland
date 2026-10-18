#ifndef PIX_DIAGNOSTIC_H
#define PIX_DIAGNOSTIC_H
#include "transport.h"
#include <stdio.h>
#define DIAG_SLOTS 8
#define DIAG_CC_SLOTS 2
#define DIAG_CC_WINDOWS 3
#ifndef DIAG_SEGMENT
#define DIAG_SEGMENT (8u * 1024u * 1024u)
#endif
#ifndef DIAG_POST
#define DIAG_POST (8u * 1024u * 1024u)
#endif
#ifndef DIAG_CC_POST
#define DIAG_CC_POST (2u * 1024u * 1024u)
#endif
typedef struct diagnostic diagnostic;
diagnostic *diag_open(const char *directory);
int diag_before(diagnostic *, const ts_decoder *, const ts_check *, const uint8_t *, size_t,
                uint64_t received_ms, unsigned queued, unsigned high_water);
int diag_after(diagnostic *, const ts_decoder *, const ts_check *, int failed);
void diag_issue(void *, const char *, unsigned pid, uint64_t packet, unsigned expected,
                unsigned actual);
int diag_close(diagnostic *);
int diag_replay(const char *, ts_decoder *, ts_check *, ts_sink, void *);
int diag_replay_chunks(const char *, ts_decoder *, ts_check *, ts_sink, void *, size_t);
#endif
