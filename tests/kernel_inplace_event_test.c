/* Bridge-ABI contract for in-place DISPATCHER_HEADER events. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);

#define CANONICAL_LO 0x00010000u
#define EVENT_VA     0x00101000u
#define EVENT2_VA    0x00101100u
#define RAM_SIZE     (64u * 1024u * 1024u)

recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }
void recomp_diag_thread_start(uint32_t start, uint32_t low, uint32_t high)
{ (void)start; (void)low; (void)high; }
void recomp_diag_thread_end(void) {}
void recomp_diag_record(uint32_t kind, uint32_t target, uint32_t site, uint32_t value)
{ (void)kind; (void)target; (void)site; (void)value; }
/* A2h NULL-slot latch stubs: the real implementation is in the game (src/diagnostics.c), which
 * this standalone fixture does not link. Inert by design -- see apu_watch_fixture_test.c. */
void jsrf_slot_latch_install(uint32_t raw_value, uint32_t installed_value)
{ (void)raw_value; (void)installed_value; }
void jsrf_slot_latch_sample(uint32_t tid, uint32_t call_index, uint32_t ordinal,
                            uint32_t before, uint32_t after)
{ (void)tid; (void)call_index; (void)ordinal; (void)before; (void)after; }

extern ptrdiff_t g_xbox_mem_offset;

static unsigned checks;
static uint8_t *ram;

static int check(int condition, const char *name)
{
    ++checks;
    if (!condition) fprintf(stderr, "FAIL: %s\n", name);
    return condition;
}

static uint8_t *native(uint32_t va)
{
    return ram + (va - CANONICAL_LO);
}

static volatile LONG *signal_state(uint32_t va)
{
    return (volatile LONG *)(native(va) + 4);
}

static void init_header(uint32_t va, uint8_t type, LONG signaled)
{
    uint8_t *p = native(va);
    uint32_t list = va + 8u;
    memset(p, 0, 16);
    p[0] = type;
    p[2] = 4;
    *(LONG *)(p + 4) = signaled;
    *(uint32_t *)(p + 8) = list;
    *(uint32_t *)(p + 12) = list;
}

typedef struct Waiter {
    uint32_t va;
    HANDLE entered;
    volatile LONG status;
} Waiter;

static DWORD WINAPI wait_worker(void *argument)
{
    Waiter *waiter = argument;
    SetEvent(waiter->entered);
    waiter->status = (LONG)xbox_test_bridge_KeWaitForSingleObject(waiter->va, FALSE, -1);
    return 0;
}

static int run_notification_clear_wait(void)
{
    int ok = 1;
    init_header(EVENT_VA, (uint8_t)XboxNotificationEvent, 1);
    xbox_inplace_event_test_reset();
    ok &= check(xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 0) == STATUS_SUCCESS,
                "pre-signaled notification wait completes");
    ok &= check(*signal_state(EVENT_VA) == 1, "notification wait leaves SignalState");
    ok &= check(xbox_test_bridge_NtClearEvent(EVENT_VA) == STATUS_SUCCESS,
                "NtClearEvent in-place succeeds");
    ok &= check(*signal_state(EVENT_VA) == 0, "NtClearEvent stores SignalState 0");
    ok &= check(xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 25) == STATUS_TIMEOUT,
                "clear then wait times out");
    ok &= check(xbox_test_bridge_KeSetEvent(EVENT_VA, 1, FALSE) == 0,
                "KeSetEvent previous was 0");
    ok &= check(*signal_state(EVENT_VA) == 1, "KeSetEvent stores SignalState 1");
    ok &= check(xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 0) == STATUS_SUCCESS,
                "set then wait completes");
    ok &= check(xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 0) == STATUS_SUCCESS,
                "notification stays signaled for a second wait");
    ok &= check(xbox_test_bridge_KeResetEvent(EVENT_VA) == 1, "KeResetEvent previous 1");
    ok &= check(*signal_state(EVENT_VA) == 0, "KeResetEvent stores 0");
    ok &= check(xbox_test_bridge_NtSetEvent(EVENT_VA) == STATUS_SUCCESS,
                "NtSetEvent in-place succeeds");
    ok &= check(*signal_state(EVENT_VA) == 1, "NtSetEvent stores SignalState 1");
    xbox_test_bridge_NtClearEvent(EVENT_VA);
    *signal_state(EVENT_VA) = 1;
    ok &= check(xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 0) == STATUS_SUCCESS,
                "same-VA reinit SignalState 1 resyncs the host event");
    ok &= check(xbox_test_bridge_NtWaitForSingleObject(EVENT_VA, FALSE, 0) == STATUS_SUCCESS,
                "NtWaitForSingleObject uses the in-place path");
    return ok;
}

static int run_header_validation(void)
{
    int ok = 1;
    uint32_t near_end = 0x03FFFFFEu;
    init_header(EVENT_VA, 2, 7);
    native(EVENT_VA)[2] = 4;
    xbox_inplace_event_test_reset();
    xbox_test_bridge_NtClearEvent(EVENT_VA);
    ok &= check(*signal_state(EVENT_VA) == 7,
                "Type 2 Size 4 is not classified as an in-place event");
    init_header(EVENT_VA, (uint8_t)XboxNotificationEvent, 7);
    *(uint32_t *)(native(EVENT_VA) + 8) = 0;
    *(uint32_t *)(native(EVENT_VA) + 12) = 0;
    xbox_test_bridge_NtClearEvent(EVENT_VA);
    ok &= check(*signal_state(EVENT_VA) == 7,
                "zero wait-list is not classified as an in-place event");
    init_header(EVENT_VA, (uint8_t)XboxNotificationEvent, 1);
    xbox_test_bridge_NtClearEvent(EVENT_VA);
    ok &= check(*signal_state(EVENT_VA) == 0, "self-linked header is accepted");
    xbox_test_bridge_NtClearEvent(near_end);
    ok &= check(*signal_state(EVENT_VA) == 0,
                "crossing-bound VA does not clear a valid in-place event");
    return ok;
}

static int run_reuse_and_exhaustion(void)
{
    int ok = 1;
    unsigned i;
    init_header(EVENT_VA, (uint8_t)XboxNotificationEvent, 1);
    xbox_inplace_event_test_reset();
    xbox_test_bridge_KeSetEvent(EVENT_VA, 1, FALSE);
    init_header(EVENT_VA, (uint8_t)XboxSynchronizationEvent, 0);
    ok &= check(xbox_test_bridge_KeSetEvent(EVENT_VA, 1, FALSE) == 0,
                "reused VA with new type still sets");
    ok &= check(xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 0) == STATUS_SUCCESS,
                "reused sync event can be consumed");
    ok &= check(xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 0) == STATUS_TIMEOUT,
                "consumed sync event does not complete twice");
    xbox_inplace_event_test_reset();
    xbox_inplace_event_test_set_cap(2);
    init_header(EVENT_VA, 0, 0);
    init_header(EVENT2_VA, 0, 0);
    ok &= check(xbox_inplace_event_test_try_create(EVENT_VA, native(EVENT_VA), 0, 0),
                "first registry slot");
    ok &= check(xbox_inplace_event_test_try_create(EVENT2_VA, native(EVENT2_VA), 0, 0),
                "second registry slot");
    ok &= check(!xbox_inplace_event_test_try_create(EVENT_VA + 0x200u, native(EVENT_VA + 0x200u), 0, 0),
                "third slot reports exhaustion");
    ok &= check(xbox_inplace_event_test_used() == 2, "used count is cap");
    xbox_inplace_event_test_reset();
    xbox_inplace_event_test_set_cap(128);
    for (i = 0; i < 8; ++i)
        init_header(EVENT_VA + i * 0x20u, 0, 0);
    return ok;
}

static int run_handle_ntclear_still_works(void)
{
    HANDLE handle = NULL;
    int ok = 1;
    NTSTATUS status = xbox_NtCreateEvent(&handle, NULL, XboxNotificationEvent, TRUE);
    ok &= check(status == STATUS_SUCCESS && handle, "NtCreateEvent handle path");
    if (!handle)
        return 0;
    ok &= check(WaitForSingleObject(handle, 0) == WAIT_OBJECT_0, "created signaled");
    ok &= check(xbox_NtClearEvent(handle) == STATUS_SUCCESS, "NtClearEvent handle path");
    ok &= check(WaitForSingleObject(handle, 0) == WAIT_TIMEOUT, "handle clear is unsignaled");
    CloseHandle(handle);
    return ok;
}

static int run_forced_interleaving(void)
{
    int ok = 1;
    unsigned round;
    for (round = 0; round < 256u; ++round) {
        Waiter waiter;
        HANDLE thread;
        init_header(EVENT_VA, (uint8_t)XboxSynchronizationEvent, 0);
        xbox_inplace_event_test_reset();
        waiter.va = EVENT_VA;
        waiter.status = 0xFFFFFFFFu;
        waiter.entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!waiter.entered)
            return 0;
        thread = CreateThread(NULL, 0, wait_worker, &waiter, 0, NULL);
        if (!thread) {
            CloseHandle(waiter.entered);
            return 0;
        }
        if (WaitForSingleObject(waiter.entered, 1000) != WAIT_OBJECT_0)
            ok = 0;
        xbox_test_bridge_KeSetEvent(EVENT_VA, 1, FALSE);
        if (WaitForSingleObject(thread, 1000) != WAIT_OBJECT_0)
            ok = 0;
        ok &= check(waiter.status == (LONG)STATUS_SUCCESS, "waiter observed the first set");
        xbox_test_bridge_KeSetEvent(EVENT_VA, 1, FALSE);
        ok &= check(*signal_state(EVENT_VA) == 1,
                    "second set after consume is not lost");
        ok &= check(xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 0) == STATUS_SUCCESS,
                    "second set is waitable");
        CloseHandle(thread);
        CloseHandle(waiter.entered);
        if (!ok)
            break;
    }
    {
        Waiter waiter;
        HANDLE thread;
        init_header(EVENT_VA, (uint8_t)XboxSynchronizationEvent, 0);
        xbox_inplace_event_test_reset();
        waiter.va = EVENT_VA;
        waiter.status = 0xFFFFFFFFu;
        waiter.entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        ok &= check(waiter.entered != NULL, "pulse waiter event");
        if (!waiter.entered)
            return ok;
        thread = CreateThread(NULL, 0, wait_worker, &waiter, 0, NULL);
        ok &= check(thread != NULL, "pulse waiter thread");
        if (!thread) {
            CloseHandle(waiter.entered);
            return ok;
        }
        ok &= check(WaitForSingleObject(waiter.entered, 1000) == WAIT_OBJECT_0,
                    "pulse waiter entered");
        Sleep(20);
        xbox_test_bridge_NtPulseEvent(EVENT_VA);
        ok &= check(WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0,
                    "pulse wakes the blocked waiter");
        ok &= check(waiter.status == (LONG)STATUS_SUCCESS, "pulse waiter succeeds");
        ok &= check(*signal_state(EVENT_VA) == 0, "pulse leaves SignalState 0");
        ok &= check(xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 0) == STATUS_TIMEOUT,
                    "pulse does not leave a sticky signal");
        CloseHandle(thread);
        CloseHandle(waiter.entered);
    }
    for (round = 0; round < 64u; ++round) {
        Waiter waiter;
        HANDLE thread;
        init_header(EVENT_VA, (uint8_t)XboxSynchronizationEvent, 0);
        xbox_inplace_event_test_reset();
        waiter.va = EVENT_VA;
        waiter.status = 0xFFFFFFFFu;
        waiter.entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!waiter.entered)
            return 0;
        thread = CreateThread(NULL, 0, wait_worker, &waiter, 0, NULL);
        if (!thread) {
            CloseHandle(waiter.entered);
            return 0;
        }
        if (WaitForSingleObject(waiter.entered, 1000) != WAIT_OBJECT_0)
            ok = 0;
        Sleep(20);
        xbox_test_bridge_KeSetEvent(EVENT_VA, 1, FALSE);
        xbox_test_bridge_KeSetEvent(EVENT_VA, 1, FALSE);
        if (WaitForSingleObject(thread, 1000) != WAIT_OBJECT_0)
            ok = 0;
        ok &= check(waiter.status == (LONG)STATUS_SUCCESS,
                    "blocked waiter takes one of two overlapping sets");
        ok &= check(*signal_state(EVENT_VA) == 1,
                    "second set while waiter is blocked remains");
        ok &= check(xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 0) == STATUS_SUCCESS,
                    "leftover overlapping set is waitable");
        CloseHandle(thread);
        CloseHandle(waiter.entered);
        if (!ok)
            break;
    }
    for (round = 0; round < 64u; ++round) {
        Waiter a, b;
        HANDLE threads[2];
        DWORD done;
        unsigned successes;
        init_header(EVENT_VA, (uint8_t)XboxSynchronizationEvent, 0);
        xbox_inplace_event_test_reset();
        a.va = b.va = EVENT_VA;
        a.status = b.status = 0xFFFFFFFFu;
        a.entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        b.entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!a.entered || !b.entered)
            return 0;
        threads[0] = CreateThread(NULL, 0, wait_worker, &a, 0, NULL);
        threads[1] = CreateThread(NULL, 0, wait_worker, &b, 0, NULL);
        if (!threads[0] || !threads[1])
            return 0;
        if (WaitForSingleObject(a.entered, 1000) != WAIT_OBJECT_0 ||
            WaitForSingleObject(b.entered, 1000) != WAIT_OBJECT_0)
            ok = 0;
        Sleep(20);
        xbox_test_bridge_KeSetEvent(EVENT_VA, 1, FALSE);
        done = WaitForMultipleObjects(2, threads, FALSE, 200);
        ok &= check(done == WAIT_OBJECT_0 || done == WAIT_OBJECT_0 + 1,
                    "one signal satisfies one blocked waiter");
        successes = (a.status == (LONG)STATUS_SUCCESS) + (b.status == (LONG)STATUS_SUCCESS);
        ok &= check(successes == 1, "one signal does not satisfy two waiters");
        xbox_test_bridge_KeSetEvent(EVENT_VA, 1, FALSE);
        if (WaitForSingleObject(threads[0], 1000) != WAIT_OBJECT_0 ||
            WaitForSingleObject(threads[1], 1000) != WAIT_OBJECT_0)
            ok = 0;
        ok &= check(a.status == (LONG)STATUS_SUCCESS && b.status == (LONG)STATUS_SUCCESS,
                    "second set releases the remaining waiter");
        ok &= check(xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 0) == STATUS_TIMEOUT,
                    "two sets produce no third completion");
        CloseHandle(threads[0]);
        CloseHandle(threads[1]);
        CloseHandle(a.entered);
        CloseHandle(b.entered);
        if (!ok)
            break;
    }
    return ok;
}

typedef struct GatedWaiter {
    uint32_t va;
    int timeout_ms;
    volatile LONG status;
} GatedWaiter;

static DWORD WINAPI gated_wait_worker(void *argument)
{
    GatedWaiter *waiter = argument;
    waiter->status = (LONG)xbox_test_bridge_KeWaitForSingleObject(
        waiter->va, FALSE, waiter->timeout_ms);
    return 0;
}

static int start_two_host_waiters(GatedWaiter *a, GatedWaiter *b, HANDLE *thread_a, HANDLE *thread_b)
{
    xbox_inplace_event_test_arm_gate(JSRF_EVENT_TEST_GATE_IN_HOST_WAIT);
    a->va = b->va = EVENT_VA;
    a->timeout_ms = b->timeout_ms = -1;
    a->status = b->status = 0xFFFFFFFFu;
    *thread_a = CreateThread(NULL, 0, gated_wait_worker, a, 0, NULL);
    if (!*thread_a || !xbox_inplace_event_test_wait_hit(1000))
        return 0;
    xbox_inplace_event_test_advance_gate(JSRF_EVENT_TEST_GATE_IN_HOST_WAIT);
    *thread_b = CreateThread(NULL, 0, gated_wait_worker, b, 0, NULL);
    if (!*thread_b || !xbox_inplace_event_test_wait_hit(1000))
        return 0;
    xbox_inplace_event_test_release_gate();
    return 1;
}

static int run_gated_two_set_exact(void)
{
    int ok = 1;
    GatedWaiter a, b;
    HANDLE threads[2];

    init_header(EVENT_VA, (uint8_t)XboxSynchronizationEvent, 0);
    xbox_inplace_event_test_reset();
    ok &= check(start_two_host_waiters(&a, &b, &threads[0], &threads[1]),
                "two-set waiters entered host wait");
    if (!ok)
        return 0;
    xbox_test_bridge_KeSetEvent(EVENT_VA, 1, FALSE);
    xbox_test_bridge_KeSetEvent(EVENT_VA, 1, FALSE);
    ok &= check(WaitForSingleObject(threads[0], 1000) == WAIT_OBJECT_0 &&
                WaitForSingleObject(threads[1], 1000) == WAIT_OBJECT_0,
                "two-set waiters joined");
    ok &= check(a.status == (LONG)STATUS_SUCCESS && b.status == (LONG)STATUS_SUCCESS,
                "two sets produce two completions");
    ok &= check(xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 0) == STATUS_TIMEOUT,
                "two sets leave no third completion");
    CloseHandle(threads[0]);
    CloseHandle(threads[1]);
    return ok;
}

static int run_gated_type1_pulse_exact(void)
{
    int ok = 1;
    GatedWaiter a, b;
    HANDLE threads[2];
    unsigned successes;

    init_header(EVENT_VA, (uint8_t)XboxSynchronizationEvent, 0);
    xbox_inplace_event_test_reset();
    ok &= check(start_two_host_waiters(&a, &b, &threads[0], &threads[1]),
                "pulse waiters entered host wait");
    if (!ok)
        return 0;
    xbox_test_bridge_NtPulseEvent(EVENT_VA);
    Sleep(50);
    successes = (a.status == (LONG)STATUS_SUCCESS) + (b.status == (LONG)STATUS_SUCCESS);
    ok &= check(successes == 1, "Type-1 pulse completes exactly one waiter");
    ok &= check(xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 0) == STATUS_TIMEOUT,
                "Type-1 pulse leaves no sticky completion");
    xbox_test_bridge_KeSetEvent(EVENT_VA, 1, FALSE);
    ok &= check(WaitForSingleObject(threads[0], 1000) == WAIT_OBJECT_0 &&
                WaitForSingleObject(threads[1], 1000) == WAIT_OBJECT_0,
                "remaining pulse waiter released by a later set");
    ok &= check(xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 0) == STATUS_TIMEOUT,
                "pulse plus one set leave no extra completion");
    CloseHandle(threads[0]);
    CloseHandle(threads[1]);
    return ok;
}

static int run_gated_timeout_set_race(void)
{
    int ok = 1;
    GatedWaiter waiter;
    HANDLE thread;

    init_header(EVENT_VA, (uint8_t)XboxSynchronizationEvent, 0);
    xbox_inplace_event_test_reset();
    xbox_inplace_event_test_arm_gate(JSRF_EVENT_TEST_GATE_WAIT_RETURNED);
    waiter.va = EVENT_VA;
    waiter.timeout_ms = 0;
    waiter.status = 0xFFFFFFFFu;
    thread = CreateThread(NULL, 0, gated_wait_worker, &waiter, 0, NULL);
    ok &= check(thread != NULL, "timeout-race worker");
    if (!thread)
        return 0;
    ok &= check(xbox_inplace_event_test_wait_hit(1000), "timeout-race reached wait-returned");
    xbox_test_bridge_KeSetEvent(EVENT_VA, 1, FALSE);
    xbox_inplace_event_test_release_gate();
    ok &= check(WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0, "timeout-race joined");
    ok &= check(waiter.status == (LONG)STATUS_TIMEOUT, "timeout-race waiter times out");
    ok &= check(xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 0) == STATUS_SUCCESS,
                "timeout-race leftover set completes once");
    ok &= check(xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 0) == STATUS_TIMEOUT,
                "timeout-race does not double-complete");
    CloseHandle(thread);
    return ok;
}

static int run_gated_reset_after_wake(void)
{
    int ok = 1;
    GatedWaiter waiter;
    HANDLE thread;

    init_header(EVENT_VA, (uint8_t)XboxSynchronizationEvent, 0);
    xbox_inplace_event_test_reset();
    xbox_inplace_event_test_arm_gate(JSRF_EVENT_TEST_GATE_IN_HOST_WAIT);
    waiter.va = EVENT_VA;
    waiter.timeout_ms = -1;
    waiter.status = 0xFFFFFFFFu;
    thread = CreateThread(NULL, 0, gated_wait_worker, &waiter, 0, NULL);
    ok &= check(thread != NULL, "reset-race worker");
    if (!thread)
        return 0;
    ok &= check(xbox_inplace_event_test_wait_hit(1000), "reset-race in host wait");
    xbox_inplace_event_test_advance_gate(JSRF_EVENT_TEST_GATE_WAIT_RETURNED);
    xbox_test_bridge_KeSetEvent(EVENT_VA, 1, FALSE);
    ok &= check(xbox_inplace_event_test_wait_hit(1000), "reset-race after host wait return");
    xbox_test_bridge_KeResetEvent(EVENT_VA);
    xbox_inplace_event_test_release_gate();
    ok &= check(WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0, "reset-race joined");
    ok &= check(waiter.status == (LONG)STATUS_SUCCESS,
                "reset does not revoke an assigned completion");
    ok &= check(*signal_state(EVENT_VA) == 0, "reset cleared unconsumed state");
    ok &= check(xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 0) == STATUS_TIMEOUT,
                "reset leaves no extra completion");
    CloseHandle(thread);
    return ok;
}

static int run_gated_pulse_before_host_wait(void)
{
    int ok = 1;
    GatedWaiter waiter;
    HANDLE thread;

    init_header(EVENT_VA, (uint8_t)XboxSynchronizationEvent, 0);
    xbox_inplace_event_test_reset();
    xbox_inplace_event_test_arm_gate(JSRF_EVENT_TEST_GATE_REGISTERED);
    waiter.va = EVENT_VA;
    waiter.timeout_ms = -1;
    waiter.status = 0xFFFFFFFFu;
    thread = CreateThread(NULL, 0, gated_wait_worker, &waiter, 0, NULL);
    ok &= check(thread != NULL, "pulse-race worker");
    if (!thread)
        return 0;
    ok &= check(xbox_inplace_event_test_wait_hit(1000), "pulse-race registered");
    xbox_test_bridge_NtPulseEvent(EVENT_VA);
    xbox_inplace_event_test_release_gate();
    Sleep(20);
    xbox_test_bridge_KeSetEvent(EVENT_VA, 1, FALSE);
    ok &= check(WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0, "pulse-race joined");
    ok &= check(waiter.status == (LONG)STATUS_SUCCESS, "real set after pulse completes once");
    ok &= check(xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 0) == STATUS_TIMEOUT,
                "pulse before host wait does not leave a phantom credit");
    CloseHandle(thread);
    return ok;
}

int main(void)
{
    int ok = 1;
    ram = (uint8_t *)calloc(1, RAM_SIZE);
    if (!ram)
        return 2;
    g_xbox_mem_offset = (ptrdiff_t)ram - (ptrdiff_t)CANONICAL_LO;
    xbox_inplace_event_test_reset();
    ok &= run_notification_clear_wait();
    ok &= run_header_validation();
    ok &= run_reuse_and_exhaustion();
    ok &= run_handle_ntclear_still_works();
    ok &= run_forced_interleaving();
    ok &= run_gated_timeout_set_race();
    ok &= run_gated_reset_after_wake();
    ok &= run_gated_pulse_before_host_wait();
    ok &= run_gated_two_set_exact();
    ok &= run_gated_type1_pulse_exact();
    xbox_inplace_event_test_reset();
    xbox_inplace_event_test_release_gate();
    free(ram);
    if (ok)
        printf("PASS: %u in-place event bridge ABI checks\n", checks);
    return ok ? 0 : 1;
}
