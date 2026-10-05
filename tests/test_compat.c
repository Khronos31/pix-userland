#include "channel.h"
#include "control.h"
#include "pid_filter.h"
#include "os.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define fd_dup _dup
#define fd_dup2 _dup2
#else
#define fd_dup dup
#define fd_dup2 dup2
#endif

static void channel(const char *text, pixy_system base, pixy_system system, unsigned number,
                    int slot) {
    pix_channel c;
    memset(&c, 0xa5, sizeof(c));
    assert(!pix_channel_parse(text, base, &c));
    assert(c.system == system && c.channel == number && c.slot == slot);
}
static void test_channels(void) {
    channel("27", PIXY_TERRESTRIAL, PIXY_TERRESTRIAL, 27, -1);
    channel("T27", PIXY_BS, PIXY_TERRESTRIAL, 27, -1);
    channel("T13", PIXY_TERRESTRIAL, PIXY_TERRESTRIAL, 13, -1);
    channel("T52", PIXY_TERRESTRIAL, PIXY_TERRESTRIAL, 52, -1);
    channel("BS01_0", PIXY_TERRESTRIAL, PIXY_BS, 1, 0);
    channel("BS23_7", PIXY_TERRESTRIAL, PIXY_BS, 23, 7);
    channel("BS1", PIXY_TERRESTRIAL, PIXY_BS, 1, -1);
    channel("23", PIXY_BS, PIXY_BS, 23, -1);
    channel("CS2", PIXY_TERRESTRIAL, PIXY_CS, 2, -1);
    channel("CS02", PIXY_TERRESTRIAL, PIXY_CS, 2, -1);
    channel("CS24", PIXY_TERRESTRIAL, PIXY_CS, 24, -1);
    channel("24", PIXY_CS, PIXY_CS, 24, -1);
    const char *bad[] = {"", " ", "27 ", " 27", "+27", "-27", "t27", "T", "BS", "CS",
                         "T12", "T53", "T27_0", "BS00", "BS02", "BS25", "BS01_", "BS01_8",
                         "BS01_-1", "BS01_0_1", "BS01x", "CS0", "CS1", "CS26", "27_0",
                         "4294967296", "BS4294967296", "BS01_4294967296"};
    pix_channel c;
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++)
        assert(pix_channel_parse(bad[i], PIXY_TERRESTRIAL, &c) < 0);
    assert(pix_channel_parse(NULL, PIXY_TERRESTRIAL, &c) < 0);
    assert(pix_channel_parse("T27", PIXY_TERRESTRIAL, NULL) < 0);
    uint32_t hz;
    assert(!pixy_frequency(PIXY_TERRESTRIAL, 27, &hz) && hz == 557142857);
    assert(!pixy_frequency(PIXY_BS, 1, &hz) && hz == 1049480000);
    assert(!pixy_frequency(PIXY_CS, 2, &hz) && hz == 1613000000);
}
static void test_slots(void) {
    /* Synthetic IDs deliberately contain holes; slot means physical array position. */
    const uint8_t ids[16] = {0, 0, 0x12, 0x34, 0xff, 0xff, 0x23, 0x45,
                             0, 0, 0x34, 0x56, 0, 0, 0xab, 0xcd};
    assert(pix_select_tsid(ids, -1, -1) == 0x1234);
    assert(pix_select_tsid(ids, -1, 0) < 0);
    assert(pix_select_tsid(ids, -1, 1) == 0x1234);
    assert(pix_select_tsid(ids, -1, 2) < 0);
    assert(pix_select_tsid(ids, -1, 3) == 0x2345);
    assert(pix_select_tsid(ids, -1, 7) == 0xabcd);
    assert(pix_select_tsid(ids, 0x3456, -1) == 0x3456);
    assert(pix_select_tsid(ids, 0xabcd, -1) == 0xabcd);
    assert(pix_select_tsid(ids, 0x9999, -1) < 0);
    assert(pix_select_tsid(ids, 0, -1) < 0);
    assert(pix_select_tsid(ids, 65535, -1) < 0);
    assert(pix_select_tsid(ids, 65536, -1) < 0);
    assert(pix_select_tsid(ids, 0x1234, 1) < 0); /* Explicit selection conflict. */
    assert(pix_select_tsid(ids, -1, -2) < 0);
    assert(pix_select_tsid(ids, -1, 8) < 0);
    assert(pix_select_tsid(NULL, -1, 0) < 0);
    const uint8_t empty[16] = {0};
    assert(pix_select_tsid(empty, -1, -1) < 0);
}
static void test_commands(void) {
    pix_command c;
    assert(pix_command_parse("channel T27", PIXY_BS, &c) == 1);
    assert(!c.quit && !c.frequency && c.channel.system == PIXY_TERRESTRIAL && c.channel.channel == 27);
    assert(pix_command_parse(" channel BS01_0 \r\n", PIXY_TERRESTRIAL, &c) == 1);
    assert(c.channel.system == PIXY_BS && c.channel.slot == 0);
    assert(pix_command_parse("channel 2", PIXY_CS, &c) == 1 && c.channel.system == PIXY_CS);
    assert(pix_command_parse("tune 545142857", PIXY_TERRESTRIAL, &c) == 1);
    assert(c.frequency && c.hz == 545142857 && !c.quit);
    assert(pix_command_parse("tune 4294967295", PIXY_TERRESTRIAL, &c) == 1 && c.hz == UINT32_MAX);
    assert(pix_command_parse("quit\t", PIXY_TERRESTRIAL, &c) == 1 && c.quit && !c.frequency);
    assert(pix_command_parse("", PIXY_TERRESTRIAL, &c) == 0);
    assert(pix_command_parse(" \t\r\n", PIXY_TERRESTRIAL, &c) == 0);
    const char *bad[] = {"channel", "channel 27 extra", "channel T53", "channel BS01_8", "quit now",
                         "tune", "tune 0", "tune -1", "tune +1", "tune 4294967296", "tune 1x",
                         "tune 99999999999999999999999999999999999999999999999999999999999999999",
                         "tune 1 2", "unknown 27", "quitx", "channel27"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++)
        assert(pix_command_parse(bad[i], PIXY_TERRESTRIAL, &c) < 0);
}
struct test_pipe { int writer, saved; };
static struct test_pipe begin_pipe(void) {
    int descriptors[2];
#ifdef _WIN32
    assert(!_pipe(descriptors, 4096, _O_BINARY));
#else
    assert(!pipe(descriptors));
#endif
    struct test_pipe p = {descriptors[1], fd_dup(0)};
    assert(p.saved >= 0);
    assert(fd_dup2(descriptors[0], 0) >= 0);
    close(descriptors[0]);
    return p;
}
static void send_bytes(struct test_pipe *p, const void *data, size_t n) {
    assert(write(p->writer, data, (unsigned)n) == (int)n);
}
static void end_pipe(struct test_pipe *p) {
    if (p->writer >= 0) close(p->writer);
    assert(fd_dup2(p->saved, 0) >= 0);
    close(p->saved);
}
static int poll_command(pix_control *input, pix_command *cmd) {
    for (unsigned i = 0; i < 10; i++) {
        int result = pix_control_poll(input, PIXY_TERRESTRIAL, cmd);
        if (result) return result;
    }
    return 0;
}
static void test_pipe_poll(void) {
    struct test_pipe p = begin_pipe();
    pix_control input = {0}; pix_command cmd;
    assert(pix_control_poll(&input, PIXY_TERRESTRIAL, &cmd) == 0); /* Open but idle writer. */
    send_bytes(&p, "channel T", 9);
    assert(pix_control_poll(&input, PIXY_TERRESTRIAL, &cmd) == 0);
    send_bytes(&p, "27\r\n\nquit\n", 10);
    assert(poll_command(&input, &cmd) == 1 && cmd.channel.channel == 27);
    assert(poll_command(&input, &cmd) == 1 && cmd.quit);
    assert(pix_control_poll(&input, PIXY_TERRESTRIAL, &cmd) == 0);
    close(p.writer); p.writer = -1;
    assert(poll_command(&input, &cmd) == 2);
    end_pipe(&p);

    p = begin_pipe(); memset(&input, 0, sizeof(input));
    send_bytes(&p, "channel BS01_7", 14);
    close(p.writer); p.writer = -1;
    assert(poll_command(&input, &cmd) == 1 && cmd.channel.slot == 7);
    assert(poll_command(&input, &cmd) == 2); /* No repeated EOF fragment. */
    end_pipe(&p);

    p = begin_pipe(); memset(&input, 0, sizeof(input));
    char oversized[600]; memset(oversized, 'x', sizeof(oversized));
    send_bytes(&p, oversized, sizeof(oversized));
    send_bytes(&p, "\nquit\n", 6);
    assert(poll_command(&input, &cmd) < 0);
    assert(poll_command(&input, &cmd) == 1 && cmd.quit); /* Discard only the bad line. */
    end_pipe(&p);

    p = begin_pipe(); memset(&input, 0, sizeof(input));
    send_bytes(&p, oversized, sizeof(oversized));
    close(p.writer); p.writer = -1;
    assert(poll_command(&input, &cmd) < 0);
    assert(poll_command(&input, &cmd) == 2);
    end_pipe(&p);
    p = begin_pipe(); memset(&input, 0, sizeof(input));
    const char nul[] = "channel T27\0junk\nquit\n";
    send_bytes(&p, nul, sizeof(nul) - 1);
    assert(poll_command(&input, &cmd) < 0);
    assert(poll_command(&input, &cmd) == 1 && cmd.quit);
    end_pipe(&p);
}
static void test_pids(void) {
    uint8_t data[188 * 4], expected[188 * 2], mask[8192] = {0};
    for (unsigned i = 0; i < 4; i++) {
        memset(data + i * 188, (int)i, 188);
        data[i * 188] = 0x47; data[i * 188 + 1] = 0; data[i * 188 + 2] = (uint8_t)(10 + i % 2);
    }
    memcpy(expected, data, 188); memcpy(expected + 188, data + 376, 188);
    mask[10] = 1; size_t n = sizeof(data);
    assert(!pix_pid_filter(data, &n, mask) && n == sizeof(expected) && !memcmp(data, expected, n));
    memset(mask, 1, sizeof(mask)); assert(!pix_pid_filter(data, &n, mask) && n == sizeof(expected));
    size_t bad = 187; assert(pix_pid_filter(data, &bad, mask) < 0);
    data[0] = 0; assert(pix_pid_filter(data, &n, mask) < 0);
    data[0] = 0x47; memset(mask, 0, sizeof(mask)); assert(!pix_pid_filter(data, &n, mask) && !n);
}
int main(void) {
    test_pids(); test_channels(); test_slots(); test_commands(); test_pipe_poll();
    puts("compat: channel tokens, satellite slots, commands, and stdin polling passed");
    return 0;
}
