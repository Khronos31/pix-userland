/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PSI sections copied from the preserved DT181 CS08/CS22 TS fixtures. */
#include "dt295_saved_cs_fixture.h"

#include <string.h>

static const char *const sections[2][DT295_SAVED_CS_PACKET_COUNT] = {
    {
        "00b01d6080df00000000e0100037e40f00dae40d013ee40e015de40cb9e0df80",
        "01b01affffd5000009050005e70102cc080005170000da013e3ec83ead",
        "02b0310037d50000ea0ff00002f00ff0035201000ff04ff0035201100dfe01f00f520140fd0a000b333f00020000ffffd1e2179c",
        "02b05600dad70000ea0df00c09040005e60dc10184de01af02f00df0035201000ff04df00352011006fc13f00e52013809040005fffffd0300083c0dfe01f01552014009040005fffffd0a000b333f00020000ffff5b1623f6",
        "02b056013ef50000ea0ef00c09040005e60ec10184de01af02f00ef0035201000ff04ef00352011006fc13f00e52013809040005fffffd0300083c0dfe01f01552014009040005fffffd0a000b333f00020000ffffd17f2995",
        "02b043015dd70000ea0cf00c09040005e60cc10184de01af02f00cf0035201000ff04cf0035201100dfe01f01552014009040005fffffd0a000b333f00020000ffff09c4e07e"
    },
    {
        "00b01d7160f300000000e01000a1e40f0129e40e0138e40c015fe40deea2ce44",
        "01b01cffffd5000009050005e70102cc0a0005170001290138015f94b46d81",
        "02b01d00a1f90000ea0ff00002f00ff0035201000ff04ff0035201102ce7f9fa",
        "02b0560129e70000ea0ef00c09040005e60ec10184de01af02f00ef0035201000ff04ef00352011006fc02f00e52013809040005fffffd0300083c0dfe01f01552014009040005fffffd0a000b333f00020000ffff224f5b6d",
        "02b05e0138e50000ea0cf00c09040005e60cc10184de01af02f00cf0035201000ff04cf0035201100ff05cf00352011106fc13f00e52013809040005fffffd0300083c0dfe01f01552014009040005fffffd0a000b333f00020000fffffec8b3ce",
        "02b056015fe10000ea0df00c09040005e60dc10184de01af02f00df0035201000ff04df00352011006f20df00e52013009040005fffffd0300083d0dfe01f01552014009040005fffffd0a000b333f00020000ffffb83d8f6d"
    }
};

static const uint16_t pids[2][DT295_SAVED_CS_PACKET_COUNT] = {
    {0x0000, 0x0001, 0x040f, 0x040d, 0x040e, 0x040c},
    {0x0000, 0x0001, 0x040f, 0x040e, 0x040c, 0x040d}
};

static const uint16_t programs[2][4] = {
    {55, 218, 318, 349},
    {161, 297, 312, 351}
};

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

uint16_t dt295_saved_cs_target(int mux)
{
    if (mux == DT295_SAVED_CS08) return programs[mux][0];
    if (mux == DT295_SAVED_CS22) return programs[mux][0];
    return 0;
}

uint16_t dt295_saved_cs_program(int mux, unsigned pmt_index)
{
    if ((mux != DT295_SAVED_CS08 && mux != DT295_SAVED_CS22) || pmt_index >= 4u)
        return 0;
    return programs[mux][pmt_index];
}

int dt295_saved_cs_packet(int mux, unsigned index, uint8_t packet[188])
{
    if ((mux != DT295_SAVED_CS08 && mux != DT295_SAVED_CS22) ||
        index >= DT295_SAVED_CS_PACKET_COUNT || packet == NULL)
        return -1;
    const char *section = sections[mux][index];
    const size_t bytes = strlen(section) / 2u;
    if (bytes > 183u) return -1;
    memset(packet, 0xff, 188u);
    packet[0] = 0x47;
    packet[1] = (uint8_t)(0x40u | (pids[mux][index] >> 8));
    packet[2] = (uint8_t)pids[mux][index];
    packet[3] = 0x10;
    packet[4] = 0;
    for (size_t i = 0; i < bytes; ++i) {
        const int high = hex_value(section[i * 2u]);
        const int low = hex_value(section[i * 2u + 1u]);
        if (high < 0 || low < 0) return -1;
        packet[5u + i] = (uint8_t)((high << 4) | low);
    }
    return 0;
}
