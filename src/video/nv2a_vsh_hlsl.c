/*
 * NV2A vertex program to HLSL. See nv2a_vsh_hlsl.h for why it exists.
 *
 * Ported from Mercenaries Recompiled's src/d3d/d3d8_vsh.c (d3d8_vsh_parse,
 * d3d8_vsh_generate_hlsl and the helpers it emits), MIT License; its LICENSE
 * names no copyright holder, and its README credits Trenton Turner
 * (KraftMacAndChee). Credited here.
 *
 * The semantics are nv2a_vsh_interp.c's, not Mercenaries': where the two differ
 * this follows the interpreter, which is what JSRF is known to render with.
 * Each instruction becomes one block that reads its sources into A, B and C,
 * computes the MAC result mr and the ILU result ir, and only then writes, in
 * the interpreter's order: the MAC temp, the ILU temp (R1 when paired), the
 * output register, then a0. Temp 12 and oPos are the one register R12.
 *
 * The field layout is the table at the top of nv2a_vsh_interp.c.
 */
#include "nv2a_vsh_hlsl.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct { char *p; size_t n, cap; } VshStr;

static void vcat(VshStr *b, const char *fmt, ...)
{
    va_list ap;
    int k;

    if (b->n >= b->cap)
        return;
    va_start(ap, fmt);
    k = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    b->n = (k < 0 || (size_t)k >= b->cap - b->n) ? b->cap : b->n + (size_t)k;
}

static uint32_t field(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

/* Helpers the program body calls; each is the matching line of run_mac/run_ilu,
 * fog_factor or read_src. */
static const char k_preamble[] =
    "cbuffer VshC : register(b1) { float4 c[192]; };\n"
    "cbuffer VshX : register(b2) { float4 g_xf; float4 g_fog; };\n"
    "float vsh_inf() { return asfloat(0x7f800000u); }\n"
    /* An a0-relative index outside the constants reads c[0]. */
    "float4 vsh_crel(int i) { return c[(i >= 0 && i < 192) ? i : 0]; }\n"
    /* min, max, slt, sge as written in run_mac: max and sge compare with >=. */
    "float vsh_min1(float a, float b) { return a < b ? a : b; }\n"
    "float vsh_max1(float a, float b) { return a >= b ? a : b; }\n"
    "float4 vsh_min(float4 a, float4 b) {\n"
    "  return float4(vsh_min1(a.x, b.x), vsh_min1(a.y, b.y), vsh_min1(a.z, b.z),"
    " vsh_min1(a.w, b.w)); }\n"
    "float4 vsh_max(float4 a, float4 b) {\n"
    "  return float4(vsh_max1(a.x, b.x), vsh_max1(a.y, b.y), vsh_max1(a.z, b.z),"
    " vsh_max1(a.w, b.w)); }\n"
    "float4 vsh_slt(float4 a, float4 b) {\n"
    "  return float4(a.x < b.x ? 1.0 : 0.0, a.y < b.y ? 1.0 : 0.0, a.z < b.z ? 1.0 : 0.0,"
    " a.w < b.w ? 1.0 : 0.0); }\n"
    "float4 vsh_sge(float4 a, float4 b) {\n"
    "  return float4(a.x >= b.x ? 1.0 : 0.0, a.y >= b.y ? 1.0 : 0.0, a.z >= b.z ? 1.0 : 0.0,"
    " a.w >= b.w ? 1.0 : 0.0); }\n"
    /* rcp of 0 is +INF whatever the sign of the zero. */
    "float vsh_rcp(float x) { return x == 0.0 ? vsh_inf() : 1.0 / x; }\n"
    "float vsh_rcc(float x) {\n"
    "  float f = 1.0 / x;\n"
    "  if (abs(f) < 5.42101e-20) f = f < 0.0 ? -5.42101e-20 : 5.42101e-20;\n"
    "  if (abs(f) > 1.884467e19) f = f < 0.0 ? -1.884467e19 : 1.884467e19;\n"
    "  return f; }\n"
    /* 2^e for a whole e, built from the bits so it is exact as exp2f is. */
    "float vsh_exp2i(float e) {\n"
    "  if (isnan(e)) return e;\n"
    "  if (e > 127.0) return vsh_inf();\n"
    "  if (e < -126.0) return exp2(e);\n"
    "  return asfloat((uint)((int)e + 127) << 23); }\n"
    "float4 vsh_exp(float x) {\n"
    "  float fl = floor(x);\n"
    "  return float4(vsh_exp2i(fl), x - fl, exp2(x), 1.0); }\n"
    /* x, y = floor(log2|x|), |x| / 2^x, from the bits for normal |x|: an
     * approximate log2 near a power of two would floor to the wrong exponent. */
    "float4 vsh_log(float x) {\n"
    "  float ax = abs(x), e;\n"
    "  uint u = asuint(ax);\n"
    "  if (ax == 0.0) return float4(-vsh_inf(), 1.0, -vsh_inf(), 1.0);\n"
    "  if (u < 0x00800000u || u >= 0x7f800000u) {\n"
    "    e = floor(log2(ax));\n"
    "    return float4(e, ax / exp2(e), log2(ax), 1.0); }\n"
    "  return float4((float)((int)(u >> 23) - 127), asfloat((u & 0x007fffffu) | 0x3f800000u),"
    " log2(ax), 1.0); }\n"
    /* powf for a base >= 0, with its special cases. */
    "float vsh_pow(float b, float e) {\n"
    "  if (b == 1.0 || e == 0.0) return 1.0;\n"
    "  if (isnan(e)) return e;\n"
    "  if (b == 0.0) return e > 0.0 ? 0.0 : vsh_inf();\n"
    "  return exp2(e * log2(b)); }\n"
    "float4 vsh_lit(float4 s) {\n"
    "  float nl = s.x > 0.0 ? s.x : 0.0, nh = s.y > 0.0 ? s.y : 0.0;\n"
    "  float p = s.w < -127.9961 ? -127.9961 : (s.w > 127.9961 ? 127.9961 : s.w);\n"
    "  return float4(1.0, nl, s.x > 0.0 ? vsh_pow(nh, p) : 0.0, 1.0); }\n"
    /* The fixed-function fog unit on oFog.x; disabled reads as 1, no fog. */
    "float vsh_fog(float d) {\n"
    "  float f, px = g_fog.z, py = g_fog.w, m = g_fog.y;\n"
    "  if (g_fog.x == 0.0) return 1.0;\n"
    "  if (m == 2048.0 || m == 2050.0) f = px + exp2(d * py * 16.0) - 1.5;\n"          /* EXP, EXP_ABS */
    "  else if (m == 2049.0 || m == 2051.0) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
    "  else f = px + d * py - 1.0;\n"                                                   /* LINEAR(_ABS) */
    "  if (m == 2050.0 || m == 2051.0 || m == 2052.0) f = abs(f);\n"
    "  if (isnan(f)) f = 1.0;\n"
    "  return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f); }\n";

static const char *const k_mac_name[16] = {
    "nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4",
    "dst", "min", "max", "slt", "sge", "arl", "mac14", "mac15"
};
static const char *const k_ilu_name[8] = {
    "nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit"
};

/* The MAC result from A, B, C; ops 13 (arl), 14 and 15 leave A, as run_mac does. */
static const char *const k_mac_expr[16] = {
    "0", "A", "A * B", "A + C", "A * B + C",
    "dot(A.xyz, B.xyz).xxxx", "(dot(A.xyz, B.xyz) + B.w).xxxx", "dot(A, B).xxxx",
    "float4(1.0, A.y * B.y, A.z, B.w)", "vsh_min(A, B)", "vsh_max(A, B)",
    "vsh_slt(A, B)", "vsh_sge(A, B)", "A", "A", "A"
};
static const char *const k_ilu_expr[8] = {
    "0", "C", "vsh_rcp(C.x).xxxx", "vsh_rcc(C.x).xxxx", "rsqrt(abs(C.x)).xxxx",
    "vsh_exp(C.x)", "vsh_log(C.x)", "vsh_lit(C)"
};

/* Source `which` (0 A, 1 B, 2 C) as read_src reads it. */
static void emit_src(char *o, size_t cap, const uint32_t *ins, int which, uint32_t input_mask)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;
    char base[40];

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }
    if (mux == 1) {
        /* R12 is oPos; temps past it read R0. */
        snprintf(base, sizeof base, "R%u", reg <= 12 ? reg : 0u);
    } else if (mux == 2) {
        uint32_t v = field(ins, 1, 9, 4);
        if ((input_mask >> v) & 1u)
            snprintf(base, sizeof base, "v%u", v);
        else
            snprintf(base, sizeof base, "float4(0.0, 0.0, 0.0, 1.0)");
    } else {
        /* Mux 0 and 3 both read a constant, as read_src's else branch does. */
        uint32_t ci = field(ins, 1, 13, 8);
        if (field(ins, 3, 1, 1))
            snprintf(base, sizeof base, "vsh_crel(%u + a0)", ci);
        else
            snprintf(base, sizeof base, "c[%u]", ci < 192 ? ci : 0u);
    }
    snprintf(o, cap, "%s%s.%c%c%c%c", neg ? "-" : "", base, sw[(swz >> 6) & 3],
             sw[(swz >> 4) & 3], sw[(swz >> 2) & 3], sw[swz & 3]);
}

/* A write mask (x=8 y=4 z=2 w=1) as a swizzle. */
static void mask_str(char *o, uint32_t mask)
{
    int n = 0;
    o[n++] = '.';
    if (mask & 8) o[n++] = 'x';
    if (mask & 4) o[n++] = 'y';
    if (mask & 2) o[n++] = 'z';
    if (mask & 1) o[n++] = 'w';
    o[n] = '\0';
}

/* A temp destination: R12 is oPos, and temps past it are not written. */
static int temp_dst(char *o, size_t cap, uint32_t t)
{
    if (t > 12)
        return 0;
    snprintf(o, cap, "R%u", t);
    return 1;
}

static void emit_ins(VshStr *b, const uint32_t *ins, uint32_t n, uint32_t input_mask)
{
    uint32_t mac = field(ins, 1, 21, 4), ilu = field(ins, 1, 25, 3);
    uint32_t mac_mask = field(ins, 3, 24, 4), ilu_mask = field(ins, 3, 16, 4);
    uint32_t tdst = field(ins, 3, 20, 4), omask = field(ins, 3, 12, 4);
    uint32_t oaddr = field(ins, 3, 3, 8), out_o = field(ins, 3, 11, 1);
    uint32_t out_ilu = field(ins, 3, 2, 1);
    /* Sources read: A for any MAC op, B for the two-operand ones, C for add/mad and the ILU. */
    int use_a = mac != 0, use_b = mac >= 2 && mac <= 12 && mac != 3;
    int use_c = mac == 3 || mac == 4 || ilu != 0;
    char src[64], dst[8], m[8];

    vcat(b, "  /* %u: %s %s */ {\n", n, k_mac_name[mac], k_ilu_name[ilu]);
    if (use_a) {
        emit_src(src, sizeof src, ins, 0, input_mask);
        vcat(b, "    float4 A = %s;\n", src);
    }
    if (use_b) {
        emit_src(src, sizeof src, ins, 1, input_mask);
        vcat(b, "    float4 B = %s;\n", src);
    }
    if (use_c) {
        emit_src(src, sizeof src, ins, 2, input_mask);
        vcat(b, "    float4 C = %s;\n", src);
    }
    vcat(b, "    float4 mr = %s, ir = %s;\n", k_mac_expr[mac], k_ilu_expr[ilu]);
    if (mac && mac != 13 && mac_mask && temp_dst(dst, sizeof dst, tdst)) {
        mask_str(m, mac_mask);
        vcat(b, "    %s%s = mr%s;\n", dst, m, m);
    }
    /* Paired with any MAC op, the ILU writes R1 whatever the temp field says. */
    if (ilu && ilu_mask && temp_dst(dst, sizeof dst, mac ? 1u : tdst)) {
        mask_str(m, ilu_mask);
        vcat(b, "    %s%s = ir%s;\n", dst, m, m);
    }
    /* Output 0 is oPos, 1..12 the out registers; c[] writes never reach here. */
    if (omask && out_o && oaddr < 13) {
        mask_str(m, omask);
        if (oaddr == 0)
            vcat(b, "    R12%s = %s%s;\n", m, out_ilu ? "ir" : "mr", m);
        else
            vcat(b, "    o%u%s = %s%s;\n", oaddr, m, out_ilu ? "ir" : "mr", m);
    }
    if (mac == 13)
        vcat(b, "    a0 = (int)floor(A.x + 0.001);\n");
    vcat(b, "  }\n");
}

int nv2a_vsh_hlsl(const uint32_t (*program)[4], uint32_t len, uint32_t input_mask,
                  char *out, size_t cap)
{
    VshStr b;
    uint32_t i, n;
    int final = 0;

    if (!out || !cap)
        return 0;
    b.p = out;
    b.n = 0;
    b.cap = cap;
    out[0] = '\0';
    input_mask &= 0xFFFFu;

    vcat(&b, "%s", k_preamble);
    vcat(&b, "struct VIP {");
    if (!input_mask)
        vcat(&b, " uint vid : SV_VertexID;");
    for (i = 0; i < 16; i++)
        if ((input_mask >> i) & 1u)
            vcat(&b, " float4 v%u : ATTR%u;", i, i);
    vcat(&b, " };\n");
    vcat(&b, "struct VOP { " NV2A_VSH_HLSL_VO_FIELDS "float clip : SV_ClipDistance0; };\n");
    vcat(&b, "VOP " NV2A_VSH_HLSL_ENTRY "(VIP vin) {\n");
    vcat(&b, "  float4 R0 = 0, R1 = 0, R2 = 0, R3 = 0, R4 = 0, R5 = 0, R6 = 0, R7 = 0;\n"
             "  float4 R8 = 0, R9 = 0, R10 = 0, R11 = 0, R12 = 0;\n");
    /* Colours default opaque, texture coordinates to (0,0,0,1), fog to 0. */
    vcat(&b, "  float4 o1 = 0, o2 = 0, o3 = float4(0, 0, 0, 1), o4 = float4(0, 0, 0, 1);\n"
             "  float4 o5 = 0, o6 = 0, o7 = 0, o8 = 0;\n"
             "  float4 o9 = float4(0, 0, 0, 1), o10 = float4(0, 0, 0, 1);\n"
             "  float4 o11 = float4(0, 0, 0, 1), o12 = float4(0, 0, 0, 1);\n"
             "  int a0 = 0;\n");
    for (i = 0; i < 16; i++)
        if ((input_mask >> i) & 1u)
            vcat(&b, "  float4 v%u = vin.v%u;\n", i, i);

    for (n = 0; n < len; n++) {
        emit_ins(&b, program[n], n, input_mask);
        if (field(program[n], 3, 0, 1)) {
            final = 1;
            break;
        }
    }
    if (!final)
        return 0;

    /* The combiner VS's transform of a CPU-transformed vertex, with w = oPos.w. */
    vcat(&b, "  VOP o;\n"
             "  float w = R12.w;\n"
             "  o.p = float4((R12.x * g_xf.x - 1.0) * w, (1.0 - R12.y * g_xf.y) * w,\n"
             "               R12.z * g_xf.z * w, w);\n"
             "  o.d0 = saturate(o3); o.d1 = saturate(o4); o.fog = vsh_fog(o5.x);\n"
             "  o.t0 = o9; o.t1 = o10; o.t2 = o11; o.t3 = o12;\n"
             "  o.clip = w - 1e-3;\n"   /* the CPU path's NV_CLIP_W */
             "  return o;\n"
             "}\n");
    if (b.n >= b.cap) {
        out[0] = '\0';
        return 0;
    }
    return (int)b.n;
}
