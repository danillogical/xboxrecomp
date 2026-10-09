/* IRQL tracking in kernel_hal.c: every raise the guest can make is counted by
 * the interrupt gates, and KeGetCurrentIrql reports the tracked level.
 *
 * KeRaiseIrqlToSynchLevel used to return PASSIVE_LEVEL without recording the
 * raise, so a guest using it was invisible to xbox_IrqlBlocksInterrupts and to
 * serial guest mode's DPC gate; KeGetCurrentIrql always answered PASSIVE_LEVEL.
 *
 * Run with RECOMP_GUEST_SERIAL=1 so the device-level count is kept too. */
#include "kernel.h"
#include "guest_meter.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>

/* The guest stack the IRQL holders record; esp 0 means no guest frame is read. */
RECOMP_TLS uint32_t g_esp;
ptrdiff_t g_xbox_mem_offset;

/* kernel_hal.c's other dependencies, inert here. */
bool nv2a_hook_pci_config_read(uint32_t offset, void *buffer, uint32_t length)
{ (void)offset; (void)buffer; (void)length; return false; }
bool nv2a_hook_pci_config_write(uint32_t offset, const void *buffer, uint32_t length)
{ (void)offset; (void)buffer; (void)length; return false; }
void xbox_log(int level, const char *subsystem, const char *fmt, ...)
{ (void)level; (void)subsystem; (void)fmt; }

static unsigned checks, failures;

static void check(int ok, const char *what)
{
    ++checks;
    if (!ok) {
        ++failures;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

static HANDLE raised, release;

static DWORD WINAPI raise_on_another_thread(LPVOID unused)
{
    (void)unused;
    xbox_KeRaiseIrqlToSynchLevel();
    SetEvent(raised);
    WaitForSingleObject(release, INFINITE);
    xbox_KfLowerIrql(PASSIVE_LEVEL);
    return 0;
}

int main(void)
{
    HANDLE thread;

    check(xbox_GuestSerialEnabled(), "RECOMP_GUEST_SERIAL=1 is set for this test");
    check(xbox_KeGetCurrentIrql() == PASSIVE_LEVEL, "a thread starts at PASSIVE_LEVEL");
    check(!xbox_IrqlBlocksInterrupts(), "nothing blocks interrupts at the start");

    /* The defect: this raise was not recorded. */
    check(xbox_KeRaiseIrqlToSynchLevel() == PASSIVE_LEVEL,
          "KeRaiseIrqlToSynchLevel returns the previous level");
    check(xbox_KeGetCurrentIrql() == SYNCH_LEVEL, "KeGetCurrentIrql reports SYNCH_LEVEL");
    check(SYNCH_LEVEL == DISPATCH_LEVEL, "SYNCH_LEVEL is DISPATCH_LEVEL on a uniprocessor");
    check(xbox_IrqlBlocksInterrupts(), "a raise to SYNCH_LEVEL blocks DPC delivery");
    check(!xbox_IrqlBlocksDeviceInterrupts(), "SYNCH_LEVEL does not mask device interrupts");
    xbox_KfLowerIrql(PASSIVE_LEVEL);
    check(xbox_KeGetCurrentIrql() == PASSIVE_LEVEL, "the lower returns to PASSIVE_LEVEL");
    check(!xbox_IrqlBlocksInterrupts(), "the lower re-opens the gate");

    /* A device level masks devices as well. */
    check(xbox_KfRaiseIrql(5) == PASSIVE_LEVEL, "KfRaiseIrql returns the previous level");
    check(xbox_KeGetCurrentIrql() == 5, "KeGetCurrentIrql reports a device level");
    check(xbox_IrqlBlocksDeviceInterrupts(), "a device level masks device interrupts");
    xbox_KfLowerIrql(PASSIVE_LEVEL);
    check(!xbox_IrqlBlocksDeviceInterrupts() && !xbox_IrqlBlocksInterrupts(),
          "lowering from a device level re-opens both gates");

    /* IRQL is per thread; the gate is for the whole processor. */
    raised = CreateEventW(NULL, FALSE, FALSE, NULL);
    release = CreateEventW(NULL, FALSE, FALSE, NULL);
    thread = CreateThread(NULL, 0, raise_on_another_thread, NULL, 0, NULL);
    WaitForSingleObject(raised, INFINITE);
    check(xbox_KeGetCurrentIrql() == PASSIVE_LEVEL, "another thread's raise is not this thread's level");
    check(xbox_IrqlBlocksInterrupts(), "another thread's raise blocks DPC delivery");
    SetEvent(release);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    check(xbox_IrqlRaisedCount() == 0, "every raise was matched by a lower");

    if (failures)
        return 1;
    printf("PASS: %u IRQL tracking checks\n", checks);
    return 0;
}
