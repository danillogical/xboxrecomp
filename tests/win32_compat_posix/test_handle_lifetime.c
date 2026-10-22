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
 *   6. multi-wait  WaitForMultipleObjects(Ex) resolves each entry the way the
 *                  single-object wait does: NULL and the two pseudo handles
 *                  are always ready, this thread's own GetCurrentThread()
 *                  value stays waitable after its public alias closed, and a
 *                  real value that is not live fails the whole call with
 *                  ERROR_INVALID_HANDLE instead of being read as "ready"
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
 * one negative row and nothing else, and W32_HANDLE_TEST_ONLY=multiwait runs
 * the multi-object block alone (so a mutant of the wait path is not masked by
 * an unrelated negative). Unset -- the normal case, and what CI runs -- the
 * whole file runs.
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
/* 6. Multi-object waits: sentinels, the own-thread value, dead entries  */
/* ===================================================================== */

/*
 * WaitForMultipleObjects(Ex) resolves each entry the way the single-object
 * wait does, and the two halves of that are decided by different rows.
 *
 * Sentinels. NULL and the two pseudo handles are values this layer answers
 * without an object at all: the single wait has always returned WAIT_OBJECT_0
 * for them, so the multi-object form must call them ready -- in wait-any AND
 * wait-all, through both entry points, alertable or not -- and a wait that
 * mixes one with a real event must not fail. A shim that sends them down the
 * ordinary handle path reports WAIT_FAILED for the whole call.
 *
 * Liveness. An ordinary value goes through the same self/lifetime-aware
 * resolution the single wait uses: this thread's own GetCurrentThread() value
 * keeps working after its public alias closed, while the SAME value seen from
 * another thread, a closed value and a forged value all still fail with
 * ERROR_INVALID_HANDLE. The negative rows place the bad entry AFTER live,
 * already-pinned entries, which is the shape that leaks a pin if the failure
 * path forgets to release what it took (the ASAN companion decides that half).
 *
 * "Ready" is not faked by treating an unresolved entry as satisfied: the
 * wait-all rows pair a sentinel with an UNSIGNALLED event and require
 * WAIT_TIMEOUT. A shim that answered "everything is ready" would pass the
 * sentinel rows and fail those.
 */

#define PSEUDO_THREAD  ((HANDLE)(LONG_PTR)-2)
#define PSEUDO_PROCESS ((HANDLE)(LONG_PTR)-1)

static void test_multiwait_sentinels(void)
{
    HANDLE off = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE on  = CreateEventA(NULL, TRUE, TRUE, NULL);
    check(off != NULL && on != NULL, "CreateEventA for the sentinel rows");
    if (!off || !on) return;

    HANDLE one_null[1] = { NULL };
    HANDLE one_thr[1]  = { PSEUDO_THREAD };
    HANDLE one_proc[1] = { PSEUDO_PROCESS };
    HANDLE all_three[3] = { NULL, PSEUDO_THREAD, PSEUDO_PROCESS };

    /* One sentinel, one entry, both wait modes. */
    check(WaitForMultipleObjects(1, one_null, FALSE, 0) == WAIT_OBJECT_0,
          "multi(any, {NULL}) is WAIT_OBJECT_0");
    check(WaitForMultipleObjects(1, one_null, TRUE, 0) == WAIT_OBJECT_0,
          "multi(all, {NULL}) is WAIT_OBJECT_0");
    check(WaitForMultipleObjects(1, one_thr, FALSE, 0) == WAIT_OBJECT_0,
          "multi(any, {(HANDLE)-2}) is WAIT_OBJECT_0");
    check(WaitForMultipleObjects(1, one_thr, TRUE, 0) == WAIT_OBJECT_0,
          "multi(all, {(HANDLE)-2}) is WAIT_OBJECT_0");
    check(WaitForMultipleObjects(1, one_proc, FALSE, 0) == WAIT_OBJECT_0,
          "multi(any, {(HANDLE)-1}) is WAIT_OBJECT_0");
    check(WaitForMultipleObjects(1, one_proc, TRUE, 0) == WAIT_OBJECT_0,
          "multi(all, {(HANDLE)-1}) is WAIT_OBJECT_0");

    /* The Ex entry point, alertable and not. No APC is queued here, so the
     * result must be the same as the non-alertable one. */
    check(WaitForMultipleObjectsEx(1, one_thr, FALSE, 0, FALSE) == WAIT_OBJECT_0,
          "multiEx(any, {(HANDLE)-2}, alertable=FALSE) is WAIT_OBJECT_0");
    check(WaitForMultipleObjectsEx(1, one_thr, TRUE, 0, FALSE) == WAIT_OBJECT_0,
          "multiEx(all, {(HANDLE)-2}, alertable=FALSE) is WAIT_OBJECT_0");
    check(WaitForMultipleObjectsEx(1, one_proc, FALSE, 0, TRUE) == WAIT_OBJECT_0,
          "multiEx(any, {(HANDLE)-1}, alertable=TRUE) is WAIT_OBJECT_0");
    check(WaitForMultipleObjectsEx(3, all_three, TRUE, 0, TRUE) == WAIT_OBJECT_0,
          "multiEx(all, {NULL,-2,-1}, alertable=TRUE) is WAIT_OBJECT_0");

    /* Mixed with a real event. The unsignalled event is first, so wait-any
     * must skip it and report the sentinel's index. Each row also requires
     * that the call did not leave ERROR_INVALID_HANDLE behind: that is the
     * exact defect the sentinel branch exists to prevent, and a return-value
     * check alone would not name it. */
    HANDLE mix_thr[2]  = { off, PSEUDO_THREAD };
    HANDLE mix_proc[2] = { off, PSEUDO_PROCESS };
    HANDLE mix_null[2] = { off, NULL };
    HANDLE mix_first[2] = { PSEUDO_THREAD, off };

    DWORD r, e;

    SetLastError(0);
    r = WaitForMultipleObjects(2, mix_null, FALSE, 0); e = GetLastError();
    check(r == WAIT_OBJECT_0 + 1 && e != ERROR_INVALID_HANDLE,
          "multi(any, {unsignalled event, NULL}) reports index 1 (ret=0x%08X err=%lu)",
          r, (unsigned long)e);
    SetLastError(0);
    r = WaitForMultipleObjects(2, mix_thr, FALSE, 0); e = GetLastError();
    check(r == WAIT_OBJECT_0 + 1 && e != ERROR_INVALID_HANDLE,
          "multi(any, {unsignalled event, (HANDLE)-2}) reports index 1 (ret=0x%08X err=%lu)",
          r, (unsigned long)e);
    SetLastError(0);
    r = WaitForMultipleObjects(2, mix_proc, FALSE, 0); e = GetLastError();
    check(r == WAIT_OBJECT_0 + 1 && e != ERROR_INVALID_HANDLE,
          "multi(any, {unsignalled event, (HANDLE)-1}) reports index 1 (ret=0x%08X err=%lu)",
          r, (unsigned long)e);
    SetLastError(0);
    r = WaitForMultipleObjects(2, mix_first, FALSE, 0); e = GetLastError();
    check(r == WAIT_OBJECT_0 && e != ERROR_INVALID_HANDLE,
          "multi(any, {(HANDLE)-2, unsignalled event}) reports index 0 (ret=0x%08X err=%lu)",
          r, (unsigned long)e);
    SetLastError(0);
    r = WaitForMultipleObjectsEx(2, mix_thr, FALSE, 0, FALSE); e = GetLastError();
    check(r == WAIT_OBJECT_0 + 1 && e != ERROR_INVALID_HANDLE,
          "multiEx(any, {unsignalled event, (HANDLE)-2}) reports index 1 (ret=0x%08X err=%lu)",
          r, (unsigned long)e);
    SetLastError(0);
    r = WaitForMultipleObjectsEx(2, mix_proc, FALSE, 0, TRUE); e = GetLastError();
    check(r == WAIT_OBJECT_0 + 1 && e != ERROR_INVALID_HANDLE,
          "multiEx(any, {unsignalled event, (HANDLE)-1}, alertable) reports index 1 (ret=0x%08X err=%lu)",
          r, (unsigned long)e);

    /* wait-all with a signalled event and every sentinel: satisfied. */
    HANDLE all_on[4] = { on, NULL, PSEUDO_THREAD, PSEUDO_PROCESS };
    check(WaitForMultipleObjects(4, all_on, TRUE, 0) == WAIT_OBJECT_0,
          "multi(all, {signalled event, NULL,-2,-1}) is WAIT_OBJECT_0");
    check(WaitForMultipleObjectsEx(4, all_on, TRUE, 0, FALSE) == WAIT_OBJECT_0,
          "multiEx(all, {signalled event, NULL,-2,-1}) is WAIT_OBJECT_0");

    /* wait-all with an UNSIGNALLED event and a sentinel: the sentinel is
     * ready but the event is not, so the call must time out. This is the row
     * that stops "treat every entry as ready" from passing the block above. */
    HANDLE all_off_thr[2]  = { off, PSEUDO_THREAD };
    HANDLE all_off_null[3] = { off, NULL, PSEUDO_PROCESS };
    check(WaitForMultipleObjects(2, all_off_thr, TRUE, 20) == WAIT_TIMEOUT,
          "multi(all, {unsignalled event, (HANDLE)-2}) times out");
    check(WaitForMultipleObjects(3, all_off_null, TRUE, 20) == WAIT_TIMEOUT,
          "multi(all, {unsignalled event, NULL, (HANDLE)-1}) times out");
    check(WaitForMultipleObjectsEx(2, all_off_thr, TRUE, 20, FALSE) == WAIT_TIMEOUT,
          "multiEx(all, {unsignalled event, (HANDLE)-2}) times out");

    check(CloseHandle(off) == TRUE && CloseHandle(on) == TRUE,
          "CloseHandle of the sentinel-row events succeeds");
}

/* ---- this thread's own value, after its public alias closed ------------ */

static volatile LONG g_mw_ready;
static volatile LONG g_mw_phase;
static volatile LONG g_mw_any;      /* any: {self, unsignalled event} */
static volatile LONG g_mw_all;      /* all: {self, signalled event}   */
static volatile LONG g_mw_ex;       /* Ex, alertable, same as g_mw_any */
static volatile LONG g_mw_single;   /* single wait on self, reference  */
static volatile LONG g_mw_done;
static HANDLE         g_mw_self;
static HANDLE         g_mw_off;
static HANDLE         g_mw_on;

static DWORD WINAPI multiwait_self_worker(LPVOID unused)
{
    (void)unused;
    HANDLE self = GetCurrentThread();
    g_mw_self = self;
    InterlockedExchange(&g_mw_ready, 1);

    /* Main closes the public handle for this thread here. */
    if (!wait_flag(&g_mw_phase, 1, 5000)) return 21;

    HANDLE any_set[2] = { self, g_mw_off };
    HANDLE all_set[2] = { self, g_mw_on };

    SetLastError(0);
    InterlockedExchange(&g_mw_any, (LONG)WaitForMultipleObjects(2, any_set, FALSE, 60));
    SetLastError(0);
    InterlockedExchange(&g_mw_all, (LONG)WaitForMultipleObjects(2, all_set, TRUE, 0));
    SetLastError(0);
    InterlockedExchange(&g_mw_ex,
                        (LONG)WaitForMultipleObjectsEx(2, any_set, FALSE, 60, TRUE));
    SetLastError(0);
    InterlockedExchange(&g_mw_single, (LONG)WaitForSingleObject(self, 0));

    InterlockedExchange(&g_mw_done, 1);
    return 0;
}

static void test_multiwait_own_thread_value(void)
{
    g_mw_ready = 0;
    g_mw_phase = 0;
    g_mw_any = -1;
    g_mw_all = -1;
    g_mw_ex = -1;
    g_mw_single = -1;
    g_mw_done = 0;
    g_mw_self = NULL;

    g_mw_off = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_mw_on  = CreateEventA(NULL, TRUE, TRUE, NULL);
    check(g_mw_off != NULL && g_mw_on != NULL,
          "CreateEventA for the own-thread multi-wait rows");
    if (!g_mw_off || !g_mw_on) return;

    DWORD tid = 0;
    HANDLE th = CreateThread(NULL, 0, multiwait_self_worker, NULL, 0, &tid);
    check(th != NULL, "CreateThread for the own-thread multi-wait rows");
    if (!th) {
        CloseHandle(g_mw_off);
        CloseHandle(g_mw_on);
        return;
    }

    check(wait_flag(&g_mw_ready, 1, 3000),
          "the worker reports the value GetCurrentThread() gave it");
    check(g_mw_self == th,
          "inside the worker that value is the thread's own handle value");

    check(CloseHandle(th) == TRUE,
          "the public handle for the worker's thread object closes");

    /* The SAME value from THIS thread names no live public handle any more.
     * A multi-wait must say so, exactly as the single wait does. This is what
     * separates "this thread's own value" from "any thread object". */
    HANDLE foreign[2] = { g_mw_on, th };
    SetLastError(0);
    check(WaitForMultipleObjects(2, foreign, FALSE, 0) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "the same value from another thread fails with ERROR_INVALID_HANDLE");

    InterlockedExchange(&g_mw_phase, 1);
    check(wait_flag(&g_mw_done, 1, 5000),
          "the worker's four post-close waits all return");

    check(g_mw_any == (LONG)WAIT_TIMEOUT,
          "multi(any, {own value, unsignalled event}) times out after the close (%ld)",
          (long)g_mw_any);
    check(g_mw_all == (LONG)WAIT_TIMEOUT,
          "multi(all, {own value, signalled event}) times out: self has not exited (%ld)",
          (long)g_mw_all);
    check(g_mw_ex == (LONG)WAIT_TIMEOUT,
          "multiEx(any, alertable, own value) times out after the close (%ld)",
          (long)g_mw_ex);
    check(g_mw_single == (LONG)WAIT_TIMEOUT,
          "the single wait on the own value agrees (%ld)", (long)g_mw_single);

    /* The thread object is still alive (its trampoline holds it), so the
     * events must be unharmed by the failed call from this thread. */
    check(WaitForSingleObject(g_mw_on, 0) == WAIT_OBJECT_0,
          "the signalled event is unharmed by the rejected multi-wait");
    check(CloseHandle(g_mw_off) == TRUE && CloseHandle(g_mw_on) == TRUE,
          "CloseHandle of the own-thread-row events succeeds");
}

/* ---- values that name no live object, after live pinned entries ------- */

/*
 * Each array puts one or two LIVE entries first, so the shim has already
 * pinned something when it reaches the bad entry. Win32 fails the whole call,
 * and the shim must fail it with ERROR_INVALID_HANDLE -- not ERROR_INVALID_
 * PARAMETER, and not by skipping the entry and reporting a result it did not
 * observe. The live entries must be untouched afterwards, which is also what
 * a leaked pin on this path would eventually show as (ASAN decides that).
 */
static void multiwait_dead_row(HANDLE bad, const char *label, HANDLE on, HANDLE off)
{
    HANDLE any_set[3] = { on, off, bad };
    HANDLE all_set[3] = { on, off, bad };

    SetLastError(0);
    check(WaitForMultipleObjects(3, any_set, FALSE, 0) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "multi(any, {live, live, %s}) fails with ERROR_INVALID_HANDLE", label);
    SetLastError(0);
    check(WaitForMultipleObjects(3, all_set, TRUE, 0) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "multi(all, {live, live, %s}) fails with ERROR_INVALID_HANDLE", label);
    SetLastError(0);
    check(WaitForMultipleObjectsEx(3, any_set, FALSE, 0, TRUE) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "multiEx(any, {live, live, %s}) fails with ERROR_INVALID_HANDLE", label);
    SetLastError(0);
    check(WaitForMultipleObjectsEx(3, all_set, TRUE, 0, FALSE) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "multiEx(all, {live, live, %s}) fails with ERROR_INVALID_HANDLE", label);

    /* Dead first, live after: the same failure, so nothing depends on the
     * order of the bad entry. */
    HANDLE bad_first[3] = { bad, on, off };
    SetLastError(0);
    check(WaitForMultipleObjects(3, bad_first, FALSE, 0) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "multi(any, {%s, live, live}) fails with ERROR_INVALID_HANDLE", label);

    /* The live entries are still usable: a failed call released its pins and
     * changed nothing about them. */
    check(WaitForSingleObject(on, 0) == WAIT_OBJECT_0 && SetEvent(off) == TRUE,
          "the live entries survive the failed call with %s", label);
}

static void test_multiwait_dead_entries(HANDLE zeroed)
{
    HANDLE on  = CreateEventA(NULL, TRUE, TRUE, NULL);
    HANDLE off = CreateEventA(NULL, TRUE, FALSE, NULL);
    check(on != NULL && off != NULL, "CreateEventA for the dead-entry rows");
    if (!on || !off) return;

    multiwait_dead_row(FORGED, "forged 0x03fffffe", on, off);
    multiwait_dead_row(zeroed, "zeroed 4 KiB calloc block", on, off);

    /* A value that looks like a pseudo handle but is not one of the three the
     * shim supports: no special meaning, so it fails like any dead value. */
    multiwait_dead_row((HANDLE)(LONG_PTR)-3, "(HANDLE)-3", on, off);

    /* A real object whose last public alias was closed. Nothing is allocated
     * between the close and the call, so the value cannot have been reused. */
    HANDLE gone = CreateEventA(NULL, TRUE, FALSE, NULL);
    check(gone != NULL, "CreateEventA for the closed-value row");
    if (gone) {
        check(CloseHandle(gone) == TRUE, "the closed-value event closes");
        multiwait_dead_row(gone, "a closed event", on, off);
    }

    /* A single dead entry, no live neighbours. */
    HANDLE only_bad[1] = { FORGED };
    SetLastError(0);
    check(WaitForMultipleObjects(1, only_bad, FALSE, 0) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_HANDLE,
          "multi(any, {forged}) alone fails with ERROR_INVALID_HANDLE");

    check(CloseHandle(on) == TRUE && CloseHandle(off) == TRUE,
          "CloseHandle of the dead-entry-row events succeeds");
}

/* ---- parameter validation --------------------------------------------- */

static void test_multiwait_parameters(void)
{
    HANDLE ev = CreateEventA(NULL, TRUE, TRUE, NULL);
    check(ev != NULL, "CreateEventA for the parameter rows");
    if (!ev) return;

    HANDLE many[MAXIMUM_WAIT_OBJECTS + 1];
    for (int i = 0; i < MAXIMUM_WAIT_OBJECTS + 1; i++) many[i] = ev;
    HANDLE one[1] = { ev };

    SetLastError(0);
    check(WaitForMultipleObjects(0, one, FALSE, 0) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_PARAMETER,
          "multi(count=0) fails with ERROR_INVALID_PARAMETER");
    SetLastError(0);
    check(WaitForMultipleObjects(0, NULL, FALSE, 0) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_PARAMETER,
          "multi(count=0, handles=NULL) fails with ERROR_INVALID_PARAMETER");
    SetLastError(0);
    check(WaitForMultipleObjects(1, NULL, FALSE, 0) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_PARAMETER,
          "multi(count=1, handles=NULL) fails with ERROR_INVALID_PARAMETER");
    SetLastError(0);
    check(WaitForMultipleObjects(MAXIMUM_WAIT_OBJECTS + 1, many, FALSE, 0) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_PARAMETER,
          "multi(count=%d) fails with ERROR_INVALID_PARAMETER", MAXIMUM_WAIT_OBJECTS + 1);
    SetLastError(0);
    check(WaitForMultipleObjects(MAXIMUM_WAIT_OBJECTS + 1, many, TRUE, 0) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_PARAMETER,
          "multi(all, count=%d) fails with ERROR_INVALID_PARAMETER",
          MAXIMUM_WAIT_OBJECTS + 1);

    SetLastError(0);
    check(WaitForMultipleObjectsEx(0, one, FALSE, 0, FALSE) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_PARAMETER,
          "multiEx(count=0) fails with ERROR_INVALID_PARAMETER");
    SetLastError(0);
    check(WaitForMultipleObjectsEx(1, NULL, TRUE, 0, TRUE) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_PARAMETER,
          "multiEx(handles=NULL) fails with ERROR_INVALID_PARAMETER");
    SetLastError(0);
    check(WaitForMultipleObjectsEx(MAXIMUM_WAIT_OBJECTS + 1, many, TRUE, 0, FALSE)
              == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_PARAMETER,
          "multiEx(count=%d) fails with ERROR_INVALID_PARAMETER",
          MAXIMUM_WAIT_OBJECTS + 1);

    /* The boundary itself is accepted: MAXIMUM_WAIT_OBJECTS entries, all the
     * same signalled event, is a legal wait-all. */
    HANDLE full[MAXIMUM_WAIT_OBJECTS];
    for (int i = 0; i < MAXIMUM_WAIT_OBJECTS; i++) full[i] = ev;
    check(WaitForMultipleObjects(MAXIMUM_WAIT_OBJECTS, full, TRUE, 0) == WAIT_OBJECT_0,
          "multi(all, count=%d) is accepted and satisfied", MAXIMUM_WAIT_OBJECTS);

    check(CloseHandle(ev) == TRUE, "CloseHandle of the parameter-row event succeeds");
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
        else if (strcmp(only_row, "multiwait") == 0) {
            /* The multi-object block on its own: a mutant of the wait path
             * cannot be masked by a crash in an unrelated negative row. */
            printf("\nmulti-object waits\n");
            test_multiwait_sentinels();
            test_multiwait_own_thread_value();
            test_multiwait_dead_entries(zeroed);
            test_multiwait_parameters();
        } else {
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

    printf("\nmulti-object waits\n");
    test_multiwait_sentinels();
    test_multiwait_own_thread_value();
    test_multiwait_dead_entries(zeroed);
    test_multiwait_parameters();

    free(zeroed);

    printf("\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
