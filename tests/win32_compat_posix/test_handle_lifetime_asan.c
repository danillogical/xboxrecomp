/*
 * Handle lifetime under AddressSanitizer.
 *
 * test_handle_lifetime.c pins what a bad handle makes an API RETURN. This
 * file pins what closing one makes the shim DO to memory, which is the half
 * a plain binary can survive by accident: a registry entry that outlives its
 * object, a reference released twice, or an object freed while a public call
 * is still inside it all leave the process running and only show up as a
 * use-after-free, a double free or a leak.
 *
 * The shapes are the ones the registry has to get right:
 *
 *   1. churn        create/use/close in a loop. The allocator hands the same
 *                   address back, so a handle value repeats across objects. A
 *                   table that kept a withdrawn entry would answer "live" for
 *                   a freed object on the next round and be diagnosed here.
 *   2. aliases      a duplicate keeps the value live past the first close, and
 *                   the last close frees it exactly once (a leak and a double
 *                   free both surface here)
 *   3. racing close many threads close the same value while others keep
 *                   calling public APIs on it. Exactly one close may succeed,
 *                   nobody may crash, and every call that lost the race must
 *                   fail cleanly rather than touch freed memory.
 *   4. pinned wait  a thread inside WaitForSingleObject while the last alias
 *                   closes: the wait must return rather than run on freed
 *                   memory. A blocking wait, so the pin is provably held.
 *
 * Everything is bounded: fixed iteration counts, fixed thread counts, and
 * waits with deadlines. Run with -fsanitize=address; leaks are reported by
 * default and no suppression is used, so an object the shim drops on the
 * floor fails this test too.
 */
#include "win32_compat.h"

#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int ok, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static void check(int ok, const char *fmt, ...)
{
    char name[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(name, sizeof name, fmt, ap);
    va_end(ap);
    printf("  %-64s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* ===================================================================== */
/* 1. Churn: values repeat, entries must not                          */
/* ===================================================================== */

#define CHURN_ROUNDS 400

static void test_churn(void)
{
    int live_each_round = 1;
    int reusable = 1;

    for (int i = 0; i < CHURN_ROUNDS; i++) {
        HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!ev) { live_each_round = 0; break; }
        if (!SetEvent(ev) || WaitForSingleObject(ev, 0) != WAIT_OBJECT_0)
            live_each_round = 0;
        if (!CloseHandle(ev)) live_each_round = 0;
        /* The value must be dead immediately: the next allocation may well
         * reuse the address, and a stale table entry would make this call
         * touch a freed object. */
        if (SetEvent(ev)) reusable = 0;
    }
    check(live_each_round,
          "each round's event is live, usable and closable (%d rounds)",
          CHURN_ROUNDS);
    check(reusable, "every closed value stays dead across %d reallocations",
          CHURN_ROUNDS);
}

/* ===================================================================== */
/* 2. Duplicate aliases: the value outlives the first close              */
/* ===================================================================== */

#define ALIAS_ROUNDS 100

static void test_alias_lifetime(void)
{
    int first_close_ok = 1, last_close_ok = 1, dead_after = 1;

    for (int i = 0; i < ALIAS_ROUNDS; i++) {
        HANDLE dup = NULL;
        HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!ev) { first_close_ok = 0; break; }

        if (!DuplicateHandle(GetCurrentProcess(), ev, GetCurrentProcess(), &dup,
                             0, FALSE, 0) || dup != ev)
            first_close_ok = 0;

        /* Two public aliases, one object: the first close must leave the value
         * live, and it must drop exactly one lifetime reference. Dropping none
         * leaks the object; dropping both frees it here. */
        if (!CloseHandle(ev)) first_close_ok = 0;
        if (!SetEvent(dup)) first_close_ok = 0;
        if (WaitForSingleObject(dup, 0) != WAIT_OBJECT_0) first_close_ok = 0;

        if (!CloseHandle(dup)) last_close_ok = 0;
        SetLastError(0);
        if (SetEvent(dup) || GetLastError() != ERROR_INVALID_HANDLE)
            dead_after = 0;
    }
    check(first_close_ok,
          "the value stays usable after the first of two aliases closes");
    check(last_close_ok, "closing the last alias succeeds (%d rounds)", ALIAS_ROUNDS);
    check(dead_after, "the value is dead after the last alias closes");
}

/* ===================================================================== */
/* 3. Racing close against public calls                                  */
/* ===================================================================== */

#define CALLERS      6
#define CLOSERS      4
#define CALLER_ITERS 400
#define CLOSER_ITERS 32

static HANDLE         g_race_handle;
static volatile LONG  g_race_start;
static volatile LONG  g_race_closes;      /* successful CloseHandle calls */
static volatile LONG  g_race_attempts;    /* CloseHandle calls made */
static volatile LONG  g_race_calls;       /* public calls that returned */
static volatile LONG  g_race_bad_result;  /* a call that neither worked nor failed cleanly */
static volatile LONG  g_race_done;

static void *race_caller(void *unused)
{
    (void)unused;
    HANDLE h = g_race_handle;
    while (InterlockedExchangeAdd(&g_race_start, 0) == 0)
        sched_yield();

    for (int i = 0; i < CALLER_ITERS; i++) {
        /* Before the close these succeed; after it they must fail with the
         * documented error. Anything else means a call read a dead object. */
        SetLastError(0);
        BOOL set = SetEvent(h);
        DWORD set_err = GetLastError();
        SetLastError(0);
        BOOL reset = ResetEvent(h);
        DWORD reset_err = GetLastError();
        SetLastError(0);
        DWORD waited = WaitForSingleObject(h, 0);
        DWORD wait_err = GetLastError();

        int ok = (set || set_err == ERROR_INVALID_HANDLE) &&
                 (reset || reset_err == ERROR_INVALID_HANDLE) &&
                 (waited == WAIT_OBJECT_0 || waited == WAIT_TIMEOUT ||
                  (waited == WAIT_FAILED && wait_err == ERROR_INVALID_HANDLE));
        if (!ok) InterlockedExchange(&g_race_bad_result, 1);
        InterlockedIncrement(&g_race_calls);
    }
    InterlockedIncrement(&g_race_done);
    return NULL;
}

static void *race_closer(void *unused)
{
    (void)unused;
    HANDLE h = g_race_handle;
    while (InterlockedExchangeAdd(&g_race_start, 0) == 0)
        sched_yield();

    for (int i = 0; i < CLOSER_ITERS; i++) {
        InterlockedIncrement(&g_race_attempts);
        if (CloseHandle(h))
            InterlockedIncrement(&g_race_closes);
        else if (GetLastError() != ERROR_INVALID_HANDLE)
            InterlockedExchange(&g_race_bad_result, 1);
        sched_yield();
    }
    InterlockedIncrement(&g_race_done);
    return NULL;
}

static void test_racing_close(void)
{
    pthread_t callers[CALLERS], closers[CLOSERS];
    int made = 0;

    g_race_handle = CreateEventA(NULL, TRUE, FALSE, NULL);
    check(g_race_handle != NULL, "CreateEventA for the racing case");
    if (!g_race_handle) return;

    g_race_start = 0;
    g_race_closes = 0;
    g_race_attempts = 0;
    g_race_calls = 0;
    g_race_bad_result = 0;
    g_race_done = 0;

    for (int i = 0; i < CALLERS; i++)
        if (pthread_create(&callers[i], NULL, race_caller, NULL) != 0) made = -1;
    for (int i = 0; i < CLOSERS && made == 0; i++)
        if (pthread_create(&closers[i], NULL, race_closer, NULL) != 0) made = -1;

    check(made == 0, "start %d callers and %d closers on one handle value",
          CALLERS, CLOSERS);
    if (made != 0) { CloseHandle(g_race_handle); return; }

    InterlockedExchange(&g_race_start, 1);
    for (int i = 0; i < CALLERS; i++) pthread_join(callers[i], NULL);
    for (int i = 0; i < CLOSERS; i++) pthread_join(closers[i], NULL);

    check(g_race_closes == 1,
          "exactly one of %ld closes of the same value succeeded",
          (long)g_race_attempts);
    check(g_race_attempts > 1, "the race actually attempted more than one close");
    check(g_race_calls == (long)CALLERS * CALLER_ITERS,
          "%ld concurrent public calls all returned", (long)g_race_calls);
    check(g_race_bad_result == 0,
          "every call either worked or failed with ERROR_INVALID_HANDLE");
    check(g_race_done == CALLERS + CLOSERS, "every racing thread finished");

    SetLastError(0);
    check(SetEvent(g_race_handle) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "the value is dead once the race is over");
}

/* ===================================================================== */
/* 4. A blocking wait pinned across the last close                       */
/* ===================================================================== */

static HANDLE         g_pin_handle;
static volatile LONG  g_pin_entered;
static volatile LONG  g_pin_result;

static void *pinned_waiter(void *unused)
{
    (void)unused;
    InterlockedExchange(&g_pin_entered, 1);
    /* Long enough that the close below lands inside the wait by a wide
     * margin; the wait still has its own deadline, so this cannot hang. */
    DWORD w = WaitForSingleObject(g_pin_handle, 2000);
    InterlockedExchange(&g_pin_result, (LONG)w);
    return NULL;
}

static void test_pinned_blocking_wait(void)
{
    pthread_t t;

    g_pin_handle = CreateEventA(NULL, TRUE, FALSE, NULL);
    check(g_pin_handle != NULL, "CreateEventA for the pinned-wait case");
    if (!g_pin_handle) return;

    g_pin_entered = 0;
    g_pin_result = -1;

    if (pthread_create(&t, NULL, pinned_waiter, NULL) != 0) {
        check(0, "pthread_create for the pinned waiter");
        CloseHandle(g_pin_handle);
        return;
    }
    while (InterlockedExchangeAdd(&g_pin_entered, 0) == 0)
        sched_yield();
    /* 20 ms after the waiter announced itself, against a 2000 ms wait. */
    Sleep(20);

    check(CloseHandle(g_pin_handle) == TRUE,
          "the last alias closes while the wait is inside the shim");
    pthread_join(t, NULL);

    check(g_pin_result == (LONG)WAIT_TIMEOUT,
          "the pinned wait returned at its own deadline (%ld)",
          (long)g_pin_result);
    SetLastError(0);
    check(WaitForSingleObject(g_pin_handle, 0) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "a later wait on the same value fails cleanly");
}

/* ===================================================================== */
/* 5. Timer-queue delete consumes the handle while the helper may run    */
/* ===================================================================== */

/*
 * DeleteTimerQueueTimer cancels AND withdraws the caller's handle, and the
 * helper thread holds its own reference until it observes the cancel. That is
 * the case where a lifetime bug shows up as a use-after-free rather than as a
 * wrong return value: the delete frees on behalf of the caller while the
 * helper is still inside the callback.
 *
 * Each round creates a one-shot timer that is due immediately, deletes it
 * without waiting for the callback, and therefore races the helper on
 * purpose. The helper's own reference is what keeps this legal; a shim that
 * released it on the caller's behalf would be diagnosed here.
 */
#define TIMER_ROUNDS 100

static volatile LONG g_timer_calls;

static VOID WINAPI asan_timer_cb(PVOID param, BOOLEAN timerOrWaitFired)
{
    (void)param; (void)timerOrWaitFired;
    InterlockedIncrement(&g_timer_calls);
}

static void test_timer_delete_lifetime(void)
{
    HANDLE queue = CreateTimerQueue();
    check(queue != NULL, "CreateTimerQueue for the delete-lifetime case");
    if (!queue) return;

    g_timer_calls = 0;
    int created = 1, deleted = 1, dead_after = 1;

    for (int i = 0; i < TIMER_ROUNDS; i++) {
        HANDLE t = NULL;
        if (!CreateTimerQueueTimer(&t, queue, asan_timer_cb, NULL, 0, 0,
                                   WT_EXECUTEONLYONCE) || !t) {
            created = 0;
            break;
        }
        if (!DeleteTimerQueueTimer(queue, t, INVALID_HANDLE_VALUE))
            deleted = 0;
        /* The value must be dead at once: the helper may still be running on
         * the same object, so a lookup that still found it would pin freed
         * memory. */
        SetLastError(0);
        if (ChangeTimerQueueTimer(queue, t, 0, 0) ||
            GetLastError() != ERROR_INVALID_HANDLE)
            dead_after = 0;
    }

    check(created, "CreateTimerQueueTimer succeeds for %d rounds", TIMER_ROUNDS);
    check(deleted, "every DeleteTimerQueueTimer returns TRUE");
    check(dead_after, "every deleted timer value is dead immediately");
    check(CloseHandle(queue) == TRUE, "CloseHandle of the queue succeeds");

    /* The helpers are one-shot and due immediately, so they exit right after
     * their callback; this only lets the last few finish before the leak
     * checker runs at exit. Nothing here depends on the exact value. */
    Sleep(100);
}

/* ===================================================================== */
/* main                                                                  */
/* ===================================================================== */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("win32 handle lifetime under AddressSanitizer (POSIX)\n\n");

    test_churn();
    printf("\n");
    test_alias_lifetime();
    printf("\n");
    test_racing_close();
    printf("\n");
    test_pinned_blocking_wait();
    printf("\n");
    test_timer_delete_lifetime();

    printf("\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
