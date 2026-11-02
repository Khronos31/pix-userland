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

static uint32_t mpeg_crc(const uint8_t *bytes, size_t length)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint32_t)bytes[i] << 24;
        for (unsigned bit = 0; bit < 8u; ++bit)
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u : crc << 1;
    }
    return crc;
}

int dt295_saved_cs_primary_ca_packet(int mux, uint8_t packet[188])
{
    if (dt295_saved_cs_packet(mux, 2u, packet) != 0)
        return -1;
    uint8_t *section = packet + 5u;
    const size_t length = 3u + (((size_t)section[1] & 0x0fu) << 8) + section[2];
    const uint8_t added[] = {0x06, 0xe7, 0x10, 0xf0, 0x06, 0x09, 0x04,
                             0x00, 0x05, 0xe7, 0x11};
    if (length + sizeof(added) > 183u)
        return -1;
    memmove(section + length - 4u + sizeof(added), section + length - 4u, 4u);
    memcpy(section + length - 4u, added, sizeof(added));
    const size_t updated_length = length + sizeof(added);
    const size_t section_length = updated_length - 3u;
    section[1] = (uint8_t)((section[1] & 0xf0u) | ((section_length >> 8) & 0x0fu));
    section[2] = (uint8_t)section_length;
    const uint32_t crc = mpeg_crc(section, updated_length - 4u);
    section[updated_length - 4u] = (uint8_t)(crc >> 24);
    section[updated_length - 3u] = (uint8_t)(crc >> 16);
    section[updated_length - 2u] = (uint8_t)(crc >> 8);
    section[updated_length - 1u] = (uint8_t)crc;
    return 0;
}

int dt295_saved_cs_pmt_ecm_update_packet(int mux, uint8_t packet[188])
{
    if (dt295_saved_cs_packet(mux, 3u, packet) != 0)
        return -1;
    packet[3] = (uint8_t)((packet[3] & 0xf0u) | 1u);
    uint8_t *section = packet + 5u;
    const size_t length = 3u + (((size_t)section[1] & 0x0fu) << 8) + section[2];
    const uint8_t added[] = {0x06, 0xe7, 0x10, 0xf0, 0x06, 0x09, 0x04,
                             0x00, 0x05, 0xe7, 0x11};
    if (length + sizeof(added) > 183u)
        return -1;
    memmove(section + length - 4u + sizeof(added), section + length - 4u, 4u);
    memcpy(section + length - 4u, added, sizeof(added));
    const size_t updated_length = length + sizeof(added);
    const size_t section_length = updated_length - 3u;
    const uint8_t version = (uint8_t)(((section[5] >> 1) + 1u) & 31u);
    section[1] = (uint8_t)((section[1] & 0xf0u) | ((section_length >> 8) & 0x0fu));
    section[2] = (uint8_t)section_length;
    section[5] = (uint8_t)((section[5] & 0xc1u) | (version << 1));
    const uint32_t crc = mpeg_crc(section, updated_length - 4u);
    section[updated_length - 4u] = (uint8_t)(crc >> 24);
    section[updated_length - 3u] = (uint8_t)(crc >> 16);
    section[updated_length - 2u] = (uint8_t)(crc >> 8);
    section[updated_length - 1u] = (uint8_t)crc;
    return 0;
}

int dt295_saved_cs_pat_update_packet(int mux, uint16_t tsid, uint8_t version,
                                     uint8_t continuity, uint8_t packet[188])
{
    if (dt295_saved_cs_packet(mux, 0u, packet) != 0)
        return -1;
    packet[3] = (uint8_t)((packet[3] & 0xf0u) | (continuity & 0x0fu));
    uint8_t *section = packet + 5u;
    const size_t length = 3u + (((size_t)section[1] & 0x0fu) << 8) + section[2];
    section[3] = (uint8_t)(tsid >> 8);
    section[4] = (uint8_t)tsid;
    section[5] = (uint8_t)((section[5] & 0xc1u) | ((version & 31u) << 1));
    const uint32_t crc = mpeg_crc(section, length - 4u);
    section[length - 4u] = (uint8_t)(crc >> 24);
    section[length - 3u] = (uint8_t)(crc >> 16);
    section[length - 2u] = (uint8_t)(crc >> 8);
    section[length - 1u] = (uint8_t)crc;
    return 0;
}
