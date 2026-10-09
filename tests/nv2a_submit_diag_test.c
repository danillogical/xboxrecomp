/*
 * NV2A submit diagnostics: the rejection/recovery log lines, the published
 * NV2ASubmitState, and the RECOMP_NV2A_ADMIT_UNKNOWN switch, driven through the
 * real MMIO entry points of nv2a_core.c.
 *
 * stderr is captured through a temporary file so the tests can count the exact
 * lines the game's log parser keys on.
 */
#include "nv2a_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define DUP _dup
#define DUP2 _dup2
#define CLOSE _close
#define FILENO _fileno
#else
#include <unistd.h>
#define DUP dup
#define DUP2 dup2
#define CLOSE close
#define FILENO fileno
#endif

bool nv2a_method_implemented(uint32_t class_id, uint32_t method);

static int g_failures;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            ++g_failures;                                                   \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
    char buf[128];
    snprintf(buf, sizeof(buf), "%s=%s", name, value ? value : "");
    _putenv(buf);
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param)
{
    (void)subchannel; (void)method; (void)param;
    return 1;
}

/* ── stderr capture ──────────────────────────────────────────────────── */

static int g_saved_fd = -1;
static FILE *g_cap;

static void cap_begin(void)
{
    fflush(stderr);
    g_cap = tmpfile();
    g_saved_fd = DUP(FILENO(stderr));
    DUP2(FILENO(g_cap), FILENO(stderr));
}

/* Restore stderr and return everything written since cap_begin(). */
static const char *cap_end(void)
{
    static char text[1 << 16];
    size_t n;
    fflush(stderr);
    DUP2(g_saved_fd, FILENO(stderr));
    CLOSE(g_saved_fd);
    rewind(g_cap);
    n = fread(text, 1, sizeof(text) - 1, g_cap);
    text[n] = 0;
    fclose(g_cap);
    g_cap = NULL;
    return text;
}

static unsigned count_of(const char *text, const char *needle)
{
    unsigned n = 0;
    for (const char *p = text; (p = strstr(p, needle)) != NULL; p += strlen(needle)) ++n;
    return n;
}

/* ── fixture ─────────────────────────────────────────────────────────── */

static uint8_t g_window[1u << 20];
static uint8_t g_ramin[64u << 10];
static uint8_t g_vram[4096];

#define PB_BASE     0x1000u
#define H_KELVIN    0x0000000Du
#define H_MEMCPY    0x0000000Eu
#define H_OTHER     0x0000000Fu   /* a class the switch does not cover */
#define INST_KELVIN 0x1000u
#define INST_MEMCPY 0x1010u
#define INST_OTHER  0x1020u

#define UNKNOWN_NV097  0x17A0u    /* absent from nv2a_method_table.c */
#define UNKNOWN_NV097B 0x17A4u
#define UNKNOWN_MEMCPY 0x0FF0u

static void wr32(uint8_t *base, uint32_t off, uint32_t v)
{
    memcpy(base + off, &v, 4);
}

static void ramht_insert(uint32_t handle, uint32_t instance)
{
    uint32_t hash = 0, h = handle;
    while (h) { hash ^= h & 0x7FFu; h >>= 11; }
    wr32(g_ramin, hash * 8u, handle);
    wr32(g_ramin, hash * 8u + 4, NV_RAMHT_STATUS | (instance >> 4));
}

static NV2AState *fresh(void)
{
    NV2AState *d;
    memset(g_window, 0, sizeof(g_window));
    memset(g_ramin, 0, sizeof(g_ramin));
    nv2a_reset_standalone_for_test();
    nv2a_bind_instance_memory(0x00F00000u, g_ramin, sizeof(g_ramin));
    d = nv2a_init_standalone(g_vram, sizeof(g_vram), NULL, 0);
    nv2a_set_pushbuffer_window(d, g_window, 0, sizeof(g_window));
    d->pfifo.regs[NV_PFIFO_RAMHT] = 0;
    ramht_insert(H_KELVIN, INST_KELVIN);
    wr32(g_ramin, INST_KELVIN, 0x97u);
    ramht_insert(H_MEMCPY, INST_MEMCPY);
    wr32(g_ramin, INST_MEMCPY, 0x39u);
    ramht_insert(H_OTHER, INST_OTHER);
    wr32(g_ramin, INST_OTHER, 0x56u);
    return d;
}

typedef struct { uint32_t start, at; } Pb;

static uint32_t hdr(uint32_t subchannel, uint32_t method, uint32_t count)
{
    return (count << 18) | (subchannel << 13) | method;
}

static void pb_begin(Pb *pb, uint32_t at) { pb->start = pb->at = at; }

static void pb_word(Pb *pb, uint32_t w)
{
    wr32(g_window, pb->at, w);
    pb->at += 4;
}

static void pb_method(Pb *pb, uint32_t subchannel, uint32_t method, uint32_t param)
{
    pb_word(pb, hdr(subchannel, method, 1));
    pb_word(pb, param);
}

static void mmio_w(NV2AState *d, uint32_t addr, uint32_t v)
{
    nv2a_mmio_write(d, addr, v, 4);
}

#define USER(r) (0x800000u + (r))
/* PCRTC's block-local offsets. The block is reached by calling pcrtc_write
 * directly (it is declared by DEFINE_PROTO in nv2a_state.h) rather than through
 * nv2a_mmio_write, because the MMIO hook routes the PFIFO/USER aperture the
 * submit tests use and does not dispatch the PCRTC window. */
#define PCRTC_INTR_0     0x100u
#define PCRTC_INTR_EN_0  0x140u

static uint32_t get_ptr(NV2AState *d)
{
    return (uint32_t)nv2a_mmio_read(d, USER(NV_USER_DMA_GET), 4);
}

static void kick(NV2AState *d, uint32_t get, uint32_t put)
{
    mmio_w(d, USER(NV_USER_DMA_GET), get);
    mmio_w(d, USER(NV_USER_DMA_PUT), put);
}

static void kick_put(NV2AState *d, uint32_t put)
{
    mmio_w(d, USER(NV_USER_DMA_PUT), put);
}

static const char *diag(NV2AState *d)
{
    return nv2a_submit_diagnostic(d->pfifo.submit_diag);
}

static int generation_even(void)
{
    return (g_nv2a_submit_state.generation & 1) == 0;
}

/* The diagnostic state the walk publishes must agree with the model's own. */
static void check_state_matches_model(NV2AState *d, const char *what)
{
    CHECK(g_nv2a_submit_state.diag == d->pfifo.submit_diag,
          "%s: state diag %u != model %u", what, g_nv2a_submit_state.diag,
          d->pfifo.submit_diag);
    CHECK(generation_even(), "%s: generation %ld is odd outside a write", what,
          (long)g_nv2a_submit_state.generation);
}

/* ── tests ───────────────────────────────────────────────────────────── */

/* Tests 1-3 share one stream: 70 good submissions, a stuck one kicked 301
 * times, then a repaired one. */
static void test_rejection_logging_and_recovery(void)
{
    NV2AState *d;
    Pb pb;
    const char *log;
    uint32_t bad_at, bad_put, before_get;
    unsigned i;

    CHECK(!nv2a_method_implemented(0x97u, UNKNOWN_NV097),
          "fixture method 0x%04X is implemented; pick another", UNKNOWN_NV097);

    d = fresh();
    CHECK(g_nv2a_submit_state.successes == 0 && g_nv2a_submit_state.rejections == 0,
          "nv2a_reset_standalone_for_test did not clear g_nv2a_submit_state "
          "(successes=%u rejections=%u)", g_nv2a_submit_state.successes,
          g_nv2a_submit_state.rejections);

    /* ── 1. 70 successes, then one rejection ── */
    cap_begin();
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    kick(d, pb.start, pb.at);
    for (i = 1; i < 70; ++i) {
        pb_method(&pb, 0, 0x1760u, 0x00002042u);
        kick_put(d, pb.at);
    }
    log = cap_end();
    CHECK(get_ptr(d) == pb.at, "good stream did not drain (get=%08X put=%08X; %s)",
          get_ptr(d), pb.at, diag(d));
    CHECK(g_nv2a_submit_state.successes == 70, "successes %u, want 70",
          g_nv2a_submit_state.successes);
    CHECK(g_nv2a_submit_state.rejections == 0, "rejections %u before any failure",
          g_nv2a_submit_state.rejections);
    CHECK(count_of(log, "[PFIFO] reject ") == 0, "reject line on a clean stream");

    bad_at = pb.at;
    before_get = get_ptr(d);
    pb_method(&pb, 0, UNKNOWN_NV097, 0x12345678u);
    bad_put = pb.at;
    cap_begin();
    kick_put(d, bad_put);
    log = cap_end();

    CHECK(strcmp(diag(d), "unsupported_method") == 0, "model diag %s", diag(d));
    check_state_matches_model(d, "first rejection");
    CHECK(strcmp(nv2a_submit_diagnostic(g_nv2a_submit_state.diag), "unsupported_method") == 0,
          "state diag names %s", nv2a_submit_diagnostic(g_nv2a_submit_state.diag));
    CHECK(g_nv2a_submit_state.rejections == 1, "rejections %u, want 1",
          g_nv2a_submit_state.rejections);
    CHECK(g_nv2a_submit_state.consecutive_rejections == 1, "consecutive %u, want 1",
          g_nv2a_submit_state.consecutive_rejections);
    CHECK(g_nv2a_submit_state.successes == 70, "successes %u after a rejection, want 70",
          g_nv2a_submit_state.successes);
    CHECK(get_ptr(d) == before_get, "rejection moved GET to %08X", get_ptr(d));
    CHECK(g_nv2a_submit_state.method == UNKNOWN_NV097 && g_nv2a_submit_state.subchannel == 0 &&
          g_nv2a_submit_state.param == 0x12345678u,
          "state names method=%03X subch=%u param=%08X", g_nv2a_submit_state.method,
          g_nv2a_submit_state.subchannel, g_nv2a_submit_state.param);
    CHECK(g_nv2a_submit_state.at == d->pfifo.submit_diag_get,
          "state at=%08X, model at=%08X", g_nv2a_submit_state.at, d->pfifo.submit_diag_get);
    CHECK(g_nv2a_submit_state.get == before_get && g_nv2a_submit_state.put == bad_put,
          "state get=%08X put=%08X", g_nv2a_submit_state.get, g_nv2a_submit_state.put);
    CHECK(count_of(log, "[PFIFO] reject diag=unsupported_method ") == 1,
          "expected one reject line, got %u:\n%s",
          count_of(log, "[PFIFO] reject diag=unsupported_method "), log);
    CHECK(strstr(log, "method=17A0 subch=0 param=12345678") != NULL,
          "reject line lacks method/subch/param:\n%s", log);
    CHECK(strstr(log, "successes=70") != NULL, "reject line lacks successes=70:\n%s", log);
    {
        char at_text[32];
        snprintf(at_text, sizeof(at_text), "at=%08X", d->pfifo.submit_diag_get);
        CHECK(strstr(log, at_text) != NULL, "reject line lacks %s:\n%s", at_text, log);
    }

    /* ── 2. the same stuck stream, 300 more times ── */
    cap_begin();
    for (i = 0; i < 300; ++i) kick_put(d, bad_put);
    log = cap_end();
    CHECK(g_nv2a_submit_state.rejections == 301, "rejections %u, want 301",
          g_nv2a_submit_state.rejections);
    CHECK(g_nv2a_submit_state.consecutive_rejections == 301, "consecutive %u, want 301",
          g_nv2a_submit_state.consecutive_rejections);
    CHECK(count_of(log, "[PFIFO] reject ") == 0, "repeat kicks printed %u reject lines",
          count_of(log, "[PFIFO] reject "));
    CHECK(count_of(log, "[PFIFO] still rejecting ") == 2, "still-rejecting lines: %u:\n%s",
          count_of(log, "[PFIFO] still rejecting "), log);
    CHECK(strstr(log, "[PFIFO] still rejecting n=16 diag=unsupported_method") != NULL,
          "missing n=16 line:\n%s", log);
    CHECK(strstr(log, "[PFIFO] still rejecting n=256 diag=unsupported_method") != NULL,
          "missing n=256 line:\n%s", log);
    CHECK(count_of(log, "[PFIFO] recovered") == 0, "recovered printed while stuck");
    CHECK(get_ptr(d) == before_get, "stuck stream moved GET to %08X", get_ptr(d));
    CHECK(generation_even(), "generation odd after repeats");

    /* ── 3. repair the stream in place ── */
    wr32(g_window, bad_at, hdr(0, 0x1760u, 1));
    wr32(g_window, bad_at + 4, 0x00002042u);
    cap_begin();
    kick_put(d, bad_put);
    log = cap_end();
    CHECK(get_ptr(d) == bad_put, "repaired stream did not drain (get=%08X; %s)",
          get_ptr(d), diag(d));
    CHECK(count_of(log, "[PFIFO] recovered after 301 rejections ") == 1,
          "expected one 'recovered after 301 rejections':\n%s", log);
    CHECK(count_of(log, "[PFIFO] recovered") == 1, "recovered lines: %u",
          count_of(log, "[PFIFO] recovered"));
    CHECK(g_nv2a_submit_state.consecutive_rejections == 0, "consecutive %u after recovery",
          g_nv2a_submit_state.consecutive_rejections);
    CHECK(g_nv2a_submit_state.rejections == 301, "rejections %u changed by recovery",
          g_nv2a_submit_state.rejections);
    CHECK(g_nv2a_submit_state.successes == 71, "successes %u, want 71",
          g_nv2a_submit_state.successes);
    CHECK(g_nv2a_submit_state.diag == 0 && generation_even(), "state diag %u after recovery",
          g_nv2a_submit_state.diag);
}

/* A hold is not a rejection; and a change of diagnostic while stuck is logged. */
static void test_diag_change_logs_again(void)
{
    NV2AState *d = fresh();
    Pb pb;
    const char *log;

    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, 0x00000077u);          /* not in RAMHT */
    cap_begin();
    kick(d, pb.start, pb.at);
    log = cap_end();
    CHECK(strcmp(diag(d), "invalid_handle") == 0, "diag %s", diag(d));
    CHECK(count_of(log, "[PFIFO] reject diag=invalid_handle ") == 1, "invalid_handle line:\n%s", log);

    pb_begin(&pb, PB_BASE + 0x100u);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_method(&pb, 0, UNKNOWN_NV097, 0);
    cap_begin();
    kick(d, pb.start, pb.at);
    log = cap_end();
    CHECK(count_of(log, "[PFIFO] reject diag=unsupported_method ") == 1,
          "a changed diag was not logged:\n%s", log);
    CHECK(g_nv2a_submit_state.rejections == 2, "rejections %u, want 2",
          g_nv2a_submit_state.rejections);
}

/* 4. Forced off, the environment variable changes nothing. */
static void test_default_contract_forced_off(void)
{
    NV2AState *d;
    Pb pb;
    const char *log;

    set_env("RECOMP_NV2A_ADMIT_UNKNOWN", "1");
    d = fresh();
    nv2a_admit_unknown_override(0);
    CHECK(!nv2a_admit_unknown_enabled(), "override(0) left the switch on");

    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_method(&pb, 0, UNKNOWN_NV097, 1);
    cap_begin();
    kick(d, pb.start, pb.at);
    log = cap_end();
    CHECK(strcmp(diag(d), "unsupported_method") == 0, "diag %s", diag(d));
    CHECK(get_ptr(d) == pb.start, "unknown method moved GET to %08X", get_ptr(d));
    CHECK(g_nv2a_submit_state.admitted_unknown == 0, "admitted %u", g_nv2a_submit_state.admitted_unknown);
    CHECK(count_of(log, "admit-unknown") == 0, "admit-unknown line while off:\n%s", log);
    CHECK(strstr(log, "RECOMP_NV2A_ADMIT_UNKNOWN=1") == NULL, "banner while forced off:\n%s", log);

    nv2a_admit_unknown_override(-1);
    set_env("RECOMP_NV2A_ADMIT_UNKNOWN", NULL);
}

/* 5. Switched on: the four known classes admit unknown methods, nothing else. */
static void test_admit_unknown_on(void)
{
    NV2AState *d;
    Pb pb;
    const char *log;
    uint32_t first_put, second_put;

    set_env("RECOMP_NV2A_ADMIT_UNKNOWN", NULL);
    d = fresh();
    nv2a_admit_unknown_override(1);
    CHECK(nv2a_admit_unknown_enabled(), "override(1) left the switch off");
    CHECK(!nv2a_method_implemented(0x39u, UNKNOWN_MEMCPY),
          "fixture method 0x%04X is implemented on NV39", UNKNOWN_MEMCPY);

    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_method(&pb, 0, UNKNOWN_NV097, 0xAABBCCDDu);
    first_put = pb.at;
    cap_begin();
    kick(d, pb.start, pb.at);
    log = cap_end();
    CHECK(get_ptr(d) == first_put, "admitted stream did not drain (get=%08X; %s)",
          get_ptr(d), diag(d));
    CHECK(g_nv2a_submit_state.admitted_unknown == 1, "admitted %u, want 1",
          g_nv2a_submit_state.admitted_unknown);
    CHECK(g_nv2a_submit_state.rejections == 0, "rejections %u on an admitted stream",
          g_nv2a_submit_state.rejections);
    CHECK(count_of(log, "[PFIFO] admit-unknown class=97 method=17A0 param=AABBCCDD") == 1,
          "expected one admit-unknown line:\n%s", log);
    CHECK(count_of(log, "admit-unknown") == 1, "admit-unknown lines: %u",
          count_of(log, "admit-unknown"));
    CHECK(strstr(log, "RECOMP_NV2A_ADMIT_UNKNOWN=1") != NULL,
          "no banner naming RECOMP_NV2A_ADMIT_UNKNOWN=1:\n%s", log);

    /* The same method again: counted, not logged twice. */
    pb_method(&pb, 0, UNKNOWN_NV097, 0x11111111u);
    second_put = pb.at;
    cap_begin();
    kick_put(d, second_put);
    log = cap_end();
    CHECK(get_ptr(d) == second_put, "repeat did not drain");
    CHECK(count_of(log, "admit-unknown") == 0, "repeat printed another line:\n%s", log);
    CHECK(count_of(log, "RECOMP_NV2A_ADMIT_UNKNOWN=1") == 0, "banner printed twice");
    CHECK(g_nv2a_submit_state.admitted_unknown == 2, "admitted %u, want 2",
          g_nv2a_submit_state.admitted_unknown);

    /* A distinct (class, method) logs again. */
    pb_method(&pb, 0, UNKNOWN_NV097B, 5);
    cap_begin();
    kick_put(d, pb.at);
    log = cap_end();
    CHECK(count_of(log, "[PFIFO] admit-unknown class=97 method=17A4 param=00000005") == 1,
          "distinct method not logged once:\n%s", log);

    /* The same method number on another admitted class is a distinct key. */
    d = fresh();
    nv2a_admit_unknown_override(1);
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_method(&pb, 1, 0x0000, H_MEMCPY);
    pb_method(&pb, 1, UNKNOWN_MEMCPY, 9);
    cap_begin();
    kick(d, pb.start, pb.at);
    log = cap_end();
    CHECK(get_ptr(d) == pb.at, "NV39 unknown method not admitted (%s)", diag(d));
    CHECK(count_of(log, "[PFIFO] admit-unknown class=39 method=0FF0 param=00000009") == 1,
          "NV39 admit line missing:\n%s", log);
}

static void test_admit_unknown_boundaries(void)
{
    NV2AState *d;
    Pb pb;

    /* An unbound subchannel still rejects. */
    d = fresh();
    nv2a_admit_unknown_override(1);
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, UNKNOWN_NV097, 0);
    kick(d, pb.start, pb.at);
    CHECK(strcmp(diag(d), "unsupported_method") == 0, "unbound: diag %s", diag(d));
    CHECK(get_ptr(d) == pb.start, "unbound: GET moved to %08X", get_ptr(d));
    CHECK(g_nv2a_submit_state.admitted_unknown == 0 && g_nv2a_submit_state.rejections == 1,
          "unbound: admitted=%u rejections=%u", g_nv2a_submit_state.admitted_unknown,
          g_nv2a_submit_state.rejections);

    /* A subchannel bound to a class outside the four does not admit. */
    d = fresh();
    nv2a_admit_unknown_override(1);
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_method(&pb, 2, 0x0000, H_OTHER);
    pb_method(&pb, 2, UNKNOWN_NV097, 0);
    kick(d, pb.start, pb.at);
    CHECK(get_ptr(d) == pb.start, "other class: GET moved to %08X (%s)", get_ptr(d), diag(d));
    CHECK(g_nv2a_submit_state.admitted_unknown == 0 && g_nv2a_submit_state.rejections == 1,
          "other class: admitted=%u rejections=%u", g_nv2a_submit_state.admitted_unknown,
          g_nv2a_submit_state.rejections);

    /* An invalid RAMHT handle still rejects as invalid_handle. */
    d = fresh();
    nv2a_admit_unknown_override(1);
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, 0x00000077u);
    pb_method(&pb, 0, UNKNOWN_NV097, 0);
    kick(d, pb.start, pb.at);
    CHECK(strcmp(diag(d), "invalid_handle") == 0, "bad handle: diag %s", diag(d));
    CHECK(get_ptr(d) == pb.start, "bad handle: GET moved to %08X", get_ptr(d));
    CHECK(g_nv2a_submit_state.admitted_unknown == 0, "bad handle: admitted %u",
          g_nv2a_submit_state.admitted_unknown);
}

/* 6. The witness must cover a WHOLE walk, not a prefix of it.
 *
 * The `[PFIFO] admit-unknown` line is the only admissible provenance for
 * adding a method to the generated admission table (ledger L39), so a walk
 * that reports only its first N unknown methods silently loses the rest: the
 * run looks complete while the tail is invisible, and the offline decode of
 * the same region disagrees with the log. That is exactly what happened when
 * the report was capped at 16 -- a real run logged 0x0420-0x042C and
 * 0x0480-0x04AC and then rejected, while 35 methods were missing.
 *
 * 20 distinct unknown NV097 methods in ONE walk: every one must be reported,
 * and the reported set must be exactly the submitted set. 20 is chosen to sit
 * just above the old 16-entry cap while staying well inside the 256 capacity,
 * so the test fails on the truncation rather than on a capacity boundary. */
/* 20 consecutive NV097 methods that are absent from nv2a_method_table.c. The
 * method field is 13 bits, so the range must stay under 0x2000. */
#define MANY_UNKNOWN_BASE  0x1EA8u
#define MANY_UNKNOWN_COUNT 20u

static void test_admit_unknown_witness_is_not_truncated(void)
{
    NV2AState *d;
    Pb pb;
    const char *log;
    char needle[64];
    unsigned i, found = 0;

    /* The fixture methods must really be absent, or the walk would not admit
     * them and the test would pass for the wrong reason. */
    for (i = 0; i < MANY_UNKNOWN_COUNT; ++i) {
        uint32_t m = MANY_UNKNOWN_BASE + 4u * i;
        CHECK(!nv2a_method_implemented(0x97u, m),
              "fixture method %04X is implemented on NV097", m);
    }

    d = fresh();
    nv2a_admit_unknown_override(1);
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    for (i = 0; i < MANY_UNKNOWN_COUNT; ++i)
        pb_method(&pb, 0, MANY_UNKNOWN_BASE + 4u * i, 0x1000u + i);

    cap_begin();
    kick(d, pb.start, pb.at);
    log = cap_end();

    /* The stream itself still drains: the cap is on the WITNESS, not the walk. */
    CHECK(get_ptr(d) == pb.at, "many-unknown stream did not drain (get=%08X; %s)",
          get_ptr(d), diag(d));
    CHECK(g_nv2a_submit_state.rejections == 0, "rejections %u on an admitted stream",
          g_nv2a_submit_state.rejections);
    CHECK(g_nv2a_submit_state.admitted_unknown == MANY_UNKNOWN_COUNT,
          "admitted_unknown=%u, want %u",
          g_nv2a_submit_state.admitted_unknown, MANY_UNKNOWN_COUNT);
    CHECK(count_of(log, "admit-unknown") == MANY_UNKNOWN_COUNT,
          "admit-unknown lines: %u, want %u (the witness was truncated)\n%s",
          count_of(log, "admit-unknown"), MANY_UNKNOWN_COUNT, log);

    /* Not just the COUNT: each specific method must be named. Counting alone
     * would accept a witness that reported one method twenty times. */
    for (i = 0; i < MANY_UNKNOWN_COUNT; ++i) {
        uint32_t m = MANY_UNKNOWN_BASE + 4u * i;
        unsigned n;
        snprintf(needle, sizeof(needle),
                 "[PFIFO] admit-unknown class=97 method=%04X param=%08X", m,
                 0x1000u + i);
        n = count_of(log, needle);
        if (n == 1) ++found;
        CHECK(n == 1, "method %04X: witness line %u times, want 1", m, n);
    }
    CHECK(found == MANY_UNKNOWN_COUNT,
          "only %u of %u distinct methods were named in the witness", found,
          MANY_UNKNOWN_COUNT);
}

/* -1 reads the environment on next use; exactly "1" enables. */
static void test_override_minus_one_reads_environment(void)
{
    (void)fresh();
    set_env("RECOMP_NV2A_ADMIT_UNKNOWN", NULL);
    nv2a_admit_unknown_override(-1);
    CHECK(!nv2a_admit_unknown_enabled(), "unset variable enabled the switch");

    set_env("RECOMP_NV2A_ADMIT_UNKNOWN", "1");
    nv2a_admit_unknown_override(-1);
    CHECK(nv2a_admit_unknown_enabled(), "RECOMP_NV2A_ADMIT_UNKNOWN=1 did not enable");

    set_env("RECOMP_NV2A_ADMIT_UNKNOWN", "2");
    nv2a_admit_unknown_override(-1);
    CHECK(!nv2a_admit_unknown_enabled(), "value 2 enabled the switch");

    set_env("RECOMP_NV2A_ADMIT_UNKNOWN", "0");
    nv2a_admit_unknown_override(-1);
    CHECK(!nv2a_admit_unknown_enabled(), "value 0 enabled the switch");

    set_env("RECOMP_NV2A_ADMIT_UNKNOWN", "1");
    nv2a_admit_unknown_override(0);
    CHECK(!nv2a_admit_unknown_enabled(), "override(0) lost to the environment");

    set_env("RECOMP_NV2A_ADMIT_UNKNOWN", NULL);
    nv2a_admit_unknown_override(-1);
    CHECK(!nv2a_admit_unknown_enabled(), "override(-1) with the variable unset is enabled");
}

/* ── kick observer and stalled-walk retry ────────────────────────────── */

static unsigned g_kicks, g_commits, g_partials;
static uint32_t g_partial_value, g_commit_value, g_kick_value;

static void observer(int event, uint32_t value)
{
    if (event == NV2A_KICK) { ++g_kicks; g_kick_value = value; }
    else if (event == NV2A_COMMIT) { ++g_commits; g_commit_value = value; }
    else if (event == NV2A_COMMIT_PARTIAL) { ++g_partials; g_partial_value = value; }
}

static void observer_reset(void)
{
    g_kicks = g_commits = g_partials = 0;
    g_partial_value = g_commit_value = g_kick_value = 0;
    nv2a_set_kick_observer(observer);
}

#define H_LATE    0x00000010u   /* bound into RAMHT only after a rejection */
#define INST_LATE 0x1030u

static void test_observer_counts_kicks_and_commits(void)
{
    NV2AState *d = fresh();
    Pb pb;
    unsigned i;

    observer_reset();
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    kick(d, pb.start, pb.at);           /* GET write: not a kick */
    for (i = 1; i < 5; ++i) {
        pb_method(&pb, 0, 0x1760u, 0x00002042u);
        kick_put(d, pb.at);
    }
    CHECK(g_kicks == 5, "kicks %u, want 5", g_kicks);
    CHECK(g_commits == 5, "commits %u, want 5", g_commits);

    pb_method(&pb, 0, UNKNOWN_NV097, 0);
    kick_put(d, pb.at);
    CHECK(g_kicks == 6, "rejected submit: kicks %u, want 6", g_kicks);
    CHECK(g_commits == 5, "rejected submit committed (%u)", g_commits);

    nv2a_set_kick_observer(NULL);
    pb_begin(&pb, PB_BASE + 0x200u);
    pb_method(&pb, 0, 0x1760u, 0x00002042u);
    kick(d, pb.start, pb.at);
    CHECK(g_kicks == 6 && g_commits == 5, "a cleared observer was still called");
}

static void test_retry_with_nothing_rejected(void)
{
    NV2AState *d = fresh();
    Pb pb;
    const char *log;

    observer_reset();
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    kick(d, pb.start, pb.at);
    g_kicks = g_commits = 0;

    cap_begin();
    CHECK(!nv2a_retry_stalled_walk(d), "retry walked with nothing rejected");
    log = cap_end();
    CHECK(g_kicks == 0 && g_commits == 0, "retry with nothing rejected called the observer");
    CHECK(count_of(log, "[PFIFO] ") == 0, "retry with nothing rejected logged:\n%s", log);
    nv2a_set_kick_observer(NULL);
}

/* A rejection that becomes valid without a PUT write: the handle is bound
 * into RAMHT afterwards. */
static void test_retry_recovers_after_ramht_write(void)
{
    NV2AState *d = fresh();
    Pb pb;
    const char *log;

    observer_reset();
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_LATE);
    pb_method(&pb, 0, 0x1760u, 0x00002042u);
    kick(d, pb.start, pb.at);
    CHECK(strcmp(diag(d), "invalid_handle") == 0, "diag %s", diag(d));
    CHECK(g_nv2a_submit_state.consecutive_rejections == 1, "consecutive %u",
          g_nv2a_submit_state.consecutive_rejections);
    CHECK(g_kicks == 1 && g_commits == 0, "kicks %u commits %u", g_kicks, g_commits);

    /* Still unbound: a retry walks again, fails again, and does not commit. */
    cap_begin();
    CHECK(!nv2a_retry_stalled_walk(d), "retry committed with the handle still missing");
    log = cap_end();
    CHECK(g_commits == 0 && g_kicks == 1, "failed retry: kicks %u commits %u", g_kicks, g_commits);
    CHECK(g_nv2a_submit_state.consecutive_rejections == 2,
          "a retried rejection is counted (consecutive %u)",
          g_nv2a_submit_state.consecutive_rejections);
    CHECK(count_of(log, "[PFIFO] recovered") == 0, "recovered while still stuck");

    ramht_insert(H_LATE, INST_LATE);
    wr32(g_ramin, INST_LATE, 0x97u);
    cap_begin();
    CHECK(nv2a_retry_stalled_walk(d), "retry did not commit once the handle existed (%s)", diag(d));
    log = cap_end();
    CHECK(get_ptr(d) == pb.at, "retry left GET at %08X, want %08X", get_ptr(d), pb.at);
    CHECK(g_commits == 1, "commits %u, want 1", g_commits);
    CHECK(g_kicks == 1, "retry reported a kick (%u)", g_kicks);
    CHECK(g_nv2a_submit_state.consecutive_rejections == 0, "consecutive %u after retry",
          g_nv2a_submit_state.consecutive_rejections);
    CHECK(count_of(log, "[PFIFO] recovered after ") == 1, "recovered line missing:\n%s", log);

    CHECK(!nv2a_retry_stalled_walk(d), "retry walked again after recovery");
    CHECK(g_commits == 1, "second retry committed");
    nv2a_set_kick_observer(NULL);
}

/* The same, repaired by patching the bad method word in the pushbuffer. */
static void test_retry_recovers_after_pushbuffer_patch(void)
{
    NV2AState *d = fresh();
    Pb pb;
    uint32_t bad_at;
    const char *log;

    observer_reset();
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    bad_at = pb.at;
    pb_method(&pb, 0, UNKNOWN_NV097, 0);
    kick(d, pb.start, pb.at);
    CHECK(g_nv2a_submit_state.consecutive_rejections == 1, "consecutive %u",
          g_nv2a_submit_state.consecutive_rejections);

    wr32(g_window, bad_at, hdr(0, 0x1760u, 1));
    wr32(g_window, bad_at + 4, 0x00002042u);
    cap_begin();
    CHECK(nv2a_retry_stalled_walk(d), "retry did not commit the patched stream (%s)", diag(d));
    log = cap_end();
    CHECK(get_ptr(d) == pb.at, "GET %08X, want %08X", get_ptr(d), pb.at);
    CHECK(g_commits == 1 && g_kicks == 1, "kicks %u commits %u", g_kicks, g_commits);
    CHECK(count_of(log, "[PFIFO] recovered after ") == 1, "recovered line missing:\n%s", log);
    CHECK(g_nv2a_submit_state.consecutive_rejections == 0, "consecutive %u",
          g_nv2a_submit_state.consecutive_rejections);
    nv2a_set_kick_observer(NULL);
}

/* The budget transcript must be readable AFTER the run, and must say WHICH limit
 * fired and WHERE -- including the case that used to print nothing at all.
 *
 * Why this test exists: the walk can exhaust its budget at a packet HEADER or
 * inside a packet's PARAMETERS, and only the header path used to print. A
 * parameter-heavy stream therefore stopped SILENTLY, and the investigation could
 * only infer "inside a packet" from the absence of a dump. Worse, the submit log
 * stops after 64 walks and the old "last 32 visits" array was written only at
 * headers but indexed by total words, so its slots were sparse and out of order.
 *
 * This drives a single packet whose parameter count exceeds the word budget, so
 * the stop is deterministic and the transcript is the only record of it. */
static void test_budget_stop_transcript(void)
{
    NV2AState *d;
    Pb pb;
    const char *log;
    uint32_t i;

    d = fresh();
    CHECK(g_nv2a_submit_state.budget_stops == 0,
          "reset left budget_stops=%u", g_nv2a_submit_state.budget_stops);

    /* Build a stream that exhausts the WORD budget INSIDE a packet's parameters.
     *
     * The arithmetic is forced by the walk's own guards, so it is worth writing
     * down, including the SET_OBJECT that opens the stream (1 header + 1 handle =
     * 2 words, and it is NOT staged because SET_OBJECT is handled separately):
     *
     *   SET_OBJECT:              2 words                    words 2
     *   packet 1: count 2047  -> 1 header + 2047 params  = 2048 words, words 2050
     *   packet 2: count 2043  -> 1 header + 2043 params  = 2044 words, words 4094
     *   packet 3: count    5  -> header passes the sink check (staged 4090 + 5 =
     *                            4095 <= 4096), taking words to 4095; ONE parameter
     *                            is then read (words 4096) and the next hits the
     *                            cap with FOUR still unread.
     *
     * So the stop is INSIDE the packet with count == 4 -- not 1, which is what the
     * earlier version of this comment claimed by leaving the SET_OBJECT out of its
     * own account.
     *
     * Non-incrementing packets are used because an incrementing count that large
     * would run the method past 0x1FFC and be rejected by the method-range check
     * first -- a different diagnostic. 0x1760 is in the generated table. */
    /* Build a stream that exhausts the PACKET budget.
     *
     * This test used to drive the WORD cap from inside a packet's parameters,
     * with counts 2047/2043/5 chosen so the 4096-word cap fired four parameters
     * into the third packet. That is no longer reachable, and the reason is
     * structural rather than a changed constant: the walk now ends a UNIT at a
     * packet header when the incoming packet would not fit
     * (unit_words + 1 + count > NV2A_SUBMIT_MAX_WORDS). A packet is at most
     * 1 + 2047 = 2048 words, so a unit that starts empty always fits at least
     * one whole packet, and after that packet the unit is at most 4096 words.
     * The in-parameter word cap therefore cannot fire for a header-driven
     * packet; it survives only as a fail-closed guard.
     *
     * The 1024-PACKET cap is still reachable and still a STOP (it is
     * deliberately not a yield: 1025 one-word packets is under the word budget,
     * and treating it as a yield would make the contract case in
     * tests/test_nv2a_contract.c accept). So the transcript's job -- proving
     * that the FIRST stop is latched, readable from a dump, and frozen against
     * a retry -- is tested through the packet cap instead. */
    cap_begin();
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);                 /* bind NV097 */
    /* 1024 packets of count 1 (2 words each: header + one parameter), so the
     * stream reaches the packet cap at 2050 words -- well under the word
     * budget, which is what makes the packet cap the limit under test.
     *
     * count 1 rather than 0 matters for the retry below: with a parameter
     * present, a word inside the trajectory window can be corrupted WITHOUT
     * changing control flow, which is what lets the retry reach the same cap
     * and prove the latched transcript is not overwritten. Corrupting a
     * one-word NOP header would instead reject as an unknown opcode and never
     * reach the budget stop at all. */
    for (i = 0; i < 1024u; ++i) {
        pb_word(&pb, (1u << 18) | 0x1760u);
        pb_word(&pb, 0x22220000u + i);
    }
    kick(d, pb.start, pb.at);

    log = cap_end();
    CHECK(get_ptr(d) == pb.start,
          "a rejected budget stream must not move GET (got %08X, want %08X)",
          get_ptr(d), pb.start);
    CHECK(strcmp(nv2a_submit_diagnostic(g_nv2a_submit_state.diag),
                 "budget_exhausted") == 0,
          "diag is %s, want budget_exhausted",
          nv2a_submit_diagnostic(g_nv2a_submit_state.diag));

    /* The latched transcript is what a dump reads later. */
    CHECK(g_nv2a_submit_state.budget_stops == 1,
          "budget_stops=%u, want 1", g_nv2a_submit_state.budget_stops);
    CHECK(g_nv2a_submit_state.budget_at_packet_limit == 1,
          "the PACKET cap fired, so at_packet_limit must be 1 (got %u)",
          g_nv2a_submit_state.budget_at_packet_limit);
    CHECK(g_nv2a_submit_state.budget_in_param == 0,
          "a packet-cap stop is at a HEADER, so in_param must be 0 (got %u)",
          g_nv2a_submit_state.budget_in_param);
    CHECK(g_nv2a_submit_state.budget_packets == 1024u,
          "budget_packets=%u, want 1024", g_nv2a_submit_state.budget_packets);
    CHECK(g_nv2a_submit_state.budget_words < NV2A_SUBMIT_MAX_WORDS,
          "the word budget must NOT be reached (words=%u)",
          g_nv2a_submit_state.budget_words);
    /* local_pc is where the walk was consuming, which is NOT the rollback
     * origin `at`. Reporting `at` as the failure point is the trap this field
     * exists to avoid, so they must differ here. */
    CHECK(g_nv2a_submit_state.budget_local_pc != g_nv2a_submit_state.at,
          "budget_local_pc (%08X) must not equal the rollback origin at (%08X)",
          g_nv2a_submit_state.budget_local_pc, g_nv2a_submit_state.at);
    /* The frontier is the header the walk refused to consume. It is exactly
     * `budget_words` words past the start, because words counts every word
     * consumed and the frontier is the next unconsumed one -- asserted
     * self-consistently rather than with a hardcoded offset. */
    CHECK(g_nv2a_submit_state.budget_local_pc ==
              pb.start + g_nv2a_submit_state.budget_words * 4u,
          "budget_local_pc=%08X is not budget_words (%u) past the start %08X",
          g_nv2a_submit_state.budget_local_pc,
          g_nv2a_submit_state.budget_words, pb.start);

    /* The trajectory must be DENSE and chronological: that is what makes it
     * usable as a cyclic-walk discriminator, which the old header-only ring was
     * not. */
    CHECK(d->pfifo.budget_trace_count == 64u,
          "budget_trace_count=%u, want a full 64-word window",
          d->pfifo.budget_trace_count);
    {
        /* The window must be contiguous, so it is a real trajectory rather than a
         * sparse ring of headers. */
        CHECK(d->pfifo.budget_trace_va[63] - d->pfifo.budget_trace_va[62] == 4u,
              "trajectory addresses are not contiguous: %08X then %08X",
              d->pfifo.budget_trace_va[62], d->pfifo.budget_trace_va[63]);
        CHECK(d->pfifo.budget_trace_va[63] == g_nv2a_submit_state.budget_local_pc - 4u,
              "the newest trajectory entry (%08X) is not the word before the frontier (%08X)",
              d->pfifo.budget_trace_va[63], g_nv2a_submit_state.budget_local_pc);
    }

    /* The transcript is ALSO printed, so a run whose dump is not read still
     * shows which limit fired. */
    CHECK(count_of(log, "budget_exhausted") >= 1,
          "no budget_exhausted line in the log:\n%s", log);
    CHECK(count_of(log, "LIMIT=packets(1024)") == 1,
          "the log must name the packet cap:\n%s", log);

    /* A later retry must NOT overwrite the latched first stop: the first is the
     * event of interest, and a rejection is retried on every kick. The trajectory
     * must freeze with it -- otherwise the array would describe the most recent
     * walk while the scalars describe the first, and the two would contradict
     * each other.
     *
     * The retry DELIBERATELY changes a word the walk will consume. Retrying the
     * identical stream and comparing counts cannot detect the defect, because the
     * counts are identical either way; only the CONTENTS differ. The whole 64-word
     * window is compared, not a couple of entries: the corrupted word lands
     * mid-window, so spot-checking the ends would miss it. */
    {
        uint32_t va_before[64], word_before[64];
        uint32_t trace_before = d->pfifo.budget_trace_count;
        uint32_t pc_before = g_nv2a_submit_state.budget_local_pc;
        unsigned k, changed = 0;

        memcpy(va_before, d->pfifo.budget_trace_va, sizeof(va_before));
        memcpy(word_before, d->pfifo.budget_trace_word, sizeof(word_before));

        /* Corrupt a word INSIDE the window the retry will consume -- mid-window,
         * where a rolling trajectory would record the new value.
         *
         * The offset must land on a PARAMETER, not a header. Packets here are
         * two words (header + one parameter), so an even offset from the
         * frontier -- which is itself a header -- is another header, and
         * corrupting a header changes control flow: the retry then rejects as
         * an unknown opcode and never reaches the budget stop at all, so the
         * latched-first-stop property would go untested. 31 words is odd, so it
         * is a parameter, and it is still inside the 64-word window. */
        wr32(g_window, g_nv2a_submit_state.budget_local_pc - 31u * 4u, 0xDEADBEEFu);

        kick(d, pb.start, pb.at);

        CHECK(g_nv2a_submit_state.budget_stops == 2,
              "budget_stops=%u, want 2 after a retry", g_nv2a_submit_state.budget_stops);
        CHECK(g_nv2a_submit_state.budget_local_pc == pc_before,
              "the latched frontier was overwritten by a retry (%08X -> %08X)",
              pc_before, g_nv2a_submit_state.budget_local_pc);
        CHECK(d->pfifo.budget_trace_count == trace_before,
              "the trajectory kept rolling after the first stop (%u -> %u)",
              trace_before, d->pfifo.budget_trace_count);
        for (k = 0; k < 64u; ++k) {
            if (d->pfifo.budget_trace_word[k] != word_before[k] ||
                d->pfifo.budget_trace_va[k] != va_before[k]) {
                if (!changed) {
                    fprintf(stderr, "FAIL trajectory[%u] changed after a retry: "
                            "va %08X->%08X word %08X->%08X\n", k,
                            va_before[k], d->pfifo.budget_trace_va[k],
                            word_before[k], d->pfifo.budget_trace_word[k]);
                }
                ++changed;
            }
        }
        CHECK(changed == 0,
              "%u of 64 trajectory entries were replaced by a later walk", changed);
    }
}

/* ── the budget stop's CONTINUATION: does the same submission resume at the
 *    boundary the last committed unit published? ────────────────────────────
 *
 * The latched first-stop transcript answers "why did the walk stop". It cannot
 * answer "did the work resume correctly", because that is a property of the
 * PAIR (stop, resume) and the interesting stop is usually not the first. This
 * is the measurement that turns "benign resumable chunking" from an assumption
 * into an observation, and it is the one that must FAIL if a stop ever resumes
 * from the submission's start instead of its committed boundary -- the Case C
 * signature.
 *
 * The stream is built so a unit COMMITS before the packet cap fires, which is
 * what makes committed_get != start_get. That distinction is the whole point:
 * if nothing committed, "resumes at the committed boundary" and "restarts the
 * submission" name the same address and the test cannot discriminate. The
 * construction is therefore packets of 5 words (count 4, non-incrementing):
 * 819 of them fill a unit to 4095 words, the yield fires, that unit commits,
 * and the cap then fires in unit 2 at 1024 packets total.
 *
 * (The sibling test test_packet_cap_can_pin_get_without_a_commit records the
 * case where NO unit commits -- a packet-dense stream -- which is a genuine
 * structural property of the cap's scope, not a mistake in this one.) */
static void test_budget_stop_resumes_at_committed_boundary(void)
{
    NV2AState *d = fresh();
    Pb pb;
    uint32_t i, committed_get, start_get;
    uint32_t matched_before, mismatched_before;

    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);                 /* bind NV097 */
    for (i = 0; i < 1030u; ++i) {
        /* non-incrementing count 4 = 1 header + 4 params = 5 words */
        pb_word(&pb, 0x40000000u | (4u << 18) | 0x1760u);
        pb_word(&pb, 0x55550000u + i);
        pb_word(&pb, 0x55550001u + i);
        pb_word(&pb, 0x55550002u + i);
        pb_word(&pb, 0x55550003u + i);
    }
    cap_begin();
    kick(d, pb.start, pb.at);
    cap_end();

    start_get = pb.start;
    CHECK(strcmp(diag(d), "budget_exhausted") == 0,
          "expected budget_exhausted, got %s", diag(d));
    CHECK(d->pfifo.budget_events_total == 1,
          "budget_events_total=%u, want 1", d->pfifo.budget_events_total);

    {
        uint32_t idx = (d->pfifo.budget_event_next + NV2A_BUDGET_EVENT_MAX - 1u)
                     % NV2A_BUDGET_EVENT_MAX;
        const NV2ABudgetEvent *ev = &d->pfifo.budget_events[idx];
        CHECK(ev->seq == 1, "event seq=%u, want 1", ev->seq);
        CHECK(ev->limit_packets == 1,
              "the PACKET cap fired, so limit_packets must be 1 (got %u)",
              ev->limit_packets);
        CHECK(ev->start_get == start_get,
              "event start_get=%08X, want the walk origin %08X",
              ev->start_get, start_get);
        CHECK(ev->packets == 1024u,
              "event packets=%u, want exactly the cap (1024)", ev->packets);
        /* A unit must have committed: that is what makes the resume boundary
         * a different address from the submission origin. */
        committed_get = ev->committed_get;
        CHECK(ev->units >= 1u,
              "no unit committed before the stop (units=%u): the word yield "
              "should have fired at 819 five-word packets", ev->units);
        CHECK(committed_get != start_get,
              "nothing committed before the stop (committed_get=%08X == start=%08X): "
              "this construction cannot test the resume boundary",
              committed_get, start_get);
        CHECK(ev->local_pc != committed_get,
              "the frontier equals the committed boundary (%08X): nothing was rolled back",
              ev->local_pc);
        CHECK(ev->tail_words > 0,
              "tail_words=%u, want > 0 (a rolled-back tail is the whole point)",
              ev->tail_words);
        CHECK(d->pfifo.budget_expected_get == committed_get,
              "the outstanding stop expects a resume at %08X, want the committed "
              "boundary %08X", d->pfifo.budget_expected_get, committed_get);
        /* GET must have ADVANCED to the committed boundary, not stayed at the
         * submission origin: that is what "resumable" means. */
        CHECK(get_ptr(d) == committed_get,
              "GET is %08X but the committed boundary is %08X: the committed unit "
              "did not publish its GET", get_ptr(d), committed_get);
    }

    /* ── the resume: walk again, and check WHERE it started ──────────────
     *
     * The next walk must start at the committed boundary, because that is
     * where GET now is. The audit must therefore record a MATCH; a mismatch
     * here would mean the walk restarted the submission instead of resuming
     * its tail -- the Case C signature.
     *
     * The resume is driven with kick_put, NOT kick: `kick` writes GET as well
     * as PUT, and writing GET back to the submission origin is precisely the
     * "restart the submission" action this test is trying to detect. Using it
     * here would make the test manufacture its own mismatch (measured: it did,
     * reporting mismatched=1 at committed_get=00004FF0). The real guest
     * advances PUT and leaves GET to the model, so kick_put is the faithful
     * driver. */
    matched_before = d->pfifo.budget_resume_matched;
    mismatched_before = d->pfifo.budget_resume_mismatched;
    {
        uint32_t stalled_before = d->pfifo.budget_resume_stalled;
        uint32_t drained_before = d->pfifo.budget_resume_drained;

        cap_begin();
        kick_put(d, pb.at);
        cap_end();
        {
            uint32_t m = d->pfifo.budget_resume_matched;
            uint32_t x = d->pfifo.budget_resume_mismatched;
            CHECK(m + x == matched_before + mismatched_before + 1u,
                  "the resume was not audited at all (matched %u->%u, mismatched %u->%u)",
                  matched_before, m, mismatched_before, x);
            CHECK(m == matched_before + 1u,
                  "the resume did not begin at the committed boundary (%08X): "
                  "matched=%u mismatched=%u (a mismatch means the submission was "
                  "restarted, not resumed)", committed_get, m, x);
            CHECK(x == mismatched_before,
                  "a resume was counted as a mismatch (%u -> %u) although GET was at "
                  "the committed boundary %08X", mismatched_before, x, committed_get);
            /* The positive arm must also show that the resume was NOT a stall,
             * and that it DRAINED -- otherwise the new counters would only ever
             * be exercised in the negative direction. */
            CHECK(d->pfifo.budget_resume_stalled == stalled_before,
                  "a resume that made progress was counted as a STALL (%u -> %u)",
                  stalled_before, d->pfifo.budget_resume_stalled);
            CHECK(d->pfifo.budget_resume_drained == drained_before + 1u,
                  "a resume that reached PUT was not counted as drained "
                  "(%u -> %u)", drained_before, d->pfifo.budget_resume_drained);
        }
    }
}

/* A packet-dense stream CANNOT commit anything before the cap fires, so GET is
 * pinned and every retry repeats the same rejected walk.
 *
 * This is a real structural property of the cap's SCOPE, and it is recorded
 * rather than papered over. The word budget is per-UNIT (`unit_words` resets at
 * each unit commit) while the packet counter is WALK-CUMULATIVE (`packets` is
 * never reset). For a stream whose packets are small, the word yield
 * (`unit_words + 1 + count > NV2A_SUBMIT_MAX_WORDS`) never fires before the
 * packet cap does -- so the walk reaches the cap having committed NOTHING, and
 * `regs[NV_PFIFO_CACHE1_DMA_GET] = pc` (which is inside `if (ok)`) never runs.
 * GET therefore does not move, and `nv2a_retry_stalled_walk` re-walks the
 * identical stream to the identical stop.
 *
 * 1025 two-word packets is 2050 words -- half the word budget -- so this is not
 * a "too much work" case; it is the packet term binding at a scope the word
 * term does not share.
 *
 * WHY THIS IS A TEST AND NOT A FIX: the packet cap is deliberately a STOP
 * (compatibility ledger L40) so that 1025 one-word packets rejects rather than
 * being accepted. Changing the scope is a behaviour change with its own
 * evidence requirement. This test exists so the property is measured and
 * cannot be lost, and so that any future change to the scope has to state what
 * it did to this case. */
static void test_packet_cap_can_pin_get_without_a_commit(void)
{
    NV2AState *d = fresh();
    Pb pb;
    uint32_t i;
    uint32_t get_after_first, get_after_many;

    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);                 /* bind NV097 */
    for (i = 0; i < 1025u; ++i) {                        /* count 1 = 2 words */
        pb_word(&pb, (1u << 18) | 0x1760u);
        pb_word(&pb, 0x66660000u + i);
    }
    cap_begin();
    kick(d, pb.start, pb.at);
    cap_end();

    CHECK(strcmp(diag(d), "budget_exhausted") == 0,
          "expected budget_exhausted, got %s", diag(d));
    get_after_first = get_ptr(d);
    CHECK(get_after_first == pb.start,
          "GET moved to %08X although no unit could have committed (want the "
          "origin %08X)", get_after_first, pb.start);

    {
        uint32_t idx = (d->pfifo.budget_event_next + NV2A_BUDGET_EVENT_MAX - 1u)
                     % NV2A_BUDGET_EVENT_MAX;
        const NV2ABudgetEvent *ev = &d->pfifo.budget_events[idx];
        CHECK(ev->units == 0u,
              "units=%u, want 0: the packet cap must have fired before any unit "
              "commit for a 2-word-packet stream", ev->units);
        CHECK(ev->committed_get == ev->start_get,
              "committed_get=%08X != start_get=%08X, so a unit DID commit and "
              "this stream no longer demonstrates the zero-commit case",
              ev->committed_get, ev->start_get);
        CHECK(ev->packets == 1024u,
              "packets=%u, want 1024 (the cap)", ev->packets);
        CHECK(ev->words < NV2A_SUBMIT_MAX_WORDS,
              "words=%u reached the word budget, so this is not isolating the "
              "packet term", ev->words);
    }

    /* The retry cannot make progress: GET is pinned, so the same walk stops at
     * the same place. This is the livelock the scope asymmetry permits, and it
     * is asserted rather than described so a future change to the cap's scope
     * must consciously update it.
     *
     * AND IT IS THE CONTROL FOR THE AUDIT'S DISCRIMINATING POWER. A resume
     * audit that only compared the next walk's start GET against the previous
     * stop's committed boundary would score every one of these retries as a
     * MATCH, because GET is exactly where it was -- the default outcome. The
     * audit must instead record them as STALLS. If this ever reports matched,
     * the audit has gone vacuous again.
     *
     * ONE capture window around the whole loop: cap_begin opens a tmpfile and
     * cap_end closes it, so nesting them per iteration would leak five file
     * handles and their buffers for no benefit -- the walk's own output is what
     * matters, not which retry produced it. */
    {
        uint32_t matched_before = d->pfifo.budget_resume_matched;
        uint32_t stalled_before = d->pfifo.budget_resume_stalled;
        cap_begin();
        for (i = 0; i < 5u; ++i)
            nv2a_retry_stalled_walk(d);
        cap_end();
        CHECK(d->pfifo.budget_resume_stalled == stalled_before + 5u,
              "five retries of a zero-commit stream produced %u stall(s), want 5: "
              "a boundary-only audit would score these as matches and cannot "
              "distinguish resumption from a repeated stall",
              d->pfifo.budget_resume_stalled - stalled_before);
        CHECK(d->pfifo.budget_resume_matched == matched_before,
              "a zero-progress retry was counted as a MATCH (%u -> %u): the "
              "audit is vacuous again",
              matched_before, d->pfifo.budget_resume_matched);
        CHECK(d->pfifo.budget_resume_mismatched == 0u,
              "a zero-progress retry at the CORRECT boundary was counted as a "
              "boundary MISMATCH (%u); the two failure modes are being conflated",
              d->pfifo.budget_resume_mismatched);
    }
    get_after_many = get_ptr(d);
    CHECK(get_after_many == get_after_first,
          "GET advanced from %08X to %08X across retries of a stream that "
          "commits nothing: the zero-commit case is no longer a livelock",
          get_after_first, get_after_many);
    CHECK(d->pfifo.budget_events_total >= 6u,
          "budget_events_total=%u, want >= 6 (the first stop plus five retries "
          "that stopped again)", d->pfifo.budget_events_total);
}


/* The packet cap must be a real EXPERIMENTAL VARIABLE, and lowering it must
 * actually change the walk's behaviour.
 *
 * Why this test exists: the cap experiment ("does a smaller cap starve the
 * guest, or does continuation just chunk more?") is only evidence if the
 * override actually takes effect. An override that silently did nothing would
 * make three identical runs look like a clean invariance result and would
 * "prove" benign chunking for the wrong reason. So this asserts the negative
 * direction too: a stream that DRAINS at the default cap must STOP at a low
 * cap, and the stop must be the packet limit rather than the word limit.
 */
static void test_packet_cap_override_is_effective(void)
{
    NV2AState *d;
    Pb pb;
    uint32_t i;

    /* 100 packets of count 1 = 200 words: far below BOTH the default packet
     * cap and the word budget, so it drains cleanly. */
    nv2a_packet_cap_override(0);
    CHECK(nv2a_packet_cap() == 1024u,
          "the default packet cap is %u, want 1024", nv2a_packet_cap());

    d = fresh();
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    for (i = 0; i < 100u; ++i) {
        pb_word(&pb, (1u << 18) | 0x1760u);
        pb_word(&pb, 0x44440000u + i);
    }
    cap_begin();
    kick(d, pb.start, pb.at);
    cap_end();
    {
        uint32_t gv = get_ptr(d);
        CHECK(gv == pb.at,
              "100 packets did not drain at the default cap (get=%08X, want %08X, diag=%s)",
              gv, pb.at, diag(d));
    }
    CHECK(d->pfifo.budget_events_total == 0,
          "the default-cap walk recorded %u budget event(s), want 0",
          d->pfifo.budget_events_total);

    /* Now the same stream with a cap BELOW its packet count. This is the
     * assertion that would fail if the override were inert.
     *
     * 40 packets rather than 100: the cap under test is 32, so the stream only
     * has to exceed it, and a smaller stream keeps the capture window well
     * inside cap_end()'s static 64 KB buffer. (The transcript is a fixed
     * buffer, so a test that fills stderr past it truncates rather than
     * crashing -- but a smaller stream keeps the assertions readable.) */
    nv2a_packet_cap_override(32);
    CHECK(nv2a_packet_cap() == 32u,
          "the override did not take effect (cap=%u)", nv2a_packet_cap());

    d = fresh();
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    for (i = 0; i < 40u; ++i) {
        pb_word(&pb, (1u << 18) | 0x1760u);
        pb_word(&pb, 0x44440000u + i);
    }
    cap_begin();
    kick(d, pb.start, pb.at);
    cap_end();
    CHECK(strcmp(diag(d), "budget_exhausted") == 0,
          "a cap of 32 did not stop a 40-packet stream (diag=%s)", diag(d));
    CHECK(d->pfifo.budget_events_total == 1,
          "budget_events_total=%u, want 1 at cap 32", d->pfifo.budget_events_total);
    CHECK(g_nv2a_submit_state.budget_packets == 32u,
          "the walk consumed %u packets, want exactly the cap (32)",
          g_nv2a_submit_state.budget_packets);
    CHECK(g_nv2a_submit_state.budget_at_packet_limit == 1,
          "the PACKET cap fired, so at_packet_limit must be 1 (got %u)",
          g_nv2a_submit_state.budget_at_packet_limit);
    /* The word budget is nowhere near: this is the packet term, not the word
     * term, which is what the experiment varies. */
    CHECK(g_nv2a_submit_state.budget_words < NV2A_SUBMIT_MAX_WORDS,
          "the word budget was reached (%u) at cap 32, so this test is not "
          "isolating the packet term", g_nv2a_submit_state.budget_words);

    /* Restore the contract default for every later test. */
    nv2a_packet_cap_override(0);
    CHECK(nv2a_packet_cap() == 1024u,
          "the cap did not return to the default (got %u)", nv2a_packet_cap());
}

/* The vblank delivery audit must be capable of FAILING, i.e. it must
 * distinguish "the display stopped pulsing" from "the guest stopped
 * acknowledging". The whole point of the counters is that a run stalled on a
 * vblank-dependent wait can be classified from its dump; an audit that reports
 * the same thing in both cases would prove nothing.
 *
 * This drives the three regimes directly:
 *   1. pulse + guest W1C each frame  -> pulses == acks, already_pending == 0
 *   2. pulse, never acknowledge      -> already_pending climbs, acks stay 0
 *   3. enable cleared                -> no further line assertion, and the
 *                                       enable-cleared counter moves
 */
static void test_vblank_delivery_audit(void)
{
    NV2AState *d = fresh();
    uint32_t pulses0, acks0, stale0, cleared0, enable0;
    unsigned i;

    /* Regime 1: acknowledge every frame. */
    pcrtc_write(d, PCRTC_INTR_EN_0, NV_PCRTC_INTR_EN_0_VBLANK, 4);
    pulses0 = d->pfifo.vblank_pulses;
    acks0 = d->pfifo.vblank_guest_acks;
    stale0 = d->pfifo.vblank_already_pending;
    for (i = 0; i < 5; ++i) {
        nv2a_vblank_pulse(d);
        pcrtc_write(d, PCRTC_INTR_0, NV_PCRTC_INTR_0_VBLANK, 4);
    }
    CHECK(d->pfifo.vblank_pulses == pulses0 + 5u,
          "pulses %u, want %u", d->pfifo.vblank_pulses, pulses0 + 5u);
    CHECK(d->pfifo.vblank_guest_acks == acks0 + 5u,
          "acks %u, want %u", d->pfifo.vblank_guest_acks, acks0 + 5u);
    CHECK(d->pfifo.vblank_already_pending == stale0,
          "an acknowledged stream reported %u stale pulse(s); the audit cannot "
          "distinguish 'guest stopped acknowledging' from 'display stopped pulsing'",
          d->pfifo.vblank_already_pending - stale0);
    CHECK(d->pcrtc.pending_interrupts == 0u,
          "pending bits left %08X after acknowledgement",
          d->pcrtc.pending_interrupts);

    /* Regime 2: stop acknowledging. Every pulse AFTER the first must be counted
     * as finding the bit already set. The first is not: it is the one that SETS
     * the bit, so 4 unacknowledged pulses produce 3 stale counts -- the
     * arithmetic is asserted explicitly rather than rounded, because getting it
     * wrong by one is exactly how an audit silently misclassifies. */
    {
        uint32_t before = d->pfifo.vblank_already_pending;
        for (i = 0; i < 4; ++i)
            nv2a_vblank_pulse(d);
        CHECK(d->pfifo.vblank_already_pending == before + 3u,
              "4 unacknowledged pulses produced %u stale count(s), want 3 (the "
              "first sets the bit; only the later three find it set)",
              d->pfifo.vblank_already_pending - before);
        CHECK(d->pcrtc.pending_interrupts & NV_PCRTC_INTR_0_VBLANK,
              "the pending bit must stay set when the guest never acknowledges");
    }

    /* Regime 3: the guest clears its enable. The audit must record that the
     * enable was cleared, which is the third way delivery stops. */
    cleared0 = d->pfifo.vblank_enable_cleared;
    enable0 = d->pfifo.vblank_enable_writes;
    pcrtc_write(d, PCRTC_INTR_EN_0, 0u, 4);
    CHECK(d->pfifo.vblank_enable_writes == enable0 + 1u,
          "enable writes %u, want %u", d->pfifo.vblank_enable_writes, enable0 + 1u);
    CHECK(d->pfifo.vblank_enable_cleared == cleared0 + 1u,
          "clearing the vblank enable was not recorded (%u -> %u)",
          cleared0, d->pfifo.vblank_enable_cleared);
    CHECK(d->pfifo.vblank_enable_last == 0u,
          "enable_last=%08X, want 0", d->pfifo.vblank_enable_last);
    /* With the enable clear the PMC summary bit must drop, even though the
     * PCRTC source bit is still pending: the enable gates delivery, never the
     * source. That asymmetry is what makes the two counters independent. */
    CHECK((d->pmc.pending_interrupts & NV_PMC_INTR_0_PCRTC) == 0u,
          "PMC PCRTC bit is still set (%08X) although the guest cleared the "
          "vblank enable", d->pmc.pending_interrupts);
    CHECK(d->pcrtc.pending_interrupts & NV_PCRTC_INTR_0_VBLANK,
          "the SOURCE bit must survive a delivery-enable clear");

    /* And the last W1C value is latched, so a dump says what the guest wrote. */
    pcrtc_write(d, PCRTC_INTR_0, NV_PCRTC_INTR_0_VBLANK, 4);
    CHECK(d->pfifo.vblank_last_ack_value == NV_PCRTC_INTR_0_VBLANK,
          "last ack value %08X, want %08X", d->pfifo.vblank_last_ack_value,
          (uint32_t)NV_PCRTC_INTR_0_VBLANK);
}

/* Record what the commit consumer was handed, in order, so a test can prove
 * that a multi-unit walk delivers every method exactly once and in stream
 * order. Sized above 2 * NV2A_SUBMIT_MAX_WORDS so a two-unit stream cannot
 * overflow it. */
#define CONSUMER_MAX 8192
static uint32_t g_cons_method[CONSUMER_MAX];
static uint32_t g_cons_param[CONSUMER_MAX];
static unsigned g_cons_count;

static void consuming_observer(uint32_t subchannel, uint32_t class_id,
                               uint32_t method, uint32_t param)
{
    (void)subchannel; (void)class_id;
    if (g_cons_count < CONSUMER_MAX) {
        g_cons_method[g_cons_count] = method;
        g_cons_param[g_cons_count] = param;
    }
    ++g_cons_count;
}

static void consumer_reset(void)
{
    g_cons_count = 0;
    nv2a_set_commit_consumer(consuming_observer);
}

/* T1: a submission longer than one unit is consumed in units, in order, and
 * the walk ACCEPTS. This is the positive case for the whole change, and it is
 * the one that must fail if the yield predicate is removed.
 *
 * The stream is SET_OBJECT (2 words), then packets of 2047, 2043 and 2047
 * parameters. Unit 1 ends at packet 3's header (2 + 2048 + 2044 = 4094 words;
 * 4094 + 1 + 2047 > 4096), so unit 2 is the third packet alone.
 *
 * The third packet deliberately carries 2047 parameters rather than 5, so the
 * TWO UNITS TOGETHER stage 6138 methods -- more than sink[] holds. That is what
 * makes this test detect a missing per-unit sink_count reset: with only 4096
 * methods in total the two units would exactly fill the array and an overflow
 * would go unnoticed. */
static void test_unit_split_preserves_order(void)
{
    NV2AState *d = fresh();
    Pb pb;
    uint32_t i, expect_param = 0;
    unsigned p;

    observer_reset();
    consumer_reset();
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);                 /* bind NV097 */
    {
        static const uint32_t counts[3] = { 2047u, 2043u, 2047u };
        for (p = 0; p < 3; ++p) {
            pb_word(&pb, 0x40000000u | (counts[p] << 18) | 0x1760u);
            for (i = 0; i < counts[p]; ++i) pb_word(&pb, 0x11110000u + (expect_param++));
        }
    }
    kick(d, pb.start, pb.at);

    /* The whole stream drained: a multi-unit walk reaches PUT. */
    CHECK(get_ptr(d) == pb.at,
          "the multi-unit walk did not drain (get=%08X, want %08X; diag=%s)",
          get_ptr(d), pb.at, diag(d));
    CHECK(g_nv2a_submit_state.units == 2,
          "units=%u, want 2 (the stream is longer than one unit)",
          g_nv2a_submit_state.units);
    CHECK(g_commits == 1,
          "commits=%u, want exactly 1 (the fence is published once, at PUT)",
          g_commits);
    CHECK(g_nv2a_submit_state.budget_stops == 0,
          "budget_stops=%u, want 0 (nothing was rejected)",
          g_nv2a_submit_state.budget_stops);
    /* The sink is a per-unit record: it must hold ONE unit's worth, never the
     * sum of both. This is the assertion that catches a missing per-unit
     * reset, which would otherwise write past the end of sink[]. */
    CHECK(d->pfifo.sink_count <= NV2A_SUBMIT_MAX_WORDS,
          "sink_count=%u exceeds the sink array (%u): a unit reset is missing",
          d->pfifo.sink_count, NV2A_SUBMIT_MAX_WORDS);

    /* Every parameter, exactly once, in stream order. The SET_OBJECT handle is
     * staged too, so it is expected first. Total staged = 1 + 2047 + 2043 +
     * 2047 = 6138, which is more than sink[] holds -- that is the point. */
    CHECK(g_cons_count == 1u + 2047u + 2043u + 2047u,
          "consumer saw %u methods, want %u", g_cons_count,
          1u + 2047u + 2043u + 2047u);
    CHECK(g_cons_method[0] == 0x0000u,
          "first consumed method is %04X, want 0000 (SET_OBJECT)",
          g_cons_method[0]);
    for (i = 1; i < g_cons_count; ++i) {
        if (g_cons_method[i] != 0x1760u) {
            CHECK(0, "consumed method[%u] is %04X, want 1760 (order lost)",
                  i, g_cons_method[i]);
            break;
        }
        if (g_cons_param[i] != 0x11110000u + (i - 1u)) {
            CHECK(0, "consumed param[%u] is %08X, want %08X (order lost)",
                  i, g_cons_param[i], 0x11110000u + (i - 1u));
            break;
        }
    }
    nv2a_set_commit_consumer(NULL);
    nv2a_set_kick_observer(NULL);
}

/* T2: when a LATER unit rejects, the earlier units' methods stay committed but
 * nothing from the failing unit is delivered, GET stays at the failing unit's
 * start, and no fence is published. A retry then delivers ONLY the previously
 * failing unit -- never a second copy of unit 1.
 *
 * The stream is chosen so a yield genuinely precedes the bad packet: SET_OBJECT
 * (2 words) + a 2047-parameter packet (2048) + a 2045-parameter packet (2046)
 * reaches exactly 4096 words, and 4096 + 1 + 1 > 4096, so unit 1 ends at the
 * bad packet's header and the bad packet is unit 2's first (and only) packet. */
static void test_unit_rollback_and_retry(void)
{
    NV2AState *d = fresh();
    Pb pb;
    uint32_t i;
    unsigned after_first;

    observer_reset();
    consumer_reset();
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);                 /* bind NV097 */
    /* Unit 1: two large supported packets (4096 words exactly). */
    {
        static const uint32_t counts[2] = { 2047u, 2045u };
        uint32_t k = 0;
        for (i = 0; i < 2; ++i) {
            uint32_t j;
            pb_word(&pb, 0x40000000u | (counts[i] << 18) | 0x1760u);
            for (j = 0; j < counts[i]; ++j) pb_word(&pb, 0xAAAA0000u + (k++));
        }
    }
    /* Unit 2: one packet whose method is NOT in the table, so this unit
     * rejects. Its offset is kept so the retry can patch it. */
    {
        uint32_t bad_off = pb.at;
        pb_word(&pb, (1u << 18) | UNKNOWN_NV097);
        pb_word(&pb, 0xBBBB0000u);

        kick(d, pb.start, pb.at);

        CHECK(g_nv2a_submit_state.units == 1,
              "units=%u, want 1 (unit 1 committed, unit 2 rejected)",
              g_nv2a_submit_state.units);
        CHECK(g_commits == 0,
              "commits=%u, want 0 (a rejected later unit must not publish the fence)",
              g_commits);
        CHECK(strcmp(diag(d), "unsupported_method") == 0,
              "diag is %s, want unsupported_method", diag(d));
        /* Unit 1's methods were delivered; unit 2's were not. Unit 1 is the
         * SET_OBJECT plus both large packets' parameters. */
        CHECK(g_cons_count == 1u + 2047u + 2045u,
              "consumer saw %u methods, want %u (unit 2 must not be delivered)",
              g_cons_count, 1u + 2047u + 2045u);
        after_first = g_cons_count;

        /* GET must sit at unit 2's first header -- the rollback origin for the
         * failing unit, NOT the start of the whole submission. `bad_off` is
         * already an absolute guest address (pb.at), not an offset. */
        CHECK(get_ptr(d) == bad_off,
              "GET=%08X, want %08X (unit 2's start, not the submission start %08X)",
              get_ptr(d), bad_off, pb.start);

        /* Patch the offending method to a supported one and retry. Only unit 2
         * may be delivered now: unit 1 must not be re-sent. */
        wr32(g_window, bad_off, (1u << 18) | 0x1760u);
        kick_put(d, pb.at);

        CHECK(get_ptr(d) == pb.at,
              "the retry did not drain (get=%08X, want %08X; diag=%s)",
              get_ptr(d), pb.at, diag(d));
        CHECK(g_commits == 1,
              "commits=%u, want 1 after the retry succeeded", g_commits);
        CHECK(g_cons_count == after_first + 1u,
              "the retry delivered %u methods, want exactly 1 (unit 1 must not be redelivered)",
              g_cons_count - after_first);
    }
    nv2a_set_commit_consumer(NULL);
    nv2a_set_kick_observer(NULL);
}

/* T3: a MULTI-UNIT walk must publish each unit's admissions exactly once.
 *
 * The published `admitted_unknown` is a running total, and the commit block
 * runs once PER UNIT. A walk-cumulative counter added at each unit commit
 * therefore counts every earlier unit again: with two units admitting 4090 and
 * 6137 methods the total came out as 4090 + (4090 + 6137) = 10227 instead of
 * 6137. The count is now per unit, published and then cleared.
 *
 * The stream is deliberately built from unknown methods on every unit, so a
 * regression cannot pass by having one unit admit nothing. */
static void test_multi_unit_admission_accounting(void)
{
    NV2AState *d = fresh();
    Pb pb;
    uint32_t i, expect = 0;
    unsigned p;

    set_env("RECOMP_NV2A_ADMIT_UNKNOWN", "1");
    nv2a_admit_unknown_override(-1);
    CHECK(nv2a_admit_unknown_enabled(), "the switch did not arm");

    observer_reset();
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);                 /* bind NV097 */
    /* Two units. Unit 1: packets of 2047 and 2043 params (4094 words); unit 2:
     * one packet of 2047 params. All parameters use an unknown method, so every
     * staged method is an admission. */
    {
        static const uint32_t counts[3] = { 2047u, 2043u, 2047u };
        for (p = 0; p < 3; ++p) {
            pb_word(&pb, 0x40000000u | (counts[p] << 18) | UNKNOWN_NV097);
            for (i = 0; i < counts[p]; ++i) pb_word(&pb, 0x33330000u + (expect++));
        }
    }
    kick(d, pb.start, pb.at);

    CHECK(get_ptr(d) == pb.at,
          "the multi-unit admitted stream did not drain (get=%08X, want %08X; diag=%s)",
          get_ptr(d), pb.at, diag(d));
    CHECK(g_nv2a_submit_state.units == 2,
          "units=%u, want 2", g_nv2a_submit_state.units);
    CHECK(g_nv2a_submit_state.rejections == 0,
          "rejections=%u on a fully admitted stream",
          g_nv2a_submit_state.rejections);
    /* The exact expected total: every parameter of every unit, once. */
    CHECK(g_nv2a_submit_state.admitted_unknown == 2047u + 2043u + 2047u,
          "admitted_unknown=%u, want %u (each unit must be counted once, not re-added)",
          g_nv2a_submit_state.admitted_unknown, 2047u + 2043u + 2047u);

    set_env("RECOMP_NV2A_ADMIT_UNKNOWN", NULL);
    nv2a_admit_unknown_override(-1);
    nv2a_set_kick_observer(NULL);
}

/* ── subroutine return across unit commits ───────────────────────────── */

#define SUB_BASE 0x8000u

/* SET_OBJECT, CALL into a subroutine of two 2047-parameter packets (the second
 * overflows the first unit, so a unit commits with GET inside the subroutine),
 * then an unimplemented method, then RETURN. The main stream continues after
 * the call. `bad_at` receives the unimplemented method's address. */
static void build_long_subroutine(Pb *main_pb, uint32_t *bad_at, uint32_t *sub_end)
{
    Pb sub;
    uint32_t i, k;

    pb_begin(main_pb, PB_BASE);
    pb_method(main_pb, 0, 0x0000, H_KELVIN);
    pb_word(main_pb, SUB_BASE | 2u);                    /* CALL */
    pb_method(main_pb, 0, 0x0300, 0);                   /* after the return */

    pb_begin(&sub, SUB_BASE);
    for (k = 0; k < 2; ++k) {
        pb_word(&sub, 0x40000000u | hdr(0, 0x0300, 2047));
        for (i = 0; i < 2047; ++i) pb_word(&sub, 0);
    }
    *bad_at = sub.at;
    pb_method(&sub, 0, UNKNOWN_NV097, 0);
    pb_word(&sub, 0x00020000u);                         /* RETURN */
    *sub_end = sub.at;
}

static void test_return_survives_unit_commits(void)
{
    int via_retry;

    for (via_retry = 0; via_retry < 2; ++via_retry) {
        NV2AState *d = fresh();
        Pb pb;
        uint32_t bad_at, sub_end;
        const char *log;

        build_long_subroutine(&pb, &bad_at, &sub_end);
        cap_begin();
        kick(d, pb.start, pb.at);
        log = cap_end();
        CHECK(strcmp(diag(d), "unsupported_method") == 0, "mode %d: first walk diag %s",
              via_retry, diag(d));
        CHECK(get_ptr(d) > SUB_BASE && get_ptr(d) < sub_end,
              "mode %d: a unit did not commit inside the subroutine (get=%08X)",
              via_retry, get_ptr(d));
        CHECK(g_nv2a_submit_state.units >= 1, "mode %d: units %u", via_retry,
              g_nv2a_submit_state.units);
        (void)log;

        /* Clear the stop, then resume by a fresh PUT write or by the retry. */
        wr32(g_window, bad_at, hdr(0, 0x0300, 1));
        cap_begin();
        if (via_retry)
            CHECK(nv2a_retry_stalled_walk(d), "retry did not commit (%s)", diag(d));
        else
            kick_put(d, pb.at);
        log = cap_end();
        CHECK(strcmp(diag(d), "ok") == 0, "mode %d: resumed walk diag %s", via_retry, diag(d));
        CHECK(strstr(log, "invalid_target") == NULL,
              "mode %d: the return address was lost across the unit commit:\n%s", via_retry, log);
        CHECK(get_ptr(d) == pb.at, "mode %d: GET %08X, want PUT %08X", via_retry,
              get_ptr(d), pb.at);
        CHECK(g_nv2a_submit_state.consecutive_rejections == 0, "mode %d: consecutive %u",
              via_retry, g_nv2a_submit_state.consecutive_rejections);
    }
}

/* ── partial-commit fence ─────────────────────────────────────────────── */

#define RELEASE_METHOD 0x1D70u   /* NV097 BACK_END_WRITE_SEMAPHORE_RELEASE */

/* Unit 1: bind, optional releases, filler (4095 words with two releases; the
 * next packet cannot join it). Unit 2: a third filler packet. */
static void pb_filler(Pb *pb, uint32_t params)
{
    uint32_t i;
    pb_word(pb, 0x40000000u | hdr(0, 0x0300, params));
    for (i = 0; i < params; ++i) pb_word(pb, 0);
}

static void test_full_success_commits_without_partial(void)
{
    NV2AState *d = fresh();
    Pb pb;

    observer_reset();
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_method(&pb, 0, RELEASE_METHOD, 0x10);
    pb_method(&pb, 0, 0x1760u, 0x00002042u);
    kick(d, pb.start, pb.at);
    CHECK(get_ptr(d) == pb.at, "stream did not drain (%s)", diag(d));
    CHECK(g_commits == 1 && g_partials == 0, "commits %u partials %u, want 1 and 0",
          g_commits, g_partials);
    CHECK(g_commit_value == 0, "COMMIT carried value %08X, want 0", g_commit_value);
    CHECK(g_kick_value == 0, "KICK carried value %08X, want 0", g_kick_value);
    nv2a_set_kick_observer(NULL);
}

static void test_partial_commit_reports_the_last_release(void)
{
    NV2AState *d = fresh();
    Pb pb;
    uint32_t unit1_end;

    observer_reset();
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_method(&pb, 0, RELEASE_METHOD, 0x10);
    pb_method(&pb, 0, RELEASE_METHOD, 0x12);
    pb_filler(&pb, 2047);
    pb_filler(&pb, 2040);
    unit1_end = pb.at;
    /* Unit 2: a release the rejection must keep from being reported. */
    pb_filler(&pb, 2047);
    pb_method(&pb, 0, RELEASE_METHOD, 0x99);
    pb_method(&pb, 0, UNKNOWN_NV097, 0);
    kick(d, pb.start, pb.at);

    CHECK(strcmp(diag(d), "unsupported_method") == 0, "diag %s", diag(d));
    CHECK(get_ptr(d) == unit1_end, "GET %08X, want the unit boundary %08X", get_ptr(d), unit1_end);
    CHECK(g_nv2a_submit_state.units == 1, "units %u, want 1", g_nv2a_submit_state.units);
    CHECK(g_kicks == 1, "kicks %u", g_kicks);
    CHECK(g_commits == 0, "a stopped walk reported COMMIT (%u)", g_commits);
    CHECK(g_partials == 1, "partials %u, want 1", g_partials);
    CHECK(g_partial_value == 0x12, "PARTIAL carried %08X, want the last committed release 00000012",
          g_partial_value);
    nv2a_set_kick_observer(NULL);
}

static void test_partial_commit_without_a_release_reports_nothing(void)
{
    NV2AState *d = fresh();
    Pb pb;

    observer_reset();
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_filler(&pb, 2047);
    pb_filler(&pb, 2040);
    pb_filler(&pb, 2047);
    pb_method(&pb, 0, RELEASE_METHOD, 0x99);          /* in the uncommitted unit */
    pb_method(&pb, 0, UNKNOWN_NV097, 0);
    kick(d, pb.start, pb.at);

    CHECK(g_nv2a_submit_state.units == 1, "units %u, want 1", g_nv2a_submit_state.units);
    CHECK(g_kicks == 1 && g_commits == 0 && g_partials == 0,
          "kicks %u commits %u partials %u, want 1/0/0", g_kicks, g_commits, g_partials);

    /* A rejection that committed no unit at all reports nothing either. */
    {
        Pb p2;
        pb_begin(&p2, PB_BASE + 0x4000u);
        pb_method(&p2, 0, RELEASE_METHOD, 0x55);
        pb_method(&p2, 0, UNKNOWN_NV097, 0);
        g_kicks = g_commits = g_partials = 0;
        kick(d, p2.start, p2.at);
        CHECK(g_commits == 0 && g_partials == 0, "commits %u partials %u after a zero-unit stop",
              g_commits, g_partials);
    }
    nv2a_set_kick_observer(NULL);
}

/* ── late vblank pulse ────────────���───────────────────────────────────── */

static void test_late_vblank_wake_pulses_once(void)
{
    const uint64_t frame = 16666667ull;
    const uint64_t next = 1000000000ull;
    int pulse = 7;
    uint64_t r;

    /* Exactly at the late threshold: one pulse, re-armed one frame ahead. */
    r = nv2a_vblank_advance(next, next + 4 * frame, frame, &pulse);
    CHECK(pulse == 1, "a wake 4 frames late pulsed %d times, want 1", pulse);
    CHECK(r == next + 4 * frame + frame, "re-armed at %llu, want now+frame",
          (unsigned long long)r);

    /* Far past it: still one pulse, no burst. */
    pulse = 7;
    r = nv2a_vblank_advance(next, next + 1000 * frame, frame, &pulse);
    CHECK(pulse == 1, "a wake 1000 frames late pulsed %d times, want 1", pulse);
    CHECK(r == next + 1000 * frame + frame, "re-armed at %llu, want now+frame",
          (unsigned long long)r);

    /* The neighbouring cases keep their rule. */
    pulse = 7;
    r = nv2a_vblank_advance(next, next + 4 * frame - 1, frame, &pulse);
    CHECK(pulse == 1 && r > next + 4 * frame - 1, "just under the threshold: pulse %d", pulse);
    pulse = 7;
    r = nv2a_vblank_advance(0, next, frame, &pulse);
    CHECK(pulse == 0 && r == next + frame, "arming pass: pulse %d next %llu", pulse,
          (unsigned long long)r);
    pulse = 7;
    r = nv2a_vblank_advance(next, next - 1, frame, &pulse);
    CHECK(pulse == 0 && r == next, "early wake: pulse %d", pulse);
    pulse = 7;
    r = nv2a_vblank_advance(next, next + 4 * frame, 0, &pulse);
    CHECK(pulse == 0 && r == next, "zero frame: pulse %d", pulse);
}

/* ── PFIFO-alias PUT write ────────────────────────────────────────────── */

#define PFIFO_ALIAS_PUT (0x2000u + NV_PFIFO_CACHE1_DMA_PUT)

static void test_pfifo_alias_put_reports_kick_and_rejection(void)
{
    NV2AState *d = fresh();
    Pb pb;
    const char *log;

    observer_reset();
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_method(&pb, 0, 0x1760u, 0x00002042u);
    mmio_w(d, 0x2000u + NV_PFIFO_CACHE1_DMA_GET, pb.start);
    cap_begin();
    mmio_w(d, PFIFO_ALIAS_PUT, pb.at);
    log = cap_end();
    CHECK(get_ptr(d) == pb.at, "alias write did not drain (%s)", diag(d));
    CHECK(g_kicks == 1, "alias PUT write reported %u kicks, want 1", g_kicks);
    CHECK(g_commits == 1, "alias PUT write reported %u commits, want 1", g_commits);

    pb_method(&pb, 0, UNKNOWN_NV097, 0);
    cap_begin();
    mmio_w(d, PFIFO_ALIAS_PUT, pb.at);
    log = cap_end();
    CHECK(g_kicks == 2 && g_commits == 1, "kicks %u commits %u after a rejection", g_kicks, g_commits);
    CHECK(count_of(log, "[PFIFO] reject diag=unsupported_method ") == 1,
          "alias rejection printed %u reject lines:\n%s",
          count_of(log, "[PFIFO] reject diag=unsupported_method "), log);
    CHECK(g_nv2a_submit_state.rejections == 1, "rejections %u", g_nv2a_submit_state.rejections);
    nv2a_set_kick_observer(NULL);
}

/* ── exported state size ──────────────────────────────────────────────── */

static void test_exported_state_size(void)
{
    CHECK(g_nv2a_submit_state_size == (uint32_t)sizeof(NV2ASubmitState),
          "g_nv2a_submit_state_size %u != sizeof(NV2ASubmitState) %u",
          g_nv2a_submit_state_size, (unsigned)sizeof(NV2ASubmitState));
}

int main(void)
{
    /* Start from a known environment whatever the caller exported. */
    set_env("RECOMP_NV2A_ADMIT_UNKNOWN", NULL);
    /* The packet cap is a DIAGNOSTIC variable: a caller who exported it would
     * otherwise change what these contract tests measure, and the default is
     * the contract. */
    set_env("RECOMP_NV2A_PACKET_CAP", NULL);
    nv2a_packet_cap_override(0);

    test_rejection_logging_and_recovery();
    test_diag_change_logs_again();
    test_default_contract_forced_off();
    test_admit_unknown_on();
    test_admit_unknown_boundaries();
    test_admit_unknown_witness_is_not_truncated();
    test_override_minus_one_reads_environment();
    test_observer_counts_kicks_and_commits();
    test_retry_with_nothing_rejected();
    test_retry_recovers_after_ramht_write();
    test_retry_recovers_after_pushbuffer_patch();
    test_budget_stop_transcript();
    test_budget_stop_resumes_at_committed_boundary();
    test_packet_cap_can_pin_get_without_a_commit();
    test_packet_cap_override_is_effective();
    test_vblank_delivery_audit();
    test_unit_split_preserves_order();
    test_unit_rollback_and_retry();
    test_multi_unit_admission_accounting();
    test_return_survives_unit_commits();
    test_full_success_commits_without_partial();
    test_partial_commit_reports_the_last_release();
    test_partial_commit_without_a_release_reports_nothing();
    test_late_vblank_wake_pulses_once();
    test_pfifo_alias_put_reports_kick_and_rejection();
    test_exported_state_size();

    if (g_failures) {
        fprintf(stderr, "nv2a_submit_diag_test: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "nv2a_submit_diag_test: all checks passed\n");
    return 0;
}
