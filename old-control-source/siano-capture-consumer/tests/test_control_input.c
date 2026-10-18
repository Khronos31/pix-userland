/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../control-input.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32

struct pipe_writer {
    HANDLE handle;
    BOOL write_ok;
    BOOL close_ok;
    DWORD written;
};

static DWORD WINAPI pipe_writer_thread(void *context)
{
    struct pipe_writer *writer = context;

    writer->write_ok = WriteFile(writer->handle, "command\n", 8,
                                 &writer->written, NULL);
    Sleep(2000);
    writer->close_ok = CloseHandle(writer->handle);
    return 0;
}

static void test_anonymous_pipe(void)
{
    struct siano_control_input input;
    HANDLE reader = NULL;
    HANDLE writer = NULL;
    HANDLE writer_thread;
    struct pipe_writer state = {0};
    uint8_t buffer[16];
    ULONGLONG start;
    ULONGLONG elapsed;
    int ready;

    assert(CreatePipe(&reader, &writer, NULL, 0));
    assert(siano_control_input_init(&input, reader) == 0);
    assert(siano_control_input_ready(&input) == 0);

    state.handle = writer;
    writer_thread = CreateThread(NULL, 0, pipe_writer_thread, &state, 0, NULL);
    assert(writer_thread != NULL);
    start = GetTickCount64();
    do {
        ready = siano_control_input_ready(&input);
        assert(ready >= 0);
        if (ready == 0)
            Sleep(1);
    } while (ready == 0 && GetTickCount64() - start < 1000);
    assert(ready == 1);

    start = GetTickCount64();
    assert(siano_control_input_read(&input, buffer, sizeof(buffer)) == 8);
    elapsed = GetTickCount64() - start;
    assert(elapsed < 1500);
    assert(memcmp(buffer, "command\n", 8) == 0);

    assert(WaitForSingleObject(writer_thread, 3000) == WAIT_OBJECT_0);
    assert(state.write_ok && state.written == 8 && state.close_ok);
    assert(siano_control_input_ready(&input) == 1);
    assert(siano_control_input_read(&input, buffer, sizeof(buffer)) == 0);
    assert(siano_control_input_ready(&input) == 1);
    assert(siano_control_input_read(&input, buffer, sizeof(buffer)) == 0);
    assert(CloseHandle(writer_thread));
    assert(CloseHandle(reader));
}

static void test_disk_input(void)
{
    struct siano_control_input input;
    char directory[MAX_PATH];
    char path[MAX_PATH];
    HANDLE file;
    DWORD count;
    uint8_t buffer[8];

    assert(GetTempPathA((DWORD)sizeof(directory), directory) != 0);
    assert(GetTempFileNameA(directory, "sci", 0, path) != 0);
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_TEMPORARY, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    assert(WriteFile(file, "abcdef", 6, &count, NULL) && count == 6);
    assert(CloseHandle(file));

    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    assert(siano_control_input_init(&input, file) == 0);
    assert(siano_control_input_ready(&input) == 1);
    assert(siano_control_input_read(&input, buffer, 3) == 3);
    assert(memcmp(buffer, "abc", 3) == 0);
    assert(siano_control_input_read(&input, buffer, sizeof(buffer)) == 3);
    assert(memcmp(buffer, "def", 3) == 0);
    assert(siano_control_input_ready(&input) == 1);
    assert(siano_control_input_read(&input, buffer, sizeof(buffer)) == 0);
    assert(CloseHandle(file));
    assert(DeleteFileA(path));
}

int main(void)
{
    struct siano_control_input input;

    assert(siano_control_input_init(&input, INVALID_HANDLE_VALUE) == -EBADF);
    test_anonymous_pipe();
    test_disk_input();
    puts("control input tests: PASS");
    return 0;
}

#else

#include <unistd.h>

int main(void)
{
    struct siano_control_input input;
    int descriptors[2];
    uint8_t buffer[16];

    assert(siano_control_input_init(&input, -1) == -EBADF);
    assert(pipe(descriptors) == 0);
    assert(siano_control_input_init(&input, descriptors[0]) == 0);
    assert(siano_control_input_ready(&input) == 0);
    assert(write(descriptors[1], "command\n", 8) == 8);
    assert(siano_control_input_ready(&input) == 1);
    assert(siano_control_input_read(&input, buffer, sizeof(buffer)) == 8);
    assert(memcmp(buffer, "command\n", 8) == 0);
    assert(siano_control_input_ready(&input) == 0);
    assert(close(descriptors[1]) == 0);
    assert(siano_control_input_ready(&input) == 1);
    assert(siano_control_input_read(&input, buffer, sizeof(buffer)) == 0);
    assert(close(descriptors[0]) == 0);
    puts("control input tests: PASS");
    return 0;
}

#endif
