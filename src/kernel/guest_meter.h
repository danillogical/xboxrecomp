/**
 * guest_meter.h - How many host threads run lifted guest code at once
 *
 * Observation only, and off unless RECOMP_GUEST_METER=1 (read once, cached).
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

int  xbox_GuestMeterEnabled(void);
/* Returns a token for xbox_GuestMeterRestore; 0 when the meter is off. */
int  xbox_GuestMeterEnter(int source);
int  xbox_GuestMeterLeave(void);
void xbox_GuestMeterRestore(int token, int source);
void xbox_GuestMeterSnapshot(xbox_gmeter_record *out);
/* One [GMETER] line on stderr; nothing when the meter is off. */
void xbox_GuestMeterSummary(void);

#ifdef XBOXRECOMP_GMETER_TEST_BUILD
void xbox_GuestMeterTestReset(int enabled);
#endif

#endif /* XBOX_GUEST_METER_H */
