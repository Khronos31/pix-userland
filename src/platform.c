#include "os.h"
#include "pixy.h"
#include <errno.h>
#include <limits.h>
#ifdef _WIN32
#include <bcrypt.h>
uint64_t now_ms(void) {
    return GetTickCount64();
}
void sleep_ms(unsigned ms) {
    Sleep(ms);
}
int random_bytes(void *p, size_t n) {
    if (n > ULONG_MAX)
        return -1;
    return BCryptGenRandom(NULL, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
}
int os_output_prepare(int fd) {
    return _setmode(fd, _O_BINARY);
}
void os_output_restore(int fd, int state) {
    _setmode(fd, state);
}
#else
#include <fcntl.h>
#include <time.h>
uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (unsigned)t.tv_nsec / 1000000;
}
void sleep_ms(unsigned ms) {
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    while (nanosleep(&t, &t) && errno == EINTR) {
    }
}
int os_output_prepare(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return flags;
}
void os_output_restore(int fd, int state) {
    fcntl(fd, F_SETFL, state);
}
int random_bytes(void *p, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    uint8_t *v = p;
    if (fd < 0)
        return -1;
    while (n) {
        ssize_t r = read(fd, v, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            close(fd);
            return -1;
        }
        v += r;
        n -= (size_t)r;
    }
    close(fd);
    return 0;
}

#endif
