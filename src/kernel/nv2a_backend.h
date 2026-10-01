/*
 * NV2A render back-end interface.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) decodes what a title asks the GPU
 * to do. By default it carries that out itself, on the CPU, into guest memory.
 * A game project can instead register a back end -- a D3D11 renderer, say --
 * and the executor hands it the work in already-decoded form:
 *
 *   surface state + clears, triangles (transformed to surface pixels, with
 *   per-vertex colour and texel-space UVs), the texture each batch samples,
 *   and the flip that ends a frame.
 *
 * Everything is called on the NV2A poll thread, one call at a time, so a back
 * end may create its window and device lazily on the first call and pump its
 * window messages from flip().
 *
 * Fixed-function, pre-transformed and vertex-program batches all arrive the
 * same way: the executor transforms fixed-function vertices itself and runs
 * vertex programs on the CPU (vp_run), so a back end only ever sees surface
 * pixels. Blend, depth and alpha state arrive in Nv2aRenderState.
 *
 * ponytail: register-combiner state is not passed on. Extend Nv2aBatch when
 * it lands rather than adding callbacks.
 */
#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>

/* The colour surface being drawn into, in real (anti-aliased) pixels.
 * aa_sx/aa_sy give the anti-aliasing factor, so width/aa_sx is the logical
 * size the title thinks it renders at (e.g. 640x480). */
typedef struct {
    uint32_t color_va;          /* guest address of pixel (0,0) */
    uint32_t width, height;     /* clip rectangle size, real pixels */
    uint32_t pitch;             /* bytes per row */
    uint32_t bytes_per_pixel;   /* 2 or 4 */
    uint32_t aa_sx, aa_sy;      /* 1 or 2 each */
} Nv2aSurface;

/* A texture as the title programmed it. uv in Nv2aVertex are in texels. */
typedef struct {
    uint32_t offset;            /* guest address of texel (0,0) */
    uint32_t width, height;
    uint32_t pitch;             /* linear formats only */
    uint32_t color;             /* NV097 colour-format code */
    uint32_t addr_u, addr_v;    /* NV097 wrap mode per axis (1 wrap, 3 clamp) */
} Nv2aTexture;

typedef struct {
    float    x, y, z;           /* surface pixels; z as the title produced it */
    float    rhw;               /* 1/w, 1 for pre-transformed batches */
    uint32_t diffuse;           /* 0xAARRGGBB */
    float    u, v;              /* texels */
} Nv2aVertex;

/* Render state as the title set it, in NV2A's own (OpenGL) enum values:
 * blend factors GL_ZERO/GL_ONE/0x300..0x308/0x8001..0x8004, equations
 * GL_FUNC_ADD 0x8006 etc., compare functions GL_NEVER 0x200 .. GL_ALWAYS 0x207.
 * color_mask uses NV2A's bytes: A 0x01000000, R 0x00010000, G 0x100, B 0x1. */
typedef struct {
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t alpha_test_enable, alpha_func, alpha_ref;   /* ref 0..255 */
    uint32_t depth_test_enable, depth_func, depth_write;
    uint32_t color_mask;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t zeta_va;                                    /* 0: no depth surface */
    float    depth_min, depth_max;                       /* SET_CLIP_MIN/MAX */
} Nv2aRenderState;

typedef struct {
    const Nv2aVertex  *vertices;    /* triangle list: count is a multiple of 3 */
    uint32_t           count;
    const Nv2aTexture *texture;     /* NULL: untextured */
    const Nv2aRenderState *state;
} Nv2aBatch;

typedef struct {
    /* flags: CLEAR_SURFACE bits (Z 0x1, stencil 0x2, colour 0xF0).
     * zstencil: SET_ZSTENCIL_CLEAR_VALUE (depth in the top 24 bits for Z24S8). */
    void (*clear)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                  uint32_t flags, uint32_t argb, uint32_t zstencil);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
} Nv2aBackend;

/* Register (or, with NULL, remove) the back end. Call before the title starts
 * submitting work; the executor must also be enabled (RECOMP_PB_EXEC). */
void nv2a_backend_register(const Nv2aBackend *backend);

/* Decode a whole texture to 0xAARRGGBB, row-major, width*height entries.
 * Handles every format the executor can sample (swizzled, linear, DXT).
 * Returns 0 if the format is not supported. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

/* Where BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) values land: the guest VA
 * of the semaphore the title reads GPU progress from (for XDK D3D, the
 * pointer at device+0x30). The executor writes each release there once it
 * has executed everything before it -- which is
 * what lets D3D's fence waits and ring-space checks see real progress.
 *
 * ponytail: the title supplies the address instead of the executor resolving
 * the semaphore context DMA object through RAMIN. */
void nv2a_pb_set_semaphore_target(uint32_t guest_va);

/* ── Observation seam ────────────────────────────────────────────────────
 *
 * The executor's counters, for a caller that must not print or parse stderr.
 * `nv2a_pb_exec_report()` is the human-readable view of these same fields.
 *
 * THREADING CONTRACT. The counters are plain fields written by the submission
 * walk, which holds the PFIFO lock. These accessors take no lock of their own
 * -- deliberately, because the consumer already runs under that lock and a
 * self-locking accessor would deadlock against it. So a caller MUST either:
 *   - already be inside the consumer callback (the PFIFO lock is held), or
 *   - be a single-threaded fixture with no concurrent submission.
 * Any other caller (a GUI thread, say) may observe torn or stale values,
 * including a split 64-bit `pixels`. Do not call these from a thread that
 * races the walk without adding its own synchronisation. */
typedef struct {
    uint32_t draws;
    uint32_t clears;
    uint64_t pixels;
    uint32_t tris_drawn;
    uint32_t flips;         /* NV097_FLIP_INCREMENT_WRITE (0x12C) only */
    uint32_t flip_stalls;   /* NV097_FLIP_STALL (0x130): a swap that does not
                             * advance flip_write, so it is NOT in `flips` */
    uint32_t unhandled;     /* methods with no implementation */
} Nv2aPbExecCounters;

void nv2a_pb_exec_counters(Nv2aPbExecCounters *out);

/* Register the executor as the GPU core's committed-method consumer. Called
 * once at bring-up (before the guest runs) and gated on RECOMP_PB_EXEC by
 * presence, exactly like the legacy scan path. Idempotent.
 *
 * Why this exists: with the MMIO state owner installed, the ack worker's body
 * -- and with it the legacy PB scan that used to feed the executor -- is
 * retired, so a PB_EXEC run executed nothing and reported nothing. The
 * submission walk is the thing that actually knows which methods were
 * committed, so it feeds the executor directly.
 *
 * Call before the guest starts. There is no runtime swap: like
 * nv2a_backend_register, the registration is readiness, not a mode change. */
void nv2a_pb_exec_register_commit_consumer(void);
int  nv2a_pb_exec_consumer_registered(void);
/* Methods the consumer skipped because their class is not NV097. Same threading
 * contract as nv2a_pb_exec_counters. */
uint32_t nv2a_pb_exec_skipped_non_nv097(void);

#endif /* NV2A_BACKEND_H */
