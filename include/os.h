#ifndef PIX_OS_H
#define PIX_OS_H
#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <windows.h>
typedef CRITICAL_SECTION os_mutex;
typedef CONDITION_VARIABLE os_cond;
typedef HANDLE os_thread;
static inline int os_mutex_init(os_mutex *m, void *unused) {
    (void)unused;
    InitializeCriticalSection(m);
    return 0;
}
static inline void os_mutex_destroy(os_mutex *m) {
    DeleteCriticalSection(m);
}
static inline void os_mutex_lock(os_mutex *m) {
    EnterCriticalSection(m);
}
static inline void os_mutex_unlock(os_mutex *m) {
    LeaveCriticalSection(m);
}
static inline int os_cond_init(os_cond *c, void *unused) {
    (void)unused;
    InitializeConditionVariable(c);
    return 0;
}
static inline void os_cond_destroy(os_cond *c) {
    (void)c;
}
static inline void os_cond_signal(os_cond *c) {
    WakeConditionVariable(c);
}
static inline void os_cond_wait(os_cond *c, os_mutex *m) {
    SleepConditionVariableCS(c, m, INFINITE);
}
struct os_thread_call {
    void *(*fn)(void *);
    void *arg;
};
static inline DWORD WINAPI os_thread_entry(void *p) {
    struct os_thread_call call = *(struct os_thread_call *)p;
    free(p);
    call.fn(call.arg);
    return 0;
}
static inline int os_thread_create(os_thread *t, void *unused, void *(*fn)(void *), void *arg) {
    (void)unused;
    struct os_thread_call *p = malloc(sizeof(*p));
    if (!p)
        return -1;
    p->fn = fn;
    p->arg = arg;
    *t = CreateThread(NULL, 0, os_thread_entry, p, 0, NULL);
    if (!*t) {
        free(p);
        return -1;
    }
    return 0;
}
/* Repeated cancellation also covers the narrow interval before a blocking write starts. */
static inline int os_thread_join(os_thread t, int cancel_io) {
    if (!t || t == INVALID_HANDLE_VALUE)
        return -1;
    DWORD timeout = cancel_io ? 50 : 3500;
    DWORD result;
    while ((result = WaitForSingleObject(t, timeout)) == WAIT_TIMEOUT) {
        CancelSynchronousIo(t);
        timeout = 50;
    }
    if (result != WAIT_OBJECT_0)
        return -1;
    return CloseHandle(t) ? 0 : -1;
}
#define open _open
#define close _close
#define write _write
#define isatty _isatty
#define STDOUT_FILENO 1
#ifndef O_BINARY
#define O_BINARY _O_BINARY
#endif
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_mutex_t os_mutex;
typedef pthread_cond_t os_cond;
typedef pthread_t os_thread;
#define os_mutex_init pthread_mutex_init
#define os_mutex_destroy pthread_mutex_destroy
#define os_mutex_lock pthread_mutex_lock
#define os_mutex_unlock pthread_mutex_unlock
#define os_cond_init pthread_cond_init
#define os_cond_destroy pthread_cond_destroy
#define os_cond_signal pthread_cond_signal
#define os_cond_wait pthread_cond_wait
#define os_thread_create pthread_create
static inline int os_thread_join(os_thread t, int cancel_io) {
    (void)cancel_io;
    return pthread_join(t, NULL);
}
#define O_BINARY 0
#endif
int os_output_prepare(int fd);
int os_output_is_terminal(int fd);
void os_output_restore(int fd, int state);
#endif
