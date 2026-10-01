/* The guest concurrency meter's record, driven through the transitions the
 * runtime makes: kernel calls and returns, a spawned thread's start routine,
 * callbacks run from inside a kernel call, nesting, and a skipped transition.
 * Then serialised guest mode on the same transitions: exclusion, nesting, a
 * kernel call letting another thread in, an atomic section not doing so, the
 * bounded wait, and the yield. Built with guest_meter.c alone, so it runs
 * wherever the platform layer does. */
#include "guest_meter.h"
#include <stdio.h>
#include <string.h>

static int g_failures;

#define EXPECT(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); \
                               fputc('\n', stderr); g_failures++; } } while (0)

static xbox_gmeter_record snap(void)
{
    xbox_gmeter_record r;
    xbox_GuestMeterSnapshot(&r);
    return r;
}

/* A second guest thread, held inside its start routine until released. */
typedef struct {
    HANDLE inside, release, done;
} WORKER;

static DWORD WINAPI worker_main(LPVOID p)
{
    WORKER *w = (WORKER *)p;
    int t = xbox_GuestMeterEnter(XBOX_GM_THREAD);

    SetEvent(w->inside);
    WaitForSingleObject(w->release, INFINITE);
    xbox_GuestMeterRestore(t, XBOX_GM_THREAD);
    /* The runtime calling the kernel on its own behalf, outside guest code. */
    xbox_GuestMeterRestore(xbox_GuestMeterLeave(), XBOX_GM_KERNEL);
    SetEvent(w->done);
    return 0;
}

static void test_off_is_inert(void)
{
    xbox_gmeter_record r, zero;
    int t;

    xbox_GuestMeterTestReset(0);
    t = xbox_GuestMeterEnter(XBOX_GM_DPC);
    EXPECT(t == 0, "off: Enter returned token %d", t);
    xbox_GuestMeterRestore(t, XBOX_GM_DPC);
    t = xbox_GuestMeterLeave();
    EXPECT(t == 0, "off: Leave returned token %d", t);
    xbox_GuestMeterRestore(t, XBOX_GM_KERNEL);
    r = snap();
    memset(&zero, 0, sizeof zero);
    EXPECT(!memcmp(&r, &zero, sizeof r), "off: the record changed");
}

static void test_transitions(void)
{
    WORKER w;
    HANDLE th;
    xbox_gmeter_record r;
    int k, a, n;

    xbox_GuestMeterTestReset(1);
    w.inside = CreateEventW(NULL, TRUE, FALSE, NULL);
    w.release = CreateEventW(NULL, TRUE, FALSE, NULL);
    w.done = CreateEventW(NULL, TRUE, FALSE, NULL);

    /* The main thread's first kernel call: it was in guest code all along. */
    k = xbox_GuestMeterLeave();
    r = snap();
    EXPECT(r.entries[XBOX_GM_UNHOOKED] == 1 && r.exits[XBOX_GM_KERNEL] == 1 && r.inside == 0,
           "first kernel call: unhooked=%lld kernel exits=%lld inside=%ld",
           (long long)r.entries[XBOX_GM_UNHOOKED], (long long)r.exits[XBOX_GM_KERNEL],
           (long)r.inside);
    xbox_GuestMeterRestore(k, XBOX_GM_KERNEL);
    EXPECT(snap().inside == 1, "kernel return did not re-enter");

    /* A spawned thread enters while the main thread is inside. */
    th = CreateThread(NULL, 0, worker_main, &w, 0, NULL);
    WaitForSingleObject(w.inside, INFINITE);
    r = snap();
    EXPECT(r.inside == 2 && r.max_inside == 2 && r.contended_by[XBOX_GM_THREAD] == 1,
           "thread start: inside=%ld max=%ld contended=%lld", (long)r.inside,
           (long)r.max_inside, (long long)r.contended_by[XBOX_GM_THREAD]);

    /* A handler run by a thread already inside is nested, not an entry. */
    n = xbox_GuestMeterEnter(XBOX_GM_SEH_UNWIND);
    xbox_GuestMeterRestore(n, XBOX_GM_SEH_UNWIND);
    r = snap();
    EXPECT(r.nested[XBOX_GM_SEH_UNWIND] == 1 && r.entries[XBOX_GM_SEH_UNWIND] == 0
           && r.inside == 2, "nested handler changed the count");

    /* An APC delivered inside a kernel call, with the worker still inside. */
    k = xbox_GuestMeterLeave();
    a = xbox_GuestMeterEnter(XBOX_GM_IO_APC);
    xbox_GuestMeterRestore(a, XBOX_GM_IO_APC);
    xbox_GuestMeterRestore(k, XBOX_GM_KERNEL);
    r = snap();
    EXPECT(r.entries[XBOX_GM_IO_APC] == 1 && r.exits[XBOX_GM_IO_APC] == 1
           && r.contended_by[XBOX_GM_IO_APC] == 1 && r.contended_by[XBOX_GM_KERNEL] == 1
           && r.inside == 2 && r.max_inside == 2,
           "APC in a kernel call: entries=%lld contended=%lld/%lld inside=%ld max=%ld",
           (long long)r.entries[XBOX_GM_IO_APC], (long long)r.contended_by[XBOX_GM_IO_APC],
           (long long)r.contended_by[XBOX_GM_KERNEL], (long)r.inside, (long)r.max_inside);

    SetEvent(w.release);
    WaitForSingleObject(w.done, INFINITE);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    r = snap();
    EXPECT(r.exits[XBOX_GM_THREAD] == 1 && r.host_kernel_calls == 1 && r.inside == 1,
           "worker exit: exits=%lld host_kcalls=%lld inside=%ld",
           (long long)r.exits[XBOX_GM_THREAD], (long long)r.host_kernel_calls, (long)r.inside);

    /* A callback whose own kernel call never came back to it. */
    k = xbox_GuestMeterLeave();
    a = xbox_GuestMeterEnter(XBOX_GM_TIMER_DPC);
    (void)xbox_GuestMeterLeave();
    xbox_GuestMeterRestore(a, XBOX_GM_TIMER_DPC);
    xbox_GuestMeterRestore(k, XBOX_GM_KERNEL);
    r = snap();
    EXPECT(r.anomalies == 1 && r.inside == 1, "skipped transition: anomalies=%lld inside=%ld",
           (long long)r.anomalies, (long)r.inside);

    EXPECT(r.contended == 3, "contended total %lld, want 3", (long long)r.contended);
    xbox_GuestMeterSummary();
    CloseHandle(w.inside);
    CloseHandle(w.release);
    CloseHandle(w.done);
}

/* Many threads entering and leaving at once: the count must come back to 0
 * and entries must equal exits. */
#define STRESS_THREADS 8
#define STRESS_ROUNDS 20000

static HANDLE g_go;

static DWORD WINAPI stress_main(LPVOID p)
{
    int i;
    (void)p;
    WaitForSingleObject(g_go, INFINITE);
    for (i = 0; i < STRESS_ROUNDS; i++) {
        int t = xbox_GuestMeterEnter(XBOX_GM_DPC);
        xbox_GuestMeterRestore(xbox_GuestMeterLeave(), XBOX_GM_KERNEL);
        xbox_GuestMeterRestore(t, XBOX_GM_DPC);
    }
    return 0;
}

static void test_stress(void)
{
    HANDLE th[STRESS_THREADS];
    xbox_gmeter_record r;
    int i;

    xbox_GuestMeterTestReset(1);
    g_go = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (i = 0; i < STRESS_THREADS; i++)
        th[i] = CreateThread(NULL, 0, stress_main, NULL, 0, NULL);
    SetEvent(g_go);
    for (i = 0; i < STRESS_THREADS; i++) {
        WaitForSingleObject(th[i], INFINITE);
        CloseHandle(th[i]);
    }
    CloseHandle(g_go);
    r = snap();
    EXPECT(r.inside == 0, "stress: %ld still inside", (long)r.inside);
    EXPECT(r.entries[XBOX_GM_DPC] == (LONGLONG)STRESS_THREADS * STRESS_ROUNDS
           && r.exits[XBOX_GM_DPC] == r.entries[XBOX_GM_DPC]
           && r.entries[XBOX_GM_KERNEL] == r.entries[XBOX_GM_DPC]
           && r.exits[XBOX_GM_KERNEL] == r.entries[XBOX_GM_DPC],
           "stress: dpc %lld/%lld kernel %lld/%lld", (long long)r.entries[XBOX_GM_DPC],
           (long long)r.exits[XBOX_GM_DPC], (long long)r.entries[XBOX_GM_KERNEL],
           (long long)r.exits[XBOX_GM_KERNEL]);
    EXPECT(r.max_inside >= 1 && r.max_inside <= STRESS_THREADS, "stress: max %ld",
           (long)r.max_inside);
    EXPECT(r.anomalies == 0 && r.host_kernel_calls == 0, "stress: stray counts");
}

/* ---- serialised guest mode ---- */

#define SERIAL_LONG_MS 10000   /* a bound no passing test comes near */

static xbox_gserial_record gsnap(void)
{
    xbox_gserial_record g;
    xbox_GuestSerialSnapshot(&g);
    return g;
}

/* Spin until the serial record shows `n` threads have had to wait. */
static int wait_contended(LONGLONG n)
{
    int i;

    for (i = 0; i < 5000; i++) {
        if (gsnap().contended >= n)
            return 1;
        Sleep(1);
    }
    return 0;
}

/* Serial off: two threads are inside at once and the lock is never touched,
 * and the atomic pair is the meter's own Enter and Restore. */
static void test_serial_off_no_lock(void)
{
    WORKER w;
    HANDLE th;
    xbox_gmeter_record r;
    xbox_gserial_record g;
    int t, a;

    xbox_GuestMeterTestReset(1);
    w.inside = CreateEventW(NULL, TRUE, FALSE, NULL);
    w.release = CreateEventW(NULL, TRUE, FALSE, NULL);
    w.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    t = xbox_GuestMeterEnter(XBOX_GM_THREAD);
    th = CreateThread(NULL, 0, worker_main, &w, 0, NULL);
    EXPECT(WaitForSingleObject(w.inside, 5000) == WAIT_OBJECT_0,
           "serial off: the worker did not get inside");
    a = xbox_GuestSerialBeginAtomic(XBOX_GM_ISR);
    EXPECT(a == 2, "serial off: nested atomic token %d", a);
    xbox_GuestSerialEndAtomic(a, XBOX_GM_ISR);
    r = snap();
    EXPECT(r.max_inside == 2 && r.nested[XBOX_GM_ISR] == 1,
           "serial off: max=%ld nested isr=%lld", (long)r.max_inside,
           (long long)r.nested[XBOX_GM_ISR]);
    SetEvent(w.release);
    WaitForSingleObject(w.done, INFINITE);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    xbox_GuestMeterRestore(t, XBOX_GM_THREAD);
    xbox_GuestSerialNoteSkip(XBOX_GS_SKIP_DPC);
    xbox_GuestSerialYield();
    g = gsnap();
    EXPECT(!xbox_GuestSerialEnabled() && g.acquisitions == 0 && g.contended == 0
           && g.skipped_dpc == 0 && g.yields == 0, "serial off: the lock was used");
    CloseHandle(w.inside);
    CloseHandle(w.release);
    CloseHandle(w.done);
}

/* Many threads in and out of guest code, through quick kernel calls, nested
 * handlers and atomic sections: never two inside at once. */
#define SERIAL_THREADS 6
#define SERIAL_ROUNDS  3000

static volatile LONG g_serial_inside, g_serial_max;

static void serial_note_inside(void)
{
    LONG n = InterlockedIncrement(&g_serial_inside), max;

    while (n > (max = g_serial_max)
           && InterlockedCompareExchange(&g_serial_max, n, max) != max)
        ;
}

static DWORD WINAPI serial_stress_main(LPVOID p)
{
    int i, k, n;
    (void)p;
    WaitForSingleObject(g_go, INFINITE);
    for (i = 0; i < SERIAL_ROUNDS; i++) {
        int t = xbox_GuestMeterEnter((i & 1) ? XBOX_GM_DPC : XBOX_GM_THREAD);
        serial_note_inside();
        n = xbox_GuestMeterEnter(XBOX_GM_SEH_UNWIND);     /* nested */
        xbox_GuestMeterRestore(n, XBOX_GM_SEH_UNWIND);
        InterlockedDecrement(&g_serial_inside);
        k = xbox_GuestMeterLeave();                       /* a kernel call */
        xbox_GuestMeterRestore(k, XBOX_GM_KERNEL);
        serial_note_inside();
        InterlockedDecrement(&g_serial_inside);
        xbox_GuestMeterRestore(t, (i & 1) ? XBOX_GM_DPC : XBOX_GM_THREAD);
        if ((i & 7) == 0) {
            int a = xbox_GuestSerialBeginAtomic(XBOX_GM_ISR);
            serial_note_inside();
            k = xbox_GuestMeterLeave();                   /* kept: atomic */
            xbox_GuestMeterRestore(k, XBOX_GM_KERNEL);
            InterlockedDecrement(&g_serial_inside);
            xbox_GuestSerialEndAtomic(a, XBOX_GM_ISR);
        }
    }
    return 0;
}

static void test_serial_exclusion(void)
{
    HANDLE th[SERIAL_THREADS];
    xbox_gmeter_record r;
    xbox_gserial_record g;
    int i;

    xbox_GuestMeterTestReset(1);
    xbox_GuestSerialTestReset(1, SERIAL_LONG_MS);
    g_serial_inside = g_serial_max = 0;
    g_go = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (i = 0; i < SERIAL_THREADS; i++)
        th[i] = CreateThread(NULL, 0, serial_stress_main, NULL, 0, NULL);
    SetEvent(g_go);
    for (i = 0; i < SERIAL_THREADS; i++) {
        WaitForSingleObject(th[i], INFINITE);
        CloseHandle(th[i]);
    }
    CloseHandle(g_go);
    r = snap();
    g = gsnap();
    EXPECT(g_serial_max == 1, "serial: %ld threads were inside at once", (long)g_serial_max);
    EXPECT(r.max_inside == 1 && r.inside == 0, "serial: meter max=%ld inside=%ld",
           (long)r.max_inside, (long)r.inside);
    EXPECT(g.overruns == 0, "serial: %lld overruns", (long long)g.overruns);
    EXPECT(g.acquisitions > 0 && g.contended > 0,
           "serial: acquisitions=%lld contended=%lld, want both non-zero",
           (long long)g.acquisitions, (long long)g.contended);
    xbox_GuestMeterSummary();
}

/* A thread held at the gate until the test lets it through. */
typedef struct {
    HANDLE inside, release, done;
    int source;
} GATE_WORKER;

static DWORD WINAPI gate_worker_main(LPVOID p)
{
    GATE_WORKER *w = (GATE_WORKER *)p;
    int t = xbox_GuestMeterEnter(w->source);

    SetEvent(w->inside);
    WaitForSingleObject(w->release, INFINITE);
    xbox_GuestMeterRestore(t, w->source);
    SetEvent(w->done);
    return 0;
}

static HANDLE gate_start(GATE_WORKER *w, int source)
{
    w->inside = CreateEventW(NULL, TRUE, FALSE, NULL);
    w->release = CreateEventW(NULL, TRUE, FALSE, NULL);
    w->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    w->source = source;
    return CreateThread(NULL, 0, gate_worker_main, w, 0, NULL);
}

static void gate_finish(GATE_WORKER *w, HANDLE th)
{
    SetEvent(w->release);
    WaitForSingleObject(w->done, INFINITE);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    CloseHandle(w->inside);
    CloseHandle(w->release);
    CloseHandle(w->done);
}

/* Nesting does not re-take the lock; a kernel call lets a waiter in and the
 * return waits for it; an atomic section keeps it out across a kernel call. */
static void test_serial_handover(void)
{
    GATE_WORKER w;
    HANDLE th;
    xbox_gserial_record g;
    int t, n, k, a;

    xbox_GuestMeterTestReset(0);
    xbox_GuestSerialTestReset(1, SERIAL_LONG_MS);

    t = xbox_GuestMeterEnter(XBOX_GM_THREAD);
    n = xbox_GuestMeterEnter(XBOX_GM_SEH_UNWIND);
    a = xbox_GuestSerialBeginAtomic(XBOX_GM_SYNC_EXEC);
    xbox_GuestSerialEndAtomic(a, XBOX_GM_SYNC_EXEC);
    xbox_GuestMeterRestore(n, XBOX_GM_SEH_UNWIND);
    EXPECT(gsnap().acquisitions == 1, "nested enters took the lock %lld times",
           (long long)gsnap().acquisitions);

    th = gate_start(&w, XBOX_GM_DPC);
    EXPECT(wait_contended(1), "the second thread never waited");
    EXPECT(WaitForSingleObject(w.inside, 50) == WAIT_TIMEOUT,
           "the second thread got in while the first was inside");

    /* A blocking kernel call: the waiter runs while this thread is out. */
    k = xbox_GuestMeterLeave();
    EXPECT(WaitForSingleObject(w.inside, 5000) == WAIT_OBJECT_0,
           "a kernel call did not let the waiter in");
    SetEvent(w.release);
    xbox_GuestMeterRestore(k, XBOX_GM_KERNEL);   /* waits for the worker to leave */
    EXPECT(WaitForSingleObject(w.done, 0) == WAIT_OBJECT_0,
           "the kernel return re-entered while the worker was inside");
    gate_finish(&w, th);

    /* An atomic section: its kernel calls keep the lock. */
    a = xbox_GuestSerialBeginAtomic(XBOX_GM_ISR);
    th = gate_start(&w, XBOX_GM_DPC);
    EXPECT(wait_contended(2), "the third thread never waited");
    k = xbox_GuestMeterLeave();
    EXPECT(WaitForSingleObject(w.inside, 50) == WAIT_TIMEOUT,
           "a kernel call inside an atomic section let a waiter in");
    xbox_GuestMeterRestore(k, XBOX_GM_KERNEL);
    xbox_GuestSerialEndAtomic(a, XBOX_GM_ISR);
    xbox_GuestMeterRestore(t, XBOX_GM_THREAD);    /* now outside: the worker runs */
    EXPECT(WaitForSingleObject(w.inside, 5000) == WAIT_OBJECT_0,
           "the waiter did not get in after the section closed");
    gate_finish(&w, th);

    g = gsnap();
    EXPECT(g.overruns == 0, "handover: %lld overruns", (long long)g.overruns);
    xbox_GuestSerialNoteSkip(XBOX_GS_SKIP_ISR);
    xbox_GuestSerialNoteSkip(XBOX_GS_SKIP_DPC);
    xbox_GuestSerialNoteSkip(XBOX_GS_SKIP_DPC);
    g = gsnap();
    EXPECT(g.skipped_isr == 1 && g.skipped_dpc == 2, "skips isr=%lld dpc=%lld",
           (long long)g.skipped_isr, (long long)g.skipped_dpc);
}

/* A holder that never calls the kernel: the waiter gives up after the bound,
 * runs alongside it, and says so. */
static void test_serial_overrun(void)
{
    GATE_WORKER w;
    HANDLE th;
    xbox_gmeter_record r;
    xbox_gserial_record g;
    int t;

    xbox_GuestMeterTestReset(1);
    xbox_GuestSerialTestReset(1, 20);
    t = xbox_GuestMeterEnter(XBOX_GM_THREAD);    /* and "spins" from here */
    th = gate_start(&w, XBOX_GM_DPC);
    EXPECT(WaitForSingleObject(w.inside, 5000) == WAIT_OBJECT_0,
           "overrun: the waiter never ran");
    r = snap();
    g = gsnap();
    EXPECT(g.overruns == 1 && r.max_inside == 2,
           "overrun: overruns=%lld max=%ld", (long long)g.overruns, (long)r.max_inside);
    gate_finish(&w, th);
    xbox_GuestMeterRestore(t, XBOX_GM_THREAD);

    /* The lock is free again afterwards: an uncontended enter takes it. */
    t = xbox_GuestMeterEnter(XBOX_GM_THREAD);
    xbox_GuestMeterRestore(t, XBOX_GM_THREAD);
    g = gsnap();
    EXPECT(g.overruns == 1 && g.acquisitions == 2,
           "after overrun: overruns=%lld acquisitions=%lld",
           (long long)g.overruns, (long long)g.acquisitions);
}

/* The yield acts once per 4096 calls, and only with a waiter. */
static void test_serial_yield(void)
{
    GATE_WORKER w;
    HANDLE th;
    xbox_gserial_record g;
    int t, i;

    xbox_GuestMeterTestReset(0);
    xbox_GuestSerialTestReset(1, SERIAL_LONG_MS);
    t = xbox_GuestMeterEnter(XBOX_GM_THREAD);
    for (i = 0; i < 5000; i++)
        xbox_GuestSerialYield();                 /* nobody waiting */
    EXPECT(gsnap().yields == 0, "yield with no waiter released the lock");

    th = gate_start(&w, XBOX_GM_DPC);
    EXPECT(wait_contended(1), "yield: the worker never waited");
    SetEvent(w.release);                         /* it leaves as soon as it is in */
    for (i = 0; i < 4096; i++)
        xbox_GuestSerialYield();
    EXPECT(WaitForSingleObject(w.done, 0) == WAIT_OBJECT_0,
           "the yield came back before the worker had run");
    gate_finish(&w, th);
    xbox_GuestMeterRestore(t, XBOX_GM_THREAD);
    g = gsnap();
    EXPECT(g.yields == 1 && g.overruns == 0, "yield: yields=%lld overruns=%lld",
           (long long)g.yields, (long long)g.overruns);
}

int main(void)
{
    test_off_is_inert();
    test_transitions();
    test_stress();
    test_serial_off_no_lock();
    test_serial_exclusion();
    test_serial_handover();
    test_serial_overrun();
    test_serial_yield();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("guest meter: transitions, nesting, contention and %d-thread stress pass\n",
           STRESS_THREADS);
    printf("guest serial: exclusion over %d threads, handover, atomic sections,"
           " overrun and yield pass\n", SERIAL_THREADS);
    return 0;
}
