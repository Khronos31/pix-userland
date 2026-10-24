/* SPDX-License-Identifier: MIT */
#include "os.h"
#include <stdio.h>

static void *worker(void *context)
{
    *(int *)context = 1;
    return NULL;
}

int main(void)
{
    os_thread thread;
    int completed = 0;
    if (os_thread_create(&thread, NULL, worker, &completed) != 0) return 1;
    if (os_thread_join(thread, 0) != 0 || completed != 1) return 1;
    if (os_thread_join(INVALID_HANDLE_VALUE, 0) == 0) return 1;
    puts("Windows os_thread create/join/failure: PASS");
    return 0;
}
