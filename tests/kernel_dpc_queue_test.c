/* Bridge contract for the DPC queue: queued at most once, removable, and
 * dequeued before its routine runs. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);

#define CANONICAL_LO 0x00010000u
#define DPC_A_VA     0x00102000u
#define DPC_B_VA     0x00102040u
#define DPC_C_VA     0x00102080u
#define RAM_SIZE     (64u * 1024u * 1024u)
#define THREAD_DPCS  8u
#define THREAD_LOOPS 20000u

recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }
void recomp_diag_thread_start(uint32_t start, uint32_t low, uint32_t high)
{ (void)start; (void)low; (void)high; }
void recomp_diag_thread_end(void) {}
void recomp_diag_record(uint32_t kind, uint32_t target, uint32_t site, uint32_t value)
{ (void)kind; (void)target; (void)site; (void)value; }
/* A2h stubs: the real implementations are in the game (src/diagnostics.c),
 * which this standalone fixture does not link. Inert, as in
 * kernel_inplace_event_test.c. */
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

/* A KDPC with no routine: the drain dequeues it and runs nothing. */
static void init_dpc(uint32_t va)
{
    memset(native(va), 0, 32);
    *(uint16_t *)native(va) = 0x13;
}

static uint8_t inserted(uint32_t va)
{
    return native(va)[2];
}

static int run_queue_once(void)
{
    int ok = 1;
    init_dpc(DPC_A_VA);
    init_dpc(DPC_B_VA);
    ok &= check(xbox_test_bridge_KeInsertQueueDpc(DPC_A_VA, 1, 2) == 1, "first insert queues");
    ok &= check(inserted(DPC_A_VA) == 1, "insert sets Inserted");
    ok &= check(xbox_test_bridge_KeInsertQueueDpc(DPC_A_VA, 3, 4) == 0,
                "insert of a queued DPC returns FALSE");
    ok &= check(xbox_test_dpc_pending() == 1, "a queued DPC is queued once");
    ok &= check(xbox_test_bridge_KeInsertQueueDpc(DPC_B_VA, 5, 6) == 1, "second DPC queues");
    ok &= check(xbox_test_dpc_pending() == 2, "two DPCs pending");
    xbox_test_drain_dpcs();
    ok &= check(xbox_test_dpc_pending() == 0, "drain empties the queue");
    ok &= check(inserted(DPC_A_VA) == 0 && inserted(DPC_B_VA) == 0, "drain clears Inserted");
    ok &= check(xbox_test_bridge_KeInsertQueueDpc(DPC_A_VA, 7, 8) == 1,
                "a drained DPC can be queued again");
    xbox_test_drain_dpcs();
    return ok;
}

static int run_remove(void)
{
    int ok = 1;
    init_dpc(DPC_A_VA);
    init_dpc(DPC_B_VA);
    init_dpc(DPC_C_VA);
    ok &= check(xbox_test_bridge_KeRemoveQueueDpc(DPC_A_VA) == 0,
                "removing an unqueued DPC returns FALSE");
    xbox_test_bridge_KeInsertQueueDpc(DPC_A_VA, 0, 0);
    xbox_test_bridge_KeInsertQueueDpc(DPC_B_VA, 0, 0);
    xbox_test_bridge_KeInsertQueueDpc(DPC_C_VA, 0, 0);
    ok &= check(xbox_test_bridge_KeRemoveQueueDpc(DPC_B_VA) == 1,
                "removing a queued DPC returns TRUE");
    ok &= check(inserted(DPC_B_VA) == 0, "remove clears Inserted");
    ok &= check(xbox_test_dpc_pending() == 2, "remove takes exactly one entry");
    ok &= check(xbox_test_bridge_KeRemoveQueueDpc(DPC_B_VA) == 0, "a removed DPC is gone");
    ok &= check(xbox_test_bridge_KeRemoveQueueDpc(DPC_A_VA) == 1, "the head survives a removal");
    ok &= check(xbox_test_bridge_KeRemoveQueueDpc(DPC_C_VA) == 1, "the tail survives a removal");
    ok &= check(xbox_test_dpc_pending() == 0, "queue empty after removals");
    return ok;
}

static int run_full(void)
{
    int ok = 1, queued = 0;
    uint32_t i;
    for (i = 0; i < 80u; ++i) {
        uint32_t va = 0x00110000u + i * 32u;
        init_dpc(va);
        queued += xbox_test_bridge_KeInsertQueueDpc(va, 0, 0);
    }
    ok &= check(queued == 63, "a full queue refuses further DPCs");
    xbox_test_drain_dpcs();
    ok &= check(xbox_test_dpc_pending() == 0, "drain empties a full queue");
    return ok;
}

/* Two threads insert and remove disjoint DPCs; a torn queue loses or
 * duplicates entries, which the final counts would show. */
static DWORD WINAPI churn(void *argument)
{
    uint32_t base = (uint32_t)(uintptr_t)argument, n, k;
    for (n = 0; n < THREAD_LOOPS; ++n) {
        for (k = 0; k < THREAD_DPCS; ++k)
            xbox_test_bridge_KeInsertQueueDpc(base + k * 32u, 0, 0);
        for (k = 0; k < THREAD_DPCS; ++k)
            xbox_test_bridge_KeRemoveQueueDpc(base + k * 32u);
    }
    return 0;
}

static int run_concurrent(void)
{
    int ok = 1;
    uint32_t k, a = 0x00120000u, b = 0x00121000u;
    HANDLE t[2];
    for (k = 0; k < THREAD_DPCS; ++k) {
        init_dpc(a + k * 32u);
        init_dpc(b + k * 32u);
    }
    t[0] = CreateThread(NULL, 0, churn, (void *)(uintptr_t)a, 0, NULL);
    t[1] = CreateThread(NULL, 0, churn, (void *)(uintptr_t)b, 0, NULL);
    WaitForMultipleObjects(2, t, TRUE, INFINITE);
    CloseHandle(t[0]);
    CloseHandle(t[1]);
    ok &= check(xbox_test_dpc_pending() == 0, "concurrent insert/remove leaves the queue empty");
    for (k = 0; k < THREAD_DPCS; ++k)
        ok &= check(inserted(a + k * 32u) == 0 && inserted(b + k * 32u) == 0,
                    "concurrent insert/remove leaves Inserted clear");
    return ok;
}

int main(void)
{
    int ok = 1;
    ram = (uint8_t *)calloc(1, RAM_SIZE);
    if (!ram)
        return 2;
    g_xbox_mem_offset = (ptrdiff_t)ram - (ptrdiff_t)CANONICAL_LO;
    ok &= run_queue_once();
    ok &= run_remove();
    ok &= run_full();
    ok &= run_concurrent();
    free(ram);
    if (ok)
        printf("PASS: %u DPC queue bridge checks\n", checks);
    return ok ? 0 : 1;
}
