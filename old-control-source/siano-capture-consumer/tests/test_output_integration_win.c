/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Windows offline tests for the production output pump and stream loops. */
#define main siano_ts_program_main
#include "../siano-ts.c"
#undef main

#include <assert.h>
#include <signal.h>

static struct siano_device device;
static HANDLE watchdog_done;
static volatile LONG64 console_event_tick;

static unsigned __stdcall watchdog_thread(void *unused)
{
    (void)unused;
    if (WaitForSingleObject(watchdog_done, 15000) == WAIT_TIMEOUT)
        TerminateProcess(GetCurrentProcess(), 99);
    return 0;
}

static void test_channel_parser(void)
{
    char *t27[] = { "siano-ts", "--channel", "T27" };
    char *numeric[] = { "siano-ts", "--channel", "27" };
    struct options t_options;
    struct options n_options;
    uint32_t t_frequency;
    uint32_t n_frequency;

    optind = 1;
    assert(parse_options(3, t27, &t_options) == 0);
    optind = 1;
    assert(parse_options(3, numeric, &n_options) == 0);
    assert(t_options.have_channel && n_options.have_channel);
    assert(t_options.channel == 27 && n_options.channel == 27);
    assert(sms_channel_frequency(t_options.channel, &t_frequency) == 0);
    assert(sms_channel_frequency(n_options.channel, &n_frequency) == 0);
    assert(t_frequency == n_frequency);
}

static void diagnose_anonymous_pipe_mode_query(void)
{
    HANDLE reader;
    HANDLE writer;
    HANDLE duplicate = NULL;
    DWORD mode = 0;
    DWORD duplicate_mode = 0;
    DWORD error;
    BOOL original_query;
    BOOL duplicate_result;
    BOOL duplicate_query = FALSE;

    assert(CreatePipe(&reader, &writer, NULL, 4096));
    original_query = GetNamedPipeHandleState(writer, &mode, NULL, NULL, NULL,
                                             NULL, 0);
    error = original_query ? ERROR_SUCCESS : GetLastError();
    printf("CreatePipe original GetNamedPipeHandleState: BOOL=%d error=%lu",
           (int)original_query, (unsigned long)error);
    if (original_query)
        printf(" mode=0x%08lx", (unsigned long)mode);
    putchar('\n');

    duplicate_result = DuplicateHandle(GetCurrentProcess(), writer,
                                       GetCurrentProcess(), &duplicate,
                                       GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                                       FALSE, 0);
    error = duplicate_result ? ERROR_SUCCESS : GetLastError();
    printf("CreatePipe DuplicateHandle(GENERIC_WRITE|FILE_READ_ATTRIBUTES): BOOL=%d error=%lu",
           (int)duplicate_result, (unsigned long)error);
    if (duplicate_result) {
        duplicate_query = GetNamedPipeHandleState(duplicate, &duplicate_mode,
                                                  NULL, NULL, NULL, NULL, 0);
        error = duplicate_query ? ERROR_SUCCESS : GetLastError();
        printf("; duplicate GetNamedPipeHandleState: BOOL=%d error=%lu",
               (int)duplicate_query, (unsigned long)error);
        if (duplicate_query)
            printf(" mode=0x%08lx", (unsigned long)duplicate_mode);
        CloseHandle(duplicate);
    }
    putchar('\n');
    fflush(stdout);
    CloseHandle(writer);
    CloseHandle(reader);
}

static DWORD query_pipe_mode(HANDLE handle, const char *label)
{
    DWORD mode = 0;
    BOOL ok = GetNamedPipeHandleState(handle, &mode, NULL, NULL, NULL, NULL, 0);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();

    printf("%s GetNamedPipeHandleState: BOOL=%d error=%lu",
           label, (int)ok, (unsigned long)error);
    if (ok)
        printf(" mode=0x%08lx", (unsigned long)mode);
    putchar('\n');
    fflush(stdout);
    assert(ok);
    return mode;
}

static void diagnose_full_pipe_mode_query(int fd)
{
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    HANDLE duplicate = NULL;
    DWORD mode = 0;
    DWORD duplicate_mode = 0;
    DWORD error;
    BOOL original_query;
    BOOL duplicate_result;
    BOOL duplicate_query = FALSE;
    BOOL set_result = FALSE;

    original_query = GetNamedPipeHandleState(handle, &mode, NULL, NULL, NULL,
                                             NULL, 0);
    error = original_query ? ERROR_SUCCESS : GetLastError();
    printf("full-pipe fd=%d os_handle=%p original query: BOOL=%d error=%lu",
           fd, (void *)handle, (int)original_query, (unsigned long)error);
    if (original_query)
        printf(" mode=0x%08lx", (unsigned long)mode);

    duplicate_result = DuplicateHandle(GetCurrentProcess(), handle,
                                       GetCurrentProcess(), &duplicate,
                                       GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                                       FALSE, 0);
    error = duplicate_result ? ERROR_SUCCESS : GetLastError();
    printf("; DuplicateHandle: BOOL=%d error=%lu",
           (int)duplicate_result, (unsigned long)error);
    if (duplicate_result) {
        duplicate_query = GetNamedPipeHandleState(duplicate, &duplicate_mode,
                                                  NULL, NULL, NULL, NULL, 0);
        error = duplicate_query ? ERROR_SUCCESS : GetLastError();
        printf("; duplicate query: BOOL=%d error=%lu",
               (int)duplicate_query, (unsigned long)error);
        if (duplicate_query)
            printf(" mode=0x%08lx", (unsigned long)duplicate_mode);
    }
    putchar('\n');

    if (original_query || duplicate_query) {
        DWORD saved_mode = original_query ? mode : duplicate_mode;
        DWORD requested_mode = saved_mode | PIPE_NOWAIT;
        BOOL restored;

        set_result = SetNamedPipeHandleState(handle, &requested_mode, NULL, NULL);
        error = set_result ? ERROR_SUCCESS : GetLastError();
        printf("full-pipe SetNamedPipeHandleState(mode|PIPE_NOWAIT): BOOL=%d error=%lu\n",
               (int)set_result, (unsigned long)error);
        if (set_result) {
            restored = SetNamedPipeHandleState(handle, &saved_mode, NULL, NULL);
            error = restored ? ERROR_SUCCESS : GetLastError();
            printf("full-pipe restore mode: BOOL=%d error=%lu\n",
                   (int)restored, (unsigned long)error);
            assert(restored);
        }
    }
    fflush(stdout);
    if (duplicate_result)
        CloseHandle(duplicate);
}

static void fill_packet(uint8_t *buffer, size_t length)
{
    memset(buffer, 0, length);
    for (size_t offset = 0; offset + SIANO_OUTPUT_PACKET <= length;
         offset += SIANO_OUTPUT_PACKET)
        buffer[offset] = 0x47;
}

static int make_pipe(HANDLE *reader, bool initial_nowait)
{
    HANDLE writer;
    int fd;

    assert(CreatePipe(reader, &writer, NULL, 4096));
    if (initial_nowait) {
        DWORD mode = PIPE_NOWAIT;

        assert(SetNamedPipeHandleState(writer, &mode, NULL, NULL));
    }
    fd = _open_osfhandle((intptr_t)writer, _O_BINARY | _O_WRONLY);
    assert(fd >= 0);
    return fd;
}

static void fill_pipe_nowait(int fd)
{
    HANDLE writer = (HANDLE)_get_osfhandle(fd);
    uint8_t filler[4096] = {0};

    for (;;) {
        DWORD count = 0;

        if (!WriteFile(writer, filler, sizeof(filler), &count, NULL)) {
            DWORD error = GetLastError();

            assert(error == ERROR_NO_DATA || error == ERROR_PIPE_BUSY);
            return;
        }
        if (count == 0)
            return;
    }
}

static void test_full_wait_pipe_init_failure(void)
{
    static uint8_t filler[1024U * 1024U];
    HANDLE reader;
    HANDLE writer;
    HANDLE fd_handle;
    struct siano_output_pump pump;
    DWORD out_size = 0;
    DWORD available = 0;
    DWORD mode = 0;
    DWORD after_mode = 0;
    DWORD count;
    DWORD flags = 0;
    int fd;
    int rc;

    assert(CreatePipe(&reader, &writer, NULL, 4096));
    fd = _open_osfhandle((intptr_t)writer, _O_BINARY | _O_WRONLY);
    assert(fd >= 0);
    fd_handle = (HANDLE)_get_osfhandle(fd);
    assert(GetNamedPipeInfo(fd_handle, &flags, &out_size, NULL, NULL));
    assert(out_size != 0);
    assert(out_size <= sizeof(filler));
    assert(WriteFile(fd_handle, filler, out_size, &count, NULL));
    assert(count == out_size);
    assert(PeekNamedPipe(reader, NULL, 0, NULL, &available, NULL));
    assert(available >= out_size);
    mode = query_pipe_mode(fd_handle, "prefilled PIPE_WAIT before pump_init");
    assert((mode & PIPE_NOWAIT) == 0);
    diagnose_full_pipe_mode_query(fd);
    rc = siano_output_pump_init(&pump, fd);
    count = GetLastError();
    printf("prefilled PIPE_WAIT pump_init: rc=%d GetLastError=%lu\n",
           rc, (unsigned long)count);
    fflush(stdout);
    assert(rc == -EIO);
    after_mode = query_pipe_mode(fd_handle, "prefilled PIPE_WAIT after pump_init");
    assert(after_mode == mode);
    _close(fd);
    CloseHandle(reader);
}

static void make_device(void)
{
    assert(init_device_state(&device, NULL, false) == 0);
}

static uint8_t captured[SIANO_OUTPUT_PACKET * 2];
static size_t captured_len;
static size_t hook_size;

static int64_t partial_write(const uint8_t *data, size_t length)
{
    size_t count = length < hook_size ? length : hook_size;

    memcpy(captured + captured_len, data, count);
    captured_len += count;
    return (int64_t)count;
}

static void test_partial_and_zero_resume(void)
{
    HANDLE reader;
    int fd = make_pipe(&reader, false);
    struct siano_output_pump pump;
    uint8_t packet[SIANO_OUTPUT_PACKET];

    {
        int rc = siano_output_pump_init(&pump, fd);
        DWORD error = GetLastError();

        printf("first partial-test siano_output_pump_init: rc=%d GetLastError=%lu\n",
               rc, (unsigned long)error);
        fflush(stdout);
        assert(rc == 0);
    }
    fill_packet(packet, sizeof(packet));
    captured_len = 0;
    hook_size = 0;
    siano_output_test_set_write_hook(partial_write);
    siano_output_pump_feed(&pump, packet, sizeof(packet));
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_PENDING);
    assert(siano_output_pump_pending(&pump) == sizeof(packet));
    hook_size = 73;
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_PENDING);
    hook_size = sizeof(packet);
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    assert(captured_len == sizeof(packet));
    assert(memcmp(captured, packet, sizeof(packet)) == 0);
    siano_output_test_set_write_hook(NULL);
    siano_output_pump_destroy(&pump);
    _close(fd);
    CloseHandle(reader);
}

static void test_healthy_anonymous_pipe(void)
{
    HANDLE reader;
    HANDLE writer;
    int fd;
    struct siano_output_pump pump;
    uint8_t packet[SIANO_OUTPUT_PACKET * 2];
    uint8_t received[sizeof(packet)];
    DWORD count;

    assert(CreatePipe(&reader, &writer, NULL, 4096));
    fd = _open_osfhandle((intptr_t)writer, _O_BINARY | _O_WRONLY);
    assert(fd >= 0);
    assert(siano_output_pump_init(&pump, fd) == 0);
    fill_packet(packet, sizeof(packet));
    packet[1] = 0x22;
    packet[SIANO_OUTPUT_PACKET + 1] = 0x33;
    siano_output_pump_feed(&pump, packet, sizeof(packet));
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    siano_output_pump_destroy(&pump);
    assert(ReadFile(reader, received, sizeof(received), &count, NULL));
    assert(count == sizeof(received));
    assert(memcmp(received, packet, sizeof(packet)) == 0);
    _close(fd);
    CloseHandle(reader);
}

static void drain_pipe(HANDLE reader)
{
    uint8_t buffer[4096];

    for (;;) {
        DWORD available = 0;
        DWORD count;

        assert(PeekNamedPipe(reader, NULL, 0, NULL, &available, NULL));
        if (available == 0)
            return;
        if (available > sizeof(buffer))
            available = sizeof(buffer);
        assert(ReadFile(reader, buffer, available, &count, NULL));
        assert(count != 0);
    }
}

static void test_real_pipe_resume(void)
{
    HANDLE reader;
    int fd = make_pipe(&reader, false);
    struct siano_output_pump pump;
    uint8_t packet[SIANO_OUTPUT_PACKET];
    uint8_t received[sizeof(packet)];
    DWORD count;

    assert(siano_output_pump_init(&pump, fd) == 0);
    fill_pipe_nowait(fd);
    fill_packet(packet, sizeof(packet));
    siano_output_pump_feed(&pump, packet, sizeof(packet));
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_PENDING);
    assert(siano_output_pump_pending(&pump) == sizeof(packet));
    drain_pipe(reader);
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    siano_output_pump_destroy(&pump);
    assert(ReadFile(reader, received, sizeof(received), &count, NULL));
    assert(count == sizeof(received));
    assert(memcmp(received, packet, sizeof(packet)) == 0);
    _close(fd);
    CloseHandle(reader);
}

struct blocking_write_args {
    HANDLE writer;
    DWORD count;
};

static DWORD WINAPI blocking_write(void *arg)
{
    struct blocking_write_args *args = arg;
    uint8_t byte = 0x5a;

    return WriteFile(args->writer, &byte, 1, &args->count, NULL) ? 0 : GetLastError();
}

static void test_mode_restoration(void)
{
    HANDLE reader;
    int fd = make_pipe(&reader, false);
    struct siano_output_pump pump;
    struct blocking_write_args args;
    HANDLE thread;
    uint8_t byte;
    DWORD count;
    DWORD restored_mode;
    DWORD restore_error;

    assert(siano_output_pump_init(&pump, fd) == 0);
    fill_pipe_nowait(fd);
    siano_output_pump_destroy(&pump);
    restore_error = GetLastError();
    restored_mode = query_pipe_mode((HANDLE)_get_osfhandle(fd),
                                    "default WAIT after pump destroy");
    if (restored_mode & PIPE_NOWAIT) {
        printf("default WAIT restoration failed: GetLastError=%lu\n",
               (unsigned long)restore_error);
        fflush(stdout);
    }
    assert((restored_mode & PIPE_NOWAIT) == 0);
    args.writer = (HANDLE)_get_osfhandle(fd);
    args.count = 0;
    thread = CreateThread(NULL, 0, blocking_write, &args, 0, NULL);
    assert(thread != NULL);
    assert(WaitForSingleObject(thread, 100) == WAIT_TIMEOUT);
    assert(ReadFile(reader, &byte, 1, &count, NULL) && count == 1);
    assert(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0);
    assert(GetExitCodeThread(thread, &count) && count == 0);
    assert(args.count == 1);
    CloseHandle(thread);
    _close(fd);
    CloseHandle(reader);

    fd = make_pipe(&reader, true);
    assert(siano_output_pump_init(&pump, fd) == 0);
    fill_pipe_nowait(fd);
    siano_output_pump_destroy(&pump);
    restored_mode = query_pipe_mode((HANDLE)_get_osfhandle(fd),
                                    "preexisting NOWAIT after pump destroy");
    assert((restored_mode & PIPE_NOWAIT) != 0);
    args.writer = (HANDLE)_get_osfhandle(fd);
    args.count = 0;
    count = 0;
    if (WriteFile(args.writer, &byte, 1, &count, NULL))
        assert(count == 0);
    else
        assert(GetLastError() == ERROR_NO_DATA);
    _close(fd);
    CloseHandle(reader);
}

static unsigned __stdcall stop_after(void *unused)
{
    (void)unused;
    Sleep(100);
    assert(on_console(CTRL_C_EVENT) == TRUE);
    InterlockedExchange64(&console_event_tick, (LONG64)GetTickCount64());
    return 0;
}

struct fail_args {
    int error;
};

static unsigned __stdcall fail_after(void *arg)
{
    struct fail_args *args = arg;
    Sleep(100);
    siano_stream_state_fail(&device.state, args->error);
    return 0;
}

static unsigned __stdcall overflow_after(void *unused)
{
    uint8_t packet[SIANO_OUTPUT_PACKET];

    (void)unused;
    Sleep(100);
    fill_packet(packet, sizeof(packet));
    for (size_t i = 0; i <= TS_QUEUE_SLOTS; i++)
        ts_enqueue(&device.ts, 0, packet, sizeof(packet));
    return 0;
}

static void test_full_pipe_signal(void)
{
    HANDLE reader;
    HANDLE writer;
    int fd = make_pipe(&reader, false);
    struct siano_output_pump pump;
    uint8_t packet[SIANO_OUTPUT_PACKET * 2];
    HANDLE thread;
    ULONGLONG started;
    ULONGLONG returned;
    ULONGLONG event_tick;
    stop_requested = 0;
    InterlockedExchange64(&console_event_tick, 0);
    make_device();
    fill_packet(packet, sizeof(packet));
    ts_enqueue(&device.ts, 0, packet, sizeof(packet));
    assert(siano_output_pump_init(&pump, fd) == 0);
    fill_pipe_nowait(fd);
    thread = (HANDLE)_beginthreadex(NULL, 0, stop_after, NULL, 0, NULL);
    assert(thread != NULL);
    started = GetTickCount64();
    assert(stream_ts(&device, &pump, 0) == 0);
    returned = GetTickCount64();
    assert(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    event_tick = (ULONGLONG)InterlockedCompareExchange64(&console_event_tick, 0, 0);
    assert(stop_requested);
    assert(event_tick >= started && returned - event_tick < 2000);
    siano_output_pump_destroy(&pump);
    _close(fd);
    CloseHandle(reader);
    destroy_device_sync_state(&device);

    /* Exercise a normal inherited-handle shape with ordinary CreatePipe
     * access; the output setup must query the exact mode before changing it. */
    assert(CreatePipe(&reader, &writer, NULL, 4096));
    fd = _open_osfhandle((intptr_t)writer, _O_BINARY | _O_WRONLY);
    assert(fd >= 0);
    assert(siano_output_pump_init(&pump, fd) == 0);
    siano_output_pump_destroy(&pump);
    _close(fd);
    CloseHandle(reader);
}

static void test_duration(void)
{
    HANDLE reader;
    int fd = make_pipe(&reader, false);
    struct siano_output_pump pump;
    uint8_t packet[SIANO_OUTPUT_PACKET];
    ULONGLONG started;
    int rc;

    stop_requested = 0;
    make_device();
    fill_packet(packet, sizeof(packet));
    ts_enqueue(&device.ts, 0, packet, sizeof(packet));
    assert(siano_output_pump_init(&pump, fd) == 0);
    fill_pipe_nowait(fd);
    started = GetTickCount64();
    rc = stream_ts(&device, &pump, 1);
    assert(rc == 0);
    assert(GetTickCount64() - started >= 900);
    assert(GetTickCount64() - started < 2000);
    siano_output_pump_destroy(&pump);
    _close(fd);
    CloseHandle(reader);
    destroy_device_sync_state(&device);
}

static void test_usb_error_and_drop(void)
{
    HANDLE reader;
    int fd = make_pipe(&reader, false);
    struct siano_output_pump pump;
    struct fail_args args = { LIBUSB_ERROR_IO };
    uint8_t packet[SIANO_OUTPUT_PACKET];
    HANDLE thread;
    int rc;

    stop_requested = 0;
    make_device();
    fill_packet(packet, sizeof(packet));
    ts_enqueue(&device.ts, 0, packet, sizeof(packet));
    assert(siano_output_pump_init(&pump, fd) == 0);
    fill_pipe_nowait(fd);
    thread = (HANDLE)_beginthreadex(NULL, 0, fail_after, &args, 0, NULL);
    assert(thread != NULL);
    rc = stream_ts(&device, &pump, 0);
    assert(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    assert(rc == LIBUSB_ERROR_NO_DEVICE);
    siano_output_pump_destroy(&pump);
    _close(fd);
    CloseHandle(reader);
    destroy_device_sync_state(&device);

    fd = make_pipe(&reader, false);
    stop_requested = 0;
    make_device();
    ts_queue_set_fail_on_drop(&device.ts, true);
    ts_enqueue(&device.ts, 0, packet, sizeof(packet));
    assert(siano_output_pump_init(&pump, fd) == 0);
    fill_pipe_nowait(fd);
    thread = (HANDLE)_beginthreadex(NULL, 0, overflow_after, NULL, 0, NULL);
    assert(thread != NULL);
    {
        ULONGLONG started = GetTickCount64();

        rc = stream_ts(&device, &pump, 0);
        assert(GetTickCount64() - started < 2000);
    }
    assert(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    assert(rc == -ENOBUFS);
    assert(device.ts.drops == 1);
    siano_output_pump_destroy(&pump);
    _close(fd);
    CloseHandle(reader);
    destroy_device_sync_state(&device);
}

static void test_epipe(void)
{
    HANDLE reader;
    HANDLE writer;
    int fd;
    struct siano_output_pump pump;
    uint8_t packet[SIANO_OUTPUT_PACKET];

    assert(CreatePipe(&reader, &writer, NULL, 4096));
    fd = _open_osfhandle((intptr_t)writer, _O_BINARY | _O_WRONLY);
    assert(fd >= 0);
    assert(siano_output_pump_init(&pump, fd) == 0);
    CloseHandle(reader);
    fill_packet(packet, sizeof(packet));
    siano_output_pump_feed(&pump, packet, sizeof(packet));
    assert(siano_output_pump_step(&pump) == -EPIPE);
    siano_output_pump_destroy(&pump);
    _close(fd);
}

static void test_retune_partial_packet(void)
{
    HANDLE reader;
    int fd = make_pipe(&reader, false);
    struct siano_output_pump pump;
    uint8_t old_packets[SIANO_OUTPUT_PACKET * 3];
    uint8_t new_packet[SIANO_OUTPUT_PACKET];

    make_device();
    assert(siano_output_pump_init(&pump, fd) == 0);
    fill_packet(old_packets, sizeof(old_packets));
    old_packets[1] = 0x11;
    old_packets[SIANO_OUTPUT_PACKET + 1] = 0x22;
    old_packets[2 * SIANO_OUTPUT_PACKET + 1] = 0x33;
    fill_packet(new_packet, sizeof(new_packet));
    new_packet[1] = 0xa4;
    captured_len = 0;
    hook_size = 100;
    siano_output_test_set_write_hook(partial_write);
    ts_enqueue(&device.ts, device.ts.epoch, old_packets, sizeof(old_packets));
    siano_output_pump_feed(&pump, old_packets, sizeof(old_packets));
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_PENDING);
    assert(captured_len == 100);
    assert(pump.packet_off == 100);
    hook_size = 0;
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_PENDING);
    assert(captured_len == 100);

    ts_queue_retune_begin(&device.ts);
    assert(!stream_retune_finish(&device, &pump, -EIO));
    assert(siano_output_pump_busy(&pump));
    assert(pump.packet_off == 100);
    assert(pump.scratch_len == sizeof(old_packets));
    assert(pump.send_off == 0);
    assert(pump.packet_resume_off == 0);
    assert(device.ts.count == 1);

    ts_queue_retune_begin(&device.ts);
    assert(stream_retune_finish(&device, &pump, 0));
    assert(siano_output_pump_busy(&pump));
    assert(siano_output_pump_pending(&pump) == SIANO_OUTPUT_PACKET - 100);
    assert(pump.packet_off == 100);
    assert(pump.packet_resume_off == 100);
    assert(device.ts.count == 0);

    ts_enqueue(&device.ts, device.ts.epoch, old_packets, sizeof(old_packets));
    ts_queue_retune_begin(&device.ts);
    assert(stream_retune_finish(&device, &pump, 0));
    assert(siano_output_pump_pending(&pump) == SIANO_OUTPUT_PACKET - 100);
    assert(pump.packet_off == 100);
    assert(device.ts.count == 0);

    hook_size = 20;
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_PENDING);
    assert(captured_len == 120);
    assert(pump.packet_off == 120);
    assert(siano_output_pump_pending(&pump) == SIANO_OUTPUT_PACKET - 120);
    ts_enqueue(&device.ts, device.ts.epoch, old_packets, sizeof(old_packets));
    ts_queue_retune_begin(&device.ts);
    assert(stream_retune_finish(&device, &pump, 0));
    assert(siano_output_pump_pending(&pump) == SIANO_OUTPUT_PACKET - 120);
    assert(pump.packet_off == 120);
    assert(pump.packet_resume_off == 120);
    assert(device.ts.count == 0);

    hook_size = SIANO_OUTPUT_PACKET - 120;
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    assert(captured_len == SIANO_OUTPUT_PACKET);
    assert(memcmp(captured, old_packets, SIANO_OUTPUT_PACKET) == 0);
    hook_size = SIANO_OUTPUT_PACKET;
    siano_output_pump_feed(&pump, new_packet, sizeof(new_packet));
    assert(siano_output_pump_step(&pump) == SIANO_OUTPUT_DONE);
    assert(captured_len == SIANO_OUTPUT_PACKET * 2);
    assert(memcmp(captured + SIANO_OUTPUT_PACKET, new_packet,
                  SIANO_OUTPUT_PACKET) == 0);
    assert(captured[0] == 0x47);
    assert(captured[SIANO_OUTPUT_PACKET] == 0x47);
    assert(captured[SIANO_OUTPUT_PACKET + 1] == 0xa4);

    siano_output_test_set_write_hook(NULL);
    siano_output_pump_destroy(&pump);
    _close(fd);
    CloseHandle(reader);
    destroy_device_sync_state(&device);
}

static void test_control_quit(void)
{
    HANDLE input_reader;
    HANDLE input_writer;
    HANDLE old_input;
    HANDLE output_reader;
    int output_fd = make_pipe(&output_reader, false);
    struct siano_output_pump pump;
    struct options options;
    uint8_t packet[SIANO_OUTPUT_PACKET];
    bool pid_filters_added = false;
    DWORD written;

    memset(&options, 0, sizeof(options));
    assert(CreatePipe(&input_reader, &input_writer, NULL, 4096));
    assert(WriteFile(input_writer, "quit\n", 5, &written, NULL));
    old_input = GetStdHandle(STD_INPUT_HANDLE);
    assert(SetStdHandle(STD_INPUT_HANDLE, input_reader));
    stop_requested = 0;
    make_device();
    fill_packet(packet, sizeof(packet));
    ts_enqueue(&device.ts, 0, packet, sizeof(packet));
    assert(siano_output_pump_init(&pump, output_fd) == 0);
    fill_pipe_nowait(output_fd);
    assert(stream_ts_control(&device, &options, &pid_filters_added, &pump) == 0);
    siano_output_pump_destroy(&pump);
    assert(SetStdHandle(STD_INPUT_HANDLE, old_input));
    CloseHandle(input_reader);
    CloseHandle(input_writer);
    _close(output_fd);
    CloseHandle(output_reader);
    destroy_device_sync_state(&device);
}

int main(void)
{
    HANDLE watchdog;

    watchdog_done = CreateEvent(NULL, TRUE, FALSE, NULL);
    assert(watchdog_done != NULL);
    watchdog = (HANDLE)_beginthreadex(NULL, 0, watchdog_thread, NULL, 0, NULL);
    assert(watchdog != NULL);
    test_channel_parser();
    diagnose_anonymous_pipe_mode_query();
    test_healthy_anonymous_pipe();
    test_partial_and_zero_resume();
    test_real_pipe_resume();
    test_full_pipe_signal();
    test_duration();
    test_usb_error_and_drop();
    test_control_quit();
    test_epipe();
    test_retune_partial_packet();
    test_full_wait_pipe_init_failure();
    test_mode_restoration();
    SetEvent(watchdog_done);
    assert(WaitForSingleObject(watchdog, 2000) == WAIT_OBJECT_0);
    CloseHandle(watchdog);
    CloseHandle(watchdog_done);
    puts("Windows output integration tests: PASS");
    return 0;
}
