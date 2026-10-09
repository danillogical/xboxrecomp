/* A file-I/O APC must not run inside the completion that queues it.
 *
 * JSRF's ADX poller (0x140BA0) sets an in-flight flag, issues the read, and
 * only then looks at that flag. The completion clears the flag. The poller
 * promotes the file only if the flag was still set and the alertable 0 ms
 * delay at 0x145C28 then ran the completion. Delivering the APC inside
 * NtReadFile skips that delay and leaves the file in state 2.
 *
 * This fixture drives bridge_complete_file_io and bridge_KeDelayExecutionThread.
 * The first assertion fails if the APC runs before any alertable wait.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "d3d/d3d8_xbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An alertable wait or delay that delivered APCs returns this. */
#ifndef STATUS_USER_APC
#define STATUS_USER_APC ((NTSTATUS)0x000000C0L)
#endif

#define EVENT_VA 0x00101000u

typedef void (*recomp_func_t)(void);

recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }
void recomp_diag_thread_start(uint32_t start, uint32_t low, uint32_t high)
{ (void)start; (void)low; (void)high; }
void recomp_diag_thread_end(void) {}
void recomp_diag_record(uint32_t kind, uint32_t target, uint32_t site, uint32_t value)
{ (void)kind; (void)target; (void)site; (void)value; }
void jsrf_slot_latch_install(uint32_t raw_value, uint32_t installed_value)
{ (void)raw_value; (void)installed_value; }
void jsrf_slot_latch_sample(uint32_t tid, uint32_t call_index, uint32_t ordinal,
                            uint32_t before, uint32_t after)
{ (void)tid; (void)call_index; (void)ordinal; (void)before; (void)after; }
void jsrf_slot_watch_handshake(uint32_t slot_va) { (void)slot_va; }
void jsrf_slot_watch_alias_armed(uint32_t mapped_mask, uint32_t protect_mask, uint32_t alias_count)
{ (void)mapped_mask; (void)protect_mask; (void)alias_count; }
int jsrf_slot_watch_alias_touch(uint32_t alias_index, uint32_t fault_va, uint64_t rip,
                                uint32_t value, uint32_t published)
{ (void)alias_index; (void)fault_va; (void)rip; (void)value; (void)published; return 0; }
void jsrf_slot_watch_write(uint32_t provenance, uint32_t before, uint32_t after,
                           uint64_t rip, uint32_t ordinal)
{ (void)provenance; (void)before; (void)after; (void)rip; (void)ordinal; }
int  xbox_VideoIsPlaying(void) { return 0; }
int  xbox_VideoPlayFile(const char *host_path) { (void)host_path; return 0; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowPresent(void) {}
uint32_t xbox_FramebufferPresentSerial(void) { return 0; }
unsigned long long xbox_FramebufferPresentHash(void) { return 0; }
void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars) { (void)name; (void)max_chars; }
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
IDirect3DDevice8 *xbox_GetD3DDevice(void) { return NULL; }

extern ptrdiff_t g_xbox_mem_offset;
extern RECOMP_TLS uint32_t g_esp;

#define CANONICAL_LO 0x00010000u
#define RAM_SIZE     (64u * 1024u * 1024u)
#define TEST_STACK   0x0003F000u
#define APC_VA       0x00F10000u
#define HIT_MAX      80

static unsigned checks;
static uint8_t *ram;
static int hits;
static uint32_t hit_ctx[HIT_MAX];
static uint32_t hit_ios[HIT_MAX];

static int check(int condition, const char *name)
{
    ++checks;
    if (!condition) fprintf(stderr, "FAIL: %s\n", name);
    return condition;
}

/* stdcall shape: deliver_one_apc pushes a dummy return and three args, and
 * the routine's ret pops the return. Context 0xA1 queues a follow-up that
 * must stay pending until the next alertable wait. */
static void test_apc(void)
{
    uint32_t ctx = *(uint32_t *)((uintptr_t)(g_esp + 4u) + g_xbox_mem_offset);
    uint32_t ios = *(uint32_t *)((uintptr_t)(g_esp + 8u) + g_xbox_mem_offset);

    if (hits < HIT_MAX) {
        hit_ctx[hits] = ctx;
        hit_ios[hits] = ios;
    }
    hits++;
    if (ctx == 0xA1u)
        xbox_test_complete_file_io(0, APC_VA, 0xA2u, 0xA2A2u);
    g_esp += 4;
}

/* A notification event's DISPATCHER_HEADER, in place in guest RAM. */
static void init_header(uint32_t va, uint8_t type, LONG signaled)
{
    uint8_t *p = ram + (va - CANONICAL_LO);
    uint32_t list = va + 8u;
    memset(p, 0, 16);
    p[0] = type;
    p[2] = 4;
    *(LONG *)(p + 4) = signaled;
    *(uint32_t *)(p + 8) = list;
    *(uint32_t *)(p + 12) = list;
}

static void queue(uint32_t ctx, uint32_t ios)
{
    xbox_test_complete_file_io(0, APC_VA, ctx, ios);
}

int main(void)
{
    int ok = 1;
    int before;
    int i;
    ULONGLONG t0;
    NTSTATUS st;

    ram = (uint8_t *)calloc(1, RAM_SIZE);
    if (!ram) return 2;
    g_xbox_mem_offset = (ptrdiff_t)ram - (ptrdiff_t)CANONICAL_LO;
    g_esp = TEST_STACK;
    xbox_test_set_file_apc_routine(APC_VA, test_apc);

    queue(0x11u, 0x22u);
    ok &= check(xbox_test_file_apc_pending() == 1, "completion queues the APC");
    ok &= check(hits == 0, "completion does not run the APC");

    st = xbox_test_bridge_KeDelayExecutionThread(FALSE, 0);
    ok &= check(st == STATUS_SUCCESS, "non-alertable zero delay succeeds");
    ok &= check(xbox_test_file_apc_pending() == 1, "non-alertable delay leaves the APC");
    ok &= check(hits == 0, "non-alertable delay does not run the APC");

    st = xbox_test_bridge_KeDelayExecutionThread(TRUE, 0);
    ok &= check(st == STATUS_USER_APC, "alertable delay that delivered an APC returns STATUS_USER_APC");
    ok &= check(xbox_test_file_apc_pending() == 0, "alertable delay drains the queue");
    ok &= check(hits == 1, "alertable delay runs the APC once");
    ok &= check(hits == 1 && hit_ctx[0] == 0x11u && hit_ios[0] == 0x22u,
                "the APC sees its context and status block");
    ok &= check(g_esp == TEST_STACK, "the delay restores the guest stack");

    st = xbox_test_bridge_KeDelayExecutionThread(TRUE, 0);
    ok &= check(st == STATUS_SUCCESS && hits == 1 && xbox_test_file_apc_pending() == 0,
                "a second alertable delay does not run it again");

    before = hits;
    queue(0xB1u, 0xB100u);
    queue(0xB2u, 0xB200u);
    ok &= check(xbox_test_file_apc_pending() == 2, "two APCs stay queued");
    st = xbox_test_bridge_KeDelayExecutionThread(TRUE, 0);
    ok &= check(st == STATUS_USER_APC && hits == before + 2, "both APCs run");
    ok &= check(hit_ctx[before] == 0xB1u && hit_ctx[before + 1] == 0xB2u,
                "APCs run in queue order");
    ok &= check(xbox_test_file_apc_pending() == 0, "the batch is empty afterwards");

    before = hits;
    queue(0xA1u, 0xA100u);
    st = xbox_test_bridge_KeDelayExecutionThread(TRUE, 0);
    ok &= check(st == STATUS_USER_APC && hits == before + 1 && hit_ctx[before] == 0xA1u,
                "an APC queued by an APC does not run in the same drain");
    ok &= check(xbox_test_file_apc_pending() == 1, "the follow-up stays pending");
    st = xbox_test_bridge_KeDelayExecutionThread(TRUE, 0);
    ok &= check(st == STATUS_USER_APC && hits == before + 2 && hit_ctx[before + 1] == 0xA2u
                && hit_ios[before + 1] == 0xA2A2u,
                "the next alertable delay runs the follow-up");
    ok &= check(xbox_test_file_apc_pending() == 0, "the follow-up is gone");

    before = hits;
    for (i = 0; i < 32; i++)
        queue(0xE000u + (uint32_t)i, 0);
    ok &= check(xbox_test_file_apc_pending() == 32 && hits == before,
                "thirty-two APCs fit without running");
    queue(0xE100u, 0);
    ok &= check(hits == before + 1 && hit_ctx[before] == 0xE100u,
                "the overflow APC runs inline");
    ok &= check(xbox_test_file_apc_pending() == 32, "the queued batch is still there");
    st = xbox_test_bridge_KeDelayExecutionThread(TRUE, 0);
    ok &= check(st == STATUS_USER_APC && hits == before + 33
                && xbox_test_file_apc_pending() == 0,
                "the alertable delay runs the queued batch");
    ok &= check(g_esp == TEST_STACK, "overflow delivery restores the guest stack");

    /* The title's APC routine is NtUserIoApcDispatcher, not the stub above.
     * The thunk already pops that stdcall frame. A second pop of 12 leaves
     * the waiter high, which is the slot 0x145C28's pop esi then reads. */
    xbox_test_arm_io_apc_dispatcher();
    g_esp = TEST_STACK;
    /* 0x20000 is inside the fixture RAM. The dispatcher reads the status
     * block before it looks the completion up, so a low address faults. */
    xbox_test_complete_file_io(0, 0xFE000068u, 0x001401B0u, 0x00020000u);
    ok &= check(xbox_test_file_apc_pending() == 1, "dispatcher APC stays queued");
    ok &= check(hits == before + 33, "queuing the dispatcher does not run the stub");
    st = xbox_test_bridge_KeDelayExecutionThread(TRUE, 0);
    ok &= check(st == STATUS_USER_APC, "dispatcher APC delay reports the delivery");
    ok &= check(xbox_test_file_apc_pending() == 0, "dispatcher APC delay drains it");
    ok &= check(g_esp == TEST_STACK, "dispatcher APC delivery restores the guest stack");

    /* ── Delivery ends the wait ─────────────────────────────────────── */
    xbox_test_set_file_apc_routine(APC_VA, test_apc);
    g_esp = TEST_STACK;

    /* An alertable delay with nothing queued is an ordinary delay. */
    t0 = GetTickCount64();
    st = xbox_test_bridge_KeDelayExecutionThread(TRUE, 30);
    ok &= check(st == STATUS_SUCCESS, "alertable delay with no APC returns STATUS_SUCCESS");
    ok &= check(GetTickCount64() - t0 >= 15, "alertable delay with no APC still sleeps");

    /* A queued APC ends a long alertable delay at once. */
    before = hits;
    queue(0xC1u, 0xC100u);
    t0 = GetTickCount64();
    st = xbox_test_bridge_KeDelayExecutionThread(TRUE, 2000);
    ok &= check(st == STATUS_USER_APC, "2 s alertable delay with an APC returns STATUS_USER_APC");
    ok &= check(GetTickCount64() - t0 < 1000, "the APC ended the 2 s delay promptly");
    ok &= check(hits == before + 1 && xbox_test_file_apc_pending() == 0,
                "the delay ran the queued APC");

    /* Unsignalled event, APC queued, long timeout: the APC wins. */
    init_header(EVENT_VA, (uint8_t)XboxNotificationEvent, 1);
    xbox_test_bridge_KeResetEvent(EVENT_VA);
    before = hits;
    queue(0xD1u, 0xD100u);
    t0 = GetTickCount64();
    st = xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, TRUE, 10000);
    ok &= check(st == STATUS_USER_APC, "alertable wait on an unsignalled event returns STATUS_USER_APC");
    ok &= check(GetTickCount64() - t0 < 2000, "the APC ended the 10 s wait promptly");
    ok &= check(hits == before + 1 && xbox_test_file_apc_pending() == 0,
                "the wait ran the queued APC");

    /* Signalled event, APC queued: the object's status wins and the APC waits. */
    xbox_test_bridge_KeSetEvent(EVENT_VA, 1, FALSE);
    before = hits;
    queue(0xD2u, 0xD200u);
    st = xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, TRUE, 10000);
    ok &= check(st == STATUS_SUCCESS, "alertable wait on a signalled event returns STATUS_SUCCESS");
    ok &= check(xbox_test_file_apc_pending() == 1, "the APC is still queued after a signalled wait");
    ok &= check(hits == before, "a signalled wait does not run the APC");

    /* A non-alertable wait never drains. */
    xbox_test_bridge_KeResetEvent(EVENT_VA);
    st = xbox_test_bridge_KeWaitForSingleObject(EVENT_VA, FALSE, 30);
    ok &= check(st == STATUS_TIMEOUT, "non-alertable wait times out");
    ok &= check(xbox_test_file_apc_pending() == 1 && hits == before,
                "a non-alertable wait leaves the APC alone");

    /* Clean up: the next alertable delay delivers it. */
    st = xbox_test_bridge_KeDelayExecutionThread(TRUE, 0);
    ok &= check(st == STATUS_USER_APC && hits == before + 1 && xbox_test_file_apc_pending() == 0,
                "the leftover APC is delivered by a later alertable delay");
    ok &= check(g_esp == TEST_STACK, "waits restore the guest stack");

    free(ram);
    printf("%s: %u file-APC checks passed\n", ok ? "PASS" : "FAIL", checks);
    return ok ? 0 : 1;
}
