// The Vulkan back end's register-combiner pipeline (src/video/nv2a_vk_backend.c): one vertex and
// one pixel shader for every combiner program. ps_rc interprets the RcConsts registers statement for
// statement as nv2a_rc_eval (src/kernel/nv2a_combiner.c), with the texture stages of rc_stage_fetch
// (src/kernel/nv2a_pb_exec.c) for the modes vb_rc_supported admits (0-5, CUBEMAP on a 2D texture).
// Built twice by xbr_add_spirv: vs_rc with -fvk-invert-y (nv2a_vk_stage0.hlsl's y convention), and ps_rc.

struct Push {          // VbConstants; the same block as nv2a_vk_stage0.hlsl's
    float4 xf;         // 2/target_w, 2/target_h, 1/z_scale, (unused here)
    float4 tx;         // (unused here), (unused here), alpha func index (-1 off), alpha ref
};
[[vk::push_constant]] ConstantBuffer<Push> g;

// VbRcConstants. Every member is a 16-byte vector or an array of them, so std140, the D3D cbuffer
// rules and scalar layout all give these offsets.
struct RcConsts {
    uint4  stage[8];   //   0: general stage s: color_icw, alpha_icw, color_ocw, alpha_ocw
    float4 c0[8];      // 128: FACTOR0[s] r,g,b,a
    float4 c1[8];      // 256: FACTOR1[s]
    float4 fc0;        // 384: SPECULAR_FOG_FACTOR0 (final c0)
    float4 fc1;        // 400: SPECULAR_FOG_FACTOR1 (final c1)
    float4 fogc;       // 416: fog colour r,g,b,(a unused)
    float4 ts[4];      // 432: texture stage st: coordinate scale x,y; z 1 if a texture is bound
    uint4  ctl;        // 496: stages (<= 8), (control >> 8) & 0x111, stage_program & 0xFFFFF,
                       //      clip_plane_mode & 0xFFFF
    uint4  fin;        // 512: final0, final1, 0, 0
};                     // 528 bytes
[[vk::binding(0, 0)]] ConstantBuffer<RcConsts> rc;
[[vk::binding(1, 0)]] Texture2D tx0;
[[vk::binding(2, 0)]] Texture2D tx1;
[[vk::binding(3, 0)]] Texture2D tx2;
[[vk::binding(4, 0)]] Texture2D tx3;
[[vk::binding(5, 0)]] SamplerState sm0;
[[vk::binding(6, 0)]] SamplerState sm1;
[[vk::binding(7, 0)]] SamplerState sm2;
[[vk::binding(8, 0)]] SamplerState sm3;

struct VI {            // VbRcVertex, 92 bytes
    [[vk::location(0)]] float3 p : POSITION;
    [[vk::location(1)]] float rhw : TEXCOORD5;
    [[vk::location(2)]] float4 d0 : COLOR0;    // 0xAARRGGBB read as R8G8B8A8: B,G,R,A
    [[vk::location(3)]] float4 d1 : COLOR1;
    [[vk::location(4)]] float fog : TEXCOORD4;
    [[vk::location(5)]] float4 t0 : TEXCOORD0;
    [[vk::location(6)]] float4 t1 : TEXCOORD1;
    [[vk::location(7)]] float4 t2 : TEXCOORD2;
    [[vk::location(8)]] float4 t3 : TEXCOORD3;
};

// Colours and fog interpolate linearly in screen space, texture coordinates perspective-correct,
// as the CPU program path does (NV2A_VSH_HLSL_VO_FIELDS).
struct VO {
    float4 p : SV_Position;
    [[vk::location(0)]] noperspective float4 d0 : COLOR0;
    [[vk::location(1)]] noperspective float4 d1 : COLOR1;
    [[vk::location(2)]] noperspective float fog : TEXCOORD4;
    [[vk::location(3)]] float4 t0 : TEXCOORD0;
    [[vk::location(4)]] float4 t1 : TEXCOORD1;
    [[vk::location(5)]] float4 t2 : TEXCOORD2;
    [[vk::location(6)]] float4 t3 : TEXCOORD3;
};

// vs_main's screen-to-clip transform, w = 1/rhw.
VO vs_rc(VI i)
{
    VO o;
    float w = 1.0 / i.rhw;
    o.p = float4((i.p.x * g.xf.x - 1.0) * w, (1.0 - i.p.y * g.xf.y) * w,
                 i.p.z * g.xf.z * w, w);
    o.d0 = i.d0.zyxw;
    o.d1 = i.d1.zyxw;
    o.fog = i.fog;
    o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;
    return o;
}

// PS_INPUTMAPPING, nv2a_combiner.c map_in.
float3 map_in(float3 x, uint b)
{
    float3 p = max(x, 0.0);
    switch ((b >> 5) & 7) {
    case 0:  return p;
    case 1:  return 1.0 - saturate(x);
    case 2:  return 2.0 * p - 1.0;
    case 3:  return -2.0 * p + 1.0;
    case 4:  return p - 0.5;
    case 5:  return -p + 0.5;
    case 6:  return x;
    default: return -x;
    }
}

// PS_COMBINEROUTPUT scale/bias then the [-1, 1] clamp, map_out and its callers.
float3 map_out(float3 x, uint m)
{
    switch (m) {
    case 0x08: x = x - 0.5; break;
    case 0x10: x = x * 2.0; break;
    case 0x18: x = (x - 0.5) * 2.0; break;
    case 0x20: x = x * 4.0; break;
    case 0x30: x = x * 0.5; break;
    default: break;
    }
    return clamp(x, -1.0, 1.0);
}

// read_rgb / read_alpha on register value r for input byte b.
float3 in_rgb(float4 r, uint b) { return map_in((b & 0x10) != 0 ? r.aaa : r.rgb, b); }
float in_a(float4 r, uint b) { return map_in(((b & 0x10) != 0 ? r.a : r.b).xxx, b).x; }

// What texture stage st contributes, rc_stage_fetch for modes 0-5. An unbound stage (ts.z 0)
// reads white, as rc_texel does for an unusable texture.
float4 fetch(uint st, Texture2D tx, SamplerState sm, float4 t)
{
    uint m = (rc.ctl.z >> (st * 5)) & 0x1F;
    float4 ts = rc.ts[st];
    if (m == 0)                                   // NONE
        return float4(0, 0, 0, 1);
    if (m == 1 || m == 2) {                       // PROJECT2D / 3D
        float q = t.w != 0.0 ? t.w : 1.0;
        return ts.z > 0.5 ? tx.SampleLevel(sm, t.xy / q * ts.xy, 0) : float4(1, 1, 1, 1);
    }
    if (m == 3)                                   // CUBEMAP on a 2D texture: no divide
        return ts.z > 0.5 ? tx.SampleLevel(sm, t.xy * ts.xy, 0) : float4(1, 1, 1, 1);
    if (m == 4)                                   // PASSTHRU
        return saturate(t);
    if (m == 5) {                                 // CLIPPLANE
        uint cp = rc.ctl.w >> (st * 4);
        if ((cp & 1) != 0 ? t.x >= 0.0 : t.x < 0.0) discard;
        if ((cp & 2) != 0 ? t.y >= 0.0 : t.y < 0.0) discard;
        if ((cp & 4) != 0 ? t.z >= 0.0 : t.z < 0.0) discard;
        if ((cp & 8) != 0 ? t.w >= 0.0 : t.w < 0.0) discard;
        return float4(0, 0, 0, 0);
    }
    return float4(0, 0, 0, 0);                    // never reached: vb_rc_supported rejects > 5
}

float4 ps_rc(VO i) : SV_Target
{
    float4 R[16];
    float4 o;
    uint k, s, d;

    [unroll] for (k = 0; k < 16; k++)
        R[k] = float4(0, 0, 0, 0);
    R[3] = float4(rc.fogc.rgb, i.fog);
    R[4] = i.d0;
    R[5] = i.d1;
    R[8] = fetch(0, tx0, sm0, i.t0);
    R[9] = fetch(1, tx1, sm1, i.t1);
    R[10] = fetch(2, tx2, sm2, i.t2);
    R[11] = fetch(3, tx3, sm3, i.t3);
    // r0.a starts as texture 0's alpha, or 1 with stage 0 off.
    R[12].a = (rc.ctl.z & 0x1F) != 0 ? R[8].a : 1.0;

    [loop] for (s = 0; s < rc.ctl.x; s++) {
        uint icw = rc.stage[s].x, aicw = rc.stage[s].y, ocw = rc.stage[s].z, aocw = rc.stage[s].w;
        uint fl = ocw >> 12, afl = aocw >> 12;
        R[1] = rc.c0[(rc.ctl.y & 0x010) != 0 ? s : 0];
        R[2] = rc.c1[(rc.ctl.y & 0x100) != 0 ? s : 0];
        bool mx = (rc.ctl.y & 1) != 0 ? R[12].a >= 0.5
                                      : ((((int)(R[12].a * 255.0)) & 1) != 0);
        // Both portions read before either writes.
        float3 A = in_rgb(R[(icw >> 24) & 0xF], icw >> 24);
        float3 B = in_rgb(R[(icw >> 16) & 0xF], icw >> 16);
        float3 C = in_rgb(R[(icw >> 8) & 0xF], icw >> 8);
        float3 D = in_rgb(R[icw & 0xF], icw);
        float aA = in_a(R[(aicw >> 24) & 0xF], aicw >> 24);
        float aB = in_a(R[(aicw >> 16) & 0xF], aicw >> 16);
        float aC = in_a(R[(aicw >> 8) & 0xF], aicw >> 8);
        float aD = in_a(R[aicw & 0xF], aicw);
        float3 pab = (fl & 2) != 0 ? dot(A, B).xxx : A * B;
        float3 pcd = (fl & 1) != 0 ? dot(C, D).xxx : C * D;
        float3 psm = (fl & 4) != 0 ? (mx ? pcd : pab) : pab + pcd;
        float3 oab = map_out(pab, fl & 0x38), ocd = map_out(pcd, fl & 0x38);
        float3 osm = map_out(psm, fl & 0x38);
        float qab = aA * aB, qcd = aC * aD;
        float qsm = (afl & 4) != 0 ? (mx ? qcd : qab) : qab + qcd;
        float wab = map_out(qab.xxx, afl & 0x38).x, wcd = map_out(qcd.xxx, afl & 0x38).x;
        float wsm = map_out(qsm.xxx, afl & 0x38).x;
        // RGB destinations; blue-to-alpha also writes the destination's .a.
        d = (ocw >> 4) & 0xF;
        if (d != 0) { R[d].rgb = oab; if ((fl & 0x80) != 0) R[d].a = oab.b; }
        d = ocw & 0xF;
        if (d != 0) { R[d].rgb = ocd; if ((fl & 0x40) != 0) R[d].a = ocd.b; }
        d = (ocw >> 8) & 0xF;
        if (d != 0) R[d].rgb = osm;
        d = (aocw >> 4) & 0xF;
        if (d != 0) R[d].a = wab;
        d = aocw & 0xF;
        if (d != 0) R[d].a = wcd;
        d = (aocw >> 8) & 0xF;
        if (d != 0) R[d].a = wsm;
        R[0] = float4(0, 0, 0, 0);                // register 0 is zero whatever was written
    }

    if (rc.fin.x != 0 || rc.fin.y != 0) {
        uint f0 = rc.fin.x, f1 = rc.fin.y, ff = f1 & 0xFF;
        R[1] = rc.fc0;
        R[2] = rc.fc1;
        // V1R0 sum: optional complements (0x40 v1, 0x20 r0), clamp (0x80).
        float3 sa = (ff & 0x40) != 0 ? 1.0 - R[5].rgb : R[5].rgb;
        float3 sb = (ff & 0x20) != 0 ? 1.0 - R[12].rgb : R[12].rgb;
        R[14] = float4((ff & 0x80) != 0 ? saturate(sa + sb) : sa + sb, 0.0);
        float3 E = in_rgb(R[(f1 >> 24) & 0xF], f1 >> 24);
        float3 F = in_rgb(R[(f1 >> 16) & 0xF], f1 >> 16);
        R[15] = float4(E * F, 0.0);
        float3 A = in_rgb(R[(f0 >> 24) & 0xF], f0 >> 24);
        float3 B = in_rgb(R[(f0 >> 16) & 0xF], f0 >> 16);
        float3 C = in_rgb(R[(f0 >> 8) & 0xF], f0 >> 8);
        float3 D = in_rgb(R[f0 & 0xF], f0);
        o.rgb = D + C * (1.0 - A) + B * A;        // D + mix(C, B, A)
        o.a = in_a(R[(f1 >> 8) & 0xF], f1 >> 8);  // alpha from G
    } else {
        o = R[12];
    }
    o = saturate(o);
    // The alpha test on the combiner's output, as gb_rc_source and the CPU path apply it.
    if (g.tx.z >= 0.0) {
        int v = (int)(o.a * 255.0 + 0.5), r = (int)g.tx.w, f = (int)g.tx.z;
        bool keep = f == 7 || (f == 1 && v < r) || (f == 2 && v == r)
                 || (f == 3 && v <= r) || (f == 4 && v > r) || (f == 5 && v != r)
                 || (f == 6 && v >= r);
        if (!keep) discard;
    }
    return o;
}
