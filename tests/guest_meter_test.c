/* The guest concurrency meter's record, driven through the transitions the
 * runtime makes: kernel calls and returns, a spawned thread's start routine,
 * callbacks run from inside a kernel call, nesting, and a skipped transition.
 * Built with guest_meter.c alone, so it runs wherever the platform layer
 * does. */
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

int main(void)
{
    test_off_is_inert();
    test_transitions();
    test_stress();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("guest meter: transitions, nesting, contention and %d-thread stress pass\n",
           STRESS_THREADS);
    return 0;
}
