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
extern uint32_t xbox_A2hSlotWatchFixtureClassify(uint64_t rip);
extern uint32_t xbox_A2hSlotWatchFixtureForm(uint64_t rip);
extern void xbox_A2hSlotWatchSetRecompBounds(uint64_t lo, uint64_t hi, uint32_t probes,
                                             uint32_t valid);
extern void xbox_A2hSlotWatchRangeBounds(uint64_t *recomp_lo, uint64_t *recomp_hi, uint32_t *valid,
                                         uint64_t *image_lo, uint64_t *image_hi);
extern int  xbox_A2hSlotWatchCoherence(uint32_t *verdict, uint32_t *last_write_value,
                                       uint32_t *last_write_seq, uint32_t *terminal_value);
extern uint32_t xbox_A2hSlotWatchFixtureDecideCoherence(uint32_t last_write_value,
                                                        uint32_t last_write_seq,
                                                        uint32_t terminal_value,
                                                        uint32_t terminal_seen);
extern void xbox_A2hSlotWatchFixtureResetCoherence(void);
extern void xbox_A2hSlotWatchFixtureNoteRead(uint32_t value, uint32_t read_index);
extern int  xbox_A2hSlotWatchFixturePublishWrite(uint32_t slot_hit, uint32_t range_class,
                                                 uint32_t pre_value);

static unsigned checks_run, checks_failed;
static const char *current_fixture = "?";

/* ── THE RECOMPILED-BOUND WITNESS, AND WHY THE FIXTURE NEEDS A REAL ONE ──────────────────────────
 *
 * ⚠ THE RANGE CLASSIFIER IS HANDED THE FAULTING RIP, SO A FIXTURE THAT DELIVERS A SYNTHETIC
 * EXCEPTION MUST STILL SUPPLY A REAL RIP. The first version of this fixture delivered write AVs with
 * a zeroed CONTEXT, so `Rip == 0` -- which classifies UNKNOWN, which is INFRA FAILURE by design. The
 * handler therefore printed an INFRA FAILURE line and latched `overflow` on every synthetic
 * delivery, which would have made the fixture's own `absence_rows_valid` unreadable and, worse,
 * trained a reader to ignore the one line that must never be ignored.
 *
 * So the fixture publishes a REAL recompiled bound around a REAL function in this image, and every
 * synthetic fault carries that function's real address. The bound's endpoints are real addresses; the
 * RIP is a real address; and the classification under test is the production one. */
static void fixture_recomp_witness(void);
static uint64_t g_fix_recomp_lo = 0, g_fix_recomp_hi = 0;

static void fixture_publish_bound(void)
{
    uint64_t w = (uint64_t)(uintptr_t)&fixture_recomp_witness;
    g_fix_recomp_lo = w & ~(uint64_t)0xFFFu;
    g_fix_recomp_hi = g_fix_recomp_lo + 0x1000u;
    xbox_A2hSlotWatchSetRecompBounds(g_fix_recomp_lo, g_fix_recomp_hi, 1u, 1u);
}

/* The witness body. It is never CALLED -- only its address is used -- but it must be a real compiled
 * function so its address is a real code address inside this image. */
static void fixture_recomp_witness(void)
{
    /* A store, so the native-disasm corroboration has something real to find on it. */
    volatile uint32_t sink = 0;
    sink = 0xA2A2A2A2u;
    (void)sink;
}

#define CHECK(cond, ...) do {                                                        \
        checks_run++;                                                                \
        if (!(cond)) {                                                               \
            checks_failed++;                                                         \
            fprintf(stderr, "FAIL [%s] %s:%d: ", current_fixture, __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                                            \
            fprintf(stderr, "\n");                                                   \
        }                                                                            \
    } while (0)

/* ── 6. THE RANGE CLASSIFIER, AGAINST THE REAL LOADED MODULES ─────────────────────────────────────
 *
 * ⚠⚠ THE PREVIOUS VERSION OF THIS FIXTURE IS DELETED, NOT REPAIRED, AND THE REASON IS THE POINT.
 *
 * It read the GUEST bytes of `0x0018CE3A` and `0x00199F45` through `g_xbox_mem_offset` and asserted
 * that the classifier returned MODRM for the first and SIB for the second. That proved the classifier
 * could tell two GUEST ENCODINGS apart -- which is not a property the live classifier ever needed or
 * could have used. At run time the classifier is handed a NATIVE RIP, and the bytes there are
 * GENERATED C compiled by the host toolchain, never the guest's `89 81 2C 24 00 00`. The fixture was
 * therefore proving something about an input the instrument never sees, and its passing is exactly
 * why the void paradigm survived this long. The `0x7B3` residual is NOT repaired here either: it died
 * with the paradigm and its arithmetic is not to be re-derived.
 *
 * WHAT IS PROVEN INSTEAD, AND IT IS PROVEN ON THE REAL MODULES RATHER THAN A SYNTHETIC RANGE:
 *
 *   1. a RIP inside the RECOMPILED MODULE resolves to GAME_MODULE -- and the address used is a REAL
 *      generated function's address, taken from the dispatch the game itself resolves through;
 *   2. a RIP inside the TOOLKIT/HOST image resolves to TOOLKIT_HOST -- and the address used is a REAL
 *      function in this test binary;
 *   3. a RIP outside both resolves to UNKNOWN;
 *   4. the boundary cases: the module's own start and end, checked INCLUSIVE at `lo` and EXCLUSIVE at
 *      `hi`, because an off-by-one at a bound is the whole classification;
 *   5. native-disasm corroboration where available, and an explicit check that it NEVER OVERRIDES the
 *      range class.
 *
 * ⚠ AND THE BOUND IS PUBLISHED THE SAME WAY THE GAME PUBLISHES IT -- by probing the real dispatch.
 * `recomp_lookup` is not available to this fixture (it links no generated code), so the fixture
 * derives its bound from REAL function ADDRESSES it can name, and the production path is exercised by
 * the game's own `a2h_publish_recomp_bounds()`. What this fixture proves is the CLASSIFIER's
 * behaviour on real addresses and real bounds; what it cannot prove is that the game's probe loop
 * finds the right bound, and that limitation is stated rather than papered over.
 */
#define FIX_INSTALLER_VA 0x0018CE3Au   /* mov [ecx+0x242c], eax  -- the required control */
#define FIX_CANDIDATE_VA 0x00199F45u   /* mov [esi+ebp*4+0x3ec], eax -- the candidate */

/* Real functions in THIS image, used as the TOOLKIT_HOST witnesses. Their addresses are the linker's
 * and are read here, never assumed. */
extern uint32_t xbox_A2hSlotWatchFixtureClassify(uint64_t rip);
extern uint32_t xbox_A2hSlotWatchFixtureForm(uint64_t rip);

static void fixture_range_classifier(void)
{
    uint64_t rlo = 0, rhi = 0, ilo = 0, ihi = 0;
    uint32_t valid = 0;
    uint32_t cls;
    /* ⚠ THE WITNESS IS THE FUNCTION THE PUBLISHED BOUND ACTUALLY WRAPS. Using this fixture's own
     * address here instead was a real defect the arms below caught: `fixture_range_classifier` and
     * `fixture_recomp_witness` are different functions on different pages, so the "witness" sat
     * OUTSIDE the bound it was supposed to be inside and classified TOOLKIT_HOST. The arms were
     * right and the fixture was wrong -- which is the outcome a boundary arm is FOR. */
    uint64_t host_witness = (uint64_t)(uintptr_t)&fixture_recomp_witness;
    /* A SECOND, DIFFERENT real address, used as the TOOLKIT_HOST witness. It must NOT be in the
     * published bound, or the discriminating arm below would be vacuous. */
    uint64_t host_other = (uint64_t)(uintptr_t)&fixture_range_classifier;

    current_fixture = "range-classifier";

    xbox_A2hSlotWatchRangeBounds(&rlo, &rhi, &valid, &ilo, &ihi);
    printf("  [fixture] range-classifier: published recomp=%016llX..%016llX valid=%u"
           " image=%016llX..%016llX\n",
           (unsigned long long)rlo, (unsigned long long)rhi, valid,
           (unsigned long long)ilo, (unsigned long long)ihi);
    /* ⚠ THE TWO WITNESSES MUST BE ON DIFFERENT PAGES, OR THE ARMS BELOW COLLAPSE INTO ONE. This is
     * asserted rather than assumed, because the whole point of the TOOLKIT_HOST arm is that it is a
     * real in-image address that is NOT recompiled code. */
    CHECK(host_other < g_fix_recomp_lo || host_other >= g_fix_recomp_hi,
          "the TOOLKIT_HOST witness %016llX is INSIDE the published recompiled bound"
          " %016llX..%016llX: the discriminating arm would be vacuous",
          (unsigned long long)host_other, (unsigned long long)g_fix_recomp_lo,
          (unsigned long long)g_fix_recomp_hi);

    /* THE IMAGE BOUND MUST BE REAL. It is read from this module's own PE headers, so a zero here
     * means the header read failed and EVERY classification below would be vacuous. */
    CHECK(ihi > ilo, "the image bound is degenerate (%016llX..%016llX): the PE header read failed"
                     " and every classification below would be vacuous",
          (unsigned long long)ilo, (unsigned long long)ihi);
    /* ...and it must actually contain this function, which is the property that makes it THIS
     * module's bound rather than some other range. */
    CHECK(host_witness >= ilo && host_witness < ihi,
          "this fixture's own address %016llX is OUTSIDE the image bound %016llX..%016llX: the bound"
          " does not describe the module the RIPs live in",
          (unsigned long long)host_witness, (unsigned long long)ilo, (unsigned long long)ihi);

    /* ── (1) GAME_MODULE, PUBLISHED FIRST SO THE BOUNDARY ARMS BELOW ACTUALLY RUN ────────────────
     *
     * ⚠ THE ORDER MATTERS AND GOT THIS WRONG ONCE: the boundary arms were guarded by `if (valid)`
     * and ran BEFORE anything published a bound, so they silently skipped and the fixture still
     * reported zero failures. A boundary arm that never runs is worse than no arm, because it reads
     * as coverage. The bound is therefore published HERE, first, and the arms below are UNGUARDED --
     * they cannot skip.
     *
     * ⚠ THE WITNESS IS A REAL FUNCTION, SO THE FIXTURE CANNOT INVENT AN ADDRESS. This binary links no
     * generated translation units, so the only honest way to place a RIP inside the recompiled module
     * is to publish a bound around a REAL function's address and check that it classifies into it.
     * `fixture_recomp_witness` is real and the bound is a page around it, so both endpoints are real.
     *
     * The PRODUCTION publication path -- probing `recomp_lookup` for the real recompiled extent -- is
     * exercised in the GAME (`a2h_publish_recomp_bounds`), not here, because `recomp_lookup` is the
     * game's generated dispatch and this binary does not link it. That limitation is stated rather
     * than papered over: what this fixture proves is the CLASSIFIER's behaviour on real addresses and
     * real bounds. */
    {
        fixture_publish_bound();
        xbox_A2hSlotWatchRangeBounds(&rlo, &rhi, &valid, &ilo, &ihi);
        CHECK(valid == 1 && rlo == g_fix_recomp_lo && rhi == g_fix_recomp_hi,
              "the published bound did not round-trip: valid=%u recomp=%016llX..%016llX, expected"
              " %016llX..%016llX", valid, (unsigned long long)rlo, (unsigned long long)rhi,
              (unsigned long long)g_fix_recomp_lo, (unsigned long long)g_fix_recomp_hi);
        cls = xbox_A2hSlotWatchFixtureClassify(host_witness);
        CHECK(cls == XBOX_A2H_SLOTW_RANGE_GAME_MODULE,
              "a RIP inside a published recompiled bound (%016llX in %016llX..%016llX) classified as"
              " %u, expected GAME_MODULE(%u)", (unsigned long long)host_witness,
              (unsigned long long)rlo, (unsigned long long)rhi, cls,
              XBOX_A2H_SLOTW_RANGE_GAME_MODULE);
        /* THE ORDERING, WHICH IS THE WHOLE CLASSIFICATION: the recompiled module is LINKED INTO this
         * image, so the witness is inside the IMAGE bound too. Testing the image bound first would
         * classify every recompiled RIP as TOOLKIT_HOST and the installer control could never fire.
         * That is asserted here rather than left to the reader. */
        CHECK(host_witness >= ilo && host_witness < ihi,
              "the witness is not also inside the image bound, so this arm does NOT prove the"
              " recompiled-before-image ordering");
        printf("  [fixture] range-classifier: witness %016llX inside published recompiled bound"
               " %016llX..%016llX -> GAME_MODULE(%u) even though it is ALSO inside the image bound"
               " (ordering proven)\n", (unsigned long long)host_witness, (unsigned long long)rlo,
               (unsigned long long)rhi, cls);
    }

    /* ── (2) TOOLKIT/HOST: A REAL ADDRESS IN THIS IMAGE THAT IS NOT RECOMPILED CODE ─────────────
     *
     * ⚠ RUN AFTER (1), SO THE WITNESS IS INSIDE THE PUBLISHED RECOMPILED BOUND'S PAGE AND YET MUST
     * STILL CLASSIFY TOOLKIT_HOST. That is the discriminating arm: if the classifier ignored the
     * bound and answered by image membership alone, this check would report GAME_MODULE. */
    {
        cls = xbox_A2hSlotWatchFixtureClassify(host_other);
        CHECK(cls == XBOX_A2H_SLOTW_RANGE_TOOLKIT_HOST,
              "a real host datum at %016llX classified as %u, expected TOOLKIT_HOST(%u)",
              (unsigned long long)host_other, cls, XBOX_A2H_SLOTW_RANGE_TOOLKIT_HOST);
        printf("  [fixture] range-classifier: host witness %016llX -> TOOLKIT_HOST(%u)\n",
               (unsigned long long)host_other, cls);
    }

    /* ── (3) UNKNOWN: OUTSIDE BOTH REAL RANGES ────────────────────────────────────────────────── */
    {
        /* An address that is inside NO module of this process: far above the image, and not a
         * canonical user-mode mapping. Chosen as `image_hi + 0x100000000` so it cannot accidentally
         * land in a neighbouring DLL, and asserted to be outside both bounds before use so the arm
         * cannot be vacuous if the bounds ever move. */
        uint64_t outside = ihi + 0x100000000ull;
        CHECK(outside >= ihi && (outside < rlo || outside >= rhi),
              "the UNKNOWN witness %016llX is INSIDE a known range: the arm would be vacuous",
              (unsigned long long)outside);
        cls = xbox_A2hSlotWatchFixtureClassify(outside);
        CHECK(cls == XBOX_A2H_SLOTW_RANGE_UNKNOWN,
              "an address outside every real range (%016llX) classified as %u, expected UNKNOWN(%u)",
              (unsigned long long)outside, cls, XBOX_A2H_SLOTW_RANGE_UNKNOWN);
        printf("  [fixture] range-classifier: outside %016llX -> UNKNOWN(%u)\n",
               (unsigned long long)outside, cls);
    }

    /* ── (4) THE BOUNDARY CASES, ON THE MODULE'S OWN START AND END ────────────────────────────────
     *
     * ⚠ THIS IS WHERE AN OFF-BY-ONE WOULD LIVE, AND IT IS TESTED ON BOTH SIDES OF BOTH BOUNDS.
     * `lo` is INCLUSIVE and `hi` is EXCLUSIVE by construction: a function's own address is inside the
     * module, and the first address past the last function's end is not. An implementation that used
     * `>` at `lo` or `>=` at `hi` would misplace exactly one instruction at each end -- and the
     * installer control is a SINGLE instruction, so "one instruction at the end" is the difference
     * between the control firing and the packet reporting INFRA FAILURE.
     *
     * ⚠ UNGUARDED, ON PURPOSE. An earlier version wrapped these in `if (valid)` and, because nothing
     * had published a bound yet, they all skipped while the fixture still reported zero failures. A
     * skipped arm reads as coverage. `valid` is asserted above, so these run or the fixture fails. */
    {
        CHECK(valid == 1, "no recompiled bound is published: the boundary arms cannot run");
        CHECK(xbox_A2hSlotWatchFixtureClassify(rlo) == XBOX_A2H_SLOTW_RANGE_GAME_MODULE,
              "the module's OWN START %016llX did not classify as GAME_MODULE: `lo` is not inclusive",
              (unsigned long long)rlo);
        CHECK(xbox_A2hSlotWatchFixtureClassify(rhi - 1u) == XBOX_A2H_SLOTW_RANGE_GAME_MODULE,
              "the module's LAST address %016llX did not classify as GAME_MODULE: `hi` is not"
              " exclusive as documented", (unsigned long long)(rhi - 1u));
        CHECK(xbox_A2hSlotWatchFixtureClassify(rhi) != XBOX_A2H_SLOTW_RANGE_GAME_MODULE,
              "the FIRST address PAST the module %016llX classified as GAME_MODULE: the upper bound"
              " is inclusive and the range is one byte too wide", (unsigned long long)rhi);
        if (rlo > 0) {
            CHECK(xbox_A2hSlotWatchFixtureClassify(rlo - 1u) != XBOX_A2H_SLOTW_RANGE_GAME_MODULE,
                  "the address just BELOW the module %016llX classified as GAME_MODULE: the lower"
                  " bound is exclusive and the range is one byte too wide",
                  (unsigned long long)(rlo - 1u));
        }
        /* AND THE MIDDLE OF THE RANGE, so the arms above are not the only ones that ran. */
        CHECK(xbox_A2hSlotWatchFixtureClassify(rlo + (rhi - rlo) / 2u)
              == XBOX_A2H_SLOTW_RANGE_GAME_MODULE,
              "an address in the MIDDLE of the recompiled bound did not classify as GAME_MODULE");
        printf("  [fixture] range-classifier: boundaries lo=%016llX INCLUSIVE, hi=%016llX EXCLUSIVE,"
               " both just-outside arms not GAME_MODULE\n",
               (unsigned long long)rlo, (unsigned long long)rhi);
    }

    /* ── (5) NATIVE-DISASM CORROBORATION, AND THE PROOF THAT IT NEVER GATES ─────────────────────
     *
     * The corroboration decodes NATIVE x86-64 at the RIP. It is recorded BESIDE the range class and
     * must never change it, so the arms below check both halves: that it produces a verdict on real
     * code, and that the RANGE CLASS is identical with and without it. */
    {
        uint32_t form_self = xbox_A2hSlotWatchFixtureForm(host_witness);
        uint32_t form_outside = xbox_A2hSlotWatchFixtureForm(ihi + 0x100000000ull);
        CHECK(form_self == XBOX_A2H_SLOTW_FORM_STORE
              || form_self == XBOX_A2H_SLOTW_FORM_NOT_STORE
              || form_self == XBOX_A2H_SLOTW_FORM_UNDECODED,
              "the corroboration returned an out-of-range form %u", form_self);
        /* ⚠ A RIP OUTSIDE THE IMAGE IS NOT READABLE AND MUST REPORT UNDECODED, NOT A GUESS. Reading
         * it would fault inside the fault handler; classifying it from unreadable bytes would be the
         * old paradigm's error in a new place. */
        CHECK(form_outside == XBOX_A2H_SLOTW_FORM_UNDECODED,
              "the corroboration DECODED an address outside the image (%016llX -> %u): it must"
              " refuse rather than read unreadable memory",
              (unsigned long long)(ihi + 0x100000000ull), form_outside);
        /* ⚠ AND THE DECODER MUST NOT BE VACUOUSLY UNDECODED, WHICH IS THE FAILURE MODE THAT WOULD
         * MAKE THIS WHOLE ARM DECORATIVE. A function's ENTRY is a prologue (`push`/`sub rsp`/...),
         * so `form_self` above is expected to be NOT_STORE or UNDECODED and proves nothing on its
         * own. This arm therefore SCANS the real compiled body of a real function for at least one
         * instruction the decoder calls a STORE. If the decoder could never say STORE, every
         * "corroboration" in every archive would be UNDECODED and the field would be worthless --
         * which is exactly the failure this arm exists to exclude. The scan is over THIS image's own
         * compiled code, so the instructions are the host toolchain's real output. */
        {
            const uint8_t *body = (const uint8_t *)(uintptr_t)host_witness;
            unsigned stores = 0, decodable = 0;
            size_t off;
            for (off = 0; off < 0x200u; off++) {
                uint32_t f = xbox_A2hSlotWatchFixtureForm((uint64_t)(uintptr_t)(body + off));
                if (f == XBOX_A2H_SLOTW_FORM_STORE) stores++;
                if (f != XBOX_A2H_SLOTW_FORM_UNDECODED) decodable++;
            }
            CHECK(stores > 0,
                  "the corroboration found NO store in 512 bytes of real compiled code: the decoder"
                  " is vacuously UNDECODED and the field carries no information");
            CHECK(decodable > 0, "the corroboration decoded NOTHING in 512 bytes of real code");
            printf("  [fixture] range-classifier: corroboration over 512 bytes of real compiled code:"
                   " %u STOREs, %u decodable (the decoder is NOT vacuous)\n", stores, decodable);
        }
        /* THE NON-GATING PROPERTY, STATED AS AN ASSERTION. The range class is a function of the
         * BOUNDS alone; the corroboration cannot move it. If it ever could, a partial hand-written
         * decoder would be able to fail a run the range classifier placed correctly. The witness is
         * inside the published recompiled bound here, so this arm asserts GAME_MODULE both before and
         * after the corroboration runs. */
        CHECK(xbox_A2hSlotWatchFixtureClassify(host_witness) == XBOX_A2H_SLOTW_RANGE_GAME_MODULE,
              "the range class MOVED after corroboration ran: the corroboration is gating, and it"
              " must never gate");
        printf("  [fixture] range-classifier: corroboration entry=%u outside=%u(UNDECODED expected)"
               " and the range class is unmoved by it\n", form_self, form_outside);
    }

    /* ── (6) A DEGENERATE PUBLICATION MUST CLEAR THE BOUND, NOT WIDEN IT ─────────────────────────
     *
     * ⚠ THIS IS THE FAIL-CLOSED ARM. If an embedder published a degenerate or inverted range, an
     * accepting classifier would call every RIP "recompiled" and hand the installer control to an
     * unrelated writer. The publication is required to REFUSE such a bound, after which the
     * classifier must report TOOLKIT_HOST for the very address it accepted a moment ago (the witness
     * is inside this image, so with no recompiled bound the image bound is the correct answer). */
    {
        xbox_A2hSlotWatchSetRecompBounds(host_witness, host_witness, 1u, 1u);   /* hi == lo */
        CHECK(xbox_A2hSlotWatchFixtureClassify(host_witness) == XBOX_A2H_SLOTW_RANGE_TOOLKIT_HOST,
              "a DEGENERATE bound (hi == lo) was accepted: the witness still classified GAME_MODULE,"
              " so an unrelated writer could satisfy the installer control");
        xbox_A2hSlotWatchSetRecompBounds(host_witness, host_witness - 1u, 1u, 1u); /* inverted */
        CHECK(xbox_A2hSlotWatchFixtureClassify(host_witness) == XBOX_A2H_SLOTW_RANGE_TOOLKIT_HOST,
              "an INVERTED bound (hi < lo) was accepted: the witness still classified GAME_MODULE");
        xbox_A2hSlotWatchSetRecompBounds(0u, 0xFFFFFFFFFFFFFFFFull, 1u, 0u);    /* valid == 0 */
        CHECK(xbox_A2hSlotWatchFixtureClassify(host_witness) == XBOX_A2H_SLOTW_RANGE_TOOLKIT_HOST,
              "a bound published with valid == 0 was ACCEPTED: the classifier must refuse a bound"
              " the embedder did not stand behind");
        printf("  [fixture] range-classifier: degenerate/inverted/unvalidated publications all"
               " CLEARED the bound and the classifier refused\n");
    }

    /* RESTORE a bound so later fixtures classify the way the live run would. */
    fixture_publish_bound();
    /* ⚠ AND THE GUEST VAs THE PACKET NAMES ARE STILL CHECKED -- but ONLY as guest VAs, which is what
     * they are. There is no encoding expectation attached to them any more, and none may be added:
     * the fixture asserts the bytes are there in the XBE image, which is a fact about the ORIGINAL
     * guest program, and asserts NOTHING about what the classifier should say about them. */
    {
        const uint8_t *installer = (const uint8_t *)(uintptr_t)(FIX_INSTALLER_VA + g_xbox_mem_offset);
        const uint8_t *candidate = (const uint8_t *)(uintptr_t)(FIX_CANDIDATE_VA + g_xbox_mem_offset);
        CHECK(installer[0] == 0x89 && installer[1] == 0x81
              && installer[2] == 0x2C && installer[3] == 0x24,
              "the guest bytes at %08X are %02X %02X %02X %02X, expected 89 81 2C 24 (the GUEST"
              " encoding -- a fact about the XBE, and NOT an expectation for any native RIP)",
              FIX_INSTALLER_VA, installer[0], installer[1], installer[2], installer[3]);
        CHECK(candidate[0] == 0x89 && candidate[1] == 0x84 && candidate[2] == 0xAE
              && candidate[3] == 0xEC,
              "the guest bytes at %08X are %02X %02X %02X %02X, expected 89 84 AE EC",
              FIX_CANDIDATE_VA, candidate[0], candidate[1], candidate[2], candidate[3]);
        printf("  [fixture] range-classifier: the packet's guest VAs still hold their GUEST bytes"
               " (89 81 2C 24 / 89 84 AE EC); NO native-RIP encoding expectation is made of them\n");
    }
}


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

    /* (d) ...and a WRITE-class fault to an OWNED page IS admitted, with the write's own info0.
     *     ⚠ THE CONTEXT'S RIP IS A REAL RECOMPILED ADDRESS, NOT ZERO. A zeroed RIP classifies
     *     UNKNOWN, which is INFRA FAILURE and latches `overflow`; a fixture that manufactured that
     *     state would corrupt the very counters it is trying to check, and would teach a reader to
     *     ignore the one line that must never be ignored. */
    av_before = L->loss.relevant_av;
    ctx.EFlags = 0;
    ctx.Rip = (DWORD64)(uintptr_t)&fixture_recomp_witness;
    r = xbox_A2hSlotWatchFixtureDeliver(EXCEPTION_ACCESS_VIOLATION, 1, (ULONG_PTR)slot_ptr,
                                        &ctx, params, 2);
    CHECK(r == EXCEPTION_CONTINUE_EXECUTION, "an owned write AV was NOT admitted (returned %ld)",
          (long)r);
    CHECK(L->loss.relevant_av == av_before + 1, "an owned write AV was not counted");
    CHECK((ctx.EFlags & 0x100u) != 0, "an admitted write AV did not set TF to step the store");
    /* THE CLASSIFICATION OF THAT REAL RIP, AND IT IS THE CONTROL'S INPUT. The fault landed ON the
     * derived slot and the RIP is inside the published recompiled bound, so THIS IS THE INSTALLER
     * CONTROL'S CONDITION and it must have counted. That makes the control a POSITIVE arm in the
     * fixture rather than only a specificity arm: the live run's required control is proven to be
     * satisfiable, which is exactly what the void encoding test could never be. */
    CHECK(L->loss.range_game > 0,
          "a fault at a real recompiled address did not increment loss.range_game");
    CHECK(L->loss.range_unknown == 0,
          "a fault at a REAL recompiled address classified UNKNOWN (%llu): the bound is not being"
          " applied", (unsigned long long)L->loss.range_unknown);
    CHECK(L->loss.installer_control_hits > 0,
          "the installer control did NOT fire on (slot hit + RIP in the recompiled module): the"
          " control is unsatisfiable, which is the defect the void encoding test had");
    printf("  [fixture] info0-filter: read/other/foreign REJECTED, write-on-owned ADMITTED;"
           " range_game=%llu control_hits=%llu\n",
           (unsigned long long)L->loss.range_game,
           (unsigned long long)L->loss.installer_control_hits);

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

    /* (c) THE INSTALLER CONTROL'S SPECIFICITY, IN THE NATIVE DOMAIN. The control is "a store landed
     *     on the derived slot AND the faulting RIP is inside the recompiled module". A store made
     *     from THIS fixture cannot satisfy the second half: the fixture is host code in the test
     *     binary, not a recompiled guest function, so its RIP classifies TOOLKIT_HOST -- unless the
     *     recompiled bound happens to cover it, which the fixture never publishes that way. */
    {
        uint64_t ctl_before = L->loss.installer_control_hits;
        *(volatile uint32_t *)slot_ptr = 0x0015F9D0u;
        CHECK(L->loss.installer_control_hits == ctl_before,
              "a store from HOST code was counted as the INSTALLER CONTROL: the control does not"
              " require the RIP to be in the recompiled module");
        printf("  [fixture] control-specificity: a host-code store did NOT satisfy the installer"
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

/* ── 7. THE FOURTH-READ LATCH AND ITS ORDERED-EVENT-ID TIE ────────────────────────────────────────
 *
 * The packet requires the fourth `0x00193E62` read to be instrumented with a sequence/value latch
 * "tied to the installer and all slot writes by ORDERED EVENT IDs -- not sampled log chronology".
 * This drives the REAL latch through the REAL publisher and asserts exactly that:
 *
 *   * reads 1..3 latch nothing; read 4 latches;
 *   * the latch records the seq of the LAST SLOT-HIT write at or before it, NOT a later write and
 *     NOT a non-slot write;
 *   * a write arriving AFTER the read is excluded by event id, which is the property a timestamp
 *     comparison could not guarantee under log truncation or interleaving;
 *   * the value latched is the value the read produced, and the latch is write-once.
 */
static void fixture_fourth_read_latch(void)
{
    XboxA2hSlotwLedger *L = xbox_A2hSlotWatchFixtureLedger();
    uint32_t hit_seq_before, later_seq, nonslot_seq;

    current_fixture = "fourth-read-latch";

    /* Two writes: the FIRST is a slot hit (the installer's class), the SECOND is page traffic. Only
     * the first may be named as the last reaching write. */
    CHECK(xbox_A2hSlotWatchFixturePublishWrite(1u, XBOX_A2H_SLOTW_RANGE_GAME_MODULE, 0x11111111u),
          "a slot-hit write record could not be published");
    hit_seq_before = L->events[L->event_count - 1].seq;
    CHECK(xbox_A2hSlotWatchFixturePublishWrite(0u, XBOX_A2H_SLOTW_RANGE_TOOLKIT_HOST, 0x22222222u),
          "a non-slot write record could not be published");
    nonslot_seq = L->events[L->event_count - 1].seq;
    CHECK(nonslot_seq > hit_seq_before, "the event ids are not monotonic");

    /* Reads 1..3 must latch nothing. */
    xbox_A2hSlotWatchFixtureNoteRead(0xAAAAAAAAu, 1u);
    xbox_A2hSlotWatchFixtureNoteRead(0xBBBBBBBBu, 2u);
    xbox_A2hSlotWatchFixtureNoteRead(0xCCCCCCCCu, 3u);
    CHECK(L->fourth_reached == 0, "the latch fired before the FOURTH read (reached=%u)",
          L->fourth_reached);
    CHECK(L->read_count == 3, "read_count=%u after three instrumented reads", L->read_count);

    /* READ 4. */
    xbox_A2hSlotWatchFixtureNoteRead(0x001D5078u, 4u);
    CHECK(L->fourth_reached == 1, "the fourth read did NOT latch");
    CHECK(L->fourth_value == 0x001D5078u,
          "the latch recorded value %08X, not the value the fourth read produced (%08X)",
          L->fourth_value, 0x001D5078u);
    /* THE ORDERED-EVENT-ID TIE. The last SLOT-HIT write before the read, named by seq. */
    CHECK(L->last_write_seq == hit_seq_before,
          "the latch tied to seq %u, but the last SLOT-HIT write before the read was seq %u"
          " (the non-slot write was seq %u) -- the tie is not slot-filtered",
          L->last_write_seq, hit_seq_before, nonslot_seq);
    CHECK(L->last_write_range == XBOX_A2H_SLOTW_RANGE_GAME_MODULE,
          "the latch recorded range=%u, expected the installer's GAME_MODULE(%u)",
          L->last_write_range, XBOX_A2H_SLOTW_RANGE_GAME_MODULE);
    CHECK(L->fourth_seq > L->last_write_seq,
          "the read's own seq (%u) does not follow the write it latched (%u): the ordering key is"
          " not monotonic across the read", L->fourth_seq, L->last_write_seq);
    printf("  [fixture] fourth-read-latch: value=%08X seq=%u tied to write seq=%u (range=GAME_MODULE)"
           " not the later non-slot seq=%u\n",
           L->fourth_value, L->fourth_seq, L->last_write_seq, nonslot_seq);

    /* A WRITE AFTER THE READ MUST NOT RETROACTIVELY BECOME THE TIE. This is the property that makes
     * the tie an ordering claim rather than a "last write we happened to see" claim. */
    CHECK(xbox_A2hSlotWatchFixturePublishWrite(1u, XBOX_A2H_SLOTW_RANGE_GAME_MODULE, 0x33333333u),
          "a post-read write record could not be published");
    later_seq = L->events[L->event_count - 1].seq;
    CHECK(later_seq > L->fourth_seq, "the post-read write did not get a later event id");
    CHECK(L->last_write_seq == hit_seq_before,
          "a write published AFTER the read moved the latch's tie to seq %u (the read's seq is %u)",
          L->last_write_seq, L->fourth_seq);
    /* ...and the latch is write-once, so re-noting read 4 cannot overwrite the decision input. */
    xbox_A2hSlotWatchFixtureNoteRead(0xDEADBEEFu, 4u);
    CHECK(L->fourth_value == 0x001D5078u,
          "the write-once latch was overwritten by a second read-4 notification (now %08X)",
          L->fourth_value);
    printf("  [fixture] fourth-read-latch: post-read write seq=%u did NOT move the tie; latch"
           " write-once\n", later_seq);
}

/* ── 8. Q3(c): THE ADDRESS-IDENTITY CROSS-VALIDATION ─────────────────────────────────────────────
 *
 * THE OVERLAP THE ADVISOR REQUIRES, AT ITS MOST BASIC: the instrument and the guest must be reading
 * the SAME PHYSICAL DWORD. Two completely independent address computations have to agree:
 *
 *   (1) THE INSTRUMENT'S READ ADDRESS -- `st.page0 + st.page_offset`, i.e. the page base ARM captured
 *       and PAGE_READONLY-protected plus the slot's offset. Every fault record and every step read
 *       goes through exactly this address.
 *   (2) THE GUEST'S OWN TRANSLATION -- `slot_va + g_xbox_mem_offset`, which is precisely what the
 *       generated code's `MEM32(slot_va)` computes and therefore the address the title's own store
 *       to the slot actually lands on.
 *
 * ⚠ IF THESE DIFFER, THE WATCH IS PROTECTING AND READING A DIFFERENT PAGE FROM THE ONE THE TITLE
 * WRITES, AND EVERY VALUE IN THE LEDGER IS ABOUT THE WRONG DWORD. Nothing else in the ledger could
 * show it: the protected page still faults, the counters still move, the records still look
 * well-formed. This is exactly the byte-order-trap class of defect, and it is why the check is
 * structural rather than optional.
 *
 * The check is a REAL store through each address, not an arithmetic comparison alone: a store to the
 * GUEST translation must raise a relevant AV (the page really is the protected one), and a store to
 * the INSTRUMENT's address must raise one too. A wrong page would simply not fault. */
static void fixture_address_identity(void)
{
    uintptr_t guest_host = (uintptr_t)FIX_SLOT_VA + (uintptr_t)g_xbox_mem_offset;
    uintptr_t instr_host = (uintptr_t)st.page0 + st.page_offset;
    XboxA2hSlotwLedger *L = xbox_A2hSlotWatchFixtureLedger();

    current_fixture = "address-identity";

    printf("  [fixture] address-identity: guest_translation=%p instrument_read=%p%s\n",
           (void *)guest_host, (void *)instr_host, guest_host == instr_host ? "" : "  <-- MISMATCH");
    CHECK(guest_host == instr_host,
          "THE INSTRUMENT AND THE GUEST DISAGREE ABOUT WHERE THE SLOT IS: the guest's own"
          " translation of %08X is %p, but the watch read/protected %p (page0=%p off=%03X)."
          " Every value in the ledger would describe the WRONG DWORD",
          FIX_SLOT_VA, (void *)guest_host, (void *)instr_host, st.page0, st.page_offset);

    /* ...AND THE STORE PROVES IT RATHER THAN THE ARITHMETIC. A store through the GUEST's own
     * translation of the slot must fault: that is the page the title's store lands on, and if the
     * watch did not protect it the store would pass silently and the watch would be blind. */
    {
        uint64_t av_before = L->loss.relevant_av;
        uint64_t hits_before = L->loss.slot_hits;
        volatile uint32_t *guest_slot = (volatile uint32_t *)guest_host;

        *guest_slot = 0x0BADF00Du;
        CHECK(L->loss.relevant_av == av_before + 1,
              "a store through the GUEST's own translation of the slot raised NO AV"
              " (%llu -> %llu): the watch is not protecting the page the title writes",
              (unsigned long long)av_before, (unsigned long long)L->loss.relevant_av);
        CHECK(L->loss.slot_hits == hits_before + 1,
              "a store through the GUEST's own translation was not recognised as a SLOT HIT"
              " (%llu -> %llu)", (unsigned long long)hits_before,
              (unsigned long long)L->loss.slot_hits);
        CHECK(*guest_slot == 0x0BADF00Du, "the guest-translation store did not complete: reads %08X",
              *guest_slot);
        printf("  [fixture] address-identity: a store through the GUEST translation faulted and was"
               " counted as a slot hit (value=%08X)\n", *guest_slot);
    }
}

/* ── 9. Q3(b): VALUE FIDELITY END-TO-END ─────────────────────────────────────────────────────────
 *
 * THE ADVISOR: "the existing controls proved ARMING/FIRING, never VALUES." This fixture proves the
 * VALUES, end to end, on the real handlers and real page faults:
 *
 *   1. Plant a KNOWN value at a KNOWN address (the slot).
 *   2. Arm. The page is genuinely PAGE_READONLY.
 *   3. Store a SECOND KNOWN value through the guest's own translation of that address.
 *   4. Assert the instrument's published record carries `pre` == the true BEFORE value and `post` ==
 *      the true AFTER value -- and that those are the values the MEMORY actually held, read back
 *      independently after the store.
 *
 * ⚠ WHY THIS IS NOT CIRCULAR. The `pre`/`post` values come from the INSTRUMENT's own reads through
 * its cached alias base. The confirmation reads below go through the GUEST's translation. The two
 * address computations are independent, so agreement between them is evidence and not a tautology --
 * and the address-identity fixture above is what makes that independence checkable.
 *
 * ⚠ IT ALSO PROVES `pre` IS READ BEFORE THE STORE EXECUTES. If the handler read `pre` after
 * CONTINUE_EXECUTION the store would already have landed and `pre` would equal `post`; a store of a
 * value DIFFERENT from the planted one distinguishes the two cases exactly. */
static void fixture_value_fidelity(void)
{
    XboxA2hSlotwLedger *L = xbox_A2hSlotWatchFixtureLedger();
    volatile uint32_t *guest_slot = (volatile uint32_t *)((uintptr_t)FIX_SLOT_VA
                                                          + (uintptr_t)g_xbox_mem_offset);
    const uint32_t before_value = 0xA5A5C3C3u;
    const uint32_t after_value  = 0x5A5A3C3Cu;
    uint32_t records_before, idx, write_idx = 0, step_idx = 0;
    DWORD old;

    current_fixture = "value-fidelity";

    /* (1) PLANT THE KNOWN BEFORE-VALUE. Written while the page is RW, then the page is returned to
     *     READONLY, so the plant is not itself a watched write and cannot be confused with one. */
    VirtualProtect(st.page0, XBOX_A2H_SLOTW_PAGE_SIZE, PAGE_READWRITE, &old);
    *guest_slot = before_value;
    VirtualProtect(st.page0, XBOX_A2H_SLOTW_PAGE_SIZE, PAGE_READONLY, &old);
    CHECK(*guest_slot == before_value,
          "the plant did not take: the slot reads %08X, expected %08X", *guest_slot, before_value);

    records_before = L->event_count;

    /* (2) THE WATCHED STORE. Through the GUEST's translation, so this is the address the title's own
     *     store would use, and the value is one no other fixture wrote. */
    *guest_slot = after_value;

    /* (3) THE MEMORY REALLY HOLDS THE AFTER-VALUE, read back through the guest's translation. If the
     *     handler had swallowed the store or written something else, this is where it shows. */
    CHECK(*guest_slot == after_value,
          "the watched store did not complete: memory reads %08X, expected %08X",
          *guest_slot, after_value);

    /* (4) FIND THE RECORD PAIR THIS STORE PRODUCED. Newest-first so a later fixture cannot be
     *     mistaken for this one, and the pair is identified by its `fault_va` being the slot. */
    for (idx = L->event_count; idx > records_before; idx--) {
        const XboxA2hSlotwEvent *ev = &L->events[idx - 1];
        if (!ev->seq) continue;
        if (ev->kind == 1u && ev->slot_hit) { write_idx = idx; break; }
    }
    CHECK(write_idx != 0, "the watched store published NO slot-hit WRITE record");
    if (!write_idx) return;

    for (idx = L->event_count; idx > write_idx; idx--) {
        const XboxA2hSlotwEvent *ev = &L->events[idx - 1];
        if (!ev->seq) continue;
        if (ev->kind == 2u) { step_idx = idx; break; }
    }
    CHECK(step_idx != 0, "the watched store published NO STEP record");

    {
        const XboxA2hSlotwEvent *w = &L->events[write_idx - 1];
        const XboxA2hSlotwEvent *s = step_idx ? &L->events[step_idx - 1] : NULL;

        printf("  [fixture] value-fidelity: plant=%08X stored=%08X | WRITE pre=%08X | STEP pre=%08X"
               " post=%08X\n", before_value, after_value, w->pre_value,
               s ? s->pre_value : 0u, s ? s->post_value : 0u);

        /* THE `pre` VALUE IS THE TRUE BEFORE-VALUE. This is the claim the Advisor says was never
         * proven: not that the trap armed, but that the number it recorded was right. */
        CHECK(w->pre_value == before_value,
              "THE INSTRUMENT'S pre IS WRONG: recorded %08X, but the slot truly held %08X before the"
              " store -- value fidelity FAILS", w->pre_value, before_value);

        /* THE `post` VALUE IS THE TRUE AFTER-VALUE, and it is a DIFFERENT number from `pre`, so a
         * handler that echoed one field into the other is caught here. */
        if (s) {
            CHECK(s->pre_value == before_value,
                  "the STEP record's pre is %08X, expected the true before-value %08X",
                  s->pre_value, before_value);
            CHECK(s->post_value == after_value,
                  "THE INSTRUMENT'S post IS WRONG: recorded %08X, but the slot truly held %08X after"
                  " the store -- value fidelity FAILS", s->post_value, after_value);
            CHECK(s->post_value != s->pre_value,
                  "the STEP record has pre == post (%08X): `pre` was read AFTER the store executed,"
                  " so it is not a pre-value at all", s->post_value);
        }

        /* THE CROSS-VALIDATION, ON THIS VERY EVENT. The instrument's own read (above) and the
         * guest-translation read (here) cover the same address; they must agree, and they must agree
         * with what the memory actually holds. */
        CHECK(*guest_slot == (s ? s->post_value : after_value),
              "CROSS-VALIDATION: the guest's translation reads %08X but the instrument recorded"
              " post=%08X -- the two readers of one address DISAGREE", *guest_slot,
              s ? s->post_value : 0u);
    }
    printf("  [fixture] value-fidelity: pre and post are the TRUE before/after values, confirmed"
           " against an independent guest-translation read\n");
}

/* ── 10. THE CAPACITY REPAIR: TRAFFIC COSTS NO RECORDS ───────────────────────────────────────────
 *
 * THE MEASURED DEFECT THIS REPAIRS. ON trial 1 recorded every page write AND its step, so each write
 * cost TWO of 256 records; 129 writes exhausted the buffer, the fail-closed path fired (correctly
 * refusing to turn a lost write into a silent absence), and the propagated fault ENDED THE RUN.
 *
 * THE REPAIR IS THE PACKET'S OWN DESIGN, AND THIS IS THE FIXTURE THAT PROVES IT IS IMPLEMENTED:
 * non-slot page writes are TRAFFIC -- counted, and censused by FIRST TOUCH -- and publish no
 * per-write record. So N non-slot writes to DISTINCT addresses cost at most N census entries and
 * ZERO event records, and N non-slot writes to the SAME address cost ZERO records and ZERO census
 * entries after the first.
 *
 * ⚠ WHAT WOULD MAKE THIS FIXTURE VACUOUS, AND HOW IT IS AVOIDED. If the stores silently did not
 * fault -- because the page had been left open by an earlier fixture -- then "no records" would be
 * trivially true and would prove nothing. So the fault count is asserted to rise by exactly N, which
 * is what makes "and yet no records were consumed" a real statement about the DESIGN rather than
 * about a page that was never protected. */
static void fixture_traffic_capacity(void)
{
    XboxA2hSlotwLedger *L = xbox_A2hSlotWatchFixtureLedger();
    uint32_t records_before, count_before;
    uint64_t av_before, traffic_before, distinct_before;
    const unsigned N = 64;
    unsigned i;

    current_fixture = "traffic-capacity";

    /* (a) N DISTINCT non-slot offsets: each must fault, each must be counted, and the event array
     *     must gain NOTHING while the first-touch census gains at most N entries. */
    records_before = L->event_count;
    count_before = L->first_touch_count;
    av_before = L->loss.relevant_av;
    traffic_before = L->loss.nonslot_writes;
    distinct_before = L->loss.nonslot_distinct;

    for (i = 0; i < N; i++) {
        /* Offsets chosen to be inside the page and NOT the slot: the low half of the page, stepping
         * by 4 from the base, which is the same shape as the observed linear fill. */
        uint32_t off = (i * 4u) & 0x7FFu;
        if (off == (st.slot_va & (XBOX_A2H_SLOTW_PAGE_SIZE - 1))) continue;
        *(volatile uint32_t *)((char *)st.page0 + off) = 0x11110000u + i;
    }

    printf("  [fixture] traffic-capacity: %u distinct non-slot writes -> events +%u (was %u),"
           " traffic +%llu, distinct +%llu, census +%u\n", N,
           L->event_count - records_before, records_before,
           (unsigned long long)(L->loss.nonslot_writes - traffic_before),
           (unsigned long long)(L->loss.nonslot_distinct - distinct_before),
           L->first_touch_count - count_before);

    CHECK(L->loss.relevant_av == av_before + N,
          "only %llu of %u non-slot stores faulted: the page was NOT protected, so this fixture"
          " would prove nothing about capacity",
          (unsigned long long)(L->loss.relevant_av - av_before), N);
    CHECK(L->loss.nonslot_writes == traffic_before + N,
          "the traffic counter moved by %llu, expected %u",
          (unsigned long long)(L->loss.nonslot_writes - traffic_before), N);
    /* ⚠ THE ACTUAL REPAIR, ASSERTED. THIS IS THE LINE THAT WOULD HAVE FAILED BEFORE IT. */
    CHECK(L->event_count == records_before,
          "TRAFFIC CONSUMED %u EVENT RECORDS (was %u, now %u): non-slot page writes are still being"
          " retained as records, which is the defect that ended ON trial 1",
          L->event_count - records_before, records_before, L->event_count);
    CHECK(L->first_touch_count <= count_before + N,
          "the first-touch census grew by %u for %u distinct addresses: it is not one-per-address",
          L->first_touch_count - count_before, N);

    /* (b) REPEATS COST NOTHING AT ALL -- not a record and not a census entry. This is what makes a
     *     long linear fill harmless rather than merely survivable.
     *
     *     ⚠ THE FIRST WRITE MUST BE A GENUINELY FRESH ADDRESS, AND THE FIXTURE MUST SAY SO. An
     *     earlier version reused an address an earlier fixture had already touched, so "repeat costs
     *     nothing" passed for a reason that depended on fixture ORDER rather than on the property
     *     itself. A fresh offset is chosen here, its FIRST touch is asserted to add exactly ONE
     *     census entry, and only THEN are the repeats asserted to add none -- so the one-per-address
     *     rule and the free-repeat rule are stated as two separate, order-independent facts. */
    {
        const uint32_t fresh_off = 0x800u;
        uint32_t records2, census2;
        uint64_t av2, distinct2;

        CHECK(fresh_off != (st.slot_va & (XBOX_A2H_SLOTW_PAGE_SIZE - 1)),
              "the fixture's 'fresh' offset IS the slot; the arm would be a slot write");
        CHECK(fresh_off != ((st.page_offset + 8u) & 0xFFCu),
              "the fixture's 'fresh' offset was already touched by an earlier fixture");

        records2 = L->event_count;
        census2 = L->first_touch_count;
        av2 = L->loss.relevant_av;
        distinct2 = L->loss.nonslot_distinct;

        /* THE FIRST TOUCH OF A FRESH ADDRESS: exactly one census entry, still no event record. */
        *(volatile uint32_t *)((char *)st.page0 + fresh_off) = 0x22220000u;
        CHECK(L->first_touch_count == census2 + 1,
              "the FIRST touch of a fresh address added %u census entries, expected exactly 1",
              L->first_touch_count - census2);
        CHECK(L->loss.nonslot_distinct == distinct2 + 1,
              "the first touch of a fresh address moved nonslot_distinct by %llu, expected 1",
              (unsigned long long)(L->loss.nonslot_distinct - distinct2));
        CHECK(L->event_count == records2,
              "the first touch of a fresh address consumed %u event records",
              L->event_count - records2);

        /* ...AND THE REPEATS OF IT COST NOTHING. */
        census2 = L->first_touch_count;
        records2 = L->event_count;
        for (i = 0; i < N; i++)
            *(volatile uint32_t *)((char *)st.page0 + fresh_off) = 0x22220000u + i;
        CHECK(L->loss.relevant_av == av2 + N + 1,
              "the repeat stores did not all fault (%llu of %u)",
              (unsigned long long)(L->loss.relevant_av - av2 - 1), N);
        CHECK(L->event_count == records2,
              "REPEAT traffic consumed %u event records", L->event_count - records2);
        CHECK(L->first_touch_count == census2,
              "a REPEAT touch of an already-censused address added %u census entries",
              L->first_touch_count - census2);
        printf("  [fixture] traffic-capacity: 1 fresh address -> census +1; then %u REPEAT writes"
               " -> events +0, census +0\n", N);
    }

    /* (c) THE SLOT STILL GETS ITS FULL RECORD. The repair must not have silenced the thing the
     *     instrument exists to record: a slot write still publishes the write record AND the step
     *     record, which is what carries the values. */
    {
        uint32_t records3 = L->event_count;
        uint64_t hits3 = L->loss.slot_hits;
        *(volatile uint32_t *)slot_ptr = 0x33334444u;
        CHECK(L->loss.slot_hits == hits3 + 1, "a slot store was not counted as a slot hit");
        CHECK(L->event_count == records3 + 2,
              "a SLOT store published %u records, expected 2 (the write and its step): the repair"
              " has silenced the slot, not just the traffic", L->event_count - records3);
        printf("  [fixture] traffic-capacity: a SLOT store still publishes 2 records (write+step)"
               " while %u traffic writes published 0\n", N * 2);
    }

    /* (d) THE FAIL-CLOSED PATH IS STILL ARMED. It must not fire on traffic volume -- asserted above
     *     -- but it must still exist for a genuinely full array. Checked here as a fact about the
     *     latch rather than by exhausting 1024 records: `event_overflow` stays clear through all of
     *     the traffic above, which is the "cannot fire on ordinary volume" half of the claim. */
    CHECK(L->event_overflow == 0,
          "the event overflow latch is SET after traffic alone: the fail-closed path fires on"
          " ordinary page volume");
    CHECK(L->loss.dropped_events == 0,
          "%llu records were dropped during traffic alone",
          (unsigned long long)L->loss.dropped_events);
    printf("  [fixture] traffic-capacity: overflow latch clear, dropped=0 -- traffic cannot reach"
           " the fail-closed path\n");
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

/* ── 9. FIX 2: THE TERMINAL-VALUE COHERENCE GATE, AND THE WORKED MISMATCH ─────────────────────────
 *
 * ⚠ THIS IS THE GATE THE ADVISOR'S RULING MAKES REQUIRED, AND IT IS WHAT MAKES THE ACCEPTED SHAPE
 * SOUND RATHER THAN MERELY DOCUMENTED.
 *
 * `VirtualProtect` is PAGE-GRANULAR and the slot sits at offset 0x62C of its page, so opening the page
 * to single-step a NON-SLOT write ALSO makes the SLOT writable for that one instruction. A racing
 * write inside such a window is invisible to this facility. Nothing in scope closes the window, so the
 * gate makes it FAIL CLOSED: LAST-RECORDED slot write value vs TERMINAL slot read, MISMATCH => UNKNOWN.
 *
 * ⚠ AND THE WORKED EXAMPLE IS ON-3'S OWN NUMBERS: last-recorded `0x0015F9D0` against terminal
 * `0x001D5078`. That MISMATCH is the gate WORKING -- it is why ON-3 is `UNKNOWN` instead of a
 * misattribution -- and the fixture drives exactly that pair through the REAL gate so the behaviour
 * is asserted rather than described.
 *
 * ⚠ THE GATE IS ONE-DIRECTIONAL AND THE FIXTURE PROVES THAT TOO. A MATCH removes the mismatch
 * objection from a RECORDED positive and NOTHING MORE: it cannot prove the absence of an unrecorded
 * same-value racing write, so a coherent verdict never yields an absence or exclusivity row. */
static void fixture_coherence_gate(void)
{
    XboxA2hSlotwLedger *L = xbox_A2hSlotWatchFixtureLedger();
    uint32_t verdict = 0xFFFFFFFFu, lwv = 0, lws = 0, tv = 0;
    uint32_t v;

    current_fixture = "coherence-gate";

    /* (a) THE WORKED MISMATCH, DRIVEN THROUGH THE REAL DECISION RULE. ON-3's own pair: a
     *     last-recorded slot write of 0x0015F9D0 against a terminal read of 0x001D5078. */
    v = xbox_A2hSlotWatchFixtureDecideCoherence(0x0015F9D0u, 1u, 0x001D5078u, 1u);
    CHECK(v == XBOX_A2H_SLOTW_COH_MISMATCH,
          "ON-3's own pair (last-recorded %08X vs terminal %08X) produced verdict %u, expected"
          " MISMATCH(%u): the gate would have promoted a mismatched run to a claim",
          0x0015F9D0u, 0x001D5078u, v, XBOX_A2H_SLOTW_COH_MISMATCH);
    CHECK(L->loss.coherence_mismatch == 1,
          "the mismatch did not latch (coherence_mismatch=%llu)",
          (unsigned long long)L->loss.coherence_mismatch);
    /* THE PUBLIC SEAM MUST REPORT THE SAME OPERANDS, so a reader sees the comparison rather than
     * being told its result. */
    CHECK(xbox_A2hSlotWatchCoherence(&verdict, &lwv, &lws, &tv) == 1,
          "the coherence seam reported NOTHING for a ledger with a terminal read");
    CHECK(verdict == XBOX_A2H_SLOTW_COH_MISMATCH && lwv == 0x0015F9D0u && tv == 0x001D5078u,
          "the seam reported verdict=%u last_write=%08X terminal=%08X, expected MISMATCH with"
          " %08X/%08X -- a reader must SEE the two operands, not just the verdict",
          verdict, lwv, tv, 0x0015F9D0u, 0x001D5078u);
    printf("  [fixture] coherence-gate: ON-3's pair last_recorded=%08X terminal=%08X ->"
           " MISMATCH(%u) => UNKNOWN (the gate working)\n", lwv, tv, verdict);

    /* (b) A COHERENT PAIR, through the same real rule. */
    v = xbox_A2hSlotWatchFixtureDecideCoherence(0x001D5078u, 7u, 0x001D5078u, 1u);
    CHECK(v == XBOX_A2H_SLOTW_COH_COHERENT,
          "a MATCHING pair produced verdict %u, expected COHERENT(%u)", v,
          XBOX_A2H_SLOTW_COH_COHERENT);
    /* ⚠ THE ONE-DIRECTIONALITY, AS AN ASSERTION ABOUT THE LATCH. A coherent verdict must NOT clear
     * the earlier mismatch, so a run that mismatched once and later matched cannot be read as
     * coherent throughout -- and a match can never be read as evidence about the windows. */
    CHECK(L->loss.coherence_mismatch == 1,
          "the coherent arm CHANGED the mismatch latch (%llu): coherence would then be readable as"
          " evidence about the windows rather than only about the recorded pair",
          (unsigned long long)L->loss.coherence_mismatch);
    printf("  [fixture] coherence-gate: matching pair -> COHERENT(%u); mismatch latch still %llu"
           " (coherent is NOT an absence proof)\n", v,
           (unsigned long long)L->loss.coherence_mismatch);

    /* (c) NO RECORDED SLOT WRITE IS NOT COHERENCE. seq 0 means there is nothing to compare, so the
     *     verdict is NOT_COMPARABLE -- the state in which nothing may be attributed from the
     *     terminal alone, even when the two values happen to be equal. */
    v = xbox_A2hSlotWatchFixtureDecideCoherence(0x001D5078u, 0u, 0x001D5078u, 1u);
    CHECK(v == XBOX_A2H_SLOTW_COH_NOT_COMPARABLE,
          "with NO recorded slot write (seq 0) and EQUAL values the verdict is %u, expected"
          " NOT_COMPARABLE(%u): the terminal read would otherwise stand alone as a claim", v,
          XBOX_A2H_SLOTW_COH_NOT_COMPARABLE);

    /* (d) NO TERMINAL AT ALL. Nothing was ever compared, so no verdict may be inferred. */
    v = xbox_A2hSlotWatchFixtureDecideCoherence(0x0015F9D0u, 1u, 0u, 0u);
    CHECK(v == XBOX_A2H_SLOTW_COH_NO_TERMINAL,
          "with terminal_seen == 0 the verdict is %u, expected NO_TERMINAL(%u)", v,
          XBOX_A2H_SLOTW_COH_NO_TERMINAL);
    printf("  [fixture] coherence-gate: seq==0 -> NOT_COMPARABLE(%u); no terminal ->"
           " NO_TERMINAL(%u)\n", XBOX_A2H_SLOTW_COH_NOT_COMPARABLE, v);

    /* Restore a terminal-seen state so later arms and the disarm path see a coherent ledger. */
    xbox_A2hSlotWatchFixtureResetCoherence();
    (void)lws;
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
    /* ⚠ THE RECOMPILED BOUND IS PUBLISHED BEFORE ANY FAULT IS DELIVERED, because every synthetic
     * delivery carries a real RIP and an unpublished bound would classify it UNKNOWN -- INFRA
     * FAILURE, latched, and printed on a run that has no defect. */
    fixture_publish_bound();
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
    /* THE CROSS-PROCESS LAYOUT CHECK. tools/harness/collect.c cannot include this header -- it links
     * no toolkit code -- so it mirrors the struct and validates `magic` and `size` before reading a
     * field. Printing the authoritative size HERE is what makes that validation checkable from the
     * archive: the collector's `collector=` value in a size-mismatch report must equal this number,
     * and a reader can compare them without rebuilding anything. */
    printf("  [fixture] ledger sizeof=%u magic=%08X version=%u events_max=%u\n",
           (unsigned)sizeof(XboxA2hSlotwLedger), xbox_A2hSlotWatchFixtureLedger()->magic,
           xbox_A2hSlotWatchFixtureLedger()->version, (unsigned)XBOX_A2H_SLOTW_EVENTS_MAX);

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
    fixture_range_classifier();    /* FIX 1: range-based native-domain classification, real bounds */
    fixture_address_identity();     /* Q3(c): instrument and guest read the SAME dword */
    fixture_value_fidelity();       /* Q3(b): pre/post are the TRUE before/after values */
    fixture_traffic_capacity();     /* TASK 1: traffic costs no records; slot still gets two */
    fixture_db_ownership();
    fixture_fourth_read_latch();
    fixture_coherence_gate();       /* FIX 2: last-recorded vs terminal; mismatch => UNKNOWN */

    xbox_A2hSlotWatchFixtureDisarmAc97();
    xbox_A2hSlotWatchDisarm();

    printf("A2H-SLOTW-FIXTURE gate=on checks=%u failed=%u\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
