// The Vulkan back end's stage-0 pipeline (src/video/nv2a_vk_backend.c): the D3D11 back end's
// s_hlsl (nv2a_d3d11_backend.c) statement for statement, so the two rasterise the same pixels.
// Built twice by xbr_add_spirv: vs_main with -fvk-invert-y, which negates the clip-space y the
// D3D11 math produces so surface pixel y lands on framebuffer row y; and ps_main.

struct Push {
    float4 xf;   // 2/target_w, 2/target_h, 1/z_scale, textured
    float4 tx;   // 1/tex_w, 1/tex_h, alpha func index (-1 off), alpha ref
};
[[vk::push_constant]] ConstantBuffer<Push> g;

struct VI {
    [[vk::location(0)]] float3 p : POSITION;
    [[vk::location(1)]] float rhw : TEXCOORD1;
    [[vk::location(2)]] float4 c : COLOR;      // 0xAARRGGBB read as R8G8B8A8: B,G,R,A
    [[vk::location(3)]] float2 t : TEXCOORD0;
};

struct VO {
    float4 p : SV_Position;
    [[vk::location(0)]] float4 c : COLOR;
    [[vk::location(1)]] float2 t : TEXCOORD0;
};

// Surface pixels to clip space, w = 1/rhw so the rasteriser interpolates perspective-correct.
VO vs_main(VI i)
{
    VO o;
    float w = 1.0 / i.rhw;
    o.p = float4((i.p.x * g.xf.x - 1.0) * w, (1.0 - i.p.y * g.xf.y) * w,
                 i.p.z * g.xf.z * w, w);
    o.c = i.c.zyxw;
    o.t = i.t * g.tx.xy;
    return o;
}

[[vk::binding(0, 0)]] Texture2D t0;
[[vk::binding(1, 0)]] SamplerState s0;

// Stage 0 modulated by the diffuse colour, then the alpha test (GL func - 0x200, ref 0..255).
float4 ps_main(VO i) : SV_Target
{
    float4 c = i.c;
    if (g.xf.w > 0.5) c *= t0.Sample(s0, i.t);
    if (g.tx.z >= 0.0) {
        int v = (int)(saturate(c.a) * 255.0 + 0.5), r = (int)g.tx.w, f = (int)g.tx.z;
        bool keep = f == 7 || (f == 1 && v < r) || (f == 2 && v == r)
                 || (f == 3 && v <= r) || (f == 4 && v > r) || (f == 5 && v != r)
                 || (f == 6 && v >= r);
        if (!keep) discard;
    }
    return c;
}
