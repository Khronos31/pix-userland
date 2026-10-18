/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIANO_QUEUE_POLICY_H
#define SIANO_QUEUE_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Called with the queue mutex held after a full-queue drop. */
bool siano_queue_note_drop(bool fail_on_drop, bool *closed, bool *overflowed);
int siano_queue_overflow_result(bool overflowed);
int siano_thread_create_error(bool allocation_failure);
/* Retune lifecycle and epoch checks are called while the queue mutex is held. */
void siano_queue_retune_begin(uint64_t *epoch, bool *discarding);
bool siano_queue_epoch_accepts(uint64_t current_epoch, bool discarding,
                               uint64_t transfer_epoch);
bool siano_queue_retune_finish(int tune_result, uint64_t *epoch, bool *discarding,
                               size_t *head, size_t *tail, size_t *count,
                               size_t *hold_len);

#endif
