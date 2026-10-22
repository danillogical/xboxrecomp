/*
 * Interlocked operations, MemoryBarrier and GetThreadId in the POSIX Win32 shim.
 *
 * These pin Win32 semantics, not the shim's implementation:
 *
 *   1. return values   InterlockedOr/And/Exchange64 return the value held
 *                      before the operation, InterlockedIncrement64 the new one
 *   2. widths          LONG ops touch 32 bits, LONGLONG ops 64; the high bit
 *                      and all-ones patterns survive without sign damage
 *   3. atomicity       many threads hitting one word lose no update
 *   4. GetThreadId     the id CreateThread reports, the id the thread sees for
 *                      itself, the pseudo-handle, and 0 for a bad handle
 *   5. MemoryBarrier   only that it can be called; a fence has no result a
 *                      single-threaded check could observe
 */
#include "win32_compat.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

#define NTHREADS 8
#define ITERS    200000

/* ---- 1/2. Return values and resulting memory --------------------------- */

static void test_or_and(void)
{
    static const LONG pats[] = { 0, -1, (LONG)0x80000000u, 0x7FFFFFFF,
                                 0x0F0F0F0F, (LONG)0xF0F0F0F0u, 1 };
    const int n = (int)(sizeof pats / sizeof pats[0]);
    int or_ok = 1, and_ok = 1;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            volatile LONG m = pats[i];
            LONG r = InterlockedOr(&m, pats[j]);
            if (r != pats[i] || m != (LONG)(pats[i] | pats[j])) or_ok = 0;
            m = pats[i];
            r = InterlockedAnd(&m, pats[j]);
            if (r != pats[i] || m != (LONG)(pats[i] & pats[j])) and_ok = 0;
        }
    }
    check(or_ok, "InterlockedOr returns the original, stores a|b (49 pairs)");
    check(and_ok, "InterlockedAnd returns the original, stores a&b (49 pairs)");

    volatile LONG m = 0x10;
    check(InterlockedOr(&m, 0x01) == 0x10 && m == 0x11,
          "InterlockedOr: returned value is not the new value");
    m = 0x11;
    check(InterlockedAnd(&m, 0x10) == 0x11 && m == 0x10,
          "InterlockedAnd: returned value is not the new value");

    /* Width: a LONG op must not touch the neighbouring 32 bits. */
    volatile LONG pair[2] = { 0, (LONG)0xDEADBEEFu };
    InterlockedOr(&pair[0], -1);
    check(pair[0] == -1 && pair[1] == (LONG)0xDEADBEEFu,
          "InterlockedOr on a LONG leaves the adjacent LONG alone");
    pair[0] = -1;
    InterlockedAnd(&pair[0], 0);
    check(pair[0] == 0 && pair[1] == (LONG)0xDEADBEEFu,
          "InterlockedAnd on a LONG leaves the adjacent LONG alone");
}

static void test_64(void)
{
    static const LONGLONG pats[] = {
        0, 1, -1, (LONGLONG)0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFll,
        0x00000000FFFFFFFFll, 0xFFFFFFFF00000000ull, 0x123456789ABCDEF0ll };
    const int n = (int)(sizeof pats / sizeof pats[0]);
    int ex_ok = 1;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            volatile LONGLONG m = pats[i];
            LONGLONG r = InterlockedExchange64(&m, pats[j]);
            if (r != pats[i] || m != pats[j]) ex_ok = 0;
        }
    check(ex_ok, "InterlockedExchange64 returns the old value, stores new (64 pairs)");

    struct { LONGLONG in, out; } inc[] = {
        { 0, 1 }, { -1, 0 }, { 0xFFFFFFFFll, 0x100000000ll },
        { 0x7FFFFFFFFFFFFFFFll, (LONGLONG)0x8000000000000000ull },
        { (LONGLONG)0x8000000000000000ull, (LONGLONG)0x8000000000000001ull },
    };
    int inc_ok = 1;
    for (size_t i = 0; i < sizeof inc / sizeof inc[0]; i++) {
        volatile LONGLONG m = inc[i].in;
        LONGLONG r = InterlockedIncrement64(&m);
        if (r != inc[i].out || m != inc[i].out) inc_ok = 0;
    }
    check(inc_ok, "InterlockedIncrement64 returns the NEW value (carry across 32 bits)");

    /* Width: the 64-bit ops carry into the high word; 32-bit ops must not. */
    volatile LONGLONG w = 0xFFFFFFFFll;
    InterlockedIncrement64(&w);
    check(w == 0x100000000ll, "InterlockedIncrement64 carries into the high 32 bits");

    volatile LONGLONG x = 0;
    LONGLONG old = InterlockedExchange64(&x, (LONGLONG)0xFFFFFFFF80000000ull);
    check(old == 0 && x == (LONGLONG)0xFFFFFFFF80000000ull,
          "InterlockedExchange64 stores all 64 bits");
}

/* ---- 3. Atomicity ------------------------------------------------------- */

static volatile LONG     g_or_word;
static volatile LONG     g_and_word;
static volatile LONGLONG g_counter;
static volatile LONGLONG g_xchg_sum;
static volatile LONG     g_start;

static DWORD WINAPI hammer(LPVOID p)
{
    LONG idx = (LONG)(LONG_PTR)p;
    while (!g_start) { }
    for (int i = 0; i < ITERS; i++) {
        /* A different bit per thread, toggled: set then clear a private bit
         * while the other threads do the same to theirs. A lost update
         * flips another thread's bit. */
        InterlockedOr(&g_or_word, 1 << idx);
        InterlockedAnd(&g_and_word, ~(1 << (idx + 8)));
        InterlockedIncrement64(&g_counter);
    }
    return 0;
}

static void test_atomicity(void)
{
    HANDLE h[NTHREADS];
    g_or_word = 0;
    g_and_word = (LONG)0xFFFFFFFFu;
    g_counter = 0;
    g_start = 0;
    for (int i = 0; i < NTHREADS; i++) {
        h[i] = CreateThread(NULL, 0, hammer, (LPVOID)(LONG_PTR)i, 0, NULL);
    }
    g_start = 1;
    for (int i = 0; i < NTHREADS; i++) {
        WaitForSingleObject(h[i], INFINITE);
        CloseHandle(h[i]);
    }
    check(g_counter == (LONGLONG)NTHREADS * ITERS,
          "InterlockedIncrement64: 8 threads x 200000 lose no increment");
    check(g_or_word == (1 << NTHREADS) - 1,
          "InterlockedOr: every thread's bit is set at the end");
    check((LONG)g_and_word == (LONG)(0xFFFFFFFFu & ~(((1u << NTHREADS) - 1) << 8)),
          "InterlockedAnd: every thread's bit is cleared at the end");
}

/* Racing set/clear on shared bits: each thread owns one bit and flips it
 * back and forth with Or/And; the other bits must come through intact. */
static volatile LONG g_flip_word;

static DWORD WINAPI flipper(LPVOID p)
{
    LONG idx = (LONG)(LONG_PTR)p;
    while (!g_start) { }
    for (int i = 0; i < ITERS; i++) {
        InterlockedOr(&g_flip_word, 1 << idx);
        InterlockedAnd(&g_flip_word, ~(1 << idx));
    }
    InterlockedOr(&g_flip_word, 1 << idx);
    return 0;
}

static void test_flip(void)
{
    HANDLE h[NTHREADS];
    g_flip_word = 0;
    g_start = 0;
    for (int i = 0; i < NTHREADS; i++)
        h[i] = CreateThread(NULL, 0, flipper, (LPVOID)(LONG_PTR)i, 0, NULL);
    g_start = 1;
    for (int i = 0; i < NTHREADS; i++) {
        WaitForSingleObject(h[i], INFINITE);
        CloseHandle(h[i]);
    }
    check(g_flip_word == (1 << NTHREADS) - 1,
          "Or/And racing on distinct bits of one word: no bit is lost");
}

/* ---- 4. GetThreadId ----------------------------------------------------- */

static volatile DWORD g_seen_id;
static volatile DWORD g_seen_via_current;
static volatile DWORD g_seen_via_pseudo;

static DWORD WINAPI report_ids(LPVOID unused)
{
    (void)unused;
    g_seen_id = GetCurrentThreadId();
    g_seen_via_current = GetThreadId(GetCurrentThread());
    g_seen_via_pseudo = GetThreadId((HANDLE)(LONG_PTR)-2);
    return 0;
}

static void test_thread_id(void)
{
    DWORD tid = 0;
    HANDLE h = CreateThread(NULL, 0, report_ids, NULL, 0, &tid);
    check(h != NULL && tid != 0, "CreateThread returns a handle and a nonzero id");
    WaitForSingleObject(h, INFINITE);

    check(GetThreadId(h) == tid, "GetThreadId(handle) == id from CreateThread");
    check(g_seen_id == tid, "GetCurrentThreadId inside the thread == that id");
    check(g_seen_via_current == g_seen_id,
          "inside the thread GetThreadId(GetCurrentThread()) == own id");
    check(g_seen_via_pseudo == g_seen_id,
          "inside the thread GetThreadId(pseudo-handle -2) == own id");
    check(GetThreadId(h) == tid, "the id is stable once the thread has exited");

    DWORD tid2 = 0;
    HANDLE h2 = CreateThread(NULL, 0, report_ids, NULL, 0, &tid2);
    WaitForSingleObject(h2, INFINITE);
    check(tid2 != 0 && tid2 != tid && GetThreadId(h2) == tid2,
          "a second thread has a distinct id");
    check(GetThreadId(h) != GetThreadId(h2), "...and the handles map to different ids");

    DWORD self = GetCurrentThreadId();
    check(self != 0 && self != tid && self != tid2,
          "the main thread's id is nonzero and distinct");
    check(GetThreadId(GetCurrentThread()) == self,
          "GetThreadId(GetCurrentThread()) == GetCurrentThreadId() on main");

    check(GetThreadId(NULL) == 0, "GetThreadId(NULL) == 0");
    check(GetThreadId(INVALID_HANDLE_VALUE) == 0,
          "GetThreadId(INVALID_HANDLE_VALUE) == 0");
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    check(ev && GetThreadId(ev) == 0, "GetThreadId(non-thread handle) == 0");
    if (ev) CloseHandle(ev);

    CloseHandle(h);
    CloseHandle(h2);
}

/* ---- 5. MemoryBarrier --------------------------------------------------- */

static void test_barrier(void)
{
    /* Behaviour of a fence is not observable here; this only proves it links
     * and returns, and leaves surrounding stores alone. */
    volatile LONG v = 7;
    MemoryBarrier();
    check(v == 7, "MemoryBarrier is callable and returns (no behavioural check)");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_or_and();
    test_64();
    test_atomicity();
    test_flip();
    test_thread_id();
    test_barrier();
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
