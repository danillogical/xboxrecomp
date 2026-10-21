/*
 * Diagnostic coverage of the Vulkan back end's rc pipeline refusals.
 *
 * `vb_rc_supported` in nv2a_vk_backend.c refuses a combiner batch whose texture
 * stage mode is > 5 (bump environment, dot product) or whose stage mode 3 uses
 * a true cube map; such a batch is drawn stage-0 style and counted in
 * `[GPUVK] ... rc_fallback`. This header records WHICH rule refused the batch
 * the first time, so a later step can size the work from the title's own state.
 *
 * HEADER-ONLY ON PURPOSE. The game's Vulkan regressions include
 * nv2a_vk_backend.c directly with stubs and link neither xbox_video nor
 * xbox_kernel, so a separate module would add link dependencies to a dozen
 * fixtures. Every translation unit that includes this header gets its own
 * private ledger; the PRODUCTION instance is the one inside nv2a_vk_backend.c,
 * which is the only one the back end writes and reports. A fixture includes
 * this header for the same rule and ledger semantics in its own unit. The
 * ledger has internal linkage, so its stable name, `g_nv2a_vk_rc_coverage`,
 * still appears in the PDB for the back end's object.
 *
 * No Vulkan include, no allocation, no lock, and no rendering decision reads
 * the counters. Recording happens only while RECOMP_PB_COVERAGE=1 (checked
 * here), on the submission-walk thread, which is where the executor calls
 * backend draw entries. There is no reset: counts are cumulative for the
 * process.
 *
 * GATE. The master gate is the executor's `pb_coverage()` in nv2a_pb_exec.c
 * (exact value "1", read once). This header keeps its own read-once cache of
 * the same variable with the same exact-1 test, because reaching the executor's
 * cache would need a kernel symbol these fixtures do not link. Both caches are
 * sampled before any draw and the environment does not change during a
 * process, so they cannot disagree. With the gate off, `vb_draw` records
 * nothing and the [PBCOV] line is never printed.
 *
 * The ledger has a stable layout for a PDB/collector reader: check magic,
 * version and size before reading the rest; `first_valid` is written last and
 * the first record is never rewritten.
 */
#ifndef XBOXRECOMP_NV2A_VK_RC_COVERAGE_H
#define XBOXRECOMP_NV2A_VK_RC_COVERAGE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define NV2A_VK_RC_COVERAGE_MAGIC   0x43524B56u   /* 'VKRC' */
#define NV2A_VK_RC_COVERAGE_VERSION 1u

/* first_reason: 1 = stage mode > 5, 2 = stage mode 3 with a cube texture.
 * first_stage_program is the raw SET_SHADER_STAGE_PROGRAM value. cube_mask has
 * one bit per texture stage (bit 0 = stage 0) for the stages that carried a
 * cube texture in that batch. */
typedef struct {
    uint32_t magic;      /* NV2A_VK_RC_COVERAGE_MAGIC */
    uint32_t version;    /* NV2A_VK_RC_COVERAGE_VERSION */
    uint32_t size;       /* sizeof(Nv2aVkRcCoverage) */
    uint64_t total;      /* == the back end's aggregate rc_fallback */
    uint64_t mode_gt5;
    uint64_t cube_mode3;
    uint32_t first_valid;          /* written LAST */
    uint32_t first_reason;         /* 1 mode > 5, 2 cube-mode 3 */
    uint32_t first_stage;
    uint32_t first_mode;
    uint32_t first_stage_program;  /* raw */
    uint32_t first_cube_mask;
    uint32_t reserved;
} Nv2aVkRcCoverage;

static Nv2aVkRcCoverage g_nv2a_vk_rc_coverage = {
    NV2A_VK_RC_COVERAGE_MAGIC,
    NV2A_VK_RC_COVERAGE_VERSION,
    (uint32_t)sizeof(Nv2aVkRcCoverage),
};

/* Exact value "1", read once. Empty, unset, "01", "true" and every other value
 * are inert, matching the executor's gate. */
static inline int nv2a_vk_rc_coverage_enabled(void)
{
    static int c = -1;

    if (c < 0) {
        const char *v = getenv("RECOMP_PB_COVERAGE");
        c = v != NULL && v[0] == '1' && v[1] == '\0';
    }
    return c;
}

/* The SINGLE rule the back end and its fixtures share: can the register-combiner
 * pipeline draw `stage_program` with the given cube-texture mask?
 *
 * Returns 0 when every stage is supported, 1 for a stage mode > 5, 2 for stage
 * mode 3 with a cube texture. Stages are scanned 0..3, and for each stage the
 * mode > 5 test comes before the cube test, exactly as vb_rc_supported did. Only
 * bits 0..19 can decide a 5-bit mode, so the caller need not mask the word.
 * On a refusal *stage / *mode name the refusing stage (either may be NULL); on a
 * supported result both are set to 0. */
static inline uint32_t nv2a_vk_rc_refusal(uint32_t stage_program, uint32_t cube_mask,
                                          uint32_t *stage, uint32_t *mode)
{
    uint32_t st;

    if (stage)
        *stage = 0;
    if (mode)
        *mode = 0;
    for (st = 0; st < 4; st++) {
        uint32_t m = (stage_program >> (st * 5)) & 0x1Fu;

        /* Per stage the mode > 5 test comes first: a bump-environment mode is
         * refused as such even if the same stage also has a cube texture. */
        if (m > 5) {
            if (stage)
                *stage = st;
            if (mode)
                *mode = m;
            return 1;
        }
        if (m == 3 && ((cube_mask >> st) & 1u)) {
            if (stage)
                *stage = st;
            if (mode)
                *mode = m;
            return 2;
        }
    }
    return 0;
}

/* Count one refusal: total plus exactly one reason counter, and claim the first
 * record once (valid last). Call only when the coverage gate is on. */
static inline void nv2a_vk_rc_coverage_record(uint32_t reason, uint32_t stage, uint32_t mode,
                                              uint32_t stage_program, uint32_t cube_mask)
{
    Nv2aVkRcCoverage *c = &g_nv2a_vk_rc_coverage;

    if (!nv2a_vk_rc_coverage_enabled())
        return;
    c->total++;
    if (reason == 1)
        c->mode_gt5++;
    else if (reason == 2)
        c->cube_mode3++;

    if (!c->first_valid && (reason == 1 || reason == 2)) {
        c->first_reason = reason;
        c->first_stage = stage;
        c->first_mode = mode;
        c->first_stage_program = stage_program;
        c->first_cube_mask = cube_mask;
        c->first_valid = 1;              /* LAST; never rewritten */
    }
}

static inline const Nv2aVkRcCoverage *nv2a_vk_rc_coverage(void)
{
    return &g_nv2a_vk_rc_coverage;
}

/* One compact line:
 *   [PBCOV] vk rc_fallback=%llu mode_gt5=%llu cube_mode3=%llu
 *           first=(reason=%u stage=%u mode=%u stage_program=%08X cube_mask=%X)
 * or `first=none` when no refusal was recorded. Read-only; prints the same
 * retained values every time. */
static inline void nv2a_vk_rc_coverage_report(void)
{
    const Nv2aVkRcCoverage *c = &g_nv2a_vk_rc_coverage;

    fprintf(stderr, "[PBCOV] vk rc_fallback=%llu mode_gt5=%llu cube_mode3=%llu first=",
            (unsigned long long)c->total,
            (unsigned long long)c->mode_gt5,
            (unsigned long long)c->cube_mode3);
    if (!c->first_valid) {
        fprintf(stderr, "none\n");
    } else {
        fprintf(stderr, "(reason=%u stage=%u mode=%u stage_program=%08X cube_mask=%X)\n",
                c->first_reason, c->first_stage, c->first_mode,
                (unsigned)c->first_stage_program, (unsigned)c->first_cube_mask);
    }
}

#endif /* XBOXRECOMP_NV2A_VK_RC_COVERAGE_H */
