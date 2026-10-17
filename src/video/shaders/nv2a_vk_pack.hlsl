// The Vulkan back end's readback pack kernel (src/video/nv2a_vk_backend.c): one thread per TWO
// pixels of a 16-bit render target's dirty rectangle, turning the target's own B8G8R8A8_UNORM
// image into the guest's little-endian 5:6:5 halfwords straight into the device-local staging
// buffer, which a copy-engine command then moves to host memory. The host publishes the packed
// slice with one memcpy per row, so no per-pixel work is left on the critical path.
//
// Built by xbr_add_spirv as -T cs_6_0 (no invert-y: this is a compute entry), so nothing is
// compiled or translated at run time. A probe of exactly this shader was compiled and validated
// outside the tree (dxc -spirv -T cs_6_0 -E cs_pack -fspv-target-env=vulkan1.1, then
// spirv-val --target-env vulkan1.1: OK, SPIR-V 1.3).
//
// The arithmetic is bit-exact with the CPU loop this replaces. For a pixel's exact 8-bit
// channels r8/g8/b8 the guest halfword is the TRUNCATED
//
//     (r8 >> 3) << 11 | (g8 >> 2) << 5 | (b8 >> 3)
//
// which is what that loop computed from the little-endian B8G8R8A8 word c (B | G<<8 | R<<16 |
// A<<24):
//
//     ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F)
//
//   - `c >> 8` moves R's byte into bits 0..7, and 0xF800 keeps only its five high bits:
//     R's five high bits land at 11..15 = (r8 >> 3) << 11;
//   - `c >> 5` moves G's byte into bits 3..10, and 0x07E0 keeps six bits of it:
//     G's six high bits land at 5..10 = (g8 >> 2) << 5;
//   - `c >> 3` moves B's byte into bits 5..12, and 0x001F keeps its five low bits:
//     (b8 >> 3).
// The mask-and-shift pair is therefore exactly the three truncated channel shifts above -- no
// rounding and no normalisation, and the same halfword for every input.
//
// The image is B8G8R8A8_UNORM, so Load returns each channel as byte/255; the `+ 0.5` then
// `(uint)` recovers that exact byte back (the float error is orders of magnitude under the
// half-byte the add rounds over), which is what makes this bit-exact rather than approximate.
//
// Row stride in the staging buffer is the slice's `rect_w * 2` rounded up to a multiple of 4.
// On an odd width the last pair's second pixel is out of range, so `hw1` stays 0 and the two
// spare bytes are written but never published. Pairs and uint32 stores are what the kernel is
// built around: a 16-bit store would need the optional 16-bit-storage feature, a uint32 store
// needs none, and a 4-byte-aligned row stride is what makes the uint32 store legal.

struct PackPush {
    uint4 rect;   // x0, y0, width, height of the dirty rectangle, in surface pixels
    uint4 dst;    // byte offset of the slice in the staging buffer, row stride in bytes, 0, 0
};
[[vk::push_constant]] ConstantBuffer<PackPush> g;

[[vk::binding(0, 0)]] Texture2D<float4> src;          // the target's own B8G8R8A8_UNORM view
[[vk::binding(1, 0)]] RWByteAddressBuffer pack_dst;   // `out` is not a usable HLSL identifier

// The truncated 5:6:5 halfword of one pixel, exactly as the CPU loop this replaced wrote it.
uint pack565(float3 c)
{
    uint r = (uint)(saturate(c.r) * 255.0 + 0.5);
    uint g8 = (uint)(saturate(c.g) * 255.0 + 0.5);
    uint b = (uint)(saturate(c.b) * 255.0 + 0.5);
    return ((r >> 3) << 11) | ((g8 >> 2) << 5) | (b >> 3);
}

// Dispatch is (ceil(rect_w / 16), ceil(rect_h / 8), 1): 8 threads in x, each packing the pair at
// x = id.x * 2. `rect.z` is the width and `rect.w` the height.
[numthreads(8, 8, 1)]
void cs_pack(uint3 id : SV_DispatchThreadID)
{
    uint px = id.x * 2u, row = id.y;
    if (row >= g.rect.w || px >= g.rect.z)
        return;
    uint y = g.rect.y + row;
    uint x = g.rect.x + px;
    uint hw0 = pack565(src.Load(int3(x, y, 0)).rgb);
    uint hw1 = 0;
    if (px + 1u < g.rect.z)
        hw1 = pack565(src.Load(int3(x + 1, y, 0)).rgb);
    pack_dst.Store(g.dst.x + row * g.dst.y + (px >> 1) * 4, hw0 | (hw1 << 16));
}
