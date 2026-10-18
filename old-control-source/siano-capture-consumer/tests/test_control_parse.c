/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../control-parse.h"

#include <assert.h>
#include <stdint.h>

int main(void)
{
    uint32_t value = 99;

    assert(parse_control_line("\n", &value) == CONTROL_EMPTY);
    assert(parse_control_line(" \t\r\n", &value) == CONTROL_EMPTY);
    assert(parse_control_line("channel 13", &value) == CONTROL_CHANNEL);
    assert(value == 13);
    assert(parse_control_line("channel 62\r", &value) == CONTROL_CHANNEL);
    assert(value == 62);
    assert(parse_control_line("tune 1", &value) == CONTROL_TUNE);
    assert(value == 1);
    assert(parse_control_line("tune 4294967295", &value) == CONTROL_TUNE);
    assert(value == UINT32_MAX);
    assert(parse_control_line("quit", &value) == CONTROL_QUIT);
    assert(parse_control_line("channel 12", &value) == CONTROL_INVALID);
    assert(parse_control_line("channel 63", &value) == CONTROL_INVALID);
    assert(parse_control_line("tune 0", &value) == CONTROL_INVALID);
    assert(parse_control_line("tune 4294967296", &value) == CONTROL_INVALID);
    assert(parse_control_line("tune -1", &value) == CONTROL_INVALID);
    assert(parse_control_line("quit now", &value) == CONTROL_INVALID);
    assert(parse_control_line("unknown 27", &value) == CONTROL_INVALID);
    return 0;
}
