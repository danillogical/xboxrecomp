/**
 * NV2A register combiners, evaluated per pixel for the software rasteriser.
 *
 * Every pixel the NV2A writes goes through its register combiners: up to
 * eight general stages and a final stage that mix the vertex colours, the
 * four texture results, two constants per stage and fog. D3D's fixed-function
 * texture stages compile to them too, so a title never draws without them.
 * The executor used to stand in with "texel * diffuse" from stage 0, which is
 * right for a menu quad and wrong for anything that mixes two textures --
 * Burnout 3 composites its whole 3D scene into the frame that way.
 *
 * The encoding (ICW/OCW bit layout, input mappings, output scale/bias, mux,
 * final combiner) follows xemu's pixel-shader generator
 * (hw/xbox/nv2a/pgraph/glsl/psh.c); the comments in nv2a_combiner.c say where
 * each rule comes from. Portable C, no graphics API.
 */
#ifndef XBOXRECOMP_NV2A_COMBINER_H
#define XBOXRECOMP_NV2A_COMBINER_H

#include <stdint.h>

typedef struct {
    uint32_t color_icw[8], alpha_icw[8];    /* SET_COMBINER_COLOR/ALPHA_ICW */
    uint32_t color_ocw[8], alpha_ocw[8];    /* SET_COMBINER_COLOR/ALPHA_OCW */
    uint32_t factor0[8], factor1[8];        /* SET_COMBINER_FACTOR0/1 (ARGB) */
    uint32_t final0, final1;                /* SET_COMBINER_SPECULAR_FOG_CW0/1 */
    uint32_t final_c0, final_c1;            /* SET_SPECULAR_FOG_FACTOR */
    uint32_t control;                       /* SET_COMBINER_CONTROL */
    uint32_t stage_program;                 /* SET_SHADER_STAGE_PROGRAM */
    /* Bump environment: what stage modes 6 (BUMPENVMAP) and 7
     * (BUMPENVMAP_LUM) read. The matrix is stored canonically as
     * [m00, m01, m10, m11] per stage even though the method stream carries it
     * swizzled; see nv2a_pb_exec.c. Stage 0's words are discarded on the way
     * in (xemu pgraph.c), so its row keeps the documented D3D default 0.0 --
     * which is why a stage-0 mode 7 multiplies its texel by 0. */
    float bump_mat[4][4];               /* SET_TEXTURE_SET_BUMP_ENV_MAT
                                         * 0x1B28 + 4*i, i = 0..3; +0x40*stage */
    float bump_scale[4];                /* SET_TEXTURE_SET_BUMP_ENV_SCALE
                                         * 0x1B38; +0x40*stage */
    float bump_offset[4];               /* SET_TEXTURE_SET_BUMP_ENV_OFFSET
                                         * 0x1B3C; +0x40*stage */
    uint32_t other_stage_input;         /* SET_SHADER_OTHER_STAGE_INPUT 0x1E78:
                                         * bits 16-19 stage 2's delta source,
                                         * bits 20-23 stage 3's */
} Nv2aCombiner;

/* One pixel. Colours are r,g,b,a in [0,1]; `fog` is (fog colour, factor).
 * t[i] is what texture stage i produced. The result is clamped to [0,1]. */
void nv2a_rc_eval(const Nv2aCombiner *rc, const float v0[4],
                  const float v1[4], const float fog[4],
                  const float t[4][4], float out[4]);

/* D3DCOLOR (0xAARRGGBB) to r,g,b,a floats. */
void nv2a_rc_unpack(uint32_t argb, float out[4]);

#endif
