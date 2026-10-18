/* SPDX-License-Identifier: MIT */
#include "control.h"
#include "os.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <poll.h>
#else
#include <conio.h>
#endif
int pix_command_parse(const char *line, pixy_system system, pix_command *cmd) {
    char op[16], value[64], extra;
    int n = sscanf(line, "%15s %63s %c", op, value, &extra);
    memset(cmd, 0, sizeof(*cmd));
    if (n < 1) return 0;
    if (n == 1 && !strcmp(op, "quit")) { cmd->quit = 1; return 1; }
    if (n != 2) return -1;
    if (!strcmp(op, "channel")) return pix_channel_parse(value, system, &cmd->channel) ? -1 : 1;
    if (!strcmp(op, "tune")) {
        uint64_t hz = 0;
        for (const char *p = value; *p; p++) {
            if (*p < '0' || *p > '9') return -1;
            hz = hz * 10 + (unsigned)(*p - '0');
            if (hz > UINT32_MAX) return -1;
        }
        if (!hz) return -1;
        cmd->frequency = 1; cmd->hz = (uint32_t)hz; return 1;
    }
    return -1;
}
int pix_control_poll(pix_control *input, pixy_system system, pix_command *cmd) {
    for (unsigned i = 0; i < 256; i++) {
#ifdef _WIN32
        HANDLE h = (HANDLE)_get_osfhandle(0);
        DWORD type = GetFileType(h), available = 0;
        int eof = 0;
        if (type == FILE_TYPE_PIPE) {
            if (!PeekNamedPipe(h, NULL, 0, NULL, &available, NULL)) {
                if (GetLastError() != ERROR_BROKEN_PIPE) return -1;
                eof = 1;
            }
            if (!available && !eof) return 0;
        } else if (type == FILE_TYPE_CHAR) {
            if (!_kbhit()) return 0;
        } else if (type != FILE_TYPE_DISK) return -1;
        char ch;
        int n = eof ? 0 : type == FILE_TYPE_CHAR ? (ch = (char)_getch(), 1) : _read(0, &ch, 1);
#else
        struct pollfd p = {0, POLLIN, 0};
        int ready = poll(&p, 1, 0);
        if (ready < 0) return errno == EINTR ? 0 : -1;
        if (!ready) return 0;
        char ch;
        ssize_t n = read(0, &ch, 1);
#endif
        if (n == 0) {
            if (!input->used && !input->discard) return 2;
            ch = '\n';
        } else if (n < 0) return errno == EINTR ? 0 : -1;
        if (!ch) { input->discard = 1; continue; }
        if (ch == '\r') continue;
        if (ch == '\n') {
            if (input->discard) { input->used = 0; input->discard = 0; return -1; }
            input->line[input->used] = 0; input->used = 0;
            int result = pix_command_parse(input->line, system, cmd);
            if (result) return result;
            if (!n) return 2;
        } else if (input->used + 1 < sizeof(input->line)) input->line[input->used++] = ch;
        else input->discard = 1;
    }
    return 0;
}
