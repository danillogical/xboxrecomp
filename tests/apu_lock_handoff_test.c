/*
 * apu_lock_handoff_test.c - the lock handoff in src/apu/apu_lock_handoff.h.
 *
 * THE DEFECT THIS GUARDS. The APU frame thread holds the device lock for its
 * whole loop and re-takes it at once after any unlock; neither a pthread mutex
 * nor a CRITICAL_SECTION is fair, so a guest thread waiting in an MMIO handler
 * starved for minutes. Waiters announce themselves (apu_lock_contended) and the
 * holder hands the lock over (apu_lock_handoff) while anyone is announced.
 *
 * WHAT IT ASSERTS:
 *   1. a waiter blocked against a handoff-looping holder gets the lock on the
 *      first handoff, and its section runs before the holder's relock returns;
 *   2. the waiter count is 0 whenever nobody is inside apu_lock_contended;
 *   3. with no waiter the handoff returns holding m once and never opens a
 *      window (an unannounced thread blocked on m cannot get in);
 *   4. the handoff is bounded when the count never drops;
 *   5. (the starvation case) against a ~2 s tight holder loop, a waiter
 *      completes 50 acquisitions, each within 100 ms.
 * A NEGATIVE CONTROL runs the same holder loop with a plain unlock/relock and
 * only prints how many acquisitions the waiter got; it asserts nothing, since
 * OS scheduling may let it through.
 *
 * The shim has no trylock, so check 3 is a probe: a thread blocked in a plain
 * lock sets a flag the instant it gets m, and the holder checks the flag stays
 * clear across many no-waiter handoffs. It cannot give a false failure, but it
 * only detects a leak that the OS happens to schedule.
 *
 *     cc -I src -I src/platform -I src/nv2a -I src/apu tests/apu_lock_handoff_test.c \
 *        src/platform/win32_compat.c -lpthread -o apu_lock_handoff_test
 */
#include "apu_lock_handoff.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define WAITER_ACQUISITIONS 50
#define WINDOW_MS 2000
#define MAX_LATENCY_MS 100.0

static int g_failures;

#define CHECK(cond, what) do {                                              \
        if (cond) {                                                         \
            printf("PASS %s\n", what);                                      \
        } else {                                                            \
            g_failures++;                                                   \
            printf("FAIL %s (%s:%d: %s)\n", what, __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static QemuMutex g_m;
static volatile LONG g_waiters;
static volatile LONG g_stop;
static volatile LONG g_counter;     /* written only under g_m */
static volatile LONG g_intruder;    /* set by an unannounced thread under g_m */
static volatile LONG g_ready;       /* a helper thread is about to block on g_m */

/* Hold-time work; the volatile sink keeps the loop from folding away. */
static void busy_work(void)
{
    static volatile unsigned sink;
    for (int i = 0; i < 2000; i++) {
        sink += (unsigned)i;
    }
}

static void wait_for(volatile LONG *v, LONG at_least)
{
    double t0 = now_ms();
    while (InterlockedCompareExchange(v, 0, 0) < at_least && now_ms() - t0 < 5000) {
        sleep_ms(1);
    }
}

/* ---- 1: first handoff reaches an announced waiter ---- */

static void *single_waiter(void *arg)
{
    (void)arg;
    apu_lock_contended(&g_m, &g_waiters);
    g_counter++;
    qemu_mutex_unlock(&g_m);
    return NULL;
}

static void test_first_handoff(void)
{
    pthread_t t;

    g_counter = 0;
    qemu_mutex_lock(&g_m);
    pthread_create(&t, NULL, single_waiter, NULL);
    wait_for(&g_waiters, 1);                 /* announced, and blocked on g_m */
    sleep_ms(20);                            /* let it reach the lock call */
    int calls = 0;
    while (g_counter == 0 && calls < 4) {
        apu_lock_handoff(&g_m, &g_waiters);
        calls++;
    }
    CHECK(calls == 1, "waiter served by the first handoff call");
    CHECK(g_counter == 1, "waiter section ran before the holder's relock returned");
    qemu_mutex_unlock(&g_m);
    pthread_join(t, NULL);
    CHECK(InterlockedCompareExchange(&g_waiters, 0, 0) == 0, "count back to 0 after the waiter");
}

/* ---- 3: no waiter, no window ---- */

static void *plain_locker(void *arg)
{
    (void)arg;
    InterlockedIncrement(&g_ready);
    qemu_mutex_lock(&g_m);                   /* NOT announced */
    g_intruder = 1;
    qemu_mutex_unlock(&g_m);
    return NULL;
}

static void test_no_waiter_keeps_lock(void)
{
    pthread_t t;

    g_intruder = 0;
    g_ready = 0;
    qemu_mutex_lock(&g_m);
    pthread_create(&t, NULL, plain_locker, NULL);
    wait_for(&g_ready, 1);
    int leaked = 0;
    double t0 = now_ms();
    while (now_ms() - t0 < 200) {
        apu_lock_handoff(&g_m, &g_waiters);
        if (g_intruder) {
            leaked = 1;
            break;
        }
    }
    CHECK(!leaked, "no-waiter handoff does not release the lock");
    qemu_mutex_unlock(&g_m);                 /* held exactly once: one unlock frees it */
    pthread_join(t, NULL);                   /* a hang here means m was still held */
    CHECK(g_intruder == 1, "lock free after a single unlock following no-waiter handoffs");
}

/* ---- 4: bounded when the count never drops ---- */

static void *just_lock(void *arg)
{
    (void)arg;
    qemu_mutex_lock(&g_m);
    qemu_mutex_unlock(&g_m);
    return NULL;
}

static void test_bounded(void)
{
    pthread_t t;

    InterlockedIncrement(&g_waiters);        /* a waiter that never takes the lock */
    qemu_mutex_lock(&g_m);
    double t0 = now_ms();
    apu_lock_handoff(&g_m, &g_waiters);
    double dt = now_ms() - t0;
    InterlockedDecrement(&g_waiters);
    printf("     stuck-count handoff took %.1f ms\n", dt);
    CHECK(dt < 10000.0, "handoff returns when the count never drops");
    qemu_mutex_unlock(&g_m);
    pthread_create(&t, NULL, just_lock, NULL);
    pthread_join(t, NULL);                   /* hangs if the handoff returned holding it twice */
    CHECK(1, "handoff returned holding the lock exactly once");
}

/* ---- 5: the starvation scenario, with and without the handoff ---- */

typedef struct {
    int use_handoff;
    int done;               /* acquisitions completed */
    double max_ms;
} Scenario;

static void *waiter_main(void *arg)
{
    Scenario *s = arg;
    double t_end = now_ms() + WINDOW_MS + 3000;

    for (int i = 0; i < WAITER_ACQUISITIONS && now_ms() < t_end; i++) {
        double t0 = now_ms();
        apu_lock_contended(&g_m, &g_waiters);
        double dt = now_ms() - t0;
        g_counter++;
        qemu_mutex_unlock(&g_m);
        if (dt > s->max_ms) {
            s->max_ms = dt;
        }
        s->done++;
        sleep_ms(5);
    }
    return NULL;
}

static void *holder_main(void *arg)
{
    Scenario *s = arg;
    double t0 = now_ms();

    qemu_mutex_lock(&g_m);
    while (!g_stop && now_ms() - t0 < WINDOW_MS) {
        busy_work();
        if (s->use_handoff) {
            apu_lock_handoff(&g_m, &g_waiters);
        } else {
            qemu_mutex_unlock(&g_m);         /* plain unlock/relock, no yield */
            qemu_mutex_lock(&g_m);
        }
    }
    qemu_mutex_unlock(&g_m);
    return NULL;
}

static Scenario run_scenario(int use_handoff)
{
    Scenario s = { use_handoff, 0, 0.0 };
    pthread_t h, w;

    g_stop = 0;
    g_counter = 0;
    pthread_create(&h, NULL, holder_main, &s);
    sleep_ms(10);
    pthread_create(&w, NULL, waiter_main, &s);
    pthread_join(w, NULL);
    g_stop = 1;
    pthread_join(h, NULL);
    return s;
}

static void test_negative_control(void)
{
    Scenario s = run_scenario(0);
    printf("     NEGATIVE CONTROL (no handoff): waiter acquired %d/%d, max latency %.1f ms\n",
           s.done, WAITER_ACQUISITIONS, s.max_ms);
    CHECK(InterlockedCompareExchange(&g_waiters, 0, 0) == 0, "count is 0 after the control run");
}

static void test_starvation(void)
{
    Scenario s = run_scenario(1);
    printf("     WITH handoff: waiter acquired %d/%d, max latency %.1f ms\n",
           s.done, WAITER_ACQUISITIONS, s.max_ms);
    CHECK(s.done == WAITER_ACQUISITIONS, "waiter completes all acquisitions against a tight holder");
    CHECK(s.max_ms < MAX_LATENCY_MS, "every acquisition within 100 ms");
    CHECK(g_counter == WAITER_ACQUISITIONS, "each acquisition ran its critical section once");
    CHECK(InterlockedCompareExchange(&g_waiters, 0, 0) == 0, "count is 0 after the run");
}

int main(void)
{
    qemu_mutex_init(&g_m);
    test_first_handoff();
    test_no_waiter_keeps_lock();
    test_bounded();
    test_negative_control();
    test_starvation();
    qemu_mutex_destroy(&g_m);
    if (g_failures) {
        printf("apu_lock_handoff_test: %d FAILED\n", g_failures);
        return 1;
    }
    printf("apu_lock_handoff_test: all passed\n");
    return 0;
}
