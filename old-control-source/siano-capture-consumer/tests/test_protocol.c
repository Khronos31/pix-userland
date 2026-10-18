/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../protocol.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_header(void)
{
    uint8_t buffer[SMS_HEADER_SIZE] = {0};
    struct sms_msg_hdr_wire header;

    assert(sizeof(struct sms_msg_hdr_wire) == SMS_HEADER_SIZE);
    sms_pack_header(buffer, MSG_SMS_ISDBT_TUNE_REQ, 201, SMS_HIF_TASK, 24, 0x0400);
    assert(buffer[0] == 0x08 && buffer[1] == 0x03);
    assert(buffer[2] == 201 && buffer[3] == 11);
    assert(buffer[4] == 24 && buffer[5] == 0);
    assert(buffer[6] == 0 && buffer[7] == 4);
    assert(sms_unpack_header(buffer, sizeof(buffer), &header) == 0);
    assert(header.msg_type == MSG_SMS_ISDBT_TUNE_REQ);
    assert(header.msg_length == 24 && header.msg_flags == 0x0400);
}

static void test_framing(void)
{
    uint8_t buffer[128] = {0};
    struct sms_frame frame;
    size_t alignment = 54;
    size_t offset = alignment + 3;

    sms_pack_header(buffer, MSG_SMS_DVBT_BDA_DATA, 0, SMS_HIF_TASK, 16,
                    SMS_MSG_HDR_FLAG_SPLIT_MSG | (3U << 8));
    sms_pack_header(buffer + offset, MSG_SMS_DVBT_BDA_DATA, 0, SMS_HIF_TASK,
                    16, SMS_MSG_HDR_FLAG_SPLIT_MSG | (3U << 8));
    memset(buffer + offset + SMS_HEADER_SIZE, 0xaa, 8);
    assert(sms_frame_message(buffer, offset + 16, alignment, &frame) == 0);
    assert(frame.offset == offset);
    assert(frame.payload_offset == offset + SMS_HEADER_SIZE);
    assert(frame.payload_length == 8);
    assert(sms_frame_message(buffer, offset + 15, alignment, &frame) < 0);
}

static void test_frequency(void)
{
    uint32_t frequency;

    assert(sms_channel_frequency(13, &frequency) == 0 && frequency == 473142857U);
    assert(sms_channel_frequency(27, &frequency) == 0 && frequency == 557142857U);
    assert(sms_channel_frequency(62, &frequency) == 0 && frequency == 767142857U);
    assert(sms_channel_frequency(12, &frequency) < 0);
    assert(sms_channel_frequency(63, &frequency) < 0);
}

static void test_firmware_header(void)
{
    uint8_t synthetic[24] = {0};
    struct sms_firmware_header header;
    FILE *file;

    sms_put_le32(synthetic + 0, 173);
    sms_put_le32(synthetic + 4, 12);
    sms_put_le32(synthetic + 8, 262752);
    assert(sms_parse_firmware_header(synthetic, sizeof(synthetic), &header) == 0);
    assert(header.check_sum == 173);
    assert(header.length == 12);
    assert(header.start_address == 262752);
    assert(sms_parse_firmware_header(synthetic, 8, &header) < 0);

    file = fopen("firmware/isdbt_rio.inp", "rb");
    if (file) {
        uint8_t *data;
        long size;

        assert(fseek(file, 0, SEEK_END) == 0);
        size = ftell(file);
        assert(size == 85840);
        assert(fseek(file, 0, SEEK_SET) == 0);
        data = malloc((size_t)size);
        assert(data != NULL);
        assert(fread(data, 1, (size_t)size, file) == (size_t)size);
        fclose(file);
        assert(sms_parse_firmware_header(data, (size_t)size, &header) == 0);
        assert(header.check_sum == 173);
        assert(header.length == 85828);
        assert(header.start_address == 262752);
        free(data);
    }
}

int main(void)
{
    test_header();
    test_framing();
    test_frequency();
    test_firmware_header();
    puts("protocol tests: PASS");
    return 0;
}
