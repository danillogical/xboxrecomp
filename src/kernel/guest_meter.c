/**
 * guest_meter.c - Guest concurrency meter and serialised guest mode; see
 * guest_meter.h
 */
#include "guest_meter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Per-thread position. UNSEEN is a thread no transition has touched yet. */
enum { GM_UNSEEN = 0, GM_OUTSIDE = 1, GM_INSIDE = 2 };

static XBOX_THREAD_LOCAL int g_gm_where = GM_UNSEEN;
static volatile LONG g_gm_enabled = -1;   /* -1 until RECOMP_GUEST_METER is read */
static volatile LONG g_gm_track = -1;     /* meter or serial mode; -1 until both are read */

static volatile LONG g_gm_inside;
static volatile LONG g_gm_max;
static volatile LONGLONG g_gm_host_kernel_calls;
static volatile LONGLONG g_gm_anomalies;
static volatile LONGLONG g_gm_entries[XBOX_GM_SOURCE_COUNT];
static volatile LONGLONG g_gm_contended[XBOX_GM_SOURCE_COUNT];
static volatile LONGLONG g_gm_nested[XBOX_GM_SOURCE_COUNT];
static volatile LONGLONG g_gm_exits[XBOX_GM_SOURCE_COUNT];

/* Serial mode. The lock is an owner word taken by compare-exchange, with an
 * auto-reset event for the slow path. */
#define GS_DEFAULT_TIMEOUT_MS 100
#define GS_OVERRUN_LOG_MAX    8
#define GS_YIELD_EVERY        4096
#define GS_HANDOFF_GRACE_MS   2

static volatile LONG g_gs_enabled = -1;   /* -1 until RECOMP_GUEST_SERIAL is read */
/* What generated code's RECOMP_BACKEDGE reads: 1 once serial mode is on, so a
 * loop pays one load, not a call, when it is off. */
volatile int g_xbox_guest_serial_on = 0;
static DWORD g_gs_timeout_ms = GS_DEFAULT_TIMEOUT_MS;
static HANDLE volatile g_gs_wake;         /* one release wakes one waiter */
static volatile LONG g_gs_owner;          /* host thread id holding the lock, 0 when free */
static volatile LONG g_gs_waiters;        /* threads in the slow path */
static volatile LONG g_gs_handoff;        /* non-zero: a release left waiters; its sequence number */
static volatile LONG g_gs_release_seq;    /* numbers those releases */

static XBOX_THREAD_LOCAL int g_gs_held;   /* this thread owns the lock */
static XBOX_THREAD_LOCAL int g_gs_pin;    /* atomic sections open on this thread */
static XBOX_THREAD_LOCAL int g_gs_yield_countdown = GS_YIELD_EVERY;

static volatile LONGLONG g_gs_acquisitions;
static volatile LONGLONG g_gs_contended;
static volatile LONGLONG g_gs_overruns;
static volatile LONGLONG g_gs_skip_isr;
static volatile LONGLONG g_gs_skip_dpc;
static volatile LONGLONG g_gs_yields;

static const char *const k_gm_names[XBOX_GM_SOURCE_COUNT] = {
    "main", "unhooked", "kernel", "thread", "thread_inline", "isr", "usb_isr",
    "dpc", "timer_dpc", "io_apc", "apc_dispatch", "sync_exec", "seh_unwind",
};

static void gm_inc64(volatile LONGLONG *p)
{
#ifdef _WIN32
    InterlockedIncrement64(p);
#else
    LONGLONG v;
    do {
        v = *p;
    } while (InterlockedCompareExchange64(p, v + 1, v) != v);
#endif
}

static LONGLONG gm_read64(volatile LONGLONG *p)
{
    return InterlockedCompareExchange64(p, 0, 0);
}

int xbox_GuestMeterEnabled(void)
{
    LONG on = g_gm_enabled;

    if (on < 0) {
        const char *e = getenv("RECOMP_GUEST_METER");
        on = (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
        g_gm_enabled = on;
    }
    return (int)on;
}

static LONG gs_init(void)
{
    const char *e = getenv("RECOMP_GUEST_SERIAL");
    const char *t = getenv("RECOMP_GUEST_SERIAL_TIMEOUT_MS");
    LONG on = (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;

    if (on) {
        HANDLE ev;
        unsigned long ms = t ? strtoul(t, NULL, 10) : 0;

        /* Zero or garbage keeps the default: a zero bound would serialise nothing. */
        g_gs_timeout_ms = (ms > 0 && ms < 0x7FFFFFFFul) ? (DWORD)ms : GS_DEFAULT_TIMEOUT_MS;
        ev = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (ev && InterlockedCompareExchangePointer((PVOID volatile *)&g_gs_wake, ev, NULL)
                  != NULL) {
            CloseHandle(ev);              /* another thread got here first */
        } else if (ev) {
            fprintf(stderr, "  [GSERIAL] on: one host thread runs guest code at a time;"
                    " waits are bounded at %lu ms\n", (unsigned long)g_gs_timeout_ms);
            fflush(stderr);
        }
        if (!g_gs_wake) {
            fprintf(stderr, "  [GSERIAL] no wake event; serial mode stays off\n");
            fflush(stderr);
            on = 0;
        }
    }
    InterlockedExchange(&g_gs_enabled, on);
    g_xbox_guest_serial_on = (int)on;
    return on;
}

int xbox_GuestSerialEnabled(void)
{
    LONG on = g_gs_enabled;

    if (on < 0)
        on = gs_init();
    return (int)on;
}

/* The tracker runs for either consumer. */
static int gm_tracking(void)
{
    LONG on = g_gm_track;

    if (on < 0) {
        on = (xbox_GuestMeterEnabled() || xbox_GuestSerialEnabled()) ? 1 : 0;
        g_gm_track = on;
    }
    return (int)on;
}

static int gm_source(int source)
{
    return (source >= 0 && source < XBOX_GM_SOURCE_COUNT) ? source : XBOX_GM_UNHOOKED;
}

static int gs_try(LONG me)
{
    return InterlockedCompareExchange(&g_gs_owner, me, 0) == 0;
}

/* Take the guest lock, or give up after the bound and run anyway. Giving up
 * is counted and the first few are logged, because it means two threads are
 * in guest code at once again. */
static void gs_acquire(const char *what)
{
    LONG me = (LONG)GetCurrentThreadId();
    LONG handoff;
    ULONGLONG start, elapsed = 0;

    /* The fast path defers to a pending hand-off, or the thread that just
     * released would take the lock straight back from the one it woke. */
    if (!g_gs_handoff && gs_try(me)) {
        g_gs_held = 1;
        gm_inc64(&g_gs_acquisitions);
        return;
    }
    /* Counted before the retry so a release can never miss this waiter. */
    InterlockedIncrement(&g_gs_waiters);
    gm_inc64(&g_gs_contended);
    start = GetTickCount64();
    /* A newcomer leaves a hand-off made before it arrived to the waiter it
     * woke, without touching the event, until that waiter claims the lock.
     * The grace only bounds a hand-off nobody is left to claim. */
    handoff = g_gs_handoff;
    while (handoff && g_gs_handoff == handoff
           && GetTickCount64() - start < GS_HANDOFF_GRACE_MS)
        SwitchToThread();
    for (;;) {
        if (gs_try(me)) {
            InterlockedExchange(&g_gs_handoff, 0);
            InterlockedDecrement(&g_gs_waiters);
            g_gs_held = 1;
            gm_inc64(&g_gs_acquisitions);
            return;
        }
        elapsed = GetTickCount64() - start;
        if (elapsed >= g_gs_timeout_ms)
            break;
        WaitForSingleObject(g_gs_wake, (DWORD)(g_gs_timeout_ms - elapsed));
    }
    InterlockedDecrement(&g_gs_waiters);
    gm_inc64(&g_gs_overruns);
    if (gm_read64(&g_gs_overruns) <= GS_OVERRUN_LOG_MAX) {
        fprintf(stderr, "  [GSERIAL] overrun: %s on tid %lu waited %lu ms; holder tid %lu"
                " -- running without the lock\n", what,
                (unsigned long)me, (unsigned long)elapsed,
                (unsigned long)(DWORD)InterlockedCompareExchange(&g_gs_owner, 0, 0));
        fflush(stderr);
    }
}

static void gs_release(void)
{
    g_gs_held = 0;
    InterlockedExchange(&g_gs_owner, 0);
    /* Read after the release, so a waiter that counted itself in either saw
     * the lock free or is woken here. */
    if (InterlockedCompareExchange(&g_gs_waiters, 0, 0) > 0) {
        LONG seq = InterlockedIncrement(&g_gs_release_seq);

        InterlockedExchange(&g_gs_handoff, seq ? seq : 1);
        SetEvent(g_gs_wake);
    } else if (g_gs_handoff) {
        InterlockedExchange(&g_gs_handoff, 0);
    }
}

/* Outside -> inside on this thread. A thread first seen at a kernel call is
 * about to leave again, so it does not take the lock it would drop at once. */
static void gm_arrive(int source, int take_lock)
{
    LONG n, max;

    if (take_lock && !g_gs_held && xbox_GuestSerialEnabled())
        gs_acquire(k_gm_names[source]);
    n = InterlockedIncrement(&g_gm_inside);
    g_gm_where = GM_INSIDE;
    gm_inc64(&g_gm_entries[source]);
    if (n > 1)
        gm_inc64(&g_gm_contended[source]);
    while (n > (max = g_gm_max)
           && InterlockedCompareExchange(&g_gm_max, n, max) != max)
        ;
}

/* Inside -> outside on this thread. An open atomic section keeps the lock
 * across the kernel calls made inside it. */
static void gm_depart(int source)
{
    InterlockedDecrement(&g_gm_inside);
    g_gm_where = GM_OUTSIDE;
    gm_inc64(&g_gm_exits[source]);
    if (g_gs_held && !g_gs_pin)
        gs_release();
}

int xbox_GuestMeterEnter(int source)
{
    int prev;

    if (!gm_tracking())
        return 0;
    source = gm_source(source);
    prev = g_gm_where;
    if (prev == GM_INSIDE) {
        gm_inc64(&g_gm_nested[source]);
        return GM_INSIDE;
    }
    gm_arrive(source, 1);
    return GM_OUTSIDE;
}

int xbox_GuestMeterLeave(void)
{
    if (!gm_tracking())
        return 0;
    /* A thread that reaches the kernel without ever being handed guest code
     * by a metered path was running guest code all the same. */
    if (g_gm_where == GM_UNSEEN)
        gm_arrive(XBOX_GM_UNHOOKED, 0);
    if (g_gm_where != GM_INSIDE) {
        gm_inc64(&g_gm_host_kernel_calls);
        return 0;                       /* nothing to restore */
    }
    gm_depart(XBOX_GM_KERNEL);
    return GM_INSIDE;
}

void xbox_GuestMeterRestore(int token, int source)
{
    int now;

    if (!token)
        return;
    source = gm_source(source);
    now = g_gm_where;
    if (token == GM_INSIDE) {
        if (now != GM_INSIDE)
            gm_arrive(source, 1);
    } else if (now == GM_INSIDE) {
        gm_depart(source);
    } else {
        /* Lifted code returned to its host caller with the thread outside:
         * some transition in between was skipped. */
        gm_inc64(&g_gm_anomalies);
    }
}

int xbox_GuestSerialBeginAtomic(int source)
{
    int t = xbox_GuestMeterEnter(source);

    if (xbox_GuestSerialEnabled())
        g_gs_pin++;
    return t;
}

void xbox_GuestSerialEndAtomic(int token, int source)
{
    if (xbox_GuestSerialEnabled() && g_gs_pin > 0)
        g_gs_pin--;
    xbox_GuestMeterRestore(token, source);
    /* A section whose bracket was skipped must not leave the lock behind. */
    if (g_gs_held && !g_gs_pin && g_gm_where != GM_INSIDE)
        gs_release();
}

void xbox_GuestSerialNoteSkip(int which)
{
    if (!xbox_GuestSerialEnabled())
        return;
    gm_inc64(which == XBOX_GS_SKIP_ISR ? &g_gs_skip_isr : &g_gs_skip_dpc);
}

void xbox_GuestSerialYield(void)
{
    if (!xbox_GuestSerialEnabled() || --g_gs_yield_countdown > 0)
        return;
    g_gs_yield_countdown = GS_YIELD_EVERY;
    if (!g_gs_held || g_gs_pin || !InterlockedCompareExchange(&g_gs_waiters, 0, 0))
        return;
    /* The release hands off to a waiter, and the re-acquire queues behind it. */
    gm_inc64(&g_gs_yields);
    gs_release();
    gs_acquire("yield");
}

void xbox_GuestSerialSnapshot(xbox_gserial_record *out)
{
    memset(out, 0, sizeof(*out));
    out->acquisitions = gm_read64(&g_gs_acquisitions);
    out->contended = gm_read64(&g_gs_contended);
    out->overruns = gm_read64(&g_gs_overruns);
    out->skipped_isr = gm_read64(&g_gs_skip_isr);
    out->skipped_dpc = gm_read64(&g_gs_skip_dpc);
    out->yields = gm_read64(&g_gs_yields);
}

void xbox_GuestMeterSnapshot(xbox_gmeter_record *out)
{
    int i;

    memset(out, 0, sizeof(*out));
    out->inside = InterlockedCompareExchange(&g_gm_inside, 0, 0);
    out->max_inside = InterlockedCompareExchange(&g_gm_max, 0, 0);
    out->host_kernel_calls = gm_read64(&g_gm_host_kernel_calls);
    out->anomalies = gm_read64(&g_gm_anomalies);
    for (i = 0; i < XBOX_GM_SOURCE_COUNT; i++) {
        out->entries[i] = gm_read64(&g_gm_entries[i]);
        out->contended_by[i] = gm_read64(&g_gm_contended[i]);
        out->nested[i] = gm_read64(&g_gm_nested[i]);
        out->exits[i] = gm_read64(&g_gm_exits[i]);
        out->contended += out->contended_by[i];
    }
}

void xbox_GuestMeterSummary(void)
{
    xbox_gmeter_record r;
    char line[2048];
    int n, i;

    if (!gm_tracking())
        return;
    if (xbox_GuestSerialEnabled()) {
        xbox_gserial_record g;

        xbox_GuestSerialSnapshot(&g);
        fprintf(stderr, "  [GSERIAL] acquisitions=%lld contended=%lld overruns=%lld"
                " irql_skipped_isr=%lld irql_skipped_dpc=%lld yields=%lld timeout_ms=%lu\n",
                (long long)g.acquisitions, (long long)g.contended, (long long)g.overruns,
                (long long)g.skipped_isr, (long long)g.skipped_dpc, (long long)g.yields,
                (unsigned long)g_gs_timeout_ms);
    }
    xbox_GuestMeterSnapshot(&r);
    n = snprintf(line, sizeof line,
                 "  [GMETER] max=%ld inside=%ld contended=%lld host_kcalls=%lld"
                 " anomalies=%lld entries/contended/nested/exits:",
                 (long)r.max_inside, (long)r.inside, (long long)r.contended,
                 (long long)r.host_kernel_calls, (long long)r.anomalies);
    for (i = 0; i < XBOX_GM_SOURCE_COUNT && n > 0 && n < (int)sizeof line; i++)
        n += snprintf(line + n, sizeof line - (size_t)n, " %s=%lld/%lld/%lld/%lld",
                      k_gm_names[i], (long long)r.entries[i],
                      (long long)r.contended_by[i], (long long)r.nested[i],
                      (long long)r.exits[i]);
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
}

#ifdef XBOXRECOMP_GMETER_TEST_BUILD
void xbox_GuestMeterTestReset(int enabled)
{
    int i;

    g_gm_enabled = enabled ? 1 : 0;
    g_gs_enabled = 0;
    g_xbox_guest_serial_on = 0;
    g_gm_track = g_gm_enabled;
    g_gm_where = GM_UNSEEN;
    g_gs_held = 0;
    g_gs_pin = 0;
    g_gm_inside = 0;
    g_gm_max = 0;
    g_gm_host_kernel_calls = 0;
    g_gm_anomalies = 0;
    for (i = 0; i < XBOX_GM_SOURCE_COUNT; i++)
        g_gm_entries[i] = g_gm_contended[i] = g_gm_nested[i] = g_gm_exits[i] = 0;
}

void xbox_GuestSerialTestReset(int on, DWORD timeout_ms)
{
    if (!g_gs_wake)
        g_gs_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_gs_enabled = on ? 1 : 0;
    g_xbox_guest_serial_on = on ? 1 : 0;
    g_gm_track = (g_gm_enabled > 0 || g_gs_enabled) ? 1 : 0;
    g_gs_timeout_ms = timeout_ms ? timeout_ms : GS_DEFAULT_TIMEOUT_MS;
    g_gs_owner = g_gs_waiters = g_gs_handoff = g_gs_release_seq = 0;
    g_gs_held = 0;
    g_gs_pin = 0;
    g_gs_yield_countdown = GS_YIELD_EVERY;
    g_gs_acquisitions = g_gs_contended = g_gs_overruns = 0;
    g_gs_skip_isr = g_gs_skip_dpc = g_gs_yields = 0;
}
#endif
