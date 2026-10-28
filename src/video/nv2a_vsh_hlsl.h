/*
 * NV2A vertex program to HLSL vertex shader.
 *
 * The executor can hand a back end a vertex-program batch untransformed
 * (Nv2aProgramBatch in kernel/nv2a_backend.h). This turns the program into an
 * HLSL vertex shader that computes what nv2a_vsh_interp.c computes per vertex,
 * then applies the same screen-to-clip transform the D3D11 back end's combiner
 * vertex shader applies to a CPU-transformed vertex, so its output feeds the
 * combiner pixel shaders unchanged.
 *
 * Portable C; no graphics API. The back end compiles and binds the result.
 */
#ifndef XBOXRECOMP_NV2A_VSH_HLSL_H
#define XBOXRECOMP_NV2A_VSH_HLSL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The combiner path's interpolants, written by the generated shader and read by
 * every combiner pixel shader. Colours and fog interpolate linearly in screen
 * space and texture coordinates perspective-correct, as the CPU program path does. */
#define NV2A_VSH_HLSL_VO_FIELDS \
    "float4 p : SV_Position; noperspective float4 d0 : COLOR0;\n" \
    "  noperspective float4 d1 : COLOR1; noperspective float fog : TEXCOORD4;\n" \
    "  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2;\n" \
    "  float4 t3 : TEXCOORD3; "

#define NV2A_VSH_HLSL_ENTRY     "vs_prog"
#define NV2A_VSH_HLSL_CB_CONST  1       /* register(b1): float4 c[192] */
#define NV2A_VSH_HLSL_CB_XF     2       /* register(b2): Nv2aVshHlslXf */

/* The second constant buffer: the epilogue and the fog unit. */
typedef struct {
    float xf[4];    /* 2/target_w, 2/target_h, 1/z_scale, unused */
    float fog[4];   /* enable (0 or 1), mode (as fog_factor), param 0, param 1 */
} Nv2aVshHlslXf;

/* Write the vertex shader for `len` instructions of `program` (ending at FINAL)
 * into `out`, NUL-terminated. Input vN is `float4 vN : ATTRn` for each bit n of
 * input_mask, in ascending n; no bits gives an SV_VertexID-only input.
 * Returns the length written, or 0 if it does not fit or there is no FINAL. */
int nv2a_vsh_hlsl(const uint32_t (*program)[4], uint32_t len, uint32_t input_mask,
                  char *out, size_t cap);

/* The input registers one instruction reads, as a bit mask over v0..v15. This is the single
 * definition of the input-mask rule: kernel/nv2a_vsh_interp.c (nv2a_vsh_program_info) and the
 * host tool (nv2a_vsh_hlsl_tool.c) both use it. A source with mux 2 makes the instruction's input
 * live whether or not the unit uses it. Sources read: A for any MAC op, B for the two-operand
 * ones, C for add/mad and every ILU op. Over-counting is safe. */
static inline uint32_t nv2a_vsh_ins_input_mask(const uint32_t ins[4])
{
    uint32_t mac = (ins[1] >> 21) & 0xF, ilu = (ins[1] >> 25) & 0x7;
    uint32_t mux[3];
    int uses[3], i;

    mux[0] = (ins[2] >> 26) & 3;
    mux[1] = (ins[2] >> 11) & 3;
    mux[2] = (ins[3] >> 28) & 3;
    uses[0] = mac != 0;
    uses[1] = mac >= 2 && mac <= 12 && mac != 3;
    uses[2] = mac == 3 || mac == 4 || ilu != 0;
    for (i = 0; i < 3; i++)
        if (uses[i] && mux[i] == 2)
            return 1u << ((ins[1] >> 9) & 0xF);
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_NV2A_VSH_HLSL_H */
