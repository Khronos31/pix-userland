/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "queue-policy.h"

#include <errno.h>

int siano_thread_create_error(bool allocation_failure)
{
    return allocation_failure ? ENOMEM : EAGAIN;
}

bool siano_queue_note_drop(bool fail_on_drop, bool *closed, bool *overflowed)
{
    if (!fail_on_drop)
        return false;
    *closed = true;
    *overflowed = true;
    return true;
}

int siano_queue_overflow_result(bool overflowed)
{
    return overflowed ? -ENOBUFS : 0;
}

void siano_queue_retune_begin(uint64_t *epoch, bool *discarding)
{
    ++*epoch;
    *discarding = true;
}

bool siano_queue_epoch_accepts(uint64_t current_epoch, bool discarding,
                               uint64_t transfer_epoch)
{
    return !discarding && transfer_epoch == current_epoch;
}

bool siano_queue_retune_finish(int tune_result, uint64_t *epoch, bool *discarding,
                               size_t *head, size_t *tail, size_t *count,
                               size_t *hold_len)
{
    bool succeeded = tune_result == 0;

    if (succeeded) {
        *head = 0;
        *tail = 0;
        *count = 0;
        *hold_len = 0;
    }
    ++*epoch;
    *discarding = false;
    return succeeded;
}
