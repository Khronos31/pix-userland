/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "output-writer.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct captured {
    uint8_t packets[4][SIANO_OUTPUT_PACKET];
    size_t count;
    int reject;
};

static int capture_packet(void *user,
                          const uint8_t packet[SIANO_OUTPUT_PACKET])
{
    struct captured *captured = user;
    if (captured->reject)
        return -ECANCELED;
    assert(captured->count < 4);
    memcpy(captured->packets[captured->count++], packet,
           SIANO_OUTPUT_PACKET);
    return 0;
}

static void fill(uint8_t *data, size_t length, uint8_t marker)
{
    memset(data, marker, length);
    for (size_t offset = 0; offset + SIANO_OUTPUT_PACKET <= length;
         offset += SIANO_OUTPUT_PACKET) {
        data[offset] = 0x47;
        data[offset + 1] = marker;
    }
}

int main(void)
{
    struct siano_output_pump pump;
    struct captured captured = {0};
    uint8_t input[SIANO_OUTPUT_PACKET * 2];
    uint8_t next[SIANO_OUTPUT_PACKET];

    fill(input, sizeof(input), 0x11);
    assert(siano_output_pump_init_sink(&pump, capture_packet, &captured) == 0);
    siano_output_pump_feed(&pump, input, 187);
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    assert(captured.count == 0);
    siano_output_pump_feed(&pump, input + 187, sizeof(input) - 187);
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    assert(captured.count == 2);
    assert(memcmp(captured.packets[0], input, SIANO_OUTPUT_PACKET) == 0);
    assert(memcmp(captured.packets[1], input + SIANO_OUTPUT_PACKET,
                  SIANO_OUTPUT_PACKET) == 0);

    captured.reject = 1;
    fill(next, sizeof(next), 0x22);
    siano_output_pump_feed(&pump, next, sizeof(next));
    size_t pending_before = siano_output_pump_pending(&pump);
    size_t send_off_before = pump.send_off;
    assert(siano_output_pump_step(&pump) == -ECANCELED);
    assert(captured.count == 2);
    assert(siano_output_pump_pending(&pump) == pending_before);
    assert(pump.send_off == send_off_before);

    captured.reject = 0;
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    assert(captured.count == 3);
    assert(memcmp(captured.packets[2], next, SIANO_OUTPUT_PACKET) == 0);
    siano_output_pump_destroy(&pump);

    struct captured fresh = {0};
    uint8_t partial[187];
    fill(partial, sizeof(partial), 0x33);
    partial[0] = 0x47;
    assert(siano_output_pump_init_sink(&pump, capture_packet, &captured) == 0);
    siano_output_pump_feed(&pump, partial, sizeof(partial));
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    assert(pump.hold_len == sizeof(partial));
    siano_output_pump_destroy(&pump);

    assert(siano_output_pump_init_sink(&pump, capture_packet, &fresh) == 0);
    assert(!siano_output_pump_busy(&pump));
    assert(siano_output_pump_pending(&pump) == 0);
    siano_output_pump_feed(&pump, next, sizeof(next));
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    assert(fresh.count == 1);
    assert(memcmp(fresh.packets[0], next, SIANO_OUTPUT_PACKET) == 0);
    siano_output_pump_destroy(&pump);
    puts("capture packet sink rejection retry, partial carry reset and failure propagation: PASS");
    return 0;
}
