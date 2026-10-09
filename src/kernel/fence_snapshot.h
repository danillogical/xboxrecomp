/*
 * fence_snapshot.h - the fence value a GPU kick publishes.
 *
 * A fence mirror copies the D3D device's submitted-fence counter into the
 * notify word the title polls. Copying it live tells the title the GPU has
 * consumed commands the submission walk may have rejected. Instead the
 * counter is sampled at each PUT write (a kick, held as pending) and becomes
 * visible only when a walk that consumed that kick commits (published). A walk
 * that stops short of PUT can still publish a semaphore release it committed
 * (fence_snapshot_partial), never past the pending counter.
 *
 * Until the first commit the live counter is used, so a run without a walk
 * that reports commits keeps the live mirror; a non-zero live_mode always
 * uses the live counter.
 *
 * Threading: kick, commit and partial are called by one writer at a time (the caller
 * serializes them; in the runtime the MMIO owner lock does). value may run
 * concurrently on another thread. commit and partial store published before setting
 * have_published, with release ordering, and value loads have_published before
 * published, with acquire ordering, so a reader that sees the flag sees a
 * published value from some commit, never an unwritten one. Both are aligned
 * 32-bit words, so neither load can tear.
 *
 * Zero-initialised is the fresh state. Header-only and host-independent.
 */
#ifndef FENCE_SNAPSHOT_H
#define FENCE_SNAPSHOT_H

#include <stdint.h>

typedef struct FenceSnapshot {
    uint32_t pending;                  /* counter sampled at the last kick */
    uint32_t kicked;                   /* a kick has been sampled */
    volatile uint32_t published;       /* counter as of the last committed kick */
    volatile uint32_t have_published;  /* a commit or partial has published */
} FenceSnapshot;

#if defined(__GNUC__) || defined(__clang__)
#define FENCE_SNAPSHOT_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define FENCE_SNAPSHOT_LOAD(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#elif defined(_MSC_VER)
#include <intrin.h>
#define FENCE_SNAPSHOT_STORE(p, v) \
    ((void)_InterlockedExchange((volatile long *)(p), (long)(v)))
#define FENCE_SNAPSHOT_LOAD(p) \
    ((uint32_t)_InterlockedCompareExchange((volatile long *)(p), 0, 0))
#else
#error "fence_snapshot.h needs GCC/Clang atomics or MSVC interlocked intrinsics"
#endif

/* Record the counter as it stood at a PUT write. */
static inline void fence_snapshot_kick(FenceSnapshot *s, uint32_t counter)
{
    s->pending = counter;
    s->kicked = 1;
}

/* A walk consumed everything up to the last kick: publish its counter. */
static inline void fence_snapshot_commit(FenceSnapshot *s)
{
    if (!s->kicked)
        return;
    FENCE_SNAPSHOT_STORE(&s->published, s->pending);
    FENCE_SNAPSHOT_STORE(&s->have_published, 1u);
}

/* D3D's fence insert (0x191390) writes the release parameter as the counter
 * value before advancing it by 2, and the wait at 0x191440 treats fence T as
 * done once the notify word has reached T, so a release committed before a
 * later rejection can be published without passing the kick's own value. */
static inline void fence_snapshot_partial(FenceSnapshot *s, uint32_t release)
{
    if (!s->kicked)
        return;
    if (s->have_published && (int32_t)(release - s->published) <= 0)
        return;
    if ((int32_t)(s->pending - release) < 0)
        return;
    FENCE_SNAPSHOT_STORE(&s->published, release);
    FENCE_SNAPSHOT_STORE(&s->have_published, 1u);
}

/* Whether value() would return the published counter in non-live mode. */
static inline int fence_snapshot_has_published(const FenceSnapshot *s)
{
    return FENCE_SNAPSHOT_LOAD(&((FenceSnapshot *)s)->have_published) != 0;
}

/* The value the mirror should write: live in live mode or before any commit,
 * otherwise the counter of the last committed kick. */
static inline uint32_t fence_snapshot_value(const FenceSnapshot *s, uint32_t live,
                                            int live_mode)
{
    if (live_mode || !fence_snapshot_has_published(s))
        return live;
    return FENCE_SNAPSHOT_LOAD(&((FenceSnapshot *)s)->published);
}

#endif /* FENCE_SNAPSHOT_H */
