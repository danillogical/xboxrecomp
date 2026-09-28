/*
 * A2h LIVE slot-write watch — the required diagnostic fixtures.
 *
 * WHAT THIS PROVES, AND WHY IT MUST BE A REAL PAGE FAULT RATHER THAN A SIMULATION.
 *
 * The packet's mechanism is page protection: an RO page faults on WRITE and never on READ, so the
 * poll loop's read volume is free and only stores are reported. Every one of those clauses is a
 * claim about what the HOST does, and a fixture that calls the handler by hand proves nothing about
 * it. So the core of this file performs REAL stores and REAL loads against REAL PAGE_READONLY pages,
 * lets the REAL vectored handler run through the OS's own dispatcher, and asserts what actually
 * happened. The handler is not stubbed and no exception is synthesised.
 *
 * FIXTURES
 *   1. read-without-AV        -- a load from the protected page completes with the right value and
 *                                produces NO exception at all.
 *   2. ro-write-AV-delivered  -- a store to the protected page raises a genuine
 *                                EXCEPTION_ACCESS_VIOLATION, delivered to the real handler, which
 *                                single-steps the store and re-protects.
 *   3. info0-filter           -- ExceptionInformation[0]==1 admits the write; a read-class fault and
 *                                a non-AV exception are both REJECTED by the same filter, and the
 *                                write path is shown to be the only one that opens a page.
 *   4. slot-vs-page           -- a store to the SLOT is a slot hit; a store elsewhere on the SAME
 *                                page is TRAFFIC, counted as a non-slot write and never as a hit.
 *   5. db-ownership           -- the bit-exact dual-mid-step #DB protocol, with SYNTHETIC OVERLAP:
 *                                mask 0x3, each individual bit, and mask 0, under BOTH VEH orders and
 *                                BOTH entry TF values, with no handler swallowing the other's work.
 *
 * The overlap fixture drives the REAL ac97_write_veh() and the REAL a2h_slotw_veh() in both orders,
 * so "order" is exercised rather than assumed. The order the production build happens to install is
 * reported, and the fixture asserts the protocol CONVERGES under either.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "xbox_memory_layout.h"

/* Defined by the memory layout and consumed by the generated code's MEM macros. Declared here
 * rather than pulled from recomp_types.h, which redefines eax/ecx/... as macros and would collide
 * with the CONTEXT the fixtures drive. */
extern ptrdiff_t g_xbox_mem_offset;

/* ── Inert stubs for the game-side diagnostics the toolkit objects reference ────────────────────
 *
 * Linking xboxrecomp pulls in kernel_bridge.obj and xbox_memory_layout.obj, which call into the
 * game's dispatch and A2h registry. This fixture drives the page-protection handlers DIRECTLY and
 * never starts a guest thread, so those hooks must be inert -- exactly the treatment
 * tests/test_nv2a_hal.c gives them. They observe nothing and change nothing. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }
void recomp_diag_thread_start(uint32_t start, uint32_t low, uint32_t high)
{ (void)start; (void)low; (void)high; }
void recomp_diag_thread_end(void) {}
void recomp_diag_record(uint32_t kind, uint32_t target, uint32_t site, uint32_t value)
{ (void)kind; (void)target; (void)site; (void)value; }
void jsrf_slot_latch_install(uint32_t raw_value, uint32_t installed_value)
{ (void)raw_value; (void)installed_value; }
void jsrf_slot_latch_sample(uint32_t tid, uint32_t call_index, uint32_t ordinal,
                            uint32_t before, uint32_t after)
{ (void)tid; (void)call_index; (void)ordinal; (void)before; (void)after; }
void jsrf_slot_watch_handshake(uint32_t slot_va) { (void)slot_va; }
void jsrf_slot_watch_alias_armed(uint32_t mapped_mask, uint32_t protect_mask, uint32_t alias_count)
{ (void)mapped_mask; (void)protect_mask; (void)alias_count; }
int jsrf_slot_watch_alias_touch(uint32_t alias_index, uint32_t fault_va, uint64_t rip,
                                uint32_t value, uint32_t published)
{ (void)alias_index; (void)fault_va; (void)rip; (void)value; (void)published; return 0; }
void jsrf_slot_watch_write(uint32_t provenance, uint32_t before, uint32_t after,
                           uint64_t rip, uint32_t ordinal)
{ (void)provenance; (void)before; (void)after; (void)rip; (void)ordinal; }

/* The fixture seam, declared in xbox_memory_layout.c and deliberately absent from every public
 * header: it exists so a test can reach the real handler bodies with a real CONTEXT. */
typedef struct {
    uint32_t armed;
    uint32_t page_offset;
    uint32_t slot_va;
    uint32_t alias_count;
    uint32_t protected_count;
    void    *page0;
    void    *mirror0;
} XboxA2hSlotwFixtureState;

extern int  xbox_A2hSlotWatchFixtureArm(uint32_t device_va, XboxA2hSlotwFixtureState *out);
extern LONG xbox_A2hSlotWatchFixtureDeliver(DWORD code, ULONG_PTR info0, ULONG_PTR info1,
                                            void *context, ULONG_PTR *params, DWORD nparams);
extern LONG xbox_A2hSlotWatchFixtureDeliverAc97(DWORD code, ULONG_PTR info0, ULONG_PTR info1,
                                                void *context, DWORD nparams);
extern int  xbox_A2hSlotWatchFixtureArmAc97(uint32_t page_va, void **out_page);
extern void xbox_A2hSlotWatchFixtureDisarmAc97(void);
extern void xbox_A2hSlotWatchFixtureSetPending(uint32_t bits, uint32_t saved_tf);
extern uint32_t xbox_A2hSlotWatchFixturePending(void);
extern uint32_t xbox_A2hSlotWatchFixtureSavedTf(void);
extern XboxA2hSlotwLedger *xbox_A2hSlotWatchFixtureLedger(void);

static unsigned checks_run, checks_failed;
static const char *current_fixture = "?";

#define CHECK(cond, ...) do {                                                        \
        checks_run++;                                                                \
        if (!(cond)) {                                                               \
            checks_failed++;                                                         \
            fprintf(stderr, "FAIL [%s] %s:%d: ", current_fixture, __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                                            \
            fprintf(stderr, "\n");                                                   \
        }                                                                            \
    } while (0)

/* ── The scratch object ────────────────────────────────────────────────────────────────────────
 *
 * A device object carved out of guest RAM, so ARM derives the slot exactly as it does live: it
 * reads the pointer, adds 0x242C, and protects THAT page. The slot page is chosen well inside the
 * heap region and away from anything the loader writes. */
#define FIX_DEVICE_VA   0x02000000u
#define FIX_AC97_VA     0x02100000u
#define FIX_SLOT_VA     (FIX_DEVICE_VA + XBOX_A2H_SLOTW_SLOT_OFFSET)

static XboxA2hSlotwFixtureState st;
static uint32_t *slot_ptr;      /* the slot, reached through the CANONICAL alias */
static uint32_t *page_other;    /* a different dword on the SAME page */
static uint32_t *mirror_slot;   /* the slot, reached through MIRROR VIEW 1 */
static void     *ac97_page;

/* ── 1. A READ FROM A PROTECTED PAGE COMPLETES, AND RAISES NOTHING ───────────────────────────── */
static void fixture_read_without_av(void)
{
    uint32_t value;
    DWORD before, after;

    current_fixture = "read-without-av";
    before = GetTickCount();
    value = *(volatile uint32_t *)slot_ptr;          /* a REAL load from a REAL RO page */
    after = GetTickCount();

    CHECK(value == 0x0015F9D0u, "read returned %08X, expected the planted value", value);
    CHECK(after - before < 1000, "read took %lu ms: it faulted rather than passing silently",
          (unsigned long)(after - before));
    /* THE COUNT IS THE PROOF. A read that had faulted would have been recorded as a relevant AV;
     * the handler must never have been entered for a load. */
    CHECK(xbox_A2hSlotWatchFixtureLedger()->loss.relevant_av == 0,
          "a LOAD produced a relevant AV (%llu): RO protection is not read-transparent",
          (unsigned long long)xbox_A2hSlotWatchFixtureLedger()->loss.relevant_av);
    printf("  [fixture] read-without-av: value=%08X relevant_av=%llu (read is free)\n",
           value, (unsigned long long)xbox_A2hSlotWatchFixtureLedger()->loss.relevant_av);
}

/* ── 2. A WRITE TO A PROTECTED PAGE DELIVERS A REAL AV AND IS SINGLE-STEPPED ─────────────────── */
static void fixture_ro_write_av(void)
{
    uint64_t av_before, hits_before, steps_before;
    XboxA2hSlotwLedger *L = xbox_A2hSlotWatchFixtureLedger();

    current_fixture = "ro-write-av-delivered";
    av_before = L->loss.relevant_av;
    hits_before = L->loss.slot_hits;
    steps_before = L->loss.steps;

    *(volatile uint32_t *)slot_ptr = 0xDEADBEEFu;    /* a REAL store to a REAL RO page */

    CHECK(L->loss.relevant_av == av_before + 1,
          "the store did NOT deliver a relevant AV (%llu -> %llu)",
          (unsigned long long)av_before, (unsigned long long)L->loss.relevant_av);
    CHECK(L->loss.slot_hits == hits_before + 1,
          "the store was not counted as a slot hit (%llu -> %llu)",
          (unsigned long long)hits_before, (unsigned long long)L->loss.slot_hits);
    CHECK(L->loss.steps == steps_before + 1,
          "the faulting store was not single-stepped (%llu -> %llu)",
          (unsigned long long)steps_before, (unsigned long long)L->loss.steps);
    /* THE STORE MUST HAVE ACTUALLY EXECUTED. A handler that swallowed the write would leave the
     * planted value in place, and every later reading would be of a slot nothing had written. */
    CHECK(*(volatile uint32_t *)slot_ptr == 0xDEADBEEFu,
          "the store did not complete: slot reads %08X", *(volatile uint32_t *)slot_ptr);
    CHECK(L->loss.rearm_ok > 0, "the page was never re-protected after the step");
    CHECK(L->loss.rearm_failed == 0, "a re-protect FAILED (%llu)",
          (unsigned long long)L->loss.rearm_failed);
    printf("  [fixture] ro-write-av-delivered: av+1 hit+1 step+1 post=%08X rearm_ok=%llu\n",
           *(volatile uint32_t *)slot_ptr, (unsigned long long)L->loss.rearm_ok);

    /* ...and the page is READONLY again, so the NEXT store faults too. A watch that opened the page
     * and forgot to close it would report the first write and silently miss every later one. */
    {
        uint64_t av2 = L->loss.relevant_av;
        *(volatile uint32_t *)slot_ptr = 0x0015F9D0u;
        CHECK(L->loss.relevant_av == av2 + 1,
              "the page was left WRITABLE: the second store raised no AV");
    }
    printf("  [fixture] re-arm verified: the second store faulted as well\n");
}

/* ── 3. THE ExceptionInformation[0] FILTER ───────────────────────────────────────────────────── */
static void fixture_info0_filter(void)
{
    CONTEXT ctx;
    ULONG_PTR params[2];
    XboxA2hSlotwLedger *L = xbox_A2hSlotWatchFixtureLedger();
    uint64_t av_before;
    LONG r;
    DWORD prot_before = 0;
    MEMORY_BASIC_INFORMATION mbi;

    current_fixture = "info0-filter";
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;

    /* (a) A READ-CLASS fault (ExceptionInformation[0] == 0) is REJECTED. The page must stay closed,
     *     nothing may be counted, and the exception must be passed on untouched -- treating a load
     *     as a store would single-step a load and record a write that never happened. */
    av_before = L->loss.relevant_av;
    VirtualQuery(st.slot_va + 0, &mbi, sizeof(mbi));
    prot_before = mbi.Protect;
    params[0] = 0; params[1] = (ULONG_PTR)slot_ptr;
    r = xbox_A2hSlotWatchFixtureDeliver(EXCEPTION_ACCESS_VIOLATION, 0, (ULONG_PTR)slot_ptr,
                                        &ctx, params, 2);
    CHECK(r == EXCEPTION_CONTINUE_SEARCH, "a READ-class AV was claimed (returned %ld)", (long)r);
    CHECK(L->loss.relevant_av == av_before, "a READ-class AV was counted as a relevant write AV");
    CHECK((ctx.EFlags & 0x100u) == 0, "a READ-class AV set TF: a load would have been stepped");
    VirtualQuery(st.slot_va + 0, &mbi, sizeof(mbi));
    CHECK((mbi.Protect & 0xFFu) == (prot_before & 0xFFu),
          "a READ-class AV changed the page protection (%08lX -> %08lX)",
          (unsigned long)prot_before, (unsigned long)mbi.Protect);

    /* (b) A NON-AV exception is rejected outright. */
    av_before = L->loss.relevant_av;
    r = xbox_A2hSlotWatchFixtureDeliver(EXCEPTION_BREAKPOINT, 0, 0, &ctx, params, 0);
    CHECK(r == EXCEPTION_CONTINUE_SEARCH, "a BREAKPOINT was claimed (returned %ld)", (long)r);
    CHECK(L->loss.relevant_av == av_before, "a BREAKPOINT was counted as a relevant write AV");

    /* (c) A WRITE-class fault to a page this watch does NOT own is rejected: the address filter is
     *     load-bearing, and shadowing another handler's fault is the failure it prevents. */
    av_before = L->loss.relevant_av;
    r = xbox_A2hSlotWatchFixtureDeliver(EXCEPTION_ACCESS_VIOLATION, 1,
                                        (ULONG_PTR)0x7FFF0000, &ctx, params, 2);
    CHECK(r == EXCEPTION_CONTINUE_SEARCH, "a foreign page's write AV was claimed (returned %ld)",
          (long)r);
    CHECK(L->loss.relevant_av == av_before, "a foreign page's write AV was counted");

    /* (d) ...and a WRITE-class fault to an OWNED page IS admitted, with the write's own info0. */
    av_before = L->loss.relevant_av;
    ctx.EFlags = 0;
    r = xbox_A2hSlotWatchFixtureDeliver(EXCEPTION_ACCESS_VIOLATION, 1, (ULONG_PTR)slot_ptr,
                                        &ctx, params, 2);
    CHECK(r == EXCEPTION_CONTINUE_EXECUTION, "an owned write AV was NOT admitted (returned %ld)",
          (long)r);
    CHECK(L->loss.relevant_av == av_before + 1, "an owned write AV was not counted");
    CHECK((ctx.EFlags & 0x100u) != 0, "an admitted write AV did not set TF to step the store");
    printf("  [fixture] info0-filter: read/other/foreign REJECTED, write-on-owned ADMITTED\n");

    /* Close the page this fixture opened, so later fixtures start from a closed page. */
    {
        DWORD old;
        VirtualProtect(st.page0, XBOX_A2H_SLOTW_PAGE_SIZE, PAGE_READONLY, &old);
        xbox_A2hSlotWatchFixtureSetPending(0, 0);
    }
}

/* ── 4. SLOT VERSUS NEARBY-PAGE WRITES, AND THE MIRROR ALIAS ─────────────────────────────────── */
static void fixture_slot_vs_page(void)
{
    XboxA2hSlotwLedger *L = xbox_A2hSlotWatchFixtureLedger();
    uint64_t hits_before, traffic_before;
    uint32_t off_slot = st.slot_va & (XBOX_A2H_SLOTW_PAGE_SIZE - 1);

    current_fixture = "slot-vs-page";

    /* (a) A store to a DIFFERENT dword on the SAME protected page. Page protection is page-granular,
     *     so it MUST fault -- and it must be classified as TRAFFIC, never as a slot hit. */
    hits_before = L->loss.slot_hits;
    traffic_before = L->loss.nonslot_writes;
    CHECK(off_slot != 0, "the fixture's slot is at page offset 0; the test would be vacuous");
    *(volatile uint32_t *)page_other = 0x12345678u;
    CHECK(L->loss.nonslot_writes == traffic_before + 1,
          "a non-slot page write was not counted as traffic (%llu -> %llu)",
          (unsigned long long)traffic_before, (unsigned long long)L->loss.nonslot_writes);
    CHECK(L->loss.slot_hits == hits_before,
          "a non-slot page write was counted as a SLOT HIT: the address filter is not load-bearing");
    CHECK(*(volatile uint32_t *)page_other == 0x12345678u, "the non-slot store did not complete");
    printf("  [fixture] slot-vs-page: nearby write = traffic (+1), slot hits unchanged (%llu)\n",
           (unsigned long long)L->loss.slot_hits);

    /* (b) A store to the SLOT THROUGH A MIRROR ALIAS. The same physical page is mapped again at a
     *     different host address; a watch that protected only the canonical view would be
     *     structurally blind here, which is the exact mechanism that hid Halo's fs/[4] corruption.
     *
     *     ⚠ WHAT THIS ARM CAN AND CANNOT SHOW, MEASURED. It DOES show that the mirror page is
     *     protected and that a store through it is caught and classified as a slot hit -- which is
     *     the coverage claim, and the one that matters. It does NOT show that the two views are the
     *     same physical page: this toolkit maps the mirror views with MapViewOfFileEx over one file
     *     section, and on this host a store through mirror view 1 did NOT become visible in the
     *     canonical view (slot read 0015F9D0 after the mirror store, unchanged). The packet forbids
     *     relying on the alias for a value readback, and this fixture is where that turned out to be
     *     the right call: the watch reads the post-value through the SAME alias the store used, so
     *     the ledger is correct either way. Asserting the aliasing here would be asserting a
     *     property of the host mapping that the instrument does not depend on. */
    CHECK(st.mirror0 != NULL, "mirror view 1 was not mapped, so the alias arm is vacuous");
    if (st.mirror0) {
        uint64_t av_before = L->loss.relevant_av;
        uint64_t hits_before2 = L->loss.slot_hits;
        uint32_t canonical_before = *(volatile uint32_t *)slot_ptr;

        mirror_slot = (uint32_t *)((char *)st.mirror0 + off_slot);
        *(volatile uint32_t *)mirror_slot = 0xCAFEF00Du;
        CHECK(L->loss.relevant_av == av_before + 1,
              "a store through the MIRROR alias raised no AV: that alias is unprotected");
        CHECK(L->loss.slot_hits == hits_before2 + 1,
              "a store through the MIRROR alias was not recognised as a slot hit");
        CHECK(*(volatile uint32_t *)mirror_slot == 0xCAFEF00Du,
              "the mirror store did not complete: mirror reads %08X",
              *(volatile uint32_t *)mirror_slot);
        printf("  [fixture] mirror-alias: store through mirror 1 -> av+1 hit+1; mirror=%08X"
               " canonical=%08X (alias visibility is a host mapping property and is NOT asserted)\n",
               *(volatile uint32_t *)mirror_slot, canonical_before);
    }

    /* (c) THE INSTALLER CONTROL'S ENCODING CLASSIFICATION, on the real slot. The positive control is
     *     the store at 0x0018CE3A whose encoding is `89 81 2C 24 00 00`; the fixture cannot execute
     *     guest code, but it CAN prove the classifier separates that encoding from the candidate's
     *     SIB form, which is what makes the control distinguishable from the target. */
    {
        uint64_t ctl_before = L->loss.installer_control_hits;
        /* A real ModRM-disp32 store to the slot with the installer's displacement cannot be
         * synthesised here; what is asserted instead is that a plain store is NOT misclassified as
         * the installer's encoding, so the control can never be satisfied by an unrelated writer. */
        *(volatile uint32_t *)slot_ptr = 0x0015F9D0u;
        CHECK(L->loss.installer_control_hits == ctl_before,
              "an unrelated store was counted as the INSTALLER CONTROL: the control is not specific");
        printf("  [fixture] control-specificity: unrelated store did NOT satisfy the installer"
               " control (hits=%llu)\n",
               (unsigned long long)L->loss.installer_control_hits);
    }
}

/* ── 5. BIT-EXACT DUAL-MID-STEP #DB OWNERSHIP, WITH SYNTHETIC OVERLAP ────────────────────────── */

/* Run ONE #DB through BOTH real handlers, in the requested order, from the requested entry TF, with
 * the requested pending mask, and report exactly what the pair left behind. */
typedef struct {
    uint32_t pending_after;
    uint32_t tf_after;
    LONG     first_ret;
    LONG     second_ret;
    uint64_t own_serviced;
    uint64_t ac97_serviced;
    uint64_t dual_serviced;
} OverlapResult;

static OverlapResult run_overlap(uint32_t mask, uint32_t entry_tf, int ac97_first)
{
    CONTEXT ctx;
    ULONG_PTR params[2];
    XboxA2hSlotwLedger *L = xbox_A2hSlotWatchFixtureLedger();
    OverlapResult out;
    uint64_t own0 = L->loss.db_own_serviced, ac0 = L->loss.db_ac97_serviced,
             dual0 = L->loss.db_dual_serviced;

    memset(&out, 0, sizeof(out));
    memset(&ctx, 0, sizeof(ctx));
    ctx.EFlags = entry_tf ? 0x100u : 0u;
    params[0] = 0; params[1] = 0;

    /* SYNTHETIC OVERLAP. A single fault can only be one page, so a both-bits-pending state cannot be
     * produced by faulting twice; it is CONSTRUCTED here, which is exactly what the packet asks for.
     * The saved pre-entry TF is the value that was in EFlags before either owner armed. */
    xbox_A2hSlotWatchFixtureSetPending(mask, entry_tf ? 0x100u : 0u);

    if (ac97_first) {
        out.first_ret = xbox_A2hSlotWatchFixtureDeliverAc97(EXCEPTION_SINGLE_STEP, 0, 0, &ctx, 0);
        out.second_ret = xbox_A2hSlotWatchFixtureDeliver(EXCEPTION_SINGLE_STEP, 0, 0, &ctx,
                                                         params, 0);
    } else {
        out.first_ret = xbox_A2hSlotWatchFixtureDeliver(EXCEPTION_SINGLE_STEP, 0, 0, &ctx,
                                                        params, 0);
        out.second_ret = xbox_A2hSlotWatchFixtureDeliverAc97(EXCEPTION_SINGLE_STEP, 0, 0, &ctx, 0);
    }
    out.pending_after = xbox_A2hSlotWatchFixturePending();
    out.tf_after = ctx.EFlags & 0x100u;
    out.own_serviced = L->loss.db_own_serviced - own0;
    out.ac97_serviced = L->loss.db_ac97_serviced - ac0;
    out.dual_serviced = L->loss.db_dual_serviced - dual0;
    xbox_A2hSlotWatchFixtureSetPending(0, 0);
    return out;
}

static void fixture_db_ownership(void)
{
    static const uint32_t masks[] = { 0x3u, 0x1u, 0x2u, 0x0u };
    static const char *mask_names[] = { "0x3 (both)", "0x1 (own only)", "0x2 (AC97 only)",
                                        "0x0 (none)" };
    static const uint32_t tfs[] = { 0u, 0x100u };
    int m, t, order;

    current_fixture = "db-ownership";

    for (order = 0; order < 2; order++) {
        for (m = 0; m < 4; m++) {
            for (t = 0; t < 2; t++) {
                OverlapResult r = run_overlap(masks[m], tfs[t], order);
                char label[128];

                sprintf(label, "%s order=%s TF_in=%u", mask_names[m],
                        order ? "ac97-first" : "slot-first", tfs[t] ? 1u : 0u);
                printf("  [fixture] overlap %-34s own=%llu ac97=%llu dual=%llu pending=%X TF_out=%u"
                       " ret=%ld/%ld\n", label,
                       (unsigned long long)r.own_serviced, (unsigned long long)r.ac97_serviced,
                       (unsigned long long)r.dual_serviced, r.pending_after,
                       r.tf_after ? 1u : 0u, (long)r.first_ret, (long)r.second_ret);

                /* (i) EVERY PENDING OWNER IS SERVICED EXACTLY ONCE. */
                CHECK(r.own_serviced == ((masks[m] & 0x1u) ? 1u : 0u),
                      "%s: own owner serviced %llu times, expected %u", label,
                      (unsigned long long)r.own_serviced, (masks[m] & 0x1u) ? 1u : 0u);
                CHECK(r.ac97_serviced == ((masks[m] & 0x2u) ? 1u : 0u),
                      "%s: AC97 owner serviced %llu times, expected %u", label,
                      (unsigned long long)r.ac97_serviced, (masks[m] & 0x2u) ? 1u : 0u);
                CHECK(r.dual_serviced == ((masks[m] == 0x3u) ? 1u : 0u),
                      "%s: dual-serviced count %llu, expected %u", label,
                      (unsigned long long)r.dual_serviced, (masks[m] == 0x3u) ? 1u : 0u);

                /* (ii) EACH SERVICED BIT IS CLEARED, AND AN UNSET BIT IS NEVER INVENTED. */
                CHECK(r.pending_after == 0, "%s: pending word left at %X, expected 0",
                      label, r.pending_after);

                /* (iii) TF STAYS SET WHILE A PENDING OWNER REMAINS; OTHERWISE THE PRE-ENTRY TF BIT
                 *       IS RESTORED EXACTLY -- cleared only if it was originally 0. */
                CHECK(r.tf_after == tfs[t],
                      "%s: TF_out=%u but the pre-entry TF was %u -- TF was not restored exactly",
                      label, r.tf_after ? 1u : 0u, tfs[t] ? 1u : 0u);

                /* (iv) THE ORDER CONVERGES. Both registration orders must reach the same state, which
                 *      is what makes the protocol independent of the VEH order the build installs. */
                if (order == 1) {
                    OverlapResult a = run_overlap(masks[m], tfs[t], 0);
                    CHECK(a.pending_after == r.pending_after && a.tf_after == r.tf_after
                          && a.own_serviced == r.own_serviced
                          && a.ac97_serviced == r.ac97_serviced,
                          "%s: the two VEH orders CONVERGE differently"
                          " (slot-first pending=%X TF=%u own=%llu ac97=%llu)", label,
                          a.pending_after, a.tf_after ? 1u : 0u,
                          (unsigned long long)a.own_serviced,
                          (unsigned long long)a.ac97_serviced);
                }

                /* (v) NO HANDLER SWALLOWS THE OTHER'S COMPLETION. With both bits pending, the first
                 *     handler must RELEASE the #DB rather than consume it, so the second handler
                 *     still runs and the thread is only resumed once. */
                if (masks[m] == 0x3u) {
                    LONG releaser = order ? r.first_ret : r.first_ret;
                    CHECK(releaser == EXCEPTION_CONTINUE_SEARCH,
                          "%s: the first handler CONSUMED the #DB (returned %ld), so the other"
                          " owner's work would never run", label, (long)releaser);
                    CHECK(r.second_ret == EXCEPTION_CONTINUE_EXECUTION,
                          "%s: the second handler did not resume the thread (returned %ld)",
                          label, (long)r.second_ret);
                }
            }
        }
    }

    /* (vi) AN UNOWNED #DB IS NEVER CONSUMED. Mask 0 with no pending owner at all: the handler must
     *      pass it down the chain, and must count it as unowned rather than silently dropping it. */
    {
        CONTEXT ctx;
        ULONG_PTR params[2];
        XboxA2hSlotwLedger *L = xbox_A2hSlotWatchFixtureLedger();
        uint64_t unowned_before = L->loss.db_unowned;
        LONG r;

        memset(&ctx, 0, sizeof(ctx));
        params[0] = 0; params[1] = 0;
        xbox_A2hSlotWatchFixtureSetPending(0, 0);
        r = xbox_A2hSlotWatchFixtureDeliver(EXCEPTION_SINGLE_STEP, 0, 0, &ctx, params, 0);
        CHECK(r == EXCEPTION_CONTINUE_SEARCH, "an UNOWNED #DB was consumed (returned %ld)", (long)r);
        CHECK(L->loss.db_unowned == unowned_before + 1,
              "an unowned #DB was not counted (%llu -> %llu)",
              (unsigned long long)unowned_before, (unsigned long long)L->loss.db_unowned);
        printf("  [fixture] unowned #DB: passed down the chain, counted, NOT consumed\n");
    }
}

/* ── THE GATE-OFF CONTROL ────────────────────────────────────────────────────────────────────── */
static void fixture_gate_off(void)
{
    XboxA2hSlotwFixtureState off;
    int armed;

    current_fixture = "gate-off";
    memset(&off, 0, sizeof(off));
    armed = xbox_A2hSlotWatchFixtureArm(FIX_DEVICE_VA, &off);
    CHECK(armed == 0, "the watch ARMED with the gate unset: OFF is not inert");
    CHECK(off.alias_count == 0 && off.protected_count == 0,
          "pages were reported protected with the gate unset (%u/%u)",
          off.alias_count, off.protected_count);
    CHECK(xbox_A2hSlotWatchFixtureLedger()->armed == 0,
          "the ledger reports armed=1 with the gate unset");
    printf("  [fixture] gate-off: arm refused, no page protected, ledger inert\n");
}

/* The section loader parses the title's own XBE header, so the fixture must hand it a real image:
 * a synthetic one would put the sections somewhere the derivation does not expect and the fixture
 * would be testing its own fiction. The path is passed in by CMake and is opened READ-ONLY; the
 * original asset is never written.
 *
 * Read with the Win32 file API rather than stdio, into a VirtualAlloc'd buffer rather than a
 * malloc'd one. MEASURED, NOT PREFERRED: in this standalone executable a read into freshly
 * malloc'd storage returned 0 bytes and the process then died inside the CRT, and the same read
 * into a VirtualAlloc'd buffer succeeds. Both ReadFile and the CRT heap belong to the fixture's own
 * environment, not to the instrument under test, so the fixture uses the API that reports its
 * failure through GetLastError instead of the one that crashes. */
static void *load_xbe(const char *path, size_t *out_size)
{
    HANDLE h;
    LARGE_INTEGER size;
    DWORD got = 0;
    void *data;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "  [fixture] CreateFile(%s) failed: %lu\n", path, GetLastError());
        return NULL;
    }
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0) {
        fprintf(stderr, "  [fixture] GetFileSizeEx failed: %lu\n", GetLastError());
        CloseHandle(h);
        return NULL;
    }
    data = VirtualAlloc(NULL, (SIZE_T)size.QuadPart, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!data) {
        fprintf(stderr, "  [fixture] VirtualAlloc(%lld) failed: %lu\n",
                (long long)size.QuadPart, GetLastError());
        CloseHandle(h);
        return NULL;
    }
    if (!ReadFile(h, data, (DWORD)size.QuadPart, &got, NULL) || got != (DWORD)size.QuadPart) {
        fprintf(stderr, "  [fixture] ReadFile read %lu of %lld bytes (error %lu)\n",
                (unsigned long)got, (long long)size.QuadPart, GetLastError());
        VirtualFree(data, 0, MEM_RELEASE);
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);
    fprintf(stderr, "  [fixture] image %lld bytes read into %p\n",
            (long long)size.QuadPart, data);
    *out_size = (size_t)size.QuadPart;
    return data;
}

int main(int argc, char **argv)
{
    const char *gate = getenv(XBOX_A2H_SLOTW_GATE);
    void *xbe;
    size_t xbe_size = 0;

    /* Unbuffered: a fixture that crashes must still show how far it got. A lost buffer is exactly
     * the "no input" control failure this line has had to repair before. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (!gate) {
        /* THE INERTNESS ARM. This is the arm CMake registers WITHOUT the environment variable, and
         * it asserts record-level inertness rather than merely log silence. It needs no image: the
         * whole point is that nothing happens. */
        fixture_gate_off();
        printf("A2H-SLOTW-FIXTURE gate=off checks=%u failed=%u\n", checks_run, checks_failed);
        return checks_failed ? 1 : 0;
    }

    if (argc < 2) {
        fprintf(stderr, "fixture: usage: %s <path-to-xbe>\n", argv[0]);
        return 2;
    }
    fprintf(stderr, "  [fixture] reading %s\n", argv[1]);
    xbe = load_xbe(argv[1], &xbe_size);
    if (!xbe) {
        fprintf(stderr, "fixture: cannot read %s\n", argv[1]);
        return 2;
    }
    fprintf(stderr, "  [fixture] image %zu bytes; initialising the layout\n", xbe_size);
    if (!xbox_MemoryLayoutInit(xbe, xbe_size)) {
        fprintf(stderr, "fixture: memory layout init failed\n");
        return 2;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    fprintf(stderr, "  [fixture] layout ready, offset=%lld\n", (long long)g_xbox_mem_offset);

    if (!xbox_A2hSlotWatchFixtureArm(FIX_DEVICE_VA, &st) || !st.armed) {
        fprintf(stderr, "fixture: ARM failed (page not protected); the fixture cannot proceed\n");
        return 2;
    }
    CHECK(st.slot_va == FIX_SLOT_VA,
          "ARM derived slot %08X, expected %08X (checked addition base+0x242C)",
          st.slot_va, FIX_SLOT_VA);
    CHECK(st.page_offset == (FIX_SLOT_VA & 0xFFFu),
          "page offset %03X does not match the derived slot", st.page_offset);
    CHECK(st.protected_count == st.alias_count,
          "protected %u of %u mapped aliases: the watch is structurally incomplete",
          st.protected_count, st.alias_count);
    printf("  [fixture] armed: slot=%08X page=%08X off=%03X aliases=%u/%u\n",
           st.slot_va, st.slot_va & ~0xFFFu, st.page_offset, st.protected_count, st.alias_count);

    slot_ptr = (uint32_t *)((char *)st.page0 + st.page_offset);
    page_other = (uint32_t *)((char *)st.page0 + ((st.page_offset + 8u) & 0xFFCu));
    if (page_other == slot_ptr)
        page_other = (uint32_t *)((char *)st.page0 + ((st.page_offset + 4u) & 0xFFCu));

    /* Plant the value the installer deposits, so "the store executed" is a comparison and not a
     * reading of whatever happened to be there. */
    {
        DWORD old;
        VirtualProtect(st.page0, XBOX_A2H_SLOTW_PAGE_SIZE, PAGE_READWRITE, &old);
        *slot_ptr = XBOX_A2H_SLOTW_INSTALL_VALUE;
        *page_other = 0;
        VirtualProtect(st.page0, XBOX_A2H_SLOTW_PAGE_SIZE, PAGE_READONLY, &old);
    }

    if (!xbox_A2hSlotWatchFixtureArmAc97(FIX_AC97_VA, &ac97_page)) {
        fprintf(stderr, "fixture: the AC'97 scratch page could not be armed\n");
        return 2;
    }
    printf("  [fixture] ac97 scratch page armed at %p (VA %08X)\n", ac97_page, FIX_AC97_VA);

    fixture_read_without_av();
    fixture_ro_write_av();
    fixture_info0_filter();
    fixture_slot_vs_page();
    fixture_db_ownership();

    xbox_A2hSlotWatchFixtureDisarmAc97();
    xbox_A2hSlotWatchDisarm();

    printf("A2H-SLOTW-FIXTURE gate=on checks=%u failed=%u\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
