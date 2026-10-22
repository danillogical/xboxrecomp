/*
 * Handle provenance and lifetime in the POSIX Win32 shim.
 *
 * POSIX has no kernel handle table, so the shim publishes every object it
 * creates in a live-handle registry keyed by the HANDLE value. A HANDLE is
 * still the object pointer (the guest bridge maps them as opaque 32-bit
 * values), which means nothing about a value proves it names live memory:
 * 0x03fffffe, a zeroed calloc block, a freed value and a value from another
 * object kind all look like pointers. The registry is the only authority, and
 * every public API has to consult it BEFORE dereferencing.
 *
 * These checks pin that contract:
 *
 *   1. semantics   events, semaphores, mutexes, threads, waitable timers,
 *                  timer-queue timers and file mappings behave as before (the
 *                  repair must not change what a valid handle does)
 *   2. aliases     DuplicateHandle returns the same value; the value lives
 *                  until the LAST public alias is closed
 *   3. rejection   forged, zeroed, null, closed and wrong-kind values fail
 *                  with the documented error and never touch memory
 *   4. pseudo      the current-thread/current-process handles and the value
 *                  GetCurrentThread() reports stay usable
 *   5. pinning     a public call in progress keeps the object alive across a
 *                  close of its last alias
 *
 * "Never touch memory" is checked by construction, not by inspection: the
 * forged values are an unmapped address and a zeroed heap block, so a shim
 * that dereferenced them before validating would fault or accept garbage
 * rather than print FAIL. The companion test_handle_lifetime_asan.c runs the
 * same shapes under AddressSanitizer, where a use-after-free is diagnosed
 * instead of merely surviving.
 *
 * Controls hook: a private harness re-runs this binary against deliberately
 * weakened copies of the shim. A crash on the first forged row would hide
 * every row after it, so W32_HANDLE_TEST_ONLY=invalid-addr|zeroed runs that
 * one negative row and nothing else. Unset -- the normal case, and what CI
 * runs -- the whole file runs.
 */
#include "win32_compat.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static const char *only_row;   /* controls hook; NULL runs everything */

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
    printf("  %-66s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* ---- small sync helpers ------------------------------------------------ */

/* Poll a shim atomic up to `ms` milliseconds. Sleep(1) yields the CPU, so a
 * thread that is spinning here does not starve the thread it waits for. */
static int wait_flag(volatile LONG *flag, LONG want, int ms)
{
    for (int i = 0; i <= ms; i++) {
        if (InterlockedExchangeAdd(flag, 0) == want) return 1;
        if (i < ms) Sleep(1);
    }
    return 0;
}

static int wait_flag_ne(volatile LONG *flag, LONG notwant, int ms)
{
    for (int i = 0; i <= ms; i++) {
        if (InterlockedExchangeAdd(flag, 0) != notwant) return 1;
        if (i < ms) Sleep(1);
    }
    return 0;
}

/* An address that is not mapped in this process on any host this shim runs
 * on: the first pages are left unmapped precisely so a stray dereference
 * faults instead of reading something. */
#define FORGED ((HANDLE)(LONG_PTR)0x03fffffe)

/* ===================================================================== */
/* 1. Valid behaviour, unchanged                                         */
/* ===================================================================== */

static void test_events(void)
{
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    check(ev != NULL, "CreateEventA(manual-reset) returns a handle");
    if (!ev) return;

    check(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT,
          "a fresh manual-reset event is unsignalled");
    check(SetEvent(ev) == TRUE, "SetEvent succeeds");
    check(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0,
          "the signalled event is waited on");
    check(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0,
          "manual reset keeps it signalled across a wait");
    check(ResetEvent(ev) == TRUE, "ResetEvent succeeds");
    check(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT,
          "the reset event is unsignalled again");
    check(PulseEvent(ev) == TRUE, "PulseEvent succeeds");
    check(CloseHandle(ev) == TRUE, "CloseHandle of the event succeeds");

    HANDLE ar = CreateEventA(NULL, FALSE, FALSE, NULL);
    check(ar != NULL, "CreateEventA(auto-reset) returns a handle");
    if (!ar) return;
    check(SetEvent(ar) == TRUE, "SetEvent on the auto-reset event succeeds");
    check(WaitForSingleObject(ar, 0) == WAIT_OBJECT_0,
          "an auto-reset event releases one waiter");
    check(WaitForSingleObject(ar, 0) == WAIT_TIMEOUT,
          "and resets itself after that wait");
    check(CloseHandle(ar) == TRUE, "CloseHandle of the auto-reset event succeeds");
}

static void test_semaphore(void)
{
    LONG prev = -1;
    HANDLE sem = CreateSemaphoreA(NULL, 0, 3, NULL);
    check(sem != NULL, "CreateSemaphoreA returns a handle");
    if (!sem) return;

    check(ReleaseSemaphore(sem, 2, &prev) == TRUE && prev == 0,
          "ReleaseSemaphore reports the previous count (0)");
    check(WaitForSingleObject(sem, 0) == WAIT_OBJECT_0,
          "the released semaphore is waitable");
    check(WaitForSingleObject(sem, 0) == WAIT_OBJECT_0,
          "the second release is waitable too");
    check(WaitForSingleObject(sem, 0) == WAIT_TIMEOUT,
          "the count is exhausted after two waits");
    check(CloseHandle(sem) == TRUE, "CloseHandle of the semaphore succeeds");
}

static void test_mutex(void)
{
    HANDLE mtx = CreateMutexA(NULL, FALSE, NULL);
    check(mtx != NULL, "CreateMutexA returns a handle");
    if (!mtx) return;

    SetLastError(0);
    check(ReleaseMutex(mtx) == FALSE && GetLastError() == ERROR_NOT_OWNER,
          "ReleaseMutex by a non-owner fails with ERROR_NOT_OWNER");
    check(WaitForSingleObject(mtx, 0) == WAIT_OBJECT_0,
          "an unowned mutex is acquired immediately");
    check(ReleaseMutex(mtx) == TRUE, "the owner can release it");
    SetLastError(0);
    check(ReleaseMutex(mtx) == FALSE && GetLastError() == ERROR_NOT_OWNER,
          "a second release is no longer owned");
    check(CloseHandle(mtx) == TRUE, "CloseHandle of the mutex succeeds");
}

static DWORD WINAPI returns_42(LPVOID unused)
{
    (void)unused;
    return 42;
}

static void test_thread(void)
{
    DWORD tid = 0, code = 0;
    HANDLE th = CreateThread(NULL, 0, returns_42, NULL, 0, &tid);
    check(th != NULL, "CreateThread returns a handle");
    if (!th) return;

    check(WaitForSingleObject(th, 5000) == WAIT_OBJECT_0,
          "the thread object signals when the thread exits");
    check(GetExitCodeThread(th, &code) == TRUE && code == 42,
          "GetExitCodeThread reports the thread's return value (42)");
    check(CloseHandle(th) == TRUE, "CloseHandle of the thread succeeds");
}

static void test_waitable_timer(void)
{
    LARGE_INTEGER due;
    due.QuadPart = -1000000;   /* 100 ns units: 100 ms relative to now */

    HANDLE t = CreateWaitableTimerW(NULL, TRUE, NULL);
    check(t != NULL, "CreateWaitableTimerW returns a handle");
    if (!t) return;

    check(SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE) == TRUE,
          "SetWaitableTimer arms a 100 ms timer");
    check(WaitForSingleObject(t, 2000) == WAIT_OBJECT_0,
          "the armed timer signals");
    check(CancelWaitableTimer(t) == TRUE, "CancelWaitableTimer succeeds");
    check(CloseHandle(t) == TRUE, "CloseHandle of the waitable timer succeeds");
}

static void test_mapping(void)
{
    HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                    0, 4096, NULL);
    check(map != NULL, "CreateFileMappingA returns a mapping handle");
    if (!map) return;

    unsigned char *view = (unsigned char *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS,
                                                         0, 0, 4096);
    check(view != NULL, "MapViewOfFile returns a view");
    if (view) {
        view[0] = 0x5A;
        view[4095] = 0xA5;
        check(view[0] == 0x5A && view[4095] == 0xA5, "the view is writable");
        check(UnmapViewOfFile(view) == TRUE, "UnmapViewOfFile releases the view");
    }
    check(CloseHandle(map) == TRUE, "CloseHandle of the mapping succeeds");
}

/* ===================================================================== */
/* 2. DuplicateHandle is a same-value alias                              */
/* ===================================================================== */

static void test_alias(void)
{
    HANDLE dup = NULL;
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    check(ev != NULL, "CreateEventA for the alias case");
    if (!ev) return;

    SetLastError(0);
    check(DuplicateHandle(GetCurrentProcess(), ev, GetCurrentProcess(), &dup,
                          0, FALSE, 0) == TRUE && dup == ev,
          "DuplicateHandle returns the SAME value (a same-value alias)");

    check(SetEvent(dup) == TRUE, "the alias is usable");
    check(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0,
          "both aliases name the same event");

    check(CloseHandle(ev) == TRUE, "closing the first of two aliases succeeds");
    check(SetEvent(dup) == TRUE,
          "the value stays usable after the first alias closes");
    check(WaitForSingleObject(dup, 0) == WAIT_OBJECT_0,
          "and still waits after the first alias closes");

    check(CloseHandle(dup) == TRUE, "closing the last alias succeeds");
    SetLastError(0);
    check(SetEvent(dup) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "the value is dead once the last alias closes");

    /* DUPLICATE_CLOSE_SOURCE consumes the source alias and leaves one. */
    HANDLE a = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE b = NULL;
    check(a != NULL, "CreateEventA for the DUPLICATE_CLOSE_SOURCE case");
    if (!a) return;
    check(DuplicateHandle(GetCurrentProcess(), a, GetCurrentProcess(), &b,
                          0, FALSE, DUPLICATE_CLOSE_SOURCE) == TRUE && b == a,
          "DuplicateHandle(DUPLICATE_CLOSE_SOURCE) keeps one alias");
    check(SetEvent(b) == TRUE, "the surviving alias is usable");
    check(CloseHandle(b) == TRUE, "closing the surviving alias succeeds");
    SetLastError(0);
    check(SetEvent(b) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "the value is dead after the surviving alias closes");
}

/* ===================================================================== */
/* 3. Rejection of values that name no live object                       */
/* ===================================================================== */

/*
 * Every entry point that takes an object handle, against one bad value.
 *
 * The values are deliberately un-dereferenceable: FORGED is unmapped, and the
 * zeroed block has kind == K_EVENT (0) so a shim that trusted the pointer
 * would treat it as a valid event and report success.
 */
static void negatives_pointer(HANDLE bad, const char *label)
{
    DWORD code = 0xDEADBEEFu;
    HANDLE dup = NULL;
    LARGE_INTEGER due;
    due.QuadPart = -1000000;

    SetLastError(0);
    check(SetEvent(bad) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "SetEvent(%s) fails with ERROR_INVALID_HANDLE", label);
    SetLastError(0);
    check(ResetEvent(bad) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "ResetEvent(%s) fails with ERROR_INVALID_HANDLE", label);
    SetLastError(0);
    check(PulseEvent(bad) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "PulseEvent(%s) fails with ERROR_INVALID_HANDLE", label);

    SetLastError(0);
    check(ReleaseSemaphore(bad, 1, NULL) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "ReleaseSemaphore(%s) fails with ERROR_INVALID_HANDLE", label);
    SetLastError(0);
    check(ReleaseMutex(bad) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "ReleaseMutex(%s) fails with ERROR_INVALID_HANDLE", label);

    SetLastError(0);
    check(GetThreadId(bad) == 0 && GetLastError() == ERROR_INVALID_HANDLE,
          "GetThreadId(%s) returns 0 with ERROR_INVALID_HANDLE", label);
    SetLastError(0);
    check(GetExitCodeThread(bad, &code) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE && code == 0xDEADBEEFu,
          "GetExitCodeThread(%s) fails and leaves the out-parameter alone", label);

    SetLastError(0);
    check(WaitForSingleObject(bad, 0) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "WaitForSingleObject(%s) returns WAIT_FAILED", label);
    SetLastError(0);
    check(DuplicateHandle(GetCurrentProcess(), bad, GetCurrentProcess(), &dup,
                          0, FALSE, 0) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "DuplicateHandle(%s) as a source fails", label);
    SetLastError(0);
    check(CloseHandle(bad) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "CloseHandle(%s) fails", label);

    SetLastError(0);
    check(ResumeThread(bad) == (DWORD)-1 && GetLastError() == ERROR_INVALID_HANDLE,
          "ResumeThread(%s) returns (DWORD)-1", label);
    SetLastError(0);
    check(SuspendThread(bad) == (DWORD)-1 && GetLastError() == ERROR_INVALID_HANDLE,
          "SuspendThread(%s) returns (DWORD)-1", label);
    SetLastError(0);
    check(TerminateThread(bad, 0) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "TerminateThread(%s) fails", label);
    SetLastError(0);
    check(SetThreadPriority(bad, 0) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "SetThreadPriority(%s) fails", label);
    SetLastError(0);
    check(GetThreadPriority(bad) == THREAD_PRIORITY_NORMAL &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "GetThreadPriority(%s) returns THREAD_PRIORITY_NORMAL", label);
    SetLastError(0);
    check(QueueUserAPC(NULL, bad, 0) == 0 && GetLastError() == ERROR_INVALID_HANDLE,
          "QueueUserAPC(%s) returns 0", label);

    SetLastError(0);
    check(CancelWaitableTimer(bad) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "CancelWaitableTimer(%s) fails", label);
    SetLastError(0);
    check(SetWaitableTimer(bad, &due, 0, NULL, NULL, FALSE) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "SetWaitableTimer(%s) fails", label);

    SetLastError(0);
    check(ChangeTimerQueueTimer(NULL, bad, 0, 0) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "ChangeTimerQueueTimer(%s) fails", label);
    SetLastError(0);
    check(DeleteTimerQueueTimer(NULL, bad, NULL) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "DeleteTimerQueueTimer(%s) fails", label);

    SetLastError(0);
    check(MapViewOfFileEx(bad, FILE_MAP_ALL_ACCESS, 0, 0, 4096, NULL) == NULL &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "MapViewOfFileEx(%s) returns NULL", label);
}

/* A null handle is the one value the shim treats as "no resource" rather than
 * as a bad one, so the checks here are the ones that still have to fail. */
static void negatives_null(void)
{
    DWORD code = 0xDEADBEEFu;

    SetLastError(0);
    check(SetEvent(NULL) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "SetEvent(NULL) fails with ERROR_INVALID_HANDLE");
    SetLastError(0);
    check(ResetEvent(NULL) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "ResetEvent(NULL) fails with ERROR_INVALID_HANDLE");
    SetLastError(0);
    check(PulseEvent(NULL) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "PulseEvent(NULL) fails with ERROR_INVALID_HANDLE");
    SetLastError(0);
    check(ReleaseSemaphore(NULL, 1, NULL) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "ReleaseSemaphore(NULL) fails with ERROR_INVALID_HANDLE");
    SetLastError(0);
    check(ReleaseMutex(NULL) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "ReleaseMutex(NULL) fails with ERROR_INVALID_HANDLE");
    SetLastError(0);
    check(GetThreadId(NULL) == 0 && GetLastError() == ERROR_INVALID_HANDLE,
          "GetThreadId(NULL) returns 0");
    SetLastError(0);
    check(GetExitCodeThread(NULL, &code) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE && code == 0xDEADBEEFu,
          "GetExitCodeThread(NULL) fails and leaves the out-parameter alone");
    SetLastError(0);
    check(CancelWaitableTimer(NULL) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "CancelWaitableTimer(NULL) fails");
    SetLastError(0);
    check(TerminateThread(NULL, 0) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "TerminateThread(NULL) fails");
    SetLastError(0);
    check(MapViewOfFileEx(NULL, FILE_MAP_ALL_ACCESS, 0, 0, 4096, NULL) == NULL &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "MapViewOfFileEx(NULL) returns NULL");
}

/* The wrong object behind a live, valid handle: the value passes the registry
 * check, so the API has to reject the KIND instead of reading a union member
 * that belongs to another object type. */
static void negatives_wrong_kind(void)
{
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE sem = CreateSemaphoreA(NULL, 1, 4, NULL);
    DWORD code = 0xDEADBEEFu;
    check(ev != NULL && sem != NULL, "CreateEventA/CreateSemaphoreA for the wrong-kind case");
    if (!ev || !sem) return;

    SetLastError(0);
    check(ReleaseMutex(ev) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "ReleaseMutex(event) fails instead of reading the mutex fields");
    SetLastError(0);
    check(GetThreadId(ev) == 0 && GetLastError() == ERROR_INVALID_HANDLE,
          "GetThreadId(event) returns 0");
    SetLastError(0);
    check(GetExitCodeThread(ev, &code) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE && code == 0xDEADBEEFu,
          "GetExitCodeThread(event) fails and leaves the out-parameter alone");
    SetLastError(0);
    check(ReleaseSemaphore(ev, 1, NULL) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "ReleaseSemaphore(event) fails");
    SetLastError(0);
    check(SetEvent(sem) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "SetEvent(semaphore) fails");
    SetLastError(0);
    check(ResumeThread(ev) == (DWORD)-1 && GetLastError() == ERROR_INVALID_HANDLE,
          "ResumeThread(event) returns (DWORD)-1");
    SetLastError(0);
    check(QueueUserAPC(NULL, ev, 0) == 0 && GetLastError() == ERROR_INVALID_HANDLE,
          "QueueUserAPC(event) returns 0");
    SetLastError(0);
    check(TerminateThread(ev, 0) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "TerminateThread(event) fails");
    SetLastError(0);
    check(CancelWaitableTimer(ev) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "CancelWaitableTimer(event) fails");
    SetLastError(0);
    check(MapViewOfFileEx(ev, FILE_MAP_ALL_ACCESS, 0, 0, 4096, NULL) == NULL &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "MapViewOfFileEx(event) returns NULL");
    SetLastError(0);
    check(ChangeTimerQueueTimer(NULL, ev, 0, 0) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "ChangeTimerQueueTimer(event) fails");
    SetLastError(0);
    check(DeleteTimerQueueTimer(NULL, ev, NULL) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "DeleteTimerQueueTimer(event) fails");

    /* The valid operations on the same handles still work afterwards. */
    check(SetEvent(ev) == TRUE, "the event is unharmed by the rejected calls");
    check(WaitForSingleObject(sem, 0) == WAIT_OBJECT_0,
          "the semaphore is unharmed by the rejected calls");
    check(CloseHandle(ev) == TRUE, "CloseHandle of the event succeeds");
    check(CloseHandle(sem) == TRUE, "CloseHandle of the semaphore succeeds");
}

/*
 * A closed value must not be reusable, must not free anything twice, and must
 * not corrupt the heap. A guard block allocated BEFORE the object catches a
 * close that wrote past its own object; a fresh object afterwards catches a
 * heap that the double close broke.
 *
 * The stale value is not expected to stay dead forever: the shim keeps
 * pointer values as handles, so a later allocation that lands on the same
 * address makes that value name a DIFFERENT live object. That is a property
 * of the value scheme, not something a check can pin, so nothing is allocated
 * between the close and these checks.
 */
static void test_closed_handle(void)
{
    unsigned char *guard = (unsigned char *)malloc(256);
    check(guard != NULL, "malloc the guard block");
    if (guard) memset(guard, 0xA5, 256);

    HANDLE ev = CreateEventA(NULL, TRUE, TRUE, NULL);
    check(ev != NULL, "CreateEventA for the closed-handle case");
    if (!ev) { free(guard); return; }

    check(CloseHandle(ev) == TRUE, "CloseHandle of a live event succeeds");
    SetLastError(0);
    check(CloseHandle(ev) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "a second CloseHandle of the same value returns FALSE");
    SetLastError(0);
    check(SetEvent(ev) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "SetEvent on the closed value fails");
    SetLastError(0);
    check(ResetEvent(ev) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "ResetEvent on the closed value fails");
    SetLastError(0);
    check(WaitForSingleObject(ev, 0) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "WaitForSingleObject on the closed value returns WAIT_FAILED");

    HANDLE dup = NULL;
    SetLastError(0);
    check(DuplicateHandle(GetCurrentProcess(), ev, GetCurrentProcess(), &dup,
                          0, FALSE, 0) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "DuplicateHandle from the closed value fails");

    int intact = 1;
    for (int i = 0; i < 256; i++)
        if (guard && guard[i] != 0xA5) intact = 0;
    check(intact, "the double close left the surrounding heap intact");

    HANDLE fresh = CreateEventA(NULL, TRUE, TRUE, NULL);
    check(fresh != NULL && SetEvent(fresh) == TRUE,
          "the heap still serves new objects after the double close");
    if (fresh) CloseHandle(fresh);
    free(guard);
}

/* A NULL out-parameter is a caller bug, not a bad handle: Win32 answers
 * ERROR_INVALID_PARAMETER, and the shim has to keep that distinct from
 * ERROR_INVALID_HANDLE so a caller can tell the two apart. */
static void test_null_out_parameters(void)
{
    DWORD tid = 0;
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE th = CreateThread(NULL, 0, returns_42, NULL, 0, &tid);
    check(ev != NULL && th != NULL, "CreateEventA/CreateThread for the NULL out-parameter case");
    if (!ev || !th) return;

    SetLastError(0);
    check(GetExitCodeThread(th, NULL) == FALSE &&
          GetLastError() == ERROR_INVALID_PARAMETER,
          "GetExitCodeThread(thread, NULL) fails with ERROR_INVALID_PARAMETER");
    SetLastError(0);
    check(DuplicateHandle(GetCurrentProcess(), ev, GetCurrentProcess(), NULL,
                          0, FALSE, 0) == FALSE &&
          GetLastError() == ERROR_INVALID_PARAMETER,
          "DuplicateHandle(..., dst = NULL) fails with ERROR_INVALID_PARAMETER");

    check(CloseHandle(ev) == TRUE, "CloseHandle of the event succeeds");
    WaitForSingleObject(th, 5000);
    check(CloseHandle(th) == TRUE, "CloseHandle of the thread succeeds");
}

/* ===================================================================== */
/* 3b. Timer-queue timers                                                */
/* ===================================================================== */

/*
 * A timer-queue timer is the shim's release-and-close API: DeleteTimerQueueTimer
 * both cancels and consumes the caller's handle, which is how the kernel HLE
 * uses it. So the handle has to be live before the delete, gone after it, and
 * a second delete of the same value has to fail rather than free anything
 * twice. The callback runs on a helper thread and is non-blocking; the test
 * waits for it on a separate event with a 2 s bound.
 *
 * The completion-event form the HLE uses, INVALID_HANDLE_VALUE, means "do not
 * signal anything" and must not be mistaken for a handle to signal.
 */
static volatile LONG g_timer_fired;
static volatile LONG g_timer_param;

static VOID WINAPI timer_callback(PVOID param, BOOLEAN timerOrWaitFired)
{
    (void)timerOrWaitFired;
    InterlockedExchange(&g_timer_param, (LONG)(LONG_PTR)param);
    InterlockedExchange(&g_timer_fired, 1);
}

static void test_timer_queue(void)
{
    HANDLE queue = CreateTimerQueue();
    check(queue != NULL, "CreateTimerQueue returns a queue handle");
    if (!queue) return;

    HANDLE done = CreateEventA(NULL, TRUE, FALSE, NULL);
    check(done != NULL, "CreateEventA for the timer callback");
    if (!done) { CloseHandle(queue); return; }

    HANDLE timer = NULL;
    g_timer_fired = 0;
    g_timer_param = 0;

    check(CreateTimerQueueTimer(&timer, queue, timer_callback,
                                (PVOID)(LONG_PTR)0x1234, 0, 0,
                                WT_EXECUTEONLYONCE) == TRUE && timer != NULL,
          "CreateTimerQueueTimer(one-shot, due now) returns a live timer");
    if (!timer) { CloseHandle(done); CloseHandle(queue); return; }

    check(wait_flag(&g_timer_fired, 1, 2000),
          "the timer callback runs within 2 s");
    check(g_timer_param == 0x1234, "the callback receives its parameter");

    /* ChangeTimerQueueTimer on a live timer succeeds (re-arm, still one-shot). */
    check(ChangeTimerQueueTimer(queue, timer, 0, 0) == TRUE,
          "ChangeTimerQueueTimer on the live timer succeeds");

    check(DeleteTimerQueueTimer(queue, timer, done) == TRUE,
          "DeleteTimerQueueTimer returns TRUE");
    check(WaitForSingleObject(done, 2000) == WAIT_OBJECT_0,
          "DeleteTimerQueueTimer signals the completion event");

    SetLastError(0);
    check(DeleteTimerQueueTimer(queue, timer, NULL) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "a second DeleteTimerQueueTimer fails with ERROR_INVALID_HANDLE");
    SetLastError(0);
    check(ChangeTimerQueueTimer(queue, timer, 0, 0) == FALSE &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "ChangeTimerQueueTimer on the consumed value fails");
    SetLastError(0);
    check(CloseHandle(timer) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "CloseHandle of the consumed value fails");

    /* The INVALID_HANDLE_VALUE completion form: nothing to signal, no fault. */
    HANDLE t2 = NULL;
    g_timer_fired = 0;
    check(CreateTimerQueueTimer(&t2, queue, timer_callback, NULL, 0, 0,
                                WT_EXECUTEONLYONCE) == TRUE && t2 != NULL,
          "CreateTimerQueueTimer for the INVALID_HANDLE_VALUE form");
    if (t2) {
        check(DeleteTimerQueueTimer(queue, t2, INVALID_HANDLE_VALUE) == TRUE,
              "DeleteTimerQueueTimer(queue, t, INVALID_HANDLE_VALUE) returns TRUE");
        SetLastError(0);
        check(ChangeTimerQueueTimer(queue, t2, 0, 0) == FALSE &&
              GetLastError() == ERROR_INVALID_HANDLE,
              "the timer is dead after the INVALID_HANDLE_VALUE delete");
    }

    check(CloseHandle(done) == TRUE, "CloseHandle of the completion event succeeds");
    check(DeleteTimerQueue(queue) == TRUE, "DeleteTimerQueue releases the queue");
    SetLastError(0);
    check(CloseHandle(queue) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "the queue value is dead after DeleteTimerQueue");
}

/* ===================================================================== */
/* 4. Pseudo handles and GetCurrentThread()                              */
/* ===================================================================== */

static volatile LONG g_self_ready;
static volatile LONG g_self_phase;
static volatile LONG g_self_parked;
static volatile LONG g_self_resume_prev;
static volatile LONG g_self_wait_after_close;
static HANDLE         g_self_handle;

static DWORD WINAPI pseudo_worker(LPVOID unused)
{
    (void)unused;
    HANDLE self = GetCurrentThread();
    g_self_handle = self;
    InterlockedExchange(&g_self_ready, 1);

    if (!wait_flag(&g_self_phase, 1, 2000)) return 11;
    InterlockedExchange(&g_self_parked, 1);
    DWORD prev = SuspendThread(self);          /* parks here until resumed */
    InterlockedExchange(&g_self_resume_prev, (LONG)prev);

    if (!wait_flag(&g_self_phase, 2, 5000)) return 12;
    /* By now main has closed the public handle for this thread. */
    DWORD w = WaitForSingleObject(self, 0);
    InterlockedExchange(&g_self_wait_after_close, (LONG)w);
    return 7;
}

static void test_pseudo(void)
{
    check(GetThreadId((HANDLE)(LONG_PTR)-2) == GetCurrentThreadId(),
          "GetThreadId((HANDLE)-2) is the calling thread's id");
    check(WaitForSingleObject((HANDLE)(LONG_PTR)-2, 0) == WAIT_OBJECT_0,
          "WaitForSingleObject((HANDLE)-2, 0) returns WAIT_OBJECT_0");
    check(WaitForSingleObject((HANDLE)(LONG_PTR)-1, 0) == WAIT_OBJECT_0,
          "WaitForSingleObject((HANDLE)-1, 0) returns WAIT_OBJECT_0");
    check(CloseHandle((HANDLE)(LONG_PTR)-2) == TRUE,
          "CloseHandle((HANDLE)-2) returns TRUE");
    check(CloseHandle((HANDLE)(LONG_PTR)-1) == TRUE,
          "CloseHandle((HANDLE)-1) returns TRUE");

    /* The value GetCurrentThread() reports is the thread's own object, and it
     * has to keep working after the public handle for that object is closed:
     * a worker pool creates its thread, closes the handle, then suspends
     * itself through the value it kept. */
    DWORD tid = 0;
    g_self_ready = 0;
    g_self_phase = 0;
    g_self_parked = 0;
    g_self_resume_prev = -1;
    g_self_wait_after_close = -1;
    g_self_handle = NULL;

    HANDLE th = CreateThread(NULL, 0, pseudo_worker, NULL, 0, &tid);
    check(th != NULL, "CreateThread for the GetCurrentThread() case");
    if (!th) return;

    check(wait_flag(&g_self_ready, 1, 2000),
          "the worker reports the value GetCurrentThread() gave it");
    check(g_self_handle == th,
          "GetCurrentThread() inside a thread is that thread's handle value");

    InterlockedExchange(&g_self_phase, 1);
    check(wait_flag(&g_self_parked, 1, 2000),
          "the worker reaches SuspendThread(its own value)");
    /* Bound: the worker sets the flag immediately before the call, and this
     * pause is three orders of magnitude longer than that window. */
    Sleep(30);

    check(ResumeThread(th) == 1,
          "ResumeThread through the public handle returns the parked count (1)");
    check(wait_flag_ne(&g_self_resume_prev, -1, 5000),
          "SuspendThread(own value) returns once resumed");
    check(g_self_resume_prev == 0,
          "SuspendThread(own value) reports the count it left (0)");

    /* Let the worker settle into its post-close wait before closing, so the
     * close and the wait are ordered by construction rather than by luck. */
    Sleep(30);
    check(CloseHandle(th) == TRUE,
          "CloseHandle of the public thread handle succeeds");
    InterlockedExchange(&g_self_phase, 2);

    check(wait_flag_ne(&g_self_wait_after_close, -1, 3000),
          "the worker's wait on its own value returns after the close");
    check(g_self_wait_after_close == (LONG)WAIT_TIMEOUT,
          "GetCurrentThread()'s value still waits after the public handle closed");
}

/* ===================================================================== */
/* 5. A pin held by a public call outlives the last close                */
/* ===================================================================== */

static HANDLE        g_inflight;
static volatile LONG g_in_wait;
static volatile LONG g_inflight_result;

static void *inflight_waiter(void *unused)
{
    (void)unused;
    InterlockedExchange(&g_in_wait, 1);
    DWORD w = WaitForSingleObject(g_inflight, 800);
    InterlockedExchange(&g_inflight_result, (LONG)w);
    return NULL;
}

static void test_inflight_close(void)
{
    g_inflight = CreateEventA(NULL, TRUE, FALSE, NULL);
    check(g_inflight != NULL, "CreateEventA for the in-flight case");
    if (!g_inflight) return;

    g_in_wait = 0;
    g_inflight_result = -1;

    pthread_t t;
    if (pthread_create(&t, NULL, inflight_waiter, NULL) != 0) {
        check(0, "pthread_create for the in-flight waiter");
        CloseHandle(g_inflight);
        return;
    }
    check(wait_flag(&g_in_wait, 1, 2000),
          "the waiter announces that it is about to wait");
    /* Bound: the waiter's wait is capped at 800 ms and this close happens
     * 50 ms after the announcement, so the call is inside the shim when the
     * handle goes. A waiter that started after the close would return
     * WAIT_FAILED at once, and that is what the result check below catches --
     * so WAIT_TIMEOUT here is itself evidence the pin was taken first. This
     * pins one close-against-one-pin; it does not claim anything about an
     * arbitrary interleaving. */
    Sleep(50);

    check(CloseHandle(g_inflight) == TRUE,
          "the last alias closes while the call is inside the shim");
    SetLastError(0);
    check(SetEvent(g_inflight) == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
          "the closed value is not resurrected for a later caller");

    pthread_join(t, NULL);
    check(g_inflight_result == (LONG)WAIT_TIMEOUT,
          "the pinned wait ran to its own deadline instead of freeing under itself");
}

/* ===================================================================== */
/* main                                                                  */
/* ===================================================================== */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    only_row = getenv("W32_HANDLE_TEST_ONLY");
    HANDLE zeroed = (HANDLE)calloc(1, 4096);
    if (!zeroed) { printf("calloc failed\n"); return 1; }

    printf("win32 handle lifetime and provenance (POSIX)\n");

    if (only_row) {
        /* Controls mode: one negative row, so a row that crashes the shim
         * under test cannot hide the rows after it. */
        if (strcmp(only_row, "invalid-addr") == 0)
            negatives_pointer(FORGED, "forged 0x03fffffe");
        else if (strcmp(only_row, "zeroed") == 0)
            negatives_pointer(zeroed, "zeroed 4 KiB calloc block");
        else {
            printf("unknown W32_HANDLE_TEST_ONLY value\n");
            free(zeroed);
            return 2;
        }
        printf("\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
        free(zeroed);
        return failures ? 1 : 0;
    }

    printf("\nvalid semantics\n");
    test_events();
    test_semaphore();
    test_mutex();
    test_thread();
    test_waitable_timer();
    test_mapping();

    printf("\nduplicate aliases\n");
    test_alias();

    printf("\nforged, zeroed and null values\n");
    negatives_pointer(FORGED, "forged 0x03fffffe");
    negatives_pointer(zeroed, "zeroed 4 KiB calloc block");
    negatives_null();
    negatives_wrong_kind();
    test_closed_handle();
    test_null_out_parameters();

    printf("\ntimer-queue timers\n");
    test_timer_queue();

    printf("\npseudo handles\n");
    test_pseudo();

    printf("\nin-flight pin\n");
    test_inflight_close();

    free(zeroed);

    printf("\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
