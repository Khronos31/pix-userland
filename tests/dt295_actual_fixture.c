/* SPDX-License-Identifier: MIT */
/* Reuse the accepted protocol emulator and asset builder without running its
 * standalone proof entrypoint. */
#define main dt295_protocol_fixture_unused_main
#define random_bytes dt295_fixture_random_bytes
#include "test_dt295_protocol.c"
#undef random_bytes
#undef main

#include <libusb.h>
#include <pthread.h>

static fake pipeline_fake;
static pthread_mutex_t pipeline_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned pipeline_route_writes;
static unsigned pipeline_pid273_route_writes;
static int pipeline_fail_route_once;
static unsigned pipeline_route_failures;

int dt295_actual_fixture_open(const char *assets_path)
{
    memset(&pipeline_fake, 0, sizeof(pipeline_fake));
    pipeline_route_writes = 0;
    pipeline_pid273_route_writes = 0;
    pipeline_fail_route_once = 0;
    pipeline_route_failures = 0;
    pipeline_fake.mem = calloc(1, MEMSIZE);
    if (!pipeline_fake.mem)
        return -1;
    pipeline_fake.now = 1000;
    wr16(pipeline_fake.mem + 0x82008, 0x10);
    pipeline_fake.mem[0x822ac] = 0x30;
    create_assets(assets_path, &pipeline_fake);
    return 0;
}

void dt295_actual_fixture_close(void)
{
    pthread_mutex_lock(&pipeline_mutex);
    free(pipeline_fake.mem);
    pipeline_fake.mem = NULL;
    pthread_mutex_unlock(&pipeline_mutex);
}

int dt295_actual_fixture_control(uint8_t request_type, uint8_t request, uint16_t value,
                                 uint16_t index, uint8_t *data, uint16_t length)
{
    size_t actual = 0;
    pthread_mutex_lock(&pipeline_mutex);
    const uint16_t pending_route_pid = (uint16_t)(pipeline_fake.mem[0x81004] << 8) |
                                       pipeline_fake.mem[0x81005];
    if (pipeline_fail_route_once && (request == 0xb8 || request == 0xb9) && length == 4 &&
        data[0] == 5 && data[1] == 1 &&
        (pending_route_pid == 0x111 || pending_route_pid == 0x112)) {
        pipeline_fail_route_once = 0;
        ++pipeline_route_failures;
        pthread_mutex_unlock(&pipeline_mutex);
        fprintf(stderr, "DT295 injection=route_write_failure reached=1 pid=%u\n",
                pending_route_pid);
        return LIBUSB_ERROR_IO;
    }
    const int rc = control(&pipeline_fake, request_type, request, value, index, data,
                           length, 500, &actual);
    if (!rc && (request == 0xb8 || request == 0xb9) && length == 4 &&
        data[0] == 5 && data[1] == 1) {
        ++pipeline_route_writes;
        const uint16_t route_pid = (uint16_t)(pipeline_fake.mem[0x81004] << 8) |
                                   pipeline_fake.mem[0x81005];
        if (route_pid == 0x111)
            ++pipeline_pid273_route_writes;
        fprintf(stderr, "DT295 route_op command=5 slot=%u ecm=%u enabled=%u pid=%u\n",
                pipeline_fake.mem[0x81000], pipeline_fake.mem[0x81001],
                (pipeline_fake.mem[0x81002] >> 7) & 1,
                route_pid);
    }
    if (!rc && request == 0xb8 && length == 4 && data[0] == 5 &&
        rd16(data + 2) >= 3 && rd16(data + 2) <= 4) {
        wr16(pipeline_fake.mem + 0x82008, 4);
        wr16(pipeline_fake.mem + 0x90074,
             (uint16_t)(rd16(pipeline_fake.mem + 0x90074) | 4));
    }
    pthread_mutex_unlock(&pipeline_mutex);
    if (rc)
        fprintf(stderr, "DT295 fixture_control_failed request=%02x type=%02x value=%04x index=%04x length=%u\n",
                request, request_type, value, index, length);
    return rc ? LIBUSB_ERROR_IO : (int)actual;
}

int dt295_actual_fixture_bulk(uint8_t endpoint, unsigned char *data, int length, int *actual)
{
    size_t transferred = 0;
    pthread_mutex_lock(&pipeline_mutex);
    const int rc = bulk(&pipeline_fake, endpoint, data, (size_t)length, 500, &transferred);
    pthread_mutex_unlock(&pipeline_mutex);
    if (rc)
        fprintf(stderr, "DT295 fixture_bulk_failed endpoint=%02x length=%d\n", endpoint, length);
    *actual = (int)transferred;
    return rc ? LIBUSB_ERROR_IO : 0;
}

int dt295_actual_fixture_interrupt(uint8_t endpoint, unsigned char *data, int length,
                                   int *actual)
{
    size_t transferred = 0;
    pthread_mutex_lock(&pipeline_mutex);
    const int rc = interrupt(&pipeline_fake, endpoint, data, (size_t)length, 500, &transferred);
    pthread_mutex_unlock(&pipeline_mutex);
    *actual = (int)transferred;
    return rc ? LIBUSB_ERROR_TIMEOUT : 0;
}

uint64_t dt295_actual_fixture_now(void)
{
    pthread_mutex_lock(&pipeline_mutex);
    const uint64_t value = pipeline_fake.now;
    pthread_mutex_unlock(&pipeline_mutex);
    return value;
}

void dt295_actual_fixture_advance(uint64_t milliseconds)
{
    pthread_mutex_lock(&pipeline_mutex);
    pipeline_fake.now += milliseconds;
    pthread_mutex_unlock(&pipeline_mutex);
}

unsigned dt295_actual_fixture_key_requests(void)
{
    pthread_mutex_lock(&pipeline_mutex);
    const unsigned value = pipeline_fake.key_requests;
    pthread_mutex_unlock(&pipeline_mutex);
    return value;
}

unsigned dt295_actual_fixture_renewal_applies(void)
{
    pthread_mutex_lock(&pipeline_mutex);
    const unsigned value = pipeline_fake.renewal_applies;
    pthread_mutex_unlock(&pipeline_mutex);
    return value;
}

unsigned dt295_actual_fixture_auth_requests(void)
{
    pthread_mutex_lock(&pipeline_mutex);
    const unsigned value = pipeline_fake.auth_requests;
    pthread_mutex_unlock(&pipeline_mutex);
    return value;
}

unsigned dt295_actual_fixture_renewal_challenges(void)
{
    pthread_mutex_lock(&pipeline_mutex);
    const unsigned value = pipeline_fake.renewal_challenges;
    pthread_mutex_unlock(&pipeline_mutex);
    return value;
}

unsigned dt295_actual_fixture_route_writes(void)
{
    pthread_mutex_lock(&pipeline_mutex);
    const unsigned value = pipeline_route_writes;
    pthread_mutex_unlock(&pipeline_mutex);
    return value;
}

unsigned dt295_actual_fixture_route_failures(void)
{
    pthread_mutex_lock(&pipeline_mutex);
    const unsigned value = pipeline_route_failures;
    pthread_mutex_unlock(&pipeline_mutex);
    return value;
}

unsigned dt295_actual_fixture_pid273_route_writes(void)
{
    pthread_mutex_lock(&pipeline_mutex);
    const unsigned value = pipeline_pid273_route_writes;
    pthread_mutex_unlock(&pipeline_mutex);
    return value;
}

void dt295_actual_fixture_fail_next_route(void)
{
    pthread_mutex_lock(&pipeline_mutex);
    pipeline_fail_route_once = 1;
    pthread_mutex_unlock(&pipeline_mutex);
}

void dt295_actual_fixture_fail_renewal(void)
{
    pthread_mutex_lock(&pipeline_mutex);
    pipeline_fake.fail_secure_id = 0x02020400;
    pthread_mutex_unlock(&pipeline_mutex);
}

static uint32_t actual_crc(const uint8_t *, size_t);
static void actual_table(uint8_t[188], unsigned, unsigned, unsigned,
                         const uint8_t *, size_t);

static void actual_table_update(uint8_t packet[188], unsigned version, unsigned cc,
                                unsigned mode)
{
    uint8_t pmt[48];
    size_t n = 0;
    const uint16_t ecm = mode == 3 ? 0x902 : 0x901;
    pmt[n++] = 0xe1; pmt[n++] = 0x01;
    pmt[n++] = 0xf0; pmt[n++] = (uint8_t)(mode == 2 ? 12 : 6);
    pmt[n++] = 0x09; pmt[n++] = 4; pmt[n++] = 0; pmt[n++] = 5;
    pmt[n++] = (uint8_t)(0xe0 | (ecm >> 8)); pmt[n++] = (uint8_t)ecm;
    if (mode == 2) {
        pmt[n++] = 0x09; pmt[n++] = 4; pmt[n++] = 0; pmt[n++] = 5;
        pmt[n++] = 0xe9; pmt[n++] = 0x02;
    }
    pmt[n++] = 0x1b; pmt[n++] = 0xe1; pmt[n++] = 0x01;
    pmt[n++] = 0xf0; pmt[n++] = 0;
    pmt[n++] = 0x0f; pmt[n++] = 0xe1; pmt[n++] = 0x02;
    pmt[n++] = 0xf0; pmt[n++] = 0;
    if (mode == 1) {
        pmt[n++] = 0x0f; pmt[n++] = 0xe1; pmt[n++] = 0x11;
        pmt[n++] = 0xf0; pmt[n++] = 0;
    }
    actual_table(packet, 0x100, 2, 1, pmt, n);
    packet[3] = (uint8_t)(0x10 | (cc & 15));
    packet[5 + 5] = (uint8_t)(0xc1 | ((version & 31u) << 1));
    const size_t section_size = 12 + n;
    const uint32_t crc = actual_crc(packet + 5, section_size - 4);
    for (unsigned i = 0; i < 4; ++i)
        packet[5 + section_size - 4 + i] = (uint8_t)(crc >> (24 - 8 * i));
}

void dt295_actual_fixture_update_packet(uint8_t packet[188], unsigned version,
                                        unsigned cc, unsigned mode)
{
    actual_table_update(packet, version, cc, mode);
}

static uint32_t actual_crc(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint32_t)data[i] << 24;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0u);
    }
    return crc;
}

static void actual_table(uint8_t packet[188], unsigned pid, unsigned tid, unsigned ext,
                         const uint8_t *payload, size_t payload_size)
{
    memset(packet, 0xff, 188);
    packet[0] = 0x47;
    packet[1] = (uint8_t)(0x40u | (pid >> 8));
    packet[2] = (uint8_t)pid;
    packet[3] = 0x10;
    packet[4] = 0;
    uint8_t *section = packet + 5;
    const size_t section_size = 12 + payload_size;
    section[0] = (uint8_t)tid;
    section[1] = (uint8_t)(0xb0u | ((section_size - 3) >> 8));
    section[2] = (uint8_t)(section_size - 3);
    section[3] = (uint8_t)(ext >> 8);
    section[4] = (uint8_t)ext;
    section[5] = 0xc1;
    section[6] = section[7] = 0;
    memcpy(section + 8, payload, payload_size);
    const uint32_t crc = actual_crc(section, section_size - 4);
    for (unsigned i = 0; i < 4; ++i)
        section[section_size - 4 + i] = (uint8_t)(crc >> (24 - 8 * i));
}

void dt295_actual_fixture_bootstrap(uint8_t packets[3][188])
{
    const uint8_t pat[] = {0, 1, 0xe1, 0x00};
    const uint8_t cat[] = {0x09, 4, 0, 5, 0xe9, 1};
    const uint8_t pmt[] = {
        0xe1, 0x01, 0xf0, 6, 0x09, 4, 0, 5, 0xe9, 1,
        0x1b, 0xe1, 0x01, 0xf0, 0,
        0x0f, 0xe1, 0x02, 0xf0, 0
    };
    actual_table(packets[0], 0, 0, 1, pat, sizeof(pat));
    actual_table(packets[1], 1, 1, 1, cat, sizeof(cat));
    actual_table(packets[2], 0x100, 2, 1, pmt, sizeof(pmt));
}

static size_t actual_shared_pmt(uint8_t *pmt, unsigned program, int include_extra)
{
    size_t n = 0;
    const uint16_t video = (uint16_t)(0x101 + (program - 1) * 2);
    const uint16_t audio = (uint16_t)(video + 1);
    pmt[n++] = (uint8_t)(0xe0 | (video >> 8)); pmt[n++] = (uint8_t)video;
    pmt[n++] = 0xf0; pmt[n++] = 6;
    pmt[n++] = 0x09; pmt[n++] = 4; pmt[n++] = 0; pmt[n++] = 5;
    pmt[n++] = 0xe9; pmt[n++] = 1;
    pmt[n++] = 0x1b; pmt[n++] = (uint8_t)(0xe0 | (video >> 8)); pmt[n++] = (uint8_t)video;
    pmt[n++] = 0xf0; pmt[n++] = 0;
    pmt[n++] = 0x0f; pmt[n++] = (uint8_t)(0xe0 | (audio >> 8)); pmt[n++] = (uint8_t)audio;
    pmt[n++] = 0xf0; pmt[n++] = 0;
    if (include_extra) {
        pmt[n++] = 0x0f; pmt[n++] = 0xe1; pmt[n++] = 0x11;
        pmt[n++] = 0xf0; pmt[n++] = 0;
    }
    return n;
}

void dt295_actual_fixture_shared_bootstrap(uint8_t packets[4][188])
{
    const uint8_t pat[] = {0, 1, 0xe1, 0, 0, 2, 0xe1, 0x10};
    const uint8_t cat[] = {0x09, 4, 0, 5, 0xe9, 1};
    uint8_t pmt[48];
    actual_table(packets[0], 0, 0, 1, pat, sizeof(pat));
    actual_table(packets[1], 1, 1, 1, cat, sizeof(cat));
    size_t n = actual_shared_pmt(pmt, 1, 0);
    actual_table(packets[2], 0x100, 2, 1, pmt, n);
    n = actual_shared_pmt(pmt, 2, 0);
    actual_table(packets[3], 0x110, 2, 2, pmt, n);
}

void dt295_actual_fixture_shared_update_packet(uint8_t packet[188], unsigned program,
                                               unsigned version, unsigned cc, int include_extra)
{
    uint8_t pmt[48];
    const size_t n = actual_shared_pmt(pmt, program, include_extra);
    const unsigned pid = 0x100 + (program - 1) * 0x10;
    actual_table(packet, pid, 2, program, pmt, n);
    packet[3] = (uint8_t)(0x10 | (cc & 15));
    packet[10] = (uint8_t)(0xc1 | ((version & 31u) << 1));
    const size_t section_size = 12 + n;
    const uint32_t crc = actual_crc(packet + 5, section_size - 4);
    for (unsigned i = 0; i < 4; ++i)
        packet[5 + section_size - 4 + i] = (uint8_t)(crc >> (24 - 8 * i));
}

void dt295_actual_fixture_data_pid(uint8_t packet[188], unsigned pid, unsigned continuity)
{
    memset(packet, 0x5a, 188);
    packet[0] = 0x47;
    packet[1] = (uint8_t)(pid >> 8);
    packet[2] = (uint8_t)pid;
    packet[3] = (uint8_t)(0x10 | (continuity & 15));
}

void dt295_actual_fixture_recording_packet(uint8_t packet[188], unsigned continuity)
{
    memset(packet, 0x5a, 188);
    packet[0] = 0x47;
    packet[1] = 0x01;
    packet[2] = 0x01;
    packet[3] = (uint8_t)(0x10 | (continuity & 15));
}

void dt295_actual_fixture_encrypt(uint8_t packet[188])
{
    uint8_t key[16], iv[16];
    for (unsigned i = 0; i < 16; ++i) {
        key[i] = (uint8_t)(0x70 + i);
        iv[i] = (uint8_t)(0xa0 + i);
    }
    packet[3] |= 0x40;
    uint8_t chain[16], block[16], encrypted[16];
    memcpy(chain, iv, 16);
    aes128 aes;
    aes_init(&aes, key);
    for (size_t offset = 4; offset + 16 <= 180; offset += 16) {
        for (unsigned i = 0; i < 16; ++i)
            block[i] = packet[offset + i] ^ chain[i];
        aes_encrypt(&aes, block, encrypted);
        memcpy(packet + offset, encrypted, 16);
        memcpy(chain, encrypted, 16);
    }
    aes_encrypt(&aes, chain, encrypted);
    for (unsigned i = 0; i < 8; ++i)
        packet[180 + i] ^= encrypted[i];
}
