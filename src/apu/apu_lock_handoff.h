/*
 * Lock handoff between the APU frame thread and the threads that wait on it.
 *
 * The frame thread holds the APU device lock for its whole loop and only
 * drops it inside condition waits. When emulation runs slower than real time
 * those waits never happen, and because neither CRITICAL_SECTION nor a pthread
 * mutex is fair, a plain unlock/relock lets the frame thread win again at
 * once: a guest thread waiting in an MMIO handler can starve for minutes.
 *
 * Waiters announce themselves in a counter; the frame thread calls
 * apu_lock_handoff() at a safe point and, while anyone is announced, releases
 * the lock until that waiter has taken it.
 *
 * Copyright (c) 2026 Burnout 3 Static Recompilation Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef MCPX_APU_LOCK_HANDOFF_H
#define MCPX_APU_LOCK_HANDOFF_H

#include "../nv2a/qemu_shim.h"

/* Most handoff rounds per call, so a steady stream of waiters cannot stall the holder. */
#define APU_LOCK_HANDOFF_MAX_ROUNDS 64

/* Most yields per round while waiting for an announced waiter to take the lock. */
#define APU_LOCK_HANDOFF_MAX_YIELDS 1024

/* Take m as a waiter the holder will hand off to; release with qemu_mutex_unlock. */
static inline void apu_lock_contended(QemuMutex *m, volatile LONG *waiters)
{
    InterlockedIncrement(waiters);
    qemu_mutex_lock(m);
    /* Decrement only once the lock is held, so the holder's relock waits for our unlock. */
    InterlockedDecrement(waiters);
}

/* Give the lock to announced waiters; the caller must hold m exactly once (no recursion). */
static inline void apu_lock_handoff(QemuMutex *m, volatile LONG *waiters)
{
    for (int round = 0; round < APU_LOCK_HANDOFF_MAX_ROUNDS; round++) {
        LONG before = InterlockedCompareExchange(waiters, 0, 0);
        if (before <= 0) {
            return;
        }
        qemu_mutex_unlock(m);
        /* A drop in the count means some waiter now holds the lock. */
        for (int y = 0; y < APU_LOCK_HANDOFF_MAX_YIELDS; y++) {
            if (InterlockedCompareExchange(waiters, 0, 0) < before) {
                break;
            }
            SwitchToThread();
        }
        qemu_mutex_lock(m);
    }
}

#endif /* MCPX_APU_LOCK_HANDOFF_H */
