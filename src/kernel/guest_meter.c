/**
 * guest_meter.c - Guest concurrency meter; see guest_meter.h
 */
#include "guest_meter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Per-thread position. UNSEEN is a thread no transition has touched yet. */
enum { GM_UNSEEN = 0, GM_OUTSIDE = 1, GM_INSIDE = 2 };

static XBOX_THREAD_LOCAL int g_gm_where = GM_UNSEEN;
static volatile LONG g_gm_enabled = -1;   /* -1 until RECOMP_GUEST_METER is read */

static volatile LONG g_gm_inside;
static volatile LONG g_gm_max;
static volatile LONGLONG g_gm_host_kernel_calls;
static volatile LONGLONG g_gm_anomalies;
static volatile LONGLONG g_gm_entries[XBOX_GM_SOURCE_COUNT];
static volatile LONGLONG g_gm_contended[XBOX_GM_SOURCE_COUNT];
static volatile LONGLONG g_gm_nested[XBOX_GM_SOURCE_COUNT];
static volatile LONGLONG g_gm_exits[XBOX_GM_SOURCE_COUNT];

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

static int gm_source(int source)
{
    return (source >= 0 && source < XBOX_GM_SOURCE_COUNT) ? source : XBOX_GM_UNHOOKED;
}

/* Outside -> inside on this thread. */
static void gm_arrive(int source)
{
    LONG n = InterlockedIncrement(&g_gm_inside);
    LONG max;

    g_gm_where = GM_INSIDE;
    gm_inc64(&g_gm_entries[source]);
    if (n > 1)
        gm_inc64(&g_gm_contended[source]);
    while (n > (max = g_gm_max)
           && InterlockedCompareExchange(&g_gm_max, n, max) != max)
        ;
}

/* Inside -> outside on this thread. */
static void gm_depart(int source)
{
    InterlockedDecrement(&g_gm_inside);
    g_gm_where = GM_OUTSIDE;
    gm_inc64(&g_gm_exits[source]);
}

int xbox_GuestMeterEnter(int source)
{
    int prev;

    if (!xbox_GuestMeterEnabled())
        return 0;
    source = gm_source(source);
    prev = g_gm_where;
    if (prev == GM_INSIDE) {
        gm_inc64(&g_gm_nested[source]);
        return GM_INSIDE;
    }
    gm_arrive(source);
    return GM_OUTSIDE;
}

int xbox_GuestMeterLeave(void)
{
    if (!xbox_GuestMeterEnabled())
        return 0;
    /* A thread that reaches the kernel without ever being handed guest code
     * by a metered path was running guest code all the same. */
    if (g_gm_where == GM_UNSEEN)
        gm_arrive(XBOX_GM_UNHOOKED);
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
            gm_arrive(source);
    } else if (now == GM_INSIDE) {
        gm_depart(source);
    } else {
        /* Lifted code returned to its host caller with the thread outside:
         * some transition in between was skipped. */
        gm_inc64(&g_gm_anomalies);
    }
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

    if (!xbox_GuestMeterEnabled())
        return;
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
    g_gm_where = GM_UNSEEN;
    g_gm_inside = 0;
    g_gm_max = 0;
    g_gm_host_kernel_calls = 0;
    g_gm_anomalies = 0;
    for (i = 0; i < XBOX_GM_SOURCE_COUNT; i++)
        g_gm_entries[i] = g_gm_contended[i] = g_gm_nested[i] = g_gm_exits[i] = 0;
}
#endif
