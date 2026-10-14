/*
 * The ahead-of-time NV2A vertex-program corpus.
 *
 * V5 (game `plan-jsrf-gameplay.md`) compiles the title's vertex programs BEFORE
 * the process starts: the game's generator extracts the static programs from the
 * XBE, translates each with nv2a_vsh_hlsl (src/video/nv2a_vsh_hlsl.c) and
 * compiles the HLSL to SPIR-V with dxc. This header is the seam between that
 * generated table and the Vulkan back end, so neither owns the other's
 * representation and the back end stays free of the game's build layout.
 *
 * The back end NEVER translates or compiles a guest program at run time. A
 * program whose identity is not in the table is declined, and the executor's
 * CPU interpreter draws the batch instead (nv2a_vk_backend.h, draw_program).
 */
#ifndef NV2A_VSH_AOT_H
#define NV2A_VSH_AOT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One program, as the generator emitted it. `hash`, `len` and `input_mask` are
 * what nv2a_vsh_program_info (kernel/nv2a_vsh_interp.h) reports for exactly
 * `words`; `digest` is the independent identity check defined below. */
typedef struct {
    uint32_t hash;                 /* FNV-1a 32 over the words, as nv2a_vsh_program_info computes it */
    uint32_t len;                  /* instructions, up to and including the first FINAL */
    uint32_t input_mask;           /* bit n set: the program reads vn */
    uint64_t digest;               /* nv2a_vsh_aot_digest over the same words */
    const uint32_t (*words)[4];    /* len instructions; the identity check's source */
    const uint32_t *spirv;         /* vs_6_0 SPIR-V, built offline with -fvk-invert-y */
    size_t spirv_size;             /* SPIR-V size in bytes */
} Nv2aVshAotEntry;

/* The identity digest, defined here so the generator and the runtime cannot
 * disagree about it:
 *
 *   h = 0xcbf29ce484222325
 *   for each instruction i in 0..len-1, for each of its four dwords in order:
 *       h ^= (uint64_t)(uint32_t)dword;
 *       h *= 0x100000001b3;
 *
 * All arithmetic is modulo 2^64, and the dwords are mixed as values, not as
 * bytes. It is a second check independent of the 32-bit hash: it is recomputed
 * from the words at hand, so a table whose identity fields were corrupted for
 * one program cannot make another program's shader be selected. */
#define NV2A_VSH_AOT_DIGEST_SEED  0xcbf29ce484222325ull
#define NV2A_VSH_AOT_DIGEST_PRIME 0x100000001b3ull

static inline uint64_t nv2a_vsh_aot_digest(const uint32_t (*words)[4], uint32_t len)
{
    uint64_t h = NV2A_VSH_AOT_DIGEST_SEED;
    uint32_t i, j;

    for (i = 0; i < len; i++)
        for (j = 0; j < 4; j++) {
            h ^= (uint64_t)words[i][j];
            h *= NV2A_VSH_AOT_DIGEST_PRIME;
        }
    return h;
}

#ifdef __cplusplus
}
#endif

#endif /* NV2A_VSH_AOT_H */
