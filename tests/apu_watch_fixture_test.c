/*
 * AC-FIX fixture -- A4b1 step 8 (docs/packets/a4b1-gp-core-port.md, A4b1-r4).
 *
 * Positive control for the ported GP core's bootstrap, the ONE translation
 * function (Device semantics 3), the ONE DMA write choke point (Device
 * semantics 5), the watched-word ledger (Device semantics 6) and the trace
 * (Device semantics 7).
 *
 * It drives the PRODUCTION code and never a test-local copy of it:
 *
 *   GPRST write handler   mcpx_apu_mmio_write(d, 0x30000 + NV_PAPU_GPRST, ..)
 *                         -> mcpx_apu_dispatch_mmio -> gp_ops.write
 *                         -> proc_rst_write            (src/apu/dsp/gp_ep.c:354)
 *   frame function        mcpx_apu_dsp_frame()          (src/apu/dsp/gp_ep.c:599)
 *   DMA choke point       apu_gp_dma_write()            (src/apu/apu_watch.c:385)
 *   translation function  apu_guest_dma_ptr()           (src/apu/apu_watch.c:306)
 *   GP input hooks        read_peripheral() / dsp_read_memory() (production)
 *   memory geometry       xbox_MemoryLayoutInit() / xbox_ContiguousAlloc()
 *
 * Every case begins with apu_watch_reset() and decides on apu_watch_snapshot(),
 * so no case depends on another's order or on cumulative counts.  Cases that
 * bootstrap or run a frame first reset GP state the way the production reset
 * path does (a GPRST write with a bit clear).
 *
 * Registered TWICE in ctest (RECOMP_APU_TRACE=1 and unset); see the game's
 * CMakeLists.txt.
 *
 * Copyright (c) 2026 Burnout 3 Static Recompilation Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu.h"
#include "apu_state.h"
#include "apu_regs.h"
#include "apu_watch.h"
#include "dsp/dsp_internal.h"
#include "dsp/dsp_dma_regs.h"
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The generated code supplies these in a real build; a standalone fixture has
 * no recompiled title, so it defines the same stubs the house-style fixture
 * (tests/kernel_inplace_event_test.c) defines. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }
void recomp_diag_thread_start(uint32_t start, uint32_t low, uint32_t high)
{ (void)start; (void)low; (void)high; }
void recomp_diag_thread_end(void) {}
void recomp_diag_record(uint32_t kind, uint32_t target, uint32_t site, uint32_t value)
{ (void)kind; (void)target; (void)site; (void)value; }

/* ============================================================
 * Geometry the fixture arranges
 * ============================================================ */

/* The GP register block is reached at APU offset 0x30000..0x3FFFF; the main
 * APU register file is below 0x20000.  A4a R1 observed exactly this split:
 * GPSADDR 0x02040 (main) and GPRST 0x3FFFC (GP block).  gp_scratch_rw reads
 * d->regs[NV_PAPU_GPSADDR], i.e. the MAIN file, so the SGE base is written
 * there; GPRST goes through the GP block so proc_rst_write runs. */
#define APU_GP_BASE 0x30000u

/* Contiguous window VAs (XBOX_CONTIG_BASE = 0x80000000, size 64 MB). */
#define CONTIG_ALLOC_SIZE   0x00400000u     /* high-water mark 0x400000        */
#define PAGE_A_VA           0x80200000u     /* case (a)/(vi): window-VA form   */
#define SGE_A_VA            0x80208000u
#define PAGE_B_VA           0x80300000u     /* case (vii): physical-offset form*/
#define SGE_B_VA            0x80308000u
#define PAGE_B_OFFSET       0x00300000u     /* < high water -> DS3 case 2      */
#define PAGE_D_VA           0x803C0000u     /* case (d): & 0x03FFFFFF image    */
#define SGE_D_VA            0x803C8000u
#define PAGE_D_LOW_VA       0x003C0000u     /* PAGE_D_VA & 0x03FFFFFF          */

/* W_va = MEM32(0x001BA858) + 0x810.  Seeded so W_va lands in mapped memory. */
#define WATCH_BASE_VA       0x001BA858u
#define WATCH_WORD_OFF      0x810u
#define W_VA                0x80400000u
#define W_SEED              (W_VA - WATCH_WORD_OFF)

/* Case (i): the destination start B differs from W_va. */
#define CASE_I_B            (W_VA - WATCH_WORD_OFF)
#define CASE_I_LEN          0x820u
#define CASE_I_DSP_ADDR     0x001000u

#define UNMAPPED_VA         0x90000000u     /* above the window, below 0xFD000000 */

#define S_A                 0x00A00001u     /* anchor site */
#define S_1                 0x00B00001u     /* first ordinary site; S_1..S_N  */

#define CAPTURE_PATH        "apu_watch_fixture_stderr.txt"

/* ============================================================
 * House-style check helper
 * ============================================================ */

static unsigned checks;
static unsigned failures;

static int check(int condition, const char *name)
{
    ++checks;
    if (!condition) {
        ++failures;
        printf("FAIL: %s\n", name);
        fflush(stdout);
    }
    return condition;
}

static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* ============================================================
 * stderr capture
 *
 * The ledger's lines go to stderr.  The fixture redirects stderr to a file and
 * reads back only the byte range a single case emitted, so a case's log check
 * cannot see another case's output.
 * ============================================================ */

static char cap_buf[4u << 20];
static size_t cap_len;
static long cap_mark;

static void cap_begin(void)
{
    fflush(stderr);
    cap_mark = ftell(stderr);
}

static void cap_end(void)
{
    FILE *f;
    long end;

    fflush(stderr);
    end = ftell(stderr);
    cap_len = 0;
    cap_buf[0] = 0;
    if (end <= cap_mark) {
        return;
    }
    f = fopen(CAPTURE_PATH, "rb");
    if (!f) {
        return;
    }
    if (fseek(f, cap_mark, SEEK_SET) != 0) {
        fclose(f);
        return;
    }
    cap_len = fread(cap_buf, 1, sizeof(cap_buf) - 1, f);
    fclose(f);
    cap_buf[cap_len] = 0;
}

/* Count non-overlapping occurrences of `needle` in the captured text. */
static unsigned cap_count(const char *needle)
{
    unsigned n = 0;
    const char *p = cap_buf;
    size_t l = strlen(needle);

    while ((p = strstr(p, needle)) != NULL) {
        ++n;
        p += l;
    }
    return n;
}

/* Find the LAST occurrence of `needle`, or NULL. */
static const char *cap_find_last(const char *needle)
{
    const char *p = cap_buf, *last = NULL;
    size_t l = strlen(needle);

    while ((p = strstr(p, needle)) != NULL) {
        last = p;
        p += l;
    }
    return last;
}

/* Ask the production emission to print the ledger's current state now.
 *
 * apu_watch_gp_bootstrap_done() is the exported "at each bootstrap, after the
 * run counters reset" emission: it calls emit_counts() and
 * emit_gpin_summary() and changes NO counter (src/apu/apu_watch.c:682-689).
 * So it is the production emitter, not a test-local copy of one, and calling
 * it makes the LAST counts/[GPIN] line in a case's output describe exactly the
 * state apu_watch_snapshot() is about to report. */
static void emit_now(void)
{
    apu_watch_gp_bootstrap_done(1);
}

/* Find the `n`-th (0-based) occurrence of `needle`, or NULL. */
static const char *cap_find_n(const char *needle, unsigned n)
{
    const char *p = cap_buf;
    size_t l = strlen(needle);

    while (p && (p = strstr(p, needle)) != NULL) {
        if (n-- == 0) {
            return p;
        }
        p += l;
    }
    return NULL;
}

/* ============================================================
 * Ledger / trace line records
 * ============================================================ */

struct latch_line {
    int present;
    char cls[32];
    unsigned seq, va, observed, payload, site, frame;
    unsigned long long insns;
    unsigned dsp_addr;
};

struct counts_line {
    int present;
    unsigned seq, boots, gp_frames;
    unsigned long long gp_insns;
    unsigned gp_clear, gp_zoz, gp_zoo, gp_nz, gp_partial;
    unsigned cpu_anchor, cpu_zero, cpu_zero_overflow, cpu_other;
    unsigned gpin_oou, frame;
};

struct gpin_line {
    int present;
    unsigned mixbuf_reads[APU_WATCH_NUM_MIXBINS];
    unsigned mixbuf_stub[APU_WATCH_NUM_MIXBINS];
    unsigned periph_reads[APU_WATCH_DSP_PERIPH_SIZE];
    unsigned periph_first_value[APU_WATCH_DSP_PERIPH_SIZE];
    unsigned fifo_reads[APU_WATCH_FIFO_COUNT];
    unsigned fifo_words[APU_WATCH_FIFO_COUNT];
    unsigned long long dma_reads[APU_WATCH_DMA_CLASSES];
    unsigned long long dma_bytes[APU_WATCH_DMA_CLASSES];
    unsigned out_of_universe, mixbuf_stub_latched, boot_scratch_latched;
};

static int parse_u32(const char *p, const char *key, unsigned *out)
{
    const char *q = strstr(p, key);
    if (!q) {
        return 0;
    }
    *out = (unsigned)strtoul(q + strlen(key), NULL, 10);
    return 1;
}

static int parse_u64(const char *p, const char *key, unsigned long long *out)
{
    const char *q = strstr(p, key);
    if (!q) {
        return 0;
    }
    *out = strtoull(q + strlen(key), NULL, 10);
    return 1;
}

static int parse_hex(const char *p, const char *key, unsigned *out)
{
    const char *q = strstr(p, key);
    if (!q) {
        return 0;
    }
    *out = (unsigned)strtoul(q + strlen(key), NULL, 16);
    return 1;
}

/* `[GPWATCH] latch class=C seq=.. va=.. observed=.. payload=.. site=..
 *  frame=.. insns=.. dsp_addr=..` */
static int parse_latch(unsigned n, const char *cls, struct latch_line *out)
{
    char needle[64];
    const char *p;

    memset(out, 0, sizeof(*out));
    snprintf(needle, sizeof(needle), "[GPWATCH] latch class=%s ", cls);
    p = cap_find_n(needle, n);
    if (!p) {
        return 0;
    }
    out->present = 1;
    snprintf(out->cls, sizeof(out->cls), "%s", cls);
    parse_u32(p, "seq=", &out->seq);
    parse_hex(p, "va=", &out->va);
    parse_hex(p, "observed=", &out->observed);
    parse_hex(p, "payload=", &out->payload);
    parse_hex(p, "site=", &out->site);
    parse_u32(p, "frame=", &out->frame);
    parse_u64(p, "insns=", &out->insns);
    parse_hex(p, "dsp_addr=", &out->dsp_addr);
    return 1;
}

static int parse_counts(unsigned n, struct counts_line *out)
{
    const char *p = cap_find_n("[GPWATCH] counts ", n);

    memset(out, 0, sizeof(*out));
    if (!p) {
        return 0;
    }
    out->present = 1;
    parse_u32(p, "seq=", &out->seq);
    parse_u32(p, "boots=", &out->boots);
    parse_u32(p, "gp_frames=", &out->gp_frames);
    parse_u64(p, "gp_insns=", &out->gp_insns);
    parse_u32(p, "GP_CLEAR=", &out->gp_clear);
    parse_u32(p, "GP_ZERO_OVER_ZERO=", &out->gp_zoz);
    parse_u32(p, "GP_ZERO_OVER_OTHER=", &out->gp_zoo);
    parse_u32(p, "GP_NONZERO_OVER=", &out->gp_nz);
    parse_u32(p, "GP_PARTIAL=", &out->gp_partial);
    parse_u32(p, "CPU_ANCHOR=", &out->cpu_anchor);
    parse_u32(p, "CPU_ZERO=", &out->cpu_zero);
    parse_u32(p, "CPU_ZERO_OVERFLOW=", &out->cpu_zero_overflow);
    parse_u32(p, "CPU_OTHER=", &out->cpu_other);
    parse_u32(p, "GPIN_OUT_OF_UNIVERSE=", &out->gpin_oou);
    parse_u32(p, "frame=", &out->frame);
    return 1;
}

/* Fields of a `[GPIN] <tag>` block, searched forward from that block's own
 * first line.  The emitter puts several fields on ONE physical line:
 *
 *   [GPIN] summary mixbuf reads=[..]
 *   [GPIN] summary mixbuf reads_while_stub=[..]
 *   [GPIN] summary periph reads=[..] first_value=[..] first_seq=[..]
 *   [GPIN] summary fifo reads=[..] words=[..]
 *   [GPIN] summary dma reads=[..] bytes=[..] first_va=[..]
 *   [GPIN] summary flags out_of_universe=.. mixbuf_stub_read=.. boot_scratch_read=..
 *
 * so the fields after the first on a line are found by their `] <field>=[`
 * spelling, not by `[GPIN] <tag> <kind> <field>=[`. */
static const char *g_gpin_base = NULL;

static int parse_array_raw(const char *raw, unsigned *out, unsigned n)
{
    const char *p, *q;
    unsigned i;

    p = g_gpin_base ? strstr(g_gpin_base, raw) : NULL;
    if (!p) {
        return 0;
    }
    q = p + strlen(raw);
    for (i = 0; i < n; ++i) {
        out[i] = (unsigned)strtoul(q, (char **)&q, 10);
        if (*q == ',') {
            ++q;
        }
    }
    return 1;
}

static int parse_array_raw64(const char *raw, unsigned long long *out, unsigned n)
{
    const char *p, *q;
    unsigned i;

    p = g_gpin_base ? strstr(g_gpin_base, raw) : NULL;
    if (!p) {
        return 0;
    }
    q = p + strlen(raw);
    for (i = 0; i < n; ++i) {
        out[i] = strtoull(q, (char **)&q, 10);
        if (*q == ',') {
            ++q;
        }
    }
    return 1;
}

/* `[GPIN] <tag> <kind> <field>=[` -- a field that starts a physical line. */
static int parse_array(const char *tag, const char *kind, const char *field,
                       unsigned *out, unsigned n)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "[GPIN] %s %s %s=[", tag, kind, field);
    return parse_array_raw(needle, out, n);
}

static int parse_array64(const char *tag, const char *kind, const char *field,
                         unsigned long long *out, unsigned n)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "[GPIN] %s %s %s=[", tag, kind, field);
    return parse_array_raw64(needle, out, n);
}

/* The periph `first_value` field is printed with %06X (apu_watch.c:859), not as
 * a decimal, so it has to be read back as hex. */
static int parse_array_hex(const char *raw, unsigned *out, unsigned n)
{
    const char *p, *q;
    unsigned i;

    p = g_gpin_base ? strstr(g_gpin_base, raw) : NULL;
    if (!p) {
        return 0;
    }
    q = p + strlen(raw);
    for (i = 0; i < n; ++i) {
        out[i] = (unsigned)strtoul(q, (char **)&q, 16);
        if (*q == ',') {
            ++q;
        }
    }
    return 1;
}

static int parse_field_hex(const char *field, unsigned *out, unsigned n)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "] %s=[", field);
    return parse_array_hex(needle, out, n);
}

/* `] <field>=[` -- a field that shares a line with the one before it. */
static int parse_field(const char *field, unsigned *out, unsigned n)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "] %s=[", field);
    return parse_array_raw(needle, out, n);
}

static int parse_field64(const char *field, unsigned long long *out, unsigned n)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "] %s=[", field);
    return parse_array_raw64(needle, out, n);
}

/* The LAST `[GPIN] <tag>` block in the captured text.  The emission cadence is
 * fixed, so a case produces several; the case calls emit_now() immediately
 * before apu_watch_snapshot(), so the last one describes the snapshot. */
static int parse_gpin(const char *tag, struct gpin_line *out)
{
    char needle[64];
    const char *p;

    memset(out, 0, sizeof(*out));
    snprintf(needle, sizeof(needle), "[GPIN] %s mixbuf reads=[", tag);
    p = cap_find_last(needle);
    if (!p) {
        g_gpin_base = NULL;
        return 0;
    }
    g_gpin_base = p;
    out->present = 1;
    parse_array(tag, "mixbuf", "reads", out->mixbuf_reads, APU_WATCH_NUM_MIXBINS);
    /* `reads_while_stub` shares the `mixbuf reads=[..]` line (apu_watch.c:847),
     * so it is found by its `] reads_while_stub=[` spelling. */
    parse_field("reads_while_stub", out->mixbuf_stub, APU_WATCH_NUM_MIXBINS);
    parse_array(tag, "periph", "reads", out->periph_reads, APU_WATCH_DSP_PERIPH_SIZE);
    parse_field_hex("first_value", out->periph_first_value, APU_WATCH_DSP_PERIPH_SIZE);
    parse_array(tag, "fifo", "reads", out->fifo_reads, APU_WATCH_FIFO_COUNT);
    parse_field("words", out->fifo_words, APU_WATCH_FIFO_COUNT);
    parse_array64(tag, "dma", "reads", out->dma_reads, APU_WATCH_DMA_CLASSES);
    parse_field64("bytes", out->dma_bytes, APU_WATCH_DMA_CLASSES);
    {
        char fneedle[64];
        const char *p;
        snprintf(fneedle, sizeof(fneedle), "[GPIN] %s flags ", tag);
        p = g_gpin_base ? strstr(g_gpin_base, fneedle) : NULL;
        if (p) {
            parse_u32(p, "out_of_universe=", &out->out_of_universe);
            parse_u32(p, "mixbuf_stub_read=", &out->mixbuf_stub_latched);
            parse_u32(p, "boot_scratch_read=", &out->boot_scratch_latched);
        }
    }
    return 1;
}

/* ============================================================
 * Line-vs-snapshot rule
 *
 * For every latch the snapshot holds, the case's output contains EXACTLY ONE
 * `[GPWATCH] latch class=<C>` line, and its fields equal the snapshot's.
 * No latch line appears for a latch the snapshot does not hold.
 *
 * The rule applies to the TRACE-ON registration only.  The ledger and the
 * [GPIN] arrays are always computed; only their emission is trace-gated, so
 * every snapshot/PRAM/memory check runs in both arms while the line checks run
 * only where there are lines.
 * ============================================================ */

static int g_trace_on;

static const char *const class_names[APU_WATCH_LATCH_CLASSES] = {
    "GP_CLEAR", "GP_ZERO_OVER_ZERO", "GP_ZERO_OVER_OTHER", "GP_NONZERO_OVER",
    "GP_PARTIAL", "CPU_ANCHOR", "CPU_ZERO", "CPU_ZERO_OVERFLOW",
    "MIXBUF_STUB_READ", "BOOT_SCRATCH_READ", "GPIN_OUT_OF_UNIVERSE",
};

static void check_line_vs_snapshot(const struct apu_watch_snapshot *s,
                                   const char *what)
{
    unsigned c;
    char name[160];

    if (!g_trace_on) {
        return;      /* no emission: the snapshot checks are the whole check */
    }

    for (c = 0; c < APU_WATCH_LATCH_CLASSES; ++c) {
        char needle[64];
        unsigned seen;
        const struct apu_watch_latch *l = &s->latch[c];

        snprintf(needle, sizeof(needle), "[GPWATCH] latch class=%s ",
                 class_names[c]);
        seen = cap_count(needle);

        /* CPU_ZERO is latched per site, so its line count is the number of
         * used sites with a fired latch, not 0/1. */
        if (c == APU_WATCH_CPU_ZERO) {
            unsigned expect = 0, i;
            for (i = 0; i < APU_WATCH_N_SITES; ++i) {
                if (s->sites[i].used && s->sites[i].zero.latched) {
                    ++expect;
                }
            }
            snprintf(name, sizeof(name), "%s: CPU_ZERO latch lines == fired site latches (%u)", what, expect);
            check(seen == expect, name);
            continue;
        }

        if (!l->latched) {
            snprintf(name, sizeof(name), "%s: no %s latch line for an unlatched %s", what, class_names[c], class_names[c]);
            check(seen == 0, name);
            continue;
        }

        {
            struct latch_line ll;
            if (!parse_latch(0, class_names[c], &ll)) {
                snprintf(name, sizeof(name), "%s: %s latch line present", what, class_names[c]);
                check(0, name);
                continue;
            }
            snprintf(name, sizeof(name), "%s: exactly one %s latch line", what, class_names[c]);
            check(seen == 1, name);
            snprintf(name, sizeof(name), "%s: %s latch seq", what, class_names[c]);
            check(ll.seq == l->seq, name);
            snprintf(name, sizeof(name), "%s: %s latch va", what, class_names[c]);
            check(ll.va == l->va, name);
            snprintf(name, sizeof(name), "%s: %s latch observed", what, class_names[c]);
            check(ll.observed == l->observed, name);
            snprintf(name, sizeof(name), "%s: %s latch payload", what, class_names[c]);
            check(ll.payload == l->payload, name);
            snprintf(name, sizeof(name), "%s: %s latch site", what, class_names[c]);
            check(ll.site == l->site, name);
            snprintf(name, sizeof(name), "%s: %s latch frame", what, class_names[c]);
            check(ll.frame == l->frame, name);
            snprintf(name, sizeof(name), "%s: %s latch insns", what, class_names[c]);
            check(ll.insns == l->insns, name);
            snprintf(name, sizeof(name), "%s: %s latch dsp_addr", what, class_names[c]);
            check(ll.dsp_addr == (l->dsp_addr & 0xFFFFFFu), name);
        }
    }

    /* CPU_ZERO site latches, one line each, field by field. */
    {
        unsigned i, n = 0;
        for (i = 0; i < APU_WATCH_N_SITES; ++i) {
            const struct apu_watch_site *st = &s->sites[i];
            struct latch_line ll;
            char nm[160];
            if (!st->used || !st->zero.latched) {
                continue;
            }
            if (!parse_latch(n, "CPU_ZERO", &ll)) {
                snprintf(nm, sizeof(nm), "%s: CPU_ZERO site latch line %u present", what, n);
                check(0, nm);
                ++n;
                continue;
            }
            snprintf(nm, sizeof(nm), "%s: CPU_ZERO site[%u] site field", what, i);
            check(ll.site == st->site_va, nm);
            snprintf(nm, sizeof(nm), "%s: CPU_ZERO site[%u] va field", what, i);
            check(ll.va == st->zero.va, nm);
            snprintf(nm, sizeof(nm), "%s: CPU_ZERO site[%u] payload field", what, i);
            check(ll.payload == st->zero.payload, nm);
            snprintf(nm, sizeof(nm), "%s: CPU_ZERO site[%u] seq field", what, i);
            check(ll.seq == st->zero.seq, nm);
            ++n;
        }
    }
}

/* Each counts line's boots/gp_frames/gp_insns/seq must equal the snapshot.
 *
 * The emission cadence is fixed (bootstrap, first GP frame, every latch, every
 * 256th se_frame), so a case that latches several times produces several counts
 * lines and the earlier ones legitimately describe earlier states.  The case
 * calls emit_now() immediately before apu_watch_snapshot(), so the LAST counts
 * line is the one that must equal the snapshot field for field. */
static void check_counts_lines(const struct apu_watch_snapshot *s,
                               const char *what)
{
    unsigned i, n = 0;
    struct counts_line cl;
    char name[160];

    if (!g_trace_on) {
        return;
    }

    while (parse_counts(n, &cl)) {
        ++n;
    }
    snprintf(name, sizeof(name), "%s: at least one counts line", what);
    check(n != 0, name);
    if (n == 0) {
        return;
    }
    if (!parse_counts(n - 1, &cl)) {
        return;
    }

    snprintf(name, sizeof(name), "%s: counts boots == snapshot", what);
    check(cl.boots == s->boots, name);
    snprintf(name, sizeof(name), "%s: counts gp_frames == snapshot", what);
    check(cl.gp_frames == s->gp_frames, name);
    snprintf(name, sizeof(name), "%s: counts gp_insns == snapshot", what);
    check(cl.gp_insns == s->gp_insns, name);
    snprintf(name, sizeof(name), "%s: counts seq == snapshot seq", what);
    check(cl.seq == s->seq, name);
    snprintf(name, sizeof(name), "%s: counts GP_CLEAR == snapshot", what);
    check(cl.gp_clear == s->counts[APU_WATCH_C_GP_CLEAR], name);
    snprintf(name, sizeof(name), "%s: counts GP_ZERO_OVER_ZERO == snapshot", what);
    check(cl.gp_zoz == s->counts[APU_WATCH_C_GP_ZERO_OVER_ZERO], name);
    snprintf(name, sizeof(name), "%s: counts GP_ZERO_OVER_OTHER == snapshot", what);
    check(cl.gp_zoo == s->counts[APU_WATCH_C_GP_ZERO_OVER_OTHER], name);
    snprintf(name, sizeof(name), "%s: counts GP_PARTIAL == snapshot", what);
    check(cl.gp_partial == s->counts[APU_WATCH_C_GP_PARTIAL], name);
    snprintf(name, sizeof(name), "%s: counts CPU_ANCHOR == snapshot", what);
    check(cl.cpu_anchor == s->counts[APU_WATCH_C_CPU_ANCHOR], name);
    snprintf(name, sizeof(name), "%s: counts CPU_ZERO == snapshot", what);
    check(cl.cpu_zero == s->counts[APU_WATCH_C_CPU_ZERO], name);
    snprintf(name, sizeof(name), "%s: counts CPU_ZERO_OVERFLOW == snapshot", what);
    check(cl.cpu_zero_overflow == s->counts[APU_WATCH_C_CPU_ZERO_OVERFLOW], name);
    snprintf(name, sizeof(name), "%s: counts CPU_OTHER == snapshot", what);
    check(cl.cpu_other == s->counts[APU_WATCH_C_CPU_OTHER], name);
    snprintf(name, sizeof(name), "%s: counts GPIN_OUT_OF_UNIVERSE == snapshot", what);
    check(cl.gpin_oou == s->counts[APU_WATCH_C_GPIN_OUT_OF_UNIVERSE], name);
}

/* Compare two arrays and report the FIRST differing element, so a mismatch
 * names the index and both values rather than just failing. */
static void cmp_u32(const unsigned *got, const unsigned *want, unsigned n,
                    const char *what, const char *field)
{
    unsigned i;
    char name[200];

    for (i = 0; i < n; ++i) {
        if (got[i] != want[i]) {
            snprintf(name, sizeof(name), "%s: [GPIN] summary %s[%u] == snapshot (got %u, want %u)",
                     what, field, i, got[i], want[i]);
            check(0, name);
            return;
        }
    }
    snprintf(name, sizeof(name), "%s: [GPIN] summary %s == snapshot", what, field);
    check(1, name);
}

static void cmp_u64(const unsigned long long *got, const unsigned long long *want,
                    unsigned n, const char *what, const char *field)
{
    unsigned i;
    char name[200];

    for (i = 0; i < n; ++i) {
        if (got[i] != want[i]) {
            snprintf(name, sizeof(name), "%s: [GPIN] summary %s[%u] == snapshot (got %llu, want %llu)",
                     what, field, i, got[i], want[i]);
            check(0, name);
            return;
        }
    }
    snprintf(name, sizeof(name), "%s: [GPIN] summary %s == snapshot", what, field);
    check(1, name);
}

/* Each [GPIN] summary line's array elements must equal the snapshot's arrays. */
static void check_gpin_summary(const struct apu_watch_snapshot *s,
                               const char *what)
{
    struct gpin_line g;
    char name[160];
    unsigned i;
    unsigned mr[APU_WATCH_NUM_MIXBINS];
    unsigned ms[APU_WATCH_NUM_MIXBINS];
    unsigned pr[APU_WATCH_DSP_PERIPH_SIZE];
    unsigned fr[APU_WATCH_FIFO_COUNT];
    unsigned long long dr[APU_WATCH_DMA_CLASSES];
    unsigned long long db[APU_WATCH_DMA_CLASSES];

    if (!g_trace_on) {
        return;
    }

    if (!parse_gpin("summary", &g)) {
        snprintf(name, sizeof(name), "%s: a [GPIN] summary line is present", what);
        check(0, name);
        return;
    }

    /* Dump the block once per case that fails, so a mismatch is diagnosable
     * from the test output alone. */
    if (getenv("APU_WATCH_FIXTURE_DUMP") && g_gpin_base) {
        const char *e = strstr(g_gpin_base, "[GPIN] summary flags ");
        note("  [gpin %s]\n%.*s\n", what,
             e ? (int)(e - g_gpin_base + 200) : 600, g_gpin_base);
    }

    for (i = 0; i < APU_WATCH_NUM_MIXBINS; ++i) {
        mr[i] = s->gpin.mixbuf[i].reads;
        ms[i] = s->gpin.mixbuf[i].reads_while_stub;
    }
    cmp_u32(g.mixbuf_reads, mr, APU_WATCH_NUM_MIXBINS, what, "mixbuf reads");
    cmp_u32(g.mixbuf_stub, ms, APU_WATCH_NUM_MIXBINS, what, "mixbuf reads_while_stub");

    for (i = 0; i < APU_WATCH_DSP_PERIPH_SIZE; ++i) {
        pr[i] = s->gpin.periph[i].reads;
    }
    cmp_u32(g.periph_reads, pr, APU_WATCH_DSP_PERIPH_SIZE, what, "periph reads");
    for (i = 0; i < APU_WATCH_DSP_PERIPH_SIZE; ++i) {
        pr[i] = s->gpin.periph[i].first_value & 0xFFFFFFu;
    }
    cmp_u32(g.periph_first_value, pr, APU_WATCH_DSP_PERIPH_SIZE, what, "periph first_value");

    for (i = 0; i < APU_WATCH_FIFO_COUNT; ++i) {
        fr[i] = s->gpin.fifo[i].reads;
    }
    cmp_u32(g.fifo_reads, fr, APU_WATCH_FIFO_COUNT, what, "fifo reads");
    for (i = 0; i < APU_WATCH_FIFO_COUNT; ++i) {
        fr[i] = s->gpin.fifo[i].words;
    }
    cmp_u32(g.fifo_words, fr, APU_WATCH_FIFO_COUNT, what, "fifo words");

    for (i = 0; i < APU_WATCH_DMA_CLASSES; ++i) {
        dr[i] = s->gpin.dma[i].reads;
        db[i] = s->gpin.dma[i].bytes;
    }
    cmp_u64(g.dma_reads, dr, APU_WATCH_DMA_CLASSES, what, "dma reads");
    cmp_u64(g.dma_bytes, db, APU_WATCH_DMA_CLASSES, what, "dma bytes");

    snprintf(name, sizeof(name), "%s: [GPIN] summary out_of_universe == snapshot", what);
    check(g.out_of_universe == s->gpin.out_of_universe, name);

    /* FIELD-BY-FIELD, against the slot the emitter actually prints.
     *
     * `[GPIN] summary flags ... mixbuf_stub_read=%u boot_scratch_read=%u` prints
     * g->mixbuf_stub_read.latched and g->boot_scratch_read.latched
     * (apu_watch.c:907-910) -- the `gpin` slots -- while the recording
     * functions fire the CLASS latches s.latch[APU_WATCH_*] and the `[GPWATCH]
     * latch` lines are emitted from those.  The printed flag was therefore
     * permanently 0 while the class latch had fired, and (vi) failed on it.
     *
     * Production now MIRRORS each class latch into its printed slot at the
     * moment it fires (apu_watch.c:566 and :641), under the same write-once
     * guard, so the at_clear copy stays consistent with it.  A4b1-r4 Ruling 1
     * (4) makes the line-vs-snapshot rule cover BOTH slots: the printed flag
     * must equal the class latch, not merely the gpin slot.  Checking only the
     * gpin slot would let the next unwritten slot hide exactly the way this one
     * did -- both sides would read 0 and agree. */
    snprintf(name, sizeof(name), "%s: [GPIN] summary mixbuf_stub_read == snapshot gpin slot", what);
    check(g.mixbuf_stub_latched == s->gpin.mixbuf_stub_read.latched, name);
    snprintf(name, sizeof(name), "%s: [GPIN] summary boot_scratch_read == snapshot gpin slot", what);
    check(g.boot_scratch_latched == s->gpin.boot_scratch_read.latched, name);

    /* The load-bearing half: the PRINTED flag against the CLASS latch. */
    snprintf(name, sizeof(name),
             "%s: [GPIN] summary mixbuf_stub_read == MIXBUF_STUB_READ class latch "
             "(printed %u, latch %u)", what, g.mixbuf_stub_latched,
             s->latch[APU_WATCH_MIXBUF_STUB_READ].latched);
    check(g.mixbuf_stub_latched == s->latch[APU_WATCH_MIXBUF_STUB_READ].latched, name);
    snprintf(name, sizeof(name),
             "%s: [GPIN] summary boot_scratch_read == BOOT_SCRATCH_READ class latch "
             "(printed %u, latch %u)", what, g.boot_scratch_latched,
             s->latch[APU_WATCH_BOOT_SCRATCH_READ].latched);
    check(g.boot_scratch_latched == s->latch[APU_WATCH_BOOT_SCRATCH_READ].latched, name);
}

/* ============================================================
 * Fixture state
 * ============================================================ */

static MCPXAPUState *d;
static uint8_t *ram;


static volatile uint32_t *ram32(uint32_t va)
{
    return (volatile uint32_t *)(ram + va);
}

/* Production GP MMIO write: APU offset 0x30000 + <gp offset>. */
static void gp_write(uint32_t off, uint32_t val)
{
    mcpx_apu_mmio_write(d, APU_GP_BASE + (uint64_t)off, val, 4);
}

/* Production main-file APU write (GPSADDR/GPSMAXSGE live here; gp_scratch_rw
 * reads d->regs[], and the guest writes them at 0x02040/0x020D4). */
static void apu_write(uint32_t off, uint32_t val)
{
    mcpx_apu_mmio_write(d, (uint64_t)off, val, 4);
}

/* The production reset path: GPRST with a bit clear -> dsp_reset. */
static void gp_state_reset(void)
{
    gp_write(NV_PAPU_GPRST, 0);
}

/* Seed the SGE table so the bootstrap reads `pages` consecutive 4 KB pages.
 * `entry0` is what the SGE entry holds: a window VA (the 0d7929c form) or a
 * physical offset (the M form). */
static void program_sge(uint32_t sge_va, uint32_t entry0, unsigned pages)
{
    unsigned i;
    for (i = 0; i < pages; ++i) {
        *ram32(sge_va + i * 8u) = entry0 + i * 0x1000u;
    }
    apu_write(NV_PAPU_GPSADDR, sge_va);
    apu_write(NV_PAPU_GPSMAXSGE, pages - 1u);
}

/* A 24-bit pattern word.  The high byte is deliberately set, so the pinned
 * bootstrap's `pram[i] &= 0x00FFFFFF` is exercised. */
static uint32_t pattern_word(unsigned i)
{
    uint32_t v = (i * 2654435761u) & 0xFFFFFFu;
    return 0xA5000000u | v;
}

static uint32_t pattern_value(unsigned i)
{
    return pattern_word(i) & 0xFFFFFFu;
}

/* Fill `bytes` at `page_va` with the pattern. */
static void fill_pattern(uint32_t page_va, unsigned bytes)
{
    unsigned i;
    for (i = 0; i < bytes / 4u; ++i) {
        *ram32(page_va + i * 4u) = pattern_word(i);
    }
}

/* Read PRAM through the pinned API. */
static void read_pram(uint32_t *out, unsigned words)
{
    unsigned i;
    for (i = 0; i < words; ++i) {
        out[i] = dsp_read_memory(d->gp.dsp, 'P', i);
    }
}

static int pram_matches_pattern(const uint32_t *pram, unsigned words)
{
    unsigned i;
    for (i = 0; i < words; ++i) {
        if (pram[i] != pattern_value(i)) {
            return 0;
        }
    }
    return 1;
}

static int pram_all_zero(const uint32_t *pram, unsigned words)
{
    unsigned i;
    for (i = 0; i < words; ++i) {
        if (pram[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* A GP DMA write through the ONE translation function and the ONE choke
 * point, with a caller-chosen destination start, length and DSP address. */
static uint8_t xfer_src[0x2000];
static uint8_t xfer_dst_shadow[0x2000];

static void gp_dma_write_at(uint32_t guest_va, size_t len, uint32_t dsp_addr,
                            uint32_t fill)
{
    uint32_t tva = 0;
    uint8_t *host;
    size_t i;

    for (i = 0; i < len; ++i) {
        xfer_src[i] = (uint8_t)fill;
    }
    host = apu_guest_dma_ptr(guest_va, (uint32_t)len, &tva);
    if (!host) {
        check(0, "gp_dma_write_at: destination translated");
        return;
    }
    apu_gp_dma_write(host, xfer_src, tva, len, dsp_addr);
}

/* Case (e)/(i)/(ii)/(iv): a zero write covering the aligned dword W_va. */
static void gp_zero_write_covering_w(void)
{
    gp_dma_write_at(W_VA, 4, CASE_I_DSP_ADDR, 0x00);
}

/* ============================================================
 * Cases
 * ============================================================ */

/* (a) After 1->3, PRAM matches the pattern for i < 0x800. */
static void case_a(void)
{
    static uint32_t pram[0x800];
    const char *what = "(a) bootstrap loads PRAM";

    apu_watch_reset();
    gp_state_reset();
    fill_pattern(PAGE_A_VA, 0x2000);
    program_sge(SGE_A_VA, PAGE_A_VA, 2);
    cap_begin();

    gp_write(NV_PAPU_GPRST, 1);
    gp_write(NV_PAPU_GPRST, 3);

    read_pram(pram, 0x800);
    cap_end();

    check(pram_matches_pattern(pram, 0x800), what);
}

/* (b) Known-bad: after 0->1 alone, PRAM is unchanged. */
static void case_b(void)
{
    static uint32_t before[0x800], after[0x800];
    const char *what = "(b) 0->1 alone does not bootstrap";

    apu_watch_reset();
    gp_state_reset();
    fill_pattern(PAGE_A_VA, 0x2000);
    program_sge(SGE_A_VA, PAGE_A_VA, 2);
    /* Bootstrap once so PRAM holds the pattern, then reset GP state and write
     * only 0->1: PRAM must not change. */
    gp_write(NV_PAPU_GPRST, 3);
    read_pram(before, 0x800);

    apu_watch_reset();
    gp_state_reset();
    cap_begin();
    gp_write(NV_PAPU_GPRST, 1);
    read_pram(after, 0x800);
    cap_end();

    check(memcmp(before, after, sizeof(before)) == 0, what);
}

/* (c) Known-bad: an SGE entry at an unmapped VA produces [GPDMA] unmapped,
 * leaves PRAM unchanged, and does not crash. */
static void case_c(void)
{
    static uint32_t before[0x800], after[0x800];
    const char *what = "(c) unmapped SGE entry fails closed";

    apu_watch_reset();
    gp_state_reset();
    fill_pattern(PAGE_A_VA, 0x2000);
    program_sge(SGE_A_VA, PAGE_A_VA, 2);
    gp_write(NV_PAPU_GPRST, 3);
    read_pram(before, 0x800);

    apu_watch_reset();
    gp_state_reset();
    /* GPSADDR itself unmapped: the SGE descriptor fetch is case 4. */
    apu_write(NV_PAPU_GPSADDR, UNMAPPED_VA);
    apu_write(NV_PAPU_GPSMAXSGE, 1);
    cap_begin();
    gp_write(NV_PAPU_GPRST, 1);
    gp_write(NV_PAPU_GPRST, 3);
    read_pram(after, 0x800);
    cap_end();

    check(cap_count("[GPDMA] unmapped ") >= 1, what);
    check(memcmp(before, after, sizeof(before)) == 0, "(c) PRAM unchanged");
}

/* (d) Known-bad: a pattern placed only at addr & 0x03FFFFFF, in mapped and
 * seeded low RAM, is not loaded. */
static void case_d(void)
{
    static uint32_t pram[0x800];
    unsigned i;
    const char *what = "(d) & 0x03FFFFFF image is not what the bootstrap loads";

    apu_watch_reset();
    gp_state_reset();

    /* The window page: pattern P. */
    fill_pattern(PAGE_D_VA, 0x2000);
    /* The 26-bit-masked low-RAM image: a DIFFERENT pattern Q, so loading it
     * would be visible. */
    for (i = 0; i < 0x2000 / 4u; ++i) {
        *ram32(PAGE_D_LOW_VA + i * 4u) = (pattern_word(i) ^ 0x00F0F0F0u);
    }

    program_sge(SGE_D_VA, PAGE_D_VA, 2);
    cap_begin();
    gp_write(NV_PAPU_GPRST, 1);
    gp_write(NV_PAPU_GPRST, 3);
    read_pram(pram, 0x800);
    cap_end();

    check(pram_matches_pattern(pram, 0x800), what);
    check(!pram_all_zero(pram, 0x800), "(d) PRAM is not silently zeroed");
}

/* (e) A zero write over 3 gives GP_CLEAR=1 with observed=3; after a reset the
 * same write over 0 gives GP_ZERO_OVER_ZERO=1 and GP_CLEAR=0; final memory is
 * identical in both cases. */
static void case_e(void)
{
    struct apu_watch_snapshot s1, s2;
    const char *what = "(e) GP_CLEAR vs GP_ZERO_OVER_ZERO";

    /* Arm 1: W_va holds 3. */
    apu_watch_reset();
    gp_state_reset();
    *ram32(WATCH_BASE_VA) = W_SEED;
    *ram32(W_VA) = 3;
    cap_begin();
    gp_zero_write_covering_w();
    apu_watch_snapshot(&s1);
    cap_end();

    check(s1.counts[APU_WATCH_C_GP_CLEAR] == 1, what);
    check(s1.latch[APU_WATCH_GP_CLEAR].latched == 1, "(e) GP_CLEAR latched");
    check(s1.latch[APU_WATCH_GP_CLEAR].observed == 3, "(e) GP_CLEAR observed=3");
    check(*ram32(W_VA) == 0, "(e) arm 1 memory ends at 0");
    check_line_vs_snapshot(&s1, "(e) arm 1");
    check_counts_lines(&s1, "(e) arm 1");

    /* Arm 2: W_va holds 0. */
    apu_watch_reset();
    gp_state_reset();
    *ram32(WATCH_BASE_VA) = W_SEED;
    *ram32(W_VA) = 0;
    cap_begin();
    gp_zero_write_covering_w();
    apu_watch_snapshot(&s2);
    cap_end();

    check(s2.counts[APU_WATCH_C_GP_ZERO_OVER_ZERO] == 1, "(e) GP_ZERO_OVER_ZERO=1");
    check(s2.counts[APU_WATCH_C_GP_CLEAR] == 0, "(e) GP_CLEAR=0");
    check(s2.latch[APU_WATCH_GP_ZERO_OVER_ZERO].observed == 0, "(e) ZOZ observed=0");
    check(*ram32(W_VA) == 0, "(e) arm 2 memory ends at 0");
    check_line_vs_snapshot(&s2, "(e) arm 2");
    check_counts_lines(&s2, "(e) arm 2");
}

/* (i) The destination start differs from W_va. */
static void case_i(void)
{
    struct apu_watch_snapshot s;
    unsigned n;
    const char *what = "(i) destination start != W_va";

    apu_watch_reset();
    gp_state_reset();
    *ram32(WATCH_BASE_VA) = W_SEED;
    *ram32(W_VA) = 0;

    cap_begin();
    /* 20+ zero writes over 0 -> GP_ZERO_OVER_ZERO. */
    for (n = 0; n < 24; ++n) {
        gp_zero_write_covering_w();
    }
    /* The guest stores 3 through the anchor site, then stores it. */
    apu_watch_set_anchor_site(S_A);
    apu_watch_cpu_store(S_A, W_VA, 3);
    *ram32(W_VA) = 3;
    /* One GP zero write whose destination starts at B = W_va - 0x810 and whose
     * length exceeds 0x814. */
    gp_dma_write_at(CASE_I_B, CASE_I_LEN, CASE_I_DSP_ADDR, 0x00);
    emit_now();    apu_watch_snapshot(&s);    cap_end();

    check(s.counts[APU_WATCH_C_GP_ZERO_OVER_ZERO] >= 20, "(i) GP_ZERO_OVER_ZERO >= 20");
    check(s.counts[APU_WATCH_C_GP_CLEAR] == 1, "(i) GP_CLEAR=1");
    check(s.latch[APU_WATCH_GP_CLEAR].observed == 3, "(i) GP_CLEAR observed=3");
    check(s.latch[APU_WATCH_GP_CLEAR].va == W_VA, "(i) GP_CLEAR va == W_va (not B)");
    check(s.latch[APU_WATCH_GP_CLEAR].payload == 0, "(i) GP_CLEAR payload=0");
    check(s.latch[APU_WATCH_GP_CLEAR].site == 0, "(i) GP_CLEAR site=0");
    check(s.latch[APU_WATCH_GP_CLEAR].insns == s.gp_insns, "(i) GP_CLEAR insns == gp_insns");
    check(s.latch[APU_WATCH_GP_CLEAR].dsp_addr == CASE_I_DSP_ADDR, "(i) GP_CLEAR dsp_addr");
    check(s.counts[APU_WATCH_C_CPU_ANCHOR] == 1, "(i) CPU_ANCHOR counted");
    check(s.latch[APU_WATCH_CPU_ANCHOR].va == W_VA, "(i) CPU_ANCHOR va == W_va");
    check(s.latch[APU_WATCH_CPU_ANCHOR].payload == 3, "(i) CPU_ANCHOR payload=3");
    check(s.latch[APU_WATCH_CPU_ANCHOR].site == S_A, "(i) CPU_ANCHOR site == S_A");
    check(s.latch[APU_WATCH_CPU_ANCHOR].seq < s.latch[APU_WATCH_GP_CLEAR].seq,
          "(i) CPU_ANCHOR.seq < GP_CLEAR.seq");
    if (g_trace_on) {
        check(cap_count("[GPWATCH] latch class=GP_CLEAR ") == 1,
              "(i) exactly one GP_CLEAR line");
    }
    check_line_vs_snapshot(&s, "(i)");
    check_counts_lines(&s, "(i)");

    /* A further zero write leaves GP_CLEAR=1 and its latch unchanged, and emits
     * no second GP_CLEAR line.  W_va now holds 0 (the exchange above landed the
     * zero), so the further write is a GP_ZERO_OVER_ZERO -- the same write the
     * GP makes, over the state the GP left. */
    {
        struct apu_watch_snapshot s2;
        unsigned seq_before = s.seq;
        cap_begin();
        gp_zero_write_covering_w();
        apu_watch_snapshot(&s2);
        cap_end();
        check(s2.counts[APU_WATCH_C_GP_CLEAR] == 1, "(i) further write leaves GP_CLEAR=1");
        check(s2.counts[APU_WATCH_C_GP_ZERO_OVER_ZERO] >= 21,
              "(i) further write is a GP_ZERO_OVER_ZERO");
        check(s2.latch[APU_WATCH_GP_CLEAR].seq == s.latch[APU_WATCH_GP_CLEAR].seq,
              "(i) further write leaves the GP_CLEAR latch unchanged");
        check(cap_count("[GPWATCH] latch class=GP_CLEAR ") == 0,
              "(i) further write emits no second GP_CLEAR line");
        check(seq_before != 0, "(i) the ledger advanced before the further write");
        /* No line-vs-snapshot check on this sub-capture: s2 still holds the
         * latches the earlier part of case (i) fired, and their lines are in
         * that part's output.  What this sub-capture must show is the absence
         * of a SECOND GP_CLEAR line, checked above. */
        check(cap_count("[GPWATCH] latch ") == 0,
              "(i) further write fires no new latch of any class");
    }
    (void)what;
}

/* (ii) A zero write over 7 gives GP_ZERO_OVER_OTHER latched with observed=7,
 * and no GP_CLEAR. */
static void case_ii(void)
{
    struct apu_watch_snapshot s;

    apu_watch_reset();
    gp_state_reset();
    *ram32(WATCH_BASE_VA) = W_SEED;
    *ram32(W_VA) = 7;
    cap_begin();
    gp_zero_write_covering_w();
    emit_now();    apu_watch_snapshot(&s);    cap_end();

    check(s.counts[APU_WATCH_C_GP_ZERO_OVER_OTHER] == 1, "(ii) GP_ZERO_OVER_OTHER=1");
    check(s.latch[APU_WATCH_GP_ZERO_OVER_OTHER].latched == 1, "(ii) latched");
    check(s.latch[APU_WATCH_GP_ZERO_OVER_OTHER].observed == 7, "(ii) observed=7");
    check(s.counts[APU_WATCH_C_GP_CLEAR] == 0, "(ii) no GP_CLEAR");
    check_line_vs_snapshot(&s, "(ii)");
    check_counts_lines(&s, "(ii)");
}

/* (iii) A 2-byte write at W_va+2 gives GP_PARTIAL latched, and neither
 * GP_CLEAR nor any GP_ZERO_*. */
static void case_iii(void)
{
    struct apu_watch_snapshot s;

    apu_watch_reset();
    gp_state_reset();
    *ram32(WATCH_BASE_VA) = W_SEED;
    *ram32(W_VA) = 0x11223344u;
    cap_begin();
    gp_dma_write_at(W_VA + 2u, 2, CASE_I_DSP_ADDR, 0xAA);
    emit_now();    apu_watch_snapshot(&s);    cap_end();

    check(s.counts[APU_WATCH_C_GP_PARTIAL] == 1, "(iii) GP_PARTIAL=1");
    check(s.latch[APU_WATCH_GP_PARTIAL].latched == 1, "(iii) latched");
    check(s.latch[APU_WATCH_GP_PARTIAL].observed == 0x11223344u,
          "(iii) observed is the dword before the write");
    check(s.counts[APU_WATCH_C_GP_CLEAR] == 0, "(iii) no GP_CLEAR");
    check(s.counts[APU_WATCH_C_GP_ZERO_OVER_ZERO] == 0, "(iii) no GP_ZERO_OVER_ZERO");
    check(s.counts[APU_WATCH_C_GP_ZERO_OVER_OTHER] == 0, "(iii) no GP_ZERO_OVER_OTHER");
    check_line_vs_snapshot(&s, "(iii)");
    check_counts_lines(&s, "(iii)");
}

/* (iv) With the word at 3, a CPU zero store from S_1 then a GP zero write. */
static void case_iv(void)
{
    struct apu_watch_snapshot s;

    apu_watch_reset();
    gp_state_reset();
    *ram32(WATCH_BASE_VA) = W_SEED;
    *ram32(W_VA) = 3;

    cap_begin();
    apu_watch_cpu_store(S_1, W_VA, 0);
    *ram32(W_VA) = 0;
    gp_zero_write_covering_w();
    emit_now();    apu_watch_snapshot(&s);    cap_end();

    check(s.counts[APU_WATCH_C_CPU_ZERO] == 1, "(iv) CPU_ZERO counted");
    check(s.sites[0].used == 1, "(iv) a site slot was taken");
    check(s.sites[0].site_va == S_1, "(iv) the slot is S_1");
    check(s.sites[0].zero.latched == 1, "(iv) CPU_ZERO latched for S_1");
    check(s.sites[0].zero.site == S_1, "(iv) CPU_ZERO site = S_1");
    check(s.sites[0].zero.va == W_VA, "(iv) CPU_ZERO va = W_va");
    check(s.sites[0].zero.payload == 0, "(iv) CPU_ZERO payload=0");
    check(s.latch[APU_WATCH_GP_ZERO_OVER_ZERO].latched == 1,
          "(iv) GP_ZERO_OVER_ZERO latched");
    check(s.latch[APU_WATCH_GP_ZERO_OVER_ZERO].observed == 0,
          "(iv) GP_ZERO_OVER_ZERO observed=0");
    check(s.counts[APU_WATCH_C_GP_CLEAR] == 0, "(iv) GP_CLEAR=0");
    check_line_vs_snapshot(&s, "(iv)");
    check_counts_lines(&s, "(iv)");
}

/* (v) Zero stores from S_1..S_N with N > N_SITES. */
static void case_v(void)
{
    struct apu_watch_snapshot s;
    unsigned n;
    const unsigned N = APU_WATCH_N_SITES + 4u;

    apu_watch_reset();
    gp_state_reset();
    *ram32(WATCH_BASE_VA) = W_SEED;

    cap_begin();
    for (n = 0; n < N; ++n) {
        apu_watch_cpu_store(S_1 + n, W_VA, 0);
    }
    emit_now();    apu_watch_snapshot(&s);    cap_end();

    check(s.counts[APU_WATCH_C_CPU_ZERO] == N, "(v) CPU_ZERO == N");
    check(s.counts[APU_WATCH_C_CPU_ZERO_OVERFLOW] >= 1, "(v) CPU_ZERO_OVERFLOW >= 1");
    check(s.latch[APU_WATCH_CPU_ZERO_OVERFLOW].latched == 1, "(v) overflow latch fired");
    check(s.latch[APU_WATCH_CPU_ZERO_OVERFLOW].site == S_1 + APU_WATCH_N_SITES,
          "(v) overflow latch site = S_{N_SITES+1}");

    {
        unsigned fired = 0, i;
        for (i = 0; i < APU_WATCH_N_SITES; ++i) {
            if (s.sites[i].used && s.sites[i].zero.latched) {
                ++fired;
                check(s.sites[i].site_va == S_1 + i, "(v) per-site latch order");
            }
        }
        check(fired == APU_WATCH_N_SITES, "(v) exactly N_SITES per-site latches");
    }

    if (g_trace_on) {
        check(cap_count("[GPWATCH] latch class=CPU_ZERO ") == APU_WATCH_N_SITES,
              "(v) one CPU_ZERO line per fired site latch");
        check(cap_count("[GPWATCH] latch class=CPU_ZERO_OVERFLOW ") == 1,
              "(v) one CPU_ZERO_OVERFLOW line");
    }
    check_line_vs_snapshot(&s, "(v)");
    check_counts_lines(&s, "(v)");
    /* A call for another target changes no counter and emits no line. */
    {
        struct apu_watch_snapshot s2;
        cap_begin();
        apu_watch_cpu_store(S_1, W_VA + 4u, 0);
        apu_watch_snapshot(&s2);
        cap_end();
        check(s2.counts[APU_WATCH_C_CPU_ZERO] == s.counts[APU_WATCH_C_CPU_ZERO],
              "(v) another target changes no counter");
        check(s2.counts[APU_WATCH_C_CPU_OTHER] == 0, "(v) another target is not CPU_OTHER");
        check(cap_count("[GPWATCH] latch ") == 0, "(v) another target emits no line");
    }
}

/* The bootstrap's scratch read is the scatter-gather transfer of the page the
 * SGE describes, and it happens inside dsp_bootstrap regardless of the program
 * image.  Case (vi) needs a program that RUNS, though, so the image is a
 * repeating `nop` (dsp_cpu.c:314, 2 cycles each): one 1000-cycle chunk then
 * retires 500 instructions and the core's own counter is non-zero. */
static void load_nop_program(uint32_t page_va)
{
    unsigned i;
    for (i = 0; i < 0x2000 / 4u; ++i) {
        *ram32(page_va + i * 4u) = 0;
    }
}

/* (vi) After a bootstrap, one call to the production frame function. */
static void case_vi(void)
{
    struct apu_watch_snapshot s;
    struct gpin_line g;
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    unsigned i;
    uint32_t core_cycles;

    apu_watch_reset();
    gp_state_reset();
    d->vp.vp_active_voices = 0;

    load_nop_program(PAGE_A_VA);
    program_sge(SGE_A_VA, PAGE_A_VA, 2);

    memset(mixbins, 0, sizeof(mixbins));

    cap_begin();
    gp_write(NV_PAPU_GPRST, 1);
    gp_write(NV_PAPU_GPRST, 3);
    mcpx_apu_dsp_frame(d, mixbins);
    core_cycles = dsp_get_cycle_count(d->gp.dsp);
    emit_now();    apu_watch_snapshot(&s);    cap_end();

    check(s.boots == 1, "(vi) boots=1");
    check(s.gp_frames == 1, "(vi) gp_frames=1");
    check(s.gp_insns == (unsigned long long)core_cycles,
          "(vi) gp_insns == the core's own counter");
    check(s.gp_insns > 0, "(vi) the core retired instructions");
    check(!g_trace_on || cap_count("[GPRUN] ") >= 1, "(vi) a [GPRUN] line is present");

    {
        struct counts_line cl;
        int found = 0;
        for (i = 0; g_trace_on; ++i) {
            if (!parse_counts(i, &cl)) {
                break;
            }
            if (cl.boots == 1 && cl.gp_frames == 1) {
                check(cl.gp_insns == s.gp_insns, "(vi) counts gp_insns == snapshot");
                check(cl.seq <= s.seq, "(vi) counts seq <= snapshot seq");
                found = 1;
            }
        }
        check(!g_trace_on || found, "(vi) a counts line with boots=1 gp_frames=1");
    }

    {
        const char *p = g_trace_on ? cap_find_n("[GPRUN] ", 0) : NULL;
        check(!g_trace_on || p != NULL, "(vi) [GPRUN] present");
        if (p) {
            check(strstr(p, "frame=") != NULL, "(vi) [GPRUN] frame=");
            check(strstr(p, "se_frame_after_boot=") != NULL, "(vi) [GPRUN] se_frame_after_boot=");
            check(strstr(p, "cycles=") != NULL, "(vi) [GPRUN] cycles=");
            check(strstr(p, "insns=") != NULL, "(vi) [GPRUN] insns=");
            check(strstr(p, "pc=") != NULL, "(vi) [GPRUN] pc=");
            check(strstr(p, "halt=") != NULL, "(vi) [GPRUN] halt=");
            check(strstr(p, "tone=") != NULL, "(vi) [GPRUN] tone=");
        }
    }

    if (!g_trace_on) {
        check(1, "(vi) [GPIN] summary checks are trace-on only");
    } else if (parse_gpin("summary", &g)) {
        check(g.dma_reads[APU_WATCH_DMA_CONTIG] != 0,
              "(vi) [GPIN] summary DMA_READ CONTIG non-zero");
        check(g.dma_reads[APU_WATCH_DMA_CONTIG] == s.gpin.dma[APU_WATCH_DMA_CONTIG].reads,
              "(vi) [GPIN] summary DMA_READ equals the snapshot");
        /* A4b1-r4 Ruling 1 (4), the line-vs-snapshot addition: (vi) asserts the
         * PRINTED boot_scratch_read equals the BOOT_SCRATCH_READ CLASS latch.
         * Both are asserted separately -- the printed flag must be 1 (the
         * witness is visible, which is the packet's (vi) requirement) AND it
         * must equal the class latch (which is the rule that stops the next
         * unwritten slot from hiding the way this one did).  Before the mirror
         * landed in apu_watch.c:566 the first held only if the slot was filled,
         * and the slot was never filled: the printed flag was 0 while the latch
         * had fired. */
        check(g.boot_scratch_latched == 1,
              "(vi) [GPIN] summary prints boot_scratch_read=1 (the witness is visible)");
        check(g.boot_scratch_latched == s.latch[APU_WATCH_BOOT_SCRATCH_READ].latched,
              "(vi) [GPIN] summary boot_scratch_read == the BOOT_SCRATCH_READ class latch");
        check(g.boot_scratch_latched == s.gpin.boot_scratch_read.latched,
              "(vi) [GPIN] summary boot_scratch_read == the snapshot's gpin slot");
    } else {
        check(0, "(vi) [GPIN] summary line present");
    }
    /* The latch the implementation fires is the CLASS latch,
     * s.latch[APU_WATCH_BOOT_SCRATCH_READ] (apu_watch.c:557), and that is the
     * one whose `[GPWATCH] latch class=BOOT_SCRATCH_READ` line is emitted and
     * whose fields apu_watch_snapshot() reports.  apu_watch.c:566 now mirrors it
     * into the printed gpin slot under the same write-once guard. */
    check(s.latch[APU_WATCH_BOOT_SCRATCH_READ].latched == 1,
          "(vi) BOOT_SCRATCH_READ latched (the reset cleared the arrays)");
    check(s.latch[APU_WATCH_BOOT_SCRATCH_READ].va == PAGE_A_VA,
          "(vi) BOOT_SCRATCH_READ va = the scratch page");
    check(s.gpin.boot_scratch_read.latched == 1,
          "(vi) the gpin slot mirrors the class latch");
    check(s.gpin.boot_scratch_read.va == s.latch[APU_WATCH_BOOT_SCRATCH_READ].va,
          "(vi) the mirrored slot carries the class latch's va");
    check(s.counts[APU_WATCH_C_BOOT_SCRATCH_READ] >= 1,
          "(vi) BOOT_SCRATCH_READ counted");
    if (s.latch[APU_WATCH_BOOT_SCRATCH_READ].latched != 1) {
        note("  [diag] class latch latched=%u va=%08X | gpin slot latched=%u | "
             "dma contig reads=%llu bytes=%llu | boots=%u gp_frames=%u\n",
             s.latch[APU_WATCH_BOOT_SCRATCH_READ].latched,
             s.latch[APU_WATCH_BOOT_SCRATCH_READ].va,
             s.gpin.boot_scratch_read.latched,
             s.gpin.dma[APU_WATCH_DMA_CONTIG].reads,
             s.gpin.dma[APU_WATCH_DMA_CONTIG].bytes, s.boots, s.gp_frames);
    }
    check_counts_lines(&s, "(vi)");
}

/* (vii) The 1->3 write emits exactly one [GPBOOT] header plus 64 pram lines;
 * the 0->1 write emits none.  Two placements. */
static void case_vii_placement(const char *label, uint32_t page_va,
                               uint32_t sge_va, uint32_t entry_value,
                               uint32_t expect_sge0_va)
{
    static uint32_t pram[0x800];
    const char *p;
    unsigned i, pram_lines = 0;
    char name[160];

    apu_watch_reset();
    gp_state_reset();
    fill_pattern(page_va, 0x2000);
    program_sge(sge_va, entry_value, 2);

    cap_begin();
    gp_write(NV_PAPU_GPRST, 1);
    gp_write(NV_PAPU_GPRST, 3);
    read_pram(pram, 0x800);
    cap_end();

    snprintf(name, sizeof(name), "(vii) %s: exactly one [GPBOOT] header", label);
    check(!g_trace_on || cap_count("[GPBOOT] n=") == 1, name);
    snprintf(name, sizeof(name), "(vii) %s: the 0->1 write emits no [GPBOOT]", label);
    check(!g_trace_on || cap_count("[GPBOOT] ") == 65, name);

    p = g_trace_on ? cap_find_n("[GPBOOT] n=", 0) : NULL;
    if (p) {
        unsigned gprst = 0, prev = 0, sge0 = 0, sge0_va = 0;
        parse_hex(p, "gprst=", &gprst);
        parse_hex(p, "prev=", &prev);
        parse_hex(p, "sge0=", &sge0);
        parse_hex(p, "sge0_va=", &sge0_va);
        snprintf(name, sizeof(name), "(vii) %s: gprst=00000003", label);
        check(gprst == 3u, name);
        snprintf(name, sizeof(name), "(vii) %s: prev=00000001", label);
        check(prev == 1u, name);
        snprintf(name, sizeof(name), "(vii) %s: sge0 is the raw entry value", label);
        check(sge0 == entry_value, name);
        snprintf(name, sizeof(name), "(vii) %s: sge0_va is the scratch page VA", label);
        check(sge0_va == expect_sge0_va, name);
    } else {
        check(!g_trace_on, "(vii) [GPBOOT] header present");
    }

    /* 64 pram lines, each matching (a). */
    for (i = 0; i < 64; ++i) {
        char needle[64];
        const char *q;
        unsigned idx = 0;
        unsigned w[8];
        unsigned k;
        snprintf(needle, sizeof(needle), "[GPBOOT] pram %03X:", i * 8u);
        q = strstr(cap_buf, needle);
        if (!q) {
            continue;
        }
        ++pram_lines;
        q += strlen(needle);
        for (k = 0; k < 8; ++k) {
            w[k] = (unsigned)strtoul(q, (char **)&q, 16);
        }
        for (k = 0; k < 8; ++k) {
            if (w[k] != pattern_value(i * 8u + k)) {
                snprintf(name, sizeof(name),
                         "(vii) %s: pram line %u word %u matches the pattern",
                         label, i, k);
                check(0, name);
            }
        }
    }
    snprintf(name, sizeof(name), "(vii) %s: 64 pram lines", label);
    check(!g_trace_on || pram_lines == 64, name);

    /* The whole-image check through the pinned API, for this placement. */
    snprintf(name, sizeof(name), "(vii) %s: PRAM matches the pattern", label);
    check(pram_matches_pattern(pram, 0x800), name);
}

static void case_vii(void)
{
    /* Placement 1: the entry holds the WINDOW VA (the 0d7929c form). */
    case_vii_placement("window VA", PAGE_A_VA, SGE_A_VA, PAGE_A_VA, PAGE_A_VA);
    /* Placement 2: the entry holds the PHYSICAL OFFSET (the M form), so the
     * inverse must run case 2 and land on the same page VA. */
    case_vii_placement("physical offset", PAGE_B_VA, SGE_B_VA, PAGE_B_OFFSET,
                       PAGE_B_VA);
}

/* ============================================================
 * The pinned DMA engine, driven from the fixture
 *
 * A4b1-r4 Ruling 1 (C)(d) requires AC-FIX (viii)'s FIFO_READ case to drive the
 * pinned DMA engine through production code with a READ-direction descriptor,
 * buf_id = 0 and a known count.  Both halves are production:
 *
 *   registers  write_peripheral(dsp, 0xFFFFD4/0xFFFFD6, ..) is the pinned
 *              peripheral-write path (dsp.c:107-118 -> dsp_dma_write), i.e.
 *              exactly what a DSP `movep` to those addresses performs;
 *   descriptor the 7-word block dsp_dma_run() reads out of DSP X memory
 *              (dsp_dma.c:137-144).
 * ============================================================ */

#define DMA_DESC_ADDR   0x0800u  /* X memory; < 0xC00, so plain XRAM       */
#define DMA_XFER_ADDR   0x0900u  /* the descriptor's dsp_offset            */
#define DMA_XFER_COUNT  0x40u    /* words; format 3 -> 4 bytes per word    */

/* One READ-direction transfer on `dsp`'s OWN DMA.
 *
 * Control word, field by field (dsp_dma.c:152-160):
 *   bit  0     dsp_interleave = 0
 *   bit  1     NODE_CONTROL_DIRECTION = 0  -> the READ arm
 *   bits 2-3   unk2 = 0
 *   bit  4     buffer_offset_writeback = 0
 *   bits 5-8   buf_id = the argument
 *   bits 10-12 format = 3 (32-bit) -> item_size 4, so transfer_size = count*4
 *   bit 13     unk13 = 0
 * Descriptor word 0 is the NEXT pointer and carries NODE_POINTER_EOL, so
 * dsp_dma_run's `while (!(s->next_block & NODE_POINTER_EOL))` performs exactly
 * ONE transfer and then stops. */
static void dma_read_transfer(DSPState *dsp, unsigned buf_id, unsigned count)
{
    uint32_t control = ((uint32_t)buf_id << 5) | (3u << 10);
    unsigned i;

    /* Deterministic start: STOPPED and not RUNNING.  dsp_reset does not touch
     * dsp->dma, so a previous case could have left the engine running. */
    write_peripheral(dsp, 0xFFFFD6u, DMA_CONTROL_ACTION_STOP);

    dsp_write_memory(dsp, 'X', DMA_DESC_ADDR + 0u,
                     DMA_DESC_ADDR | NODE_POINTER_EOL);
    dsp_write_memory(dsp, 'X', DMA_DESC_ADDR + 1u, control);
    dsp_write_memory(dsp, 'X', DMA_DESC_ADDR + 2u, count);
    dsp_write_memory(dsp, 'X', DMA_DESC_ADDR + 3u, DMA_XFER_ADDR);
    dsp_write_memory(dsp, 'X', DMA_DESC_ADDR + 4u, 0u);   /* scratch_offset */
    dsp_write_memory(dsp, 'X', DMA_DESC_ADDR + 5u, 0u);   /* scratch_base   */
    dsp_write_memory(dsp, 'X', DMA_DESC_ADDR + 6u, 0u);   /* scratch_size-1 */

    /* Distinct, non-zero source words, so the transfer is a real read of known
     * content rather than a read of zeroed memory. */
    for (i = 0; i < count; ++i) {
        dsp_write_memory(dsp, 'X', DMA_XFER_ADDR + i, 0x00A50000u | i);
    }

    write_peripheral(dsp, 0xFFFFD4u, DMA_DESC_ADDR);      /* DMA_NEXT_BLOCK */
    write_peripheral(dsp, 0xFFFFD6u, DMA_CONTROL_ACTION_START);
}

/* (viii) Every [GPIN] kind through its production hook. */
static void case_viii(void)
{
    struct apu_watch_snapshot s;
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    const char *what = "(viii)";

    /* --- one frame with the VP's active-voice count 0 --- */
    apu_watch_reset();
    gp_state_reset();
    d->vp.vp_active_voices = 0;
    memset(mixbins, 0, sizeof(mixbins));

    cap_begin();
    /* DMA_READ: the bootstrap's own scatter-gather read (production). */
    load_nop_program(PAGE_A_VA);
    program_sge(SGE_A_VA, PAGE_A_VA, 2);
    gp_write(NV_PAPU_GPRST, 1);
    gp_write(NV_PAPU_GPRST, 3);
    /* MIXBUF provenance: the pinned frame path sets the per-frame stub flag. */
    mcpx_apu_dsp_frame(d, mixbins);
    /* MIXBUF read: the production dsp_read_memory -> dsp56k_read_memory hook. */
    (void)dsp_read_memory(d->gp.dsp, 'X', GP_DSP_MIXBUF_BASE + 3u * NUM_SAMPLES_PER_FRAME);
    /* PERIPH read: the production read_peripheral.  The recorded first_value
     * must be the value the same call returned -- the offset is indexed as
     * address - DSP_PERIPH_BASE, so 0xFFFFC5 is index 0x45. */
    {
        uint32_t v = read_peripheral(d->gp.dsp, 0xFFFFC5u);
        emit_now();        apu_watch_snapshot(&s);        cap_end();
        check(s.gpin.periph[0x45].reads == 1, "(viii) PERIPH recorded under offset 0x45");
        check(s.gpin.periph[0x45].first_value == v,
              "(viii) PERIPH first_value == the value the hook returned");
        check(v == 2u, "(viii) PERIPH 0xFFFFC5 reads the start-frame interrupt bit");
    }

    check(s.gpin.dma[APU_WATCH_DMA_CONTIG].reads != 0, "(viii) DMA_READ recorded");
    check(s.gpin.dma[APU_WATCH_DMA_CONTIG].bytes != 0, "(viii) DMA_READ bytes recorded");
    check(s.gpin.mixbuf[3].reads == 1, "(viii) MIXBUF recorded under bin 3");
    check(s.gpin.mixbuf[3].reads_while_stub == 0,
          "(viii) MIXBUF reads_while_stub == 0 with vp_active_voices == 0");
    check(s.gpin.out_of_universe == 0, "(viii) GPIN_OUT_OF_UNIVERSE = 0");
    check_gpin_summary(&s, "(viii) stub=0");
    check_counts_lines(&s, "(viii) stub=0");

    /* --- after another reset, one frame with the VP's active-voice count > 0 --- */
    apu_watch_reset();
    gp_state_reset();
    d->vp.vp_active_voices = 7;
    memset(mixbins, 0, sizeof(mixbins));

    cap_begin();
    mcpx_apu_dsp_frame(d, mixbins);
    (void)dsp_read_memory(d->gp.dsp, 'X', GP_DSP_MIXBUF_BASE + 5u * NUM_SAMPLES_PER_FRAME);
    emit_now();    apu_watch_snapshot(&s);    cap_end();

    check(s.gpin.mixbuf[5].reads == 1, "(viii) MIXBUF recorded under bin 5");
    check(s.gpin.mixbuf[5].reads_while_stub == 1,
          "(viii) MIXBUF reads_while_stub != 0 with vp_active_voices > 0");
    /* The implementation fires the CLASS latch (apu_watch.c:627), which is the
     * one apu_watch_snapshot() reports and the one whose line is emitted. */
    check(s.latch[APU_WATCH_MIXBUF_STUB_READ].latched == 1,
          "(viii) MIXBUF_STUB_READ latched");
    check(s.latch[APU_WATCH_MIXBUF_STUB_READ].observed == 7,
          "(viii) MIXBUF_STUB_READ observed = vp_active_voices");
    check(s.counts[APU_WATCH_C_MIXBUF_STUB_READ] == 1,
          "(viii) MIXBUF_STUB_READ counted");
    /* A4b1-r4 Ruling 1 (4): the printed mixbuf_stub_read must equal the
     * MIXBUF_STUB_READ CLASS latch.  This is the second of the two mirrored
     * slots; check_gpin_summary below carries the same assertion for the
     * generic rule, and it is repeated here so the case that fires the latch
     * states it directly. */
    check(s.gpin.mixbuf_stub_read.latched ==
              s.latch[APU_WATCH_MIXBUF_STUB_READ].latched,
          "(viii) the gpin slot mirrors the MIXBUF_STUB_READ class latch");
    check(s.gpin.mixbuf_stub_read.observed == 7,
          "(viii) the mirrored slot carries observed = vp_active_voices");
    check(s.gpin.out_of_universe == 0, "(viii) GPIN_OUT_OF_UNIVERSE = 0 (stub>0 arm)");
    check_gpin_summary(&s, "(viii) stub>0");
    check_line_vs_snapshot(&s, "(viii) stub>0");

    /* --- FIFO_READ through the pinned DMA engine ---
     *
     * A4b1-r4 Ruling 1 (C): the earlier revision of this fixture asserted that
     * no production path reaches the FIFO_READ hook, because the read arm
     * (direction bit clear) implements only buf_id 0xE/0xF and the ONLY caller
     * of fifo_rw is the write arm, which passes the literal direction 1
     * (dsp_dma.c:273).  That assertion was RIGHT TO FAIL: it found a real
     * hook-completeness gap.  The read arm's `else` branch prints
     * "Unhandled DSP DMA buffer: 0x%x" and then FALLS THROUGH -- its
     * `assert(!"Unhandled dsp dma buffer")` is elided because the APU library
     * is built with NDEBUG -- and the loop after it mem_writes the file-static
     * intermediate `scratch_buf` into DSP memory.  A GP program that requests a
     * FIFO read therefore DOES consume an input, and it is stale content.
     *
     * The gap is now repaired in production (dsp_dma.c's read arm, the marked
     * A4b1 block after the `assert`): for `s->is_gp` and buf_id <
     * GP_INPUT_FIFO_COUNT it calls apu_gpin_fifo_read(buf_id, transfer_size);
     * any other buf_id calls apu_gpin_record_out_of_universe().  So this case
     * now DRIVES the path and asserts the recorded counts, which is what the
     * ruling requires, rather than asserting the path is unreachable. */
    apu_watch_reset();
    gp_state_reset();
    cap_begin();
    dma_read_transfer(d->gp.dsp, 0u, DMA_XFER_COUNT);
    emit_now();    apu_watch_snapshot(&s);    cap_end();

    check(s.gpin.fifo[0].reads == 1, "(viii) FIFO_READ: fifo[0].reads == 1");
    check(s.gpin.fifo[0].words == DMA_XFER_COUNT,
          "(viii) FIFO_READ: fifo[0].words == the transfer's word count");
    check(s.gpin.out_of_universe == 0,
          "(viii) FIFO_READ: out_of_universe == 0 (buf_id 0 is in universe)");
    check(!g_trace_on || cap_count("Unhandled DSP DMA buffer: 0x0") == 1,
          "(viii) FIFO_READ: the \"Unhandled DSP DMA buffer: 0x0\" line is present");
    /* The write arm's hook must NOT also fire: the read arm never calls
     * fifo_rw, so the two cannot both record one transfer. */
    {
        unsigned i, nonzero = 0;
        for (i = 0; i < APU_WATCH_FIFO_COUNT; ++i) {
            if (s.gpin.fifo[i].reads != 0) {
                ++nonzero;
            }
        }
        check(nonzero == 1, "(viii) FIFO_READ: exactly one fifo slot recorded");
    }
    /* (c) The reserved output slots are never written.  Slots
     * GP_INPUT_FIFO_COUNT..APU_WATCH_FIFO_COUNT-1 (2..5) are the output FIFOs'
     * places in the declared 6-element universe; they are GP-PRODUCED, not
     * inputs.  The ruling makes this a guard against a future hook recording
     * output traffic here. */
    {
        unsigned i, bad = 0;
        char nm[160];
        for (i = GP_INPUT_FIFO_COUNT; i < APU_WATCH_FIFO_COUNT; ++i) {
            if (s.gpin.fifo[i].reads != 0 || s.gpin.fifo[i].words != 0) {
                snprintf(nm, sizeof(nm),
                         "(viii) reserved output slot fifo[%u] is 0 "
                         "(got reads=%u words=%u)", i, s.gpin.fifo[i].reads,
                         s.gpin.fifo[i].words);
                check(0, nm);
                ++bad;
            }
        }
        check(bad == 0,
              "(viii) reserved output slots 2..5 are never written");
    }
    check_gpin_summary(&s, "(viii) fifo");
    check_line_vs_snapshot(&s, "(viii) fifo");

    /* --- the EP twin (can-fail): the SAME transfer on the EP's DMA ---
     *
     * dsp_dma.c is SHARED by the GP and the EP (gp_ep.c:599 and :649 both run
     * frames), and mem_opaque/rw_opaque do not identify the side.  The hook is
     * therefore gated on s->is_gp, copied from DSPState.is_gp in dsp_init.  An
     * ungated hook would record this EP fall-through as a GP input -- a false
     * R2-EXPL-INPUT in the opposite direction from the gap it closes.  This
     * twin is what proves the gate works: every gpin.fifo count must be 0. */
    apu_watch_reset();
    gp_state_reset();
    cap_begin();
    dma_read_transfer(d->ep.dsp, 0u, DMA_XFER_COUNT);
    emit_now();    apu_watch_snapshot(&s);    cap_end();

    {
        unsigned i, total = 0;
        for (i = 0; i < APU_WATCH_FIFO_COUNT; ++i) {
            total += s.gpin.fifo[i].reads;
        }
        check(total == 0,
              "(viii) EP twin: the EP's DMA leaves every gpin.fifo count at 0 "
              "(the is_gp gate)");
    }
    check(s.gpin.out_of_universe == 0,
          "(viii) EP twin: no out-of-universe record for an EP fall-through");
    check(!g_trace_on || cap_count("Unhandled DSP DMA buffer: 0x0") == 1,
          "(viii) EP twin: the EP fall-through still prints its line");
    (void)what;
}

/* (ix') Volume: no overflow by construction. */
static void case_ix(void)
{
    struct apu_watch_snapshot s;
    const unsigned FRAMES = 300;
    unsigned f, i;
    char name[160];

    apu_watch_reset();
    gp_state_reset();
    d->vp.vp_active_voices = 0;

    cap_begin();
    /* A full 1024-word mix-buffer sweep for each of FRAMES frames. */
    for (f = 0; f < FRAMES; ++f) {
        for (i = 0; i < DSP_MIXBUFFER_SIZE; ++i) {
            (void)dsp_read_memory(d->gp.dsp, 'X', DSP_MIXBUFFER_BASE + i);
        }
    }
    /* A bootstrap-sized DMA_READ (0x2000 bytes) through the production path. */
    load_nop_program(PAGE_A_VA);
    program_sge(SGE_A_VA, PAGE_A_VA, 2);
    gp_write(NV_PAPU_GPRST, 1);
    gp_write(NV_PAPU_GPRST, 3);
    /* Reads of all 128 peripheral offsets. */
    for (i = 0; i < APU_WATCH_DSP_PERIPH_SIZE; ++i) {
        (void)read_peripheral(d->gp.dsp, DSP_PERIPH_BASE + i);
    }
    emit_now();    apu_watch_snapshot(&s);    cap_end();

    {
        unsigned bad = 0;
        for (i = 0; i < APU_WATCH_NUM_MIXBINS; ++i) {
            unsigned expect = FRAMES * NUM_SAMPLES_PER_FRAME;
            if (s.gpin.mixbuf[i].reads != expect) {
                snprintf(name, sizeof(name),
                         "(ix') mixbuf bin %u reads == %u (got %u)", i, expect,
                         s.gpin.mixbuf[i].reads);
                check(0, name);
                ++bad;
            }
        }
        check(bad == 0, "(ix') mixbuf per-bin counts are exact");
    }

    {
        unsigned bad = 0;
        for (i = 0; i < APU_WATCH_DSP_PERIPH_SIZE; ++i) {
            if (s.gpin.periph[i].reads != 1) {
                snprintf(name, sizeof(name), "(ix') periph offset %u reads == 1 (got %u)",
                         i, s.gpin.periph[i].reads);
                check(0, name);
                ++bad;
            }
        }
        check(bad == 0, "(ix') every peripheral offset was read once");
    }

    check(s.gpin.dma[APU_WATCH_DMA_CONTIG].reads >= 2, "(ix') DMA_READ recorded");
    check(s.gpin.dma[APU_WATCH_DMA_CONTIG].bytes >= 0x2000,
          "(ix') a bootstrap-sized DMA_READ (0x2000 bytes)");
    check(s.gpin.out_of_universe == 0, "(ix') GPIN_OUT_OF_UNIVERSE = 0");
    check_gpin_summary(&s, "(ix')");
    check_counts_lines(&s, "(ix')");
}

/* (x) Out-of-universe detector. */
static void case_x(void)
{
    struct apu_watch_snapshot s;

    apu_watch_reset();
    gp_state_reset();
    cap_begin();
    apu_gpin_record_out_of_universe(APU_WATCH_GPIN_PERIPH,
                                    APU_WATCH_DSP_PERIPH_SIZE);
    emit_now();    apu_watch_snapshot(&s);    cap_end();

    check(s.gpin.out_of_universe == 1, "(x) out_of_universe counter == 1");
    check(s.counts[APU_WATCH_C_GPIN_OUT_OF_UNIVERSE] == 1,
          "(x) GPIN_OUT_OF_UNIVERSE counter == 1");
    check(s.latch[APU_WATCH_GPIN_OUT_OF_UNIVERSE].latched == 1, "(x) latch fired once");
    check(s.latch[APU_WATCH_GPIN_OUT_OF_UNIVERSE].va == APU_WATCH_DSP_PERIPH_SIZE,
          "(x) latch records the index");
    if (g_trace_on) {
        check(cap_count("[GPWATCH] latch class=GPIN_OUT_OF_UNIVERSE ") == 1,
              "(x) exactly one latch line");
    }
    check_line_vs_snapshot(&s, "(x)");
    check_counts_lines(&s, "(x)");

    /* Fires once, not twice. */
    {
        struct apu_watch_snapshot s2;
        cap_begin();
        apu_gpin_record_out_of_universe(APU_WATCH_GPIN_PERIPH,
                                        APU_WATCH_DSP_PERIPH_SIZE);
        apu_watch_snapshot(&s2);
        cap_end();
        check(s2.gpin.out_of_universe == 2, "(x) counter is uncapped");
        check(s2.latch[APU_WATCH_GPIN_OUT_OF_UNIVERSE].latched == 1,
              "(x) latch still fired once");
        if (g_trace_on) {
            check(cap_count("[GPWATCH] latch class=GPIN_OUT_OF_UNIVERSE ") == 0,
                  "(x) no second latch line");
        }
    }

    /* A4b1-r4 Ruling 1 (C)(d), the out-of-universe twin, folded into (x) as the
     * ruling permits: a READ-direction transfer on the GP's DMA whose buf_id is
     * 5 -- a buffer the pin knows nothing about in either direction (the write
     * arm implements 0x0..0x3 and the read arm 0xE/0xF).  The read arm's hook
     * routes it to the out-of-universe detector, which fails closed to UNKNOWN
     * rather than letting an unaccounted GP input through, and it must NOT
     * touch any fifo count.
     *
     * Note 5 is also inside the RESERVED output range 2..5 of the declared
     * 6-element FIFO universe: a hook that keyed the read arm by buf_id without
     * the GP_INPUT_FIFO_COUNT test would silently record GP output traffic as
     * an input here.  This twin is what makes that mistake fail. */
    {
        struct apu_watch_snapshot s2;
        unsigned i;
        char name[160];

        apu_watch_reset();
        gp_state_reset();
        cap_begin();
        dma_read_transfer(d->gp.dsp, 5u, DMA_XFER_COUNT);
        emit_now();    apu_watch_snapshot(&s2);    cap_end();

        check(s2.gpin.out_of_universe == 1,
              "(x) buf_id=5 read-direction: out_of_universe == 1");
        check(s2.counts[APU_WATCH_C_GPIN_OUT_OF_UNIVERSE] == 1,
              "(x) buf_id=5 read-direction: GPIN_OUT_OF_UNIVERSE counted once");
        check(s2.latch[APU_WATCH_GPIN_OUT_OF_UNIVERSE].latched == 1,
              "(x) buf_id=5 read-direction: the latch fired");
        check(s2.latch[APU_WATCH_GPIN_OUT_OF_UNIVERSE].va == 5u,
              "(x) buf_id=5 read-direction: the latch records the index");
        check(s2.latch[APU_WATCH_GPIN_OUT_OF_UNIVERSE].observed == APU_WATCH_GPIN_FIFO,
              "(x) buf_id=5 read-direction: the latch records kind GPIN_FIFO");
        for (i = 0; i < APU_WATCH_FIFO_COUNT; ++i) {
            if (s2.gpin.fifo[i].reads != 0 || s2.gpin.fifo[i].words != 0) {
                snprintf(name, sizeof(name),
                         "(x) buf_id=5 read-direction: fifo[%u] unchanged "
                         "(got reads=%u words=%u)", i, s2.gpin.fifo[i].reads,
                         s2.gpin.fifo[i].words);
                check(0, name);
            }
        }
        {
            unsigned total = 0;
            for (i = 0; i < APU_WATCH_FIFO_COUNT; ++i) {
                total += s2.gpin.fifo[i].reads;
            }
            snprintf(name, sizeof(name),
                     "(x) buf_id=5 read-direction: fifo counts unchanged "
                     "(total reads %u)", total);
            check(total == 0, name);
        }
        check(!g_trace_on || cap_count("Unhandled DSP DMA buffer: 0x5") == 1,
              "(x) buf_id=5 read-direction: the unhandled-buffer line names 0x5");
        check_line_vs_snapshot(&s2, "(x) buf_id=5");
        check_counts_lines(&s2, "(x) buf_id=5");
    }
}

/* (xi) at_clear freezes. */
static void case_xi(void)
{
    struct apu_watch_snapshot s;
    const char *what = "(xi) at_clear";

    apu_watch_reset();
    gp_state_reset();
    *ram32(WATCH_BASE_VA) = W_SEED;
    *ram32(W_VA) = 3;

    cap_begin();
    /* Reads before the clear. */
    (void)read_peripheral(d->gp.dsp, 0xFFFFC5u);
    (void)read_peripheral(d->gp.dsp, 0xFFFFD4u);
    /* GP_CLEAR latches -> the freeze is taken. */
    gp_zero_write_covering_w();
    /* Reads after the clear: running counters only. */
    (void)read_peripheral(d->gp.dsp, 0xFFFFC5u);
    (void)read_peripheral(d->gp.dsp, 0xFFFFD6u);
    emit_now();    apu_watch_snapshot(&s);    cap_end();

    check(s.at_clear.taken == 1, "(xi) the freeze was taken");
    check(s.at_clear.seq == s.latch[APU_WATCH_GP_CLEAR].seq, "(xi) freeze seq");
    /* PERIPH is indexed by offset = address - DSP_PERIPH_BASE (0xFFFF80):
     * 0xFFFFC5 -> 0x45, 0xFFFFD4 -> 0x54, 0xFFFFD6 -> 0x56. */
    check(s.at_clear.gpin.periph[0x45].reads == 1, "(xi) pre-clear read 0x45 in at_clear");
    check(s.at_clear.gpin.periph[0x54].reads == 1, "(xi) pre-clear read 0x54 in at_clear");
    check(s.at_clear.gpin.periph[0x56].reads == 0,
          "(xi) post-clear read is NOT in at_clear");
    check(s.gpin.periph[0x45].reads == 2, "(xi) running counter continues");
    check(s.gpin.periph[0x56].reads == 1, "(xi) running counter continues (new offset)");
    if (g_trace_on) {
        check(cap_count("[GPIN] at_clear ") >= 4, "(xi) an at_clear block was emitted");
    }
    check_line_vs_snapshot(&s, "(xi)");
    check_counts_lines(&s, "(xi)");
}

/* ---- A4b2-NR (xii): the diagnostic GP input perturbation selectors ---------
 *
 * The A4b2-NR packet's Leg 2 depends on this hook actually substituting the
 * values the GP observes, and on it being inert when the gate is absent. A hook
 * that cannot be shown to fire proves nothing, so each selector is exercised
 * here rather than assumed.
 *
 * Process isolation: apu_watch_perturb_mode() caches the environment on first
 * call (read-once, like apu_watch_trace_enabled), so ONE process can only ever
 * exercise ONE mode. The mode is therefore chosen by the CTest registration that
 * launches this binary (see CMakeLists.txt), and this case asserts against
 * whatever mode the process was started with. Every mode is registered, so every
 * selector is covered across the suite.
 *
 * What this case asserts:
 *   - absent gate  -> selector is the identity and no counter moves (inertness);
 *   - zero/max     -> the returned value is the mode's constant, and it differs
 *                     from the original whenever the original was not already
 *                     that constant;
 *   - prng         -> deterministic for a given seed and different across calls;
 *   - bad-output   -> inputs are NOT substituted (its effect is on the doorbell
 *                     classification, asserted in the live-run leg instead).
 */
static void case_xii(void)
{
    const char *e = getenv("RECOMP_APU_GP_INPUT_PERTURB");
    int mode = apu_watch_perturb_mode();
    uint32_t got;

    check(mode == apu_watch_perturb_mode(), "(xii) mode is cached (read once)");

    if (!e || !*e) {
        check(mode == APU_PERTURB_OFF, "(xii) absent gate -> OFF");
        got = apu_watch_perturb_mixbuf(0x123456u);
        check(got == 0x123456u, "(xii) absent gate -> mixbuf identity");
        got = apu_watch_perturb_periph_ffffb3(0x654321u);
        check(got == 0x654321u, "(xii) absent gate -> periph identity");
        return;
    }

    switch (mode) {
    case APU_PERTURB_ZERO:
        check(apu_watch_perturb_mixbuf(0x123456u) == 0u, "(xii) zero -> mixbuf 0");
        check(apu_watch_perturb_periph_ffffb3(0x654321u) == 0u,
              "(xii) zero -> periph 0");
        break;
    case APU_PERTURB_MAX:
        check(apu_watch_perturb_mixbuf(0x123456u) == 0xFFFFFFu,
              "(xii) max -> mixbuf 0xFFFFFF");
        check(apu_watch_perturb_periph_ffffb3(0x654321u) == 0xFFFFFFu,
              "(xii) max -> periph 0xFFFFFF");
        break;
    case APU_PERTURB_PRNG: {
        uint32_t a = apu_watch_perturb_mixbuf(0x123456u);
        uint32_t b = apu_watch_perturb_mixbuf(0x123456u);
        check(a <= 0xFFFFFFu, "(xii) prng stays 24-bit");
        check(a != b, "(xii) prng advances per call (same input, different value)");
        break;
    }
    case APU_PERTURB_BAD_OUTPUT:
        /* bad-output must NOT touch the two stub inputs; its whole point is to
         * be the control that leaves them alone. */
        check(apu_watch_perturb_mixbuf(0x123456u) == 0x123456u,
              "(xii) bad-output leaves mixbuf untouched");
        check(apu_watch_perturb_periph_ffffb3(0x654321u) == 0x654321u,
              "(xii) bad-output leaves periph untouched");
        /* And it must be able to FAIL: with the control active, a zero-payload
         * transfer covering W_va must NOT take the 3->0 exchange, so GP_CLEAR
         * must not latch. This is the can-fail property the live-run comparator
         * depends on; asserting it here means the control is proven to bite. */
        {
            struct apu_watch_snapshot s;
            apu_watch_reset();
            gp_state_reset();
            *ram32(WATCH_BASE_VA) = W_SEED;
            *ram32(W_VA) = 3;
            cap_begin();
            gp_zero_write_covering_w();
            emit_now();
            apu_watch_snapshot(&s);
            cap_end();
            check(s.latch[APU_WATCH_GP_CLEAR].seq == 0,
                  "(xii) bad-output suppresses GP_CLEAR (control bites)");
            check(s.latch[APU_WATCH_GP_NONZERO_OVER].seq != 0,
                  "(xii) bad-output routes the write to GP_NONZERO_OVER");
        }
        break;
    default:
        check(0, "(xii) unrecognised mode must not select a substitution");
        break;
    }
}

/* Trace-off registration: (a)-(e), (i)-(vi), (viii), (ix'), (x), (xi) hold as
 * snapshot, PRAM and memory checks; (c)'s unmapped line is present; and the
 * case output contains zero matching lines other than `unmapped`. */
static void check_trace_off_emission(void)
{
    static const char *const forbidden[] = {
        "[GPBOOT]", "[GPRUN]", "[GPIN]", "[GPWATCH]",
        "[GPDMA] watch", "[GPDMA] frame=",
    };
    unsigned i;

    for (i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        char name[128];
        snprintf(name, sizeof(name), "trace-off: zero '%s' lines", forbidden[i]);
        check(cap_count(forbidden[i]) == 0, name);
    }
}

/* ============================================================
 * Setup
 * ============================================================ */

/* A minimal synthetic XBE: no sections, no TLS directory.  Enough for
 * xbox_MemoryLayoutInit to build the guest mapping the fixture needs, and
 * nothing else -- the fixture never runs guest code. */
static uint8_t *make_synthetic_xbe(size_t *out_size)
{
    static uint8_t xbe[0x2000];
    memset(xbe, 0, sizeof(xbe));
    *(uint32_t *)(xbe + 0x0104) = XBOX_BASE_ADDRESS;  /* base address        */
    *(uint32_t *)(xbe + 0x0108) = 0x1000;             /* SizeOfImageHeader   */
    *(uint32_t *)(xbe + 0x011C) = 0;                  /* section count       */
    *(uint32_t *)(xbe + 0x0120) = XBOX_BASE_ADDRESS + 0x200; /* section hdrs */
    *(uint32_t *)(xbe + 0x012C) = 0;                  /* no TLS directory    */
    *(uint32_t *)(xbe + 0x0158) = 0;                  /* no kernel thunks    */
    *out_size = sizeof(xbe);
    return xbe;
}

int main(void)
{
    uint8_t *xbe;
    size_t xbe_size = 0;
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    int trace_on;

    /* Unbuffered: a fault below must not discard the record of how far the
     * fixture got. */
    setvbuf(stdout, NULL, _IONBF, 0);

    if (!freopen(CAPTURE_PATH, "w+b", stderr)) {
        printf("FAIL: cannot redirect stderr to %s\n", CAPTURE_PATH);
        return 2;
    }

    xbe = make_synthetic_xbe(&xbe_size);
    if (!xbox_MemoryLayoutInit(xbe, xbe_size)) {
        printf("FAIL: xbox_MemoryLayoutInit\n");
        return 2;
    }

    ram = (uint8_t *)xbox_GetMemoryBase();
    if (!ram) {
        printf("FAIL: xbox_GetMemoryBase\n");
        return 2;
    }
    g_apu_ram_ptr = ram;

    /* Device-semantics-3 case 2 must be reachable: arrange a non-zero
     * high-water mark through the production contiguous allocator. */
    if (xbox_ContiguousAlloc(CONTIG_ALLOC_SIZE, 4096) == 0) {
        printf("FAIL: xbox_ContiguousAlloc\n");
        return 2;
    }
    if (xbox_ContiguousAllocatedBytes() < PAGE_B_OFFSET) {
        printf("FAIL: contiguous high-water mark too low for DS3 case 2\n");
        return 2;
    }
    if (xbox_GetMappedSize() == 0) {
        printf("FAIL: low RAM is not mapped\n");
        return 2;
    }

    d = (MCPXAPUState *)calloc(1, sizeof(*d));
    if (!d) {
        printf("FAIL: MCPXAPUState\n");
        return 2;
    }
    /* The production gp_write takes d->lock, so the state needs a real
     * CRITICAL_SECTION: calloc leaves a zeroed one, which is not a valid
     * initialised lock. */
    qemu_mutex_init(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);
    mcpx_apu_dsp_init(d);

    /* Bound the frame loop.  Device semantics 2's pinned loop is
     *     do { dsp_run(d->gp.dsp, 1000); }
     *     while (!dsp_get_halt_requested(d->gp.dsp) && d->gp.realtime);
     * (gp_ep.c:625-627).  `realtime` is the production field
     * mcpx_apu_update_dsp_preference sets from g_config.audio.use_dsp
     * (gp_ep.c:31-45); false is exactly the value that path writes when the
     * DSP preference is off, and the frame path does not re-derive it (the
     * preference function caches its last value).  So one 1000-cycle chunk. */
    d->gp.realtime = false;
    d->ep.realtime = false;

    /* The frame path calls mcpx_apu_monitor_mixdown, which returns early when
     * monitor.point is MON_VP; calloc leaves it MON_AC97, the same shape the
     * existing apu_mixdown fixture relies on. */
    memset(mixbins, 0, sizeof(mixbins));

    trace_on = apu_watch_trace_enabled();
    g_trace_on = trace_on;
    printf("AC-FIX fixture: RECOMP_APU_TRACE=%s, W_va=%08X, high water=%u\n",
           trace_on ? "1" : "0", (unsigned)W_VA,
           (unsigned)xbox_ContiguousAllocatedBytes());

    /* A4b2-NR: `bad-output` is the known-bad control for the doorbell
     * classification, so by construction it SUPPRESSES GP_CLEAR. Running it
     * through the cases below would fail ~21 assertions that correctly require
     * GP_CLEAR to latch -- a property of the control, not a defect. It is
     * therefore exercised alone: its own arm asserts the selector behaviour, and
     * its doorbell effect is asserted in the live-run leg where a comparator
     * exists to observe it. */
    if (apu_watch_perturb_mode() == APU_PERTURB_BAD_OUTPUT) {
        case_xii();
        xbox_MemoryLayoutShutdown();
        free(d);
        printf("%s: %u checks, %u failed (RECOMP_APU_TRACE=%s, bad-output "
               "control-only arm)\n",
               failures ? "FAIL" : "PASS", checks, failures, trace_on ? "1" : "0");
        return failures ? 1 : 0;
    }

    case_a();
    case_b();
    case_c();
    case_d();
    case_e();
    case_i();
    case_ii();
    case_iii();
    case_iv();
    case_v();
    case_vi();
    case_vii();
    case_viii();
    case_ix();
    case_x();
    case_xi();
    case_xii();

    if (!trace_on) {
        check_trace_off_emission();
    }

    xbox_MemoryLayoutShutdown();
    free(d);

    printf("%s: %u checks, %u failed (RECOMP_APU_TRACE=%s)\n",
           failures ? "FAIL" : "PASS", checks, failures,
           trace_on ? "1" : "0");
    return failures ? 1 : 0;
}
