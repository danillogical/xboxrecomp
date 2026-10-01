/**
 * guest_meter.h - How many host threads run lifted guest code at once
 *
 * The meter is observation only, and off unless RECOMP_GUEST_METER=1 (read
 * once, cached).
 * Off, every call returns at a cached flag test. On, it touches only its own
 * counters with interlocked operations: no lock, no wait, and no guest-visible
 * state, so scheduling and guest behaviour are unchanged.
 *
 * Each host thread is outside guest code until the runtime hands it lifted
 * code, and inside until that code enters the kernel or the host invocation
 * returns. The runtime brackets every such hand-over:
 *
 *     int t = xbox_GuestMeterEnter(XBOX_GM_DPC);   guest code starts
 *     fn();
 *     xbox_GuestMeterRestore(t, XBOX_GM_DPC);       and stops again
 *
 * and kernel_thunk_dispatch brackets the kernel call the other way round with
 * xbox_GuestMeterLeave / xbox_GuestMeterRestore(t, XBOX_GM_KERNEL).
 *
 * Serialised guest mode is the same tracker with a lock behind it. It is off
 * unless RECOMP_GUEST_SERIAL=1 (read once, cached), and it turns the tracking
 * on whether or not the meter is on. An Xbox has one processor, so this lets
 * one host thread at a time run guest code.
 *
 * An outside-to-inside transition takes a single global lock, and
 * inside-to-outside releases it. A thread already inside does not take it
 * again. A kernel call is outside, so a guest thread that blocks in the kernel
 * lets the others run, as a thread that waits does on hardware.
 *
 * The main thread enters guest code without a bracket, and is first seen at
 * its first kernel call. Until then it runs without the lock. No other guest
 * thread can exist before that call, because threads, timers and interrupts
 * are all created by kernel calls.
 *
 * A wait for the lock is bounded, at 100 ms unless
 * RECOMP_GUEST_SERIAL_TIMEOUT_MS says otherwise. A guest thread that spins
 * without calling the kernel would otherwise hold everyone out for good. A
 * waiter that times out runs anyway, without the lock. That is an overrun: it
 * is counted, and the first few are logged with both host thread ids.
 *
 * Interrupt-level work runs in an atomic section,
 * xbox_GuestSerialBeginAtomic / xbox_GuestSerialEndAtomic. The section keeps
 * the lock across the kernel calls its routine makes, because nothing preempts
 * an interrupt routine or a DPC on hardware. The timer thread opens one around
 * each tick's interrupt delivery and DPC work, and skips that work for the
 * tick when the guest has raised IRQL. Serial mode off, the pair is exactly
 * xbox_GuestMeterEnter / xbox_GuestMeterRestore.
 *
 * xbox_GuestSerialYield lets a waiter in if there is one. It acts on every
 * 4096th call per thread, so a caller can put it on a hot path. Lifted code
 * calls it from RECOMP_BACKEDGE (templates/runtime/recomp_types.h), which the
 * translator emits before every loop back-edge when run with --backedge-yield;
 * the macro tests g_xbox_guest_serial_on first, so serial mode off costs a
 * loop one load.
 *
 * Serial mode off, every serial check returns at a cached flag test. The
 * [GMETER] line prints when either mode is on, so a serial run shows max=1
 * unless something overran. The [GSERIAL] line prints only in serial mode.
 */
#ifndef XBOX_GUEST_METER_H
#define XBOX_GUEST_METER_H

#include "platform/xbox_winnt.h"

/* Every transition source. The record is keyed by this enum and nothing else,
 * so it is fixed-size. */
enum xbox_gmeter_source {
    XBOX_GM_MAIN,           /* host calls xbox_GuestMeterEnter before the title entry */
    XBOX_GM_UNHOOKED,       /* thread first seen at a kernel call: entered by an unmetered
                               path -- the main thread's entry unless the host reports it */
    XBOX_GM_KERNEL,         /* entry: a kernel call returns; exit: guest calls the kernel */
    XBOX_GM_THREAD,         /* a spawned thread's start routine */
    XBOX_GM_THREAD_INLINE,  /* a start routine run on the calling thread */
    XBOX_GM_ISR,            /* a connected interrupt routine (kernel_raise_interrupt) */
    XBOX_GM_USB_ISR,        /* the OHCI interrupt routine */
    XBOX_GM_DPC,            /* a queued DPC drained by the timer thread */
    XBOX_GM_TIMER_DPC,      /* a timer's DPC on expiry */
    XBOX_GM_IO_APC,         /* a file I/O completion APC */
    XBOX_GM_IO_APC_DISPATCH,/* NtUserIoApcDispatcher's completion routine */
    XBOX_GM_SYNC_EXEC,      /* KeSynchronizeExecution's routine */
    XBOX_GM_SEH_UNWIND,     /* an exception handler called by RtlUnwind */
    XBOX_GM_SOURCE_COUNT
};

/* The decision record. Per source: entries (a thread goes from outside to
 * inside), contended (entries that found another thread already inside),
 * nested (entries by a thread that was already inside, no count change) and
 * exits (inside to outside). Fields are read one at a time, so a snapshot
 * taken while threads run is not a single instant. */
typedef struct xbox_gmeter_record {
    LONG     inside;             /* threads inside when read */
    LONG     max_inside;         /* the most ever inside at once */
    LONGLONG contended;          /* sum of contended_by[] */
    LONGLONG host_kernel_calls;  /* kernel dispatches from a thread already outside */
    LONGLONG anomalies;          /* guest code returned to the host with the thread outside */
    LONGLONG entries[XBOX_GM_SOURCE_COUNT];
    LONGLONG contended_by[XBOX_GM_SOURCE_COUNT];
    LONGLONG nested[XBOX_GM_SOURCE_COUNT];
    LONGLONG exits[XBOX_GM_SOURCE_COUNT];
} xbox_gmeter_record;

/* What serial mode did. Fields are read one at a time, like the meter's. */
typedef struct xbox_gserial_record {
    LONGLONG acquisitions;  /* times a thread took the lock */
    LONGLONG contended;     /* times a thread had to wait for it */
    LONGLONG overruns;      /* waits that timed out and ran without it */
    LONGLONG skipped_isr;   /* timer ticks whose interrupt delivery the guest's IRQL deferred */
    LONGLONG skipped_dpc;   /* timer ticks whose DPC work the guest's IRQL deferred */
    LONGLONG yields;        /* times xbox_GuestSerialYield let a waiter in */
} xbox_gserial_record;

/* Which deferral xbox_GuestSerialNoteSkip counts. */
enum { XBOX_GS_SKIP_ISR, XBOX_GS_SKIP_DPC };

int  xbox_GuestMeterEnabled(void);
/* Returns a token for xbox_GuestMeterRestore; 0 when the meter is off. */
int  xbox_GuestMeterEnter(int source);
int  xbox_GuestMeterLeave(void);
void xbox_GuestMeterRestore(int token, int source);
void xbox_GuestMeterSnapshot(xbox_gmeter_record *out);
/* A [GSERIAL] line in serial mode, then one [GMETER] line, on stderr;
 * nothing when both are off. */
void xbox_GuestMeterSummary(void);

int  xbox_GuestSerialEnabled(void);
/* Enter that also keeps the lock across kernel calls until the matching end. */
int  xbox_GuestSerialBeginAtomic(int source);
void xbox_GuestSerialEndAtomic(int token, int source);
/* Count a timer tick whose interrupt or DPC work was deferred by IRQL. */
void xbox_GuestSerialNoteSkip(int which);
/* Release and re-take the lock if another thread is waiting for it. */
void xbox_GuestSerialYield(void);
/* 1 while serial mode is on; read by RECOMP_BACKEDGE in generated code. */
extern volatile int g_xbox_guest_serial_on;
void xbox_GuestSerialSnapshot(xbox_gserial_record *out);

#ifdef XBOXRECOMP_GMETER_TEST_BUILD
/* Also turns serial mode off. */
void xbox_GuestMeterTestReset(int enabled);
/* Call after xbox_GuestMeterTestReset. A zero timeout keeps the default. */
void xbox_GuestSerialTestReset(int on, DWORD timeout_ms);
#endif

#endif /* XBOX_GUEST_METER_H */
