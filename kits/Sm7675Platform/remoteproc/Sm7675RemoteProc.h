/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SM7675_REMOTE_PROC_H
#define SM7675_REMOTE_PROC_H

#include <stdint.h>
#include "QcomSmem.h"

enum class Sm7675Remote : unsigned { Wpss, Adsp };

struct Sm7675RpmhVote {
    uint32_t address;
    uint32_t value;
};

// An IRQ handler callback may only record state and wake its worker.
int sm7675_remote_smem_alloc(volatile uint8_t *mutex, QcomSmem *smem, QcomSmemPartition *partition,
                             uint16_t item, uint32_t bytes);
int sm7675_remote_attach(Sm7675Remote owner, void (*doorbell)(uint32_t signal));
void sm7675_remote_ring(Sm7675Remote owner, uint32_t signal);
// Sleepable, serialized requests. An unacknowledged transaction is never overwritten.
int sm7675_remote_qmp(const char *message);
// Replace this owner's votes, or pass zero entries to release them.
int sm7675_remote_vote(Sm7675Remote owner, const Sm7675RpmhVote *votes, unsigned count);

#endif
