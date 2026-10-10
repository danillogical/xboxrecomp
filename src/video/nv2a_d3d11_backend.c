/*
 * Direct3D 11 render back end for the NV2A pushbuffer executor.
 *
 * The executor hands over decoded batches (kernel/nv2a_backend.h): triangles in
 * surface pixels with a per-vertex colour and texel-space UVs, the stage-0
 * texture, and the render state. This file rasterises them with D3D11 and, at
 * every flip, writes each render target it touched back into guest memory in
 * the guest's own format, so the framebuffer window, the present tracker and
 * frame dumps keep reading guest memory exactly as they do on the CPU path.
 *
 * Design:
 *   - Render targets are cached per guest colour surface (address, pitch, pixel
 *     size) as B8G8R8A8 textures, whose bytes are the guest's A8R8G8B8 layout.
 *     A target is (re)loaded from guest memory on its first use after each
 *     flip, so CPU writes between frames and partial redraws are both right.
 *     A pass that narrows the clip keeps the larger backing, the non-strict
 *     compatibility rule xemu uses, as Mercenaries-Recompiled's
 *     src/nv2a/nv2a_pgraph_d3d11.c does (MIT, credited here).
 *   - Depth is a D32_FLOAT buffer per (zeta address, target size). It is never
 *     loaded from or written to guest memory: like the CPU path, nothing the
 *     title does reads its depth surface back.
 *   - Textures are decoded with the executor's own decoder into BGRA8, so the
 *     colours match the CPU path exactly. They are cached and re-verified by a
 *     hash of their guest bytes on their first use after each flip.
 *   - A texture at the address of a live render target with the same pitch and
 *     pixel layout is sampled straight from the GPU target (through a copy when
 *     it is also the output); any other texture overlapping a target that has
 *     unflushed GPU writes forces that target back to guest memory first.
 *
 * Everything runs on the executor's thread, one call at a time, under the
 * NV2A owner lock; the only blocking wait is the readback Map at a flip (or at
 * a texture that overlaps an unflushed target).
 */
#include "nv2a_d3d11_backend.h"

#if !defined(_WIN32)

/* No D3D11 off Windows: report failure so the caller keeps the CPU path. */
int nv2a_d3d11_backend_install(void)
{
    return -1;
}

#else

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/nv2a_backend.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define GB_MAX_TARGETS   16
#define GB_MAX_DEPTHS    8
#define GB_MAX_TEXTURES  512
#define GB_MAX_BLENDS    64
#define GB_MAX_DSS       32
#define GB_MAX_DIM       4096
#define GB_VB_BYTES      (8u << 20)       /* one full executor batch is 5.5 MB */
#define GB_STATS_MS      10000

/* The input layout below reads Nv2aVertex in place. */
typedef char gb_vertex_is_28_bytes[sizeof(Nv2aVertex) == 28 ? 1 : -1];

#define GB_RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

/* Pixel position to clip space, w = 1/rhw so D3D interpolates perspective-correct;
 * the pixel shader modulates stage 0 by the diffuse colour and alpha-tests. */
static const char s_hlsl[] =
    "cbuffer C : register(b0) {\n"
    "  float4 g_xf;\n"   /* 2/target_w, 2/target_h, 1/z_scale, textured */
    "  float4 g_tx;\n"   /* 1/tex_w, 1/tex_h, alpha func index (-1 off), alpha ref */
    "};\n"
    "struct VI { float3 p : POSITION; float rhw : TEXCOORD1; float4 c : COLOR;"
    " float2 t : TEXCOORD0; };\n"
    "struct VO { float4 p : SV_Position; float4 c : COLOR; float2 t : TEXCOORD0; };\n"
    "VO vs_main(VI i) {\n"
    "  VO o;\n"
    "  float w = 1.0 / i.rhw;\n"
    "  o.p = float4((i.p.x * g_xf.x - 1.0) * w, (1.0 - i.p.y * g_xf.y) * w,\n"
    "               i.p.z * g_xf.z * w, w);\n"
    "  o.c = i.c.zyxw;\n"   /* 0xAARRGGBB read as R8G8B8A8 arrives as B,G,R,A */
    "  o.t = i.t * g_tx.xy;\n"
    "  return o;\n"
    "}\n"
    "Texture2D t0 : register(t0);\n"
    "SamplerState s0 : register(s0);\n"
    "float4 ps_main(VO i) : SV_Target {\n"
    "  float4 c = i.c;\n"
    "  if (g_xf.w > 0.5) c *= t0.Sample(s0, i.t);\n"
    "  if (g_tx.z >= 0.0) {\n"
    "    int v = (int)(saturate(c.a) * 255.0 + 0.5), r = (int)g_tx.w, f = (int)g_tx.z;\n"
    "    bool keep = f == 7 || (f == 1 && v < r) || (f == 2 && v == r)\n"
    "             || (f == 3 && v <= r) || (f == 4 && v > r) || (f == 5 && v != r)\n"
    "             || (f == 6 && v >= r);\n"
    "    if (!keep) discard;\n"
    "  }\n"
    "  return c;\n"
    "}\n";

typedef struct { float xf[4], tx[4]; } GbConstants;

typedef struct {
    int used;
    uint32_t va, pitch, bpp, w, h;
    ID3D11Texture2D *tex, *staging, *copy;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv, *copy_srv;
    int stale;                          /* must be reloaded from guest memory */
    int dirty;                          /* GPU writes not yet in guest memory */
    uint32_t dx0, dy0, dx1, dy1;        /* the rectangle they cover */
    uint64_t last_use;
} GbTarget;

typedef struct {
    int used;
    uint32_t va, w, h;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
    uint64_t last_use;
} GbDepth;

typedef struct {
    int used;
    uint32_t offset, color, w, h, pitch, span;
    uint64_t hash;
    uint32_t frame;                     /* flip count when the hash was last checked */
    uint64_t last_use;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;      /* NULL: the format does not decode */
} GbTex;

typedef struct {
    uint32_t enable, src, dst, eq, mask;
    ID3D11BlendState *bs;
} GbBlend;

typedef struct {
    uint32_t enable, func, write;
    ID3D11DepthStencilState *dss;
} GbDss;

typedef struct {
    uint64_t draws, tris, clears, dropped_tris, skipped;
    uint64_t tex_uploads, tex_hits, tex_undecodable, rt_aliases, rt_copies;
    uint64_t rt_loads, readbacks, readback_ticks;
} GbStats;

static struct {
    int ok;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *il;
    ID3D11Buffer *vb, *cb;
    UINT vb_cap, vb_pos;                /* in vertices */
    ID3D11RasterizerState *rs;
    ID3D11SamplerState *samp[4][9];     /* [filter][addr_u*3 + addr_v], made on first use */
    ID3D11RenderTargetView *bound_rtv;
    ID3D11DepthStencilView *bound_dsv;
    GbTarget targets[GB_MAX_TARGETS];
    GbDepth depths[GB_MAX_DEPTHS];
    GbTex texs[GB_MAX_TEXTURES];
    GbBlend blends[GB_MAX_BLENDS];
    unsigned nblends, next_blend;
    GbDss dsss[GB_MAX_DSS];
    unsigned ndss, next_dss;
    uint32_t *scratch;                  /* texture decode and target load buffer */
    size_t scratch_px;
    uint64_t use_clock;
    uint32_t frame;
    LARGE_INTEGER qpf;
    LONGLONG last_stats_qpc;
    int stats_said;
    unsigned fails;
    GbStats st;
} g;

static void gb_fail(const char *what, HRESULT hr)
{
    if (g.fails++ < 16) {
        fprintf(stderr, "[GPUBE] %s failed (hr=0x%08lX)\n", what, (unsigned long)hr);
        fflush(stderr);
    }
}

static uint8_t *gb_guest(uint32_t va)
{
    return (uint8_t *)xbox_GetMemoryOffset() + va;
}

static uint32_t *gb_scratch(size_t px)
{
    if (px > g.scratch_px) {
        uint32_t *p = (uint32_t *)realloc(g.scratch, px * 4);
        if (!p)
            return NULL;
        g.scratch = p;
        g.scratch_px = px;
    }
    return g.scratch;
}

/* Cheap 64-bit content hash; it only has to notice that guest bytes changed. */
static uint64_t gb_hash(const uint8_t *p, size_t n)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t)n;
    size_t i;

    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* ── Device and fixed pipeline state ─────────────────────────────────── */

typedef HRESULT (WINAPI *GbCompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                      ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                      ID3DBlob **, ID3DBlob **);

static ID3DBlob *gb_compile(GbCompileFn compile, const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = compile(s_hlsl, sizeof s_hlsl - 1, "nv2a_d3d11_backend", NULL, NULL,
                         entry, target, 0, 0, &code, &err);

    if (FAILED(hr)) {
        fprintf(stderr, "[GPUBE] shader %s failed (hr=0x%08lX): %s\n", entry,
                (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "no message");
        GB_RELEASE(err);
        GB_RELEASE(code);
        return NULL;
    }
    GB_RELEASE(err);
    return code;
}

static int gb_create_device(void)
{
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
    UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED;
    char name[256] = "unknown adapter";
    IDXGIDevice *dxgi = NULL;
    const char *force = getenv("RECOMP_GPU_WARP");
    int forced = force && strcmp(force, "1") == 0, warp = 0;
    HRESULT hr = E_FAIL;

    /* RECOMP_GPU_WARP=1 skips the hardware adapter, so a test renders the same everywhere. */
    if (!forced)
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, levels,
                               (UINT)(sizeof levels / sizeof levels[0]), D3D11_SDK_VERSION,
                               &g.dev, &got, &g.ctx);
    if (FAILED(hr)) {
        warp = 1;
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, flags, levels,
                               (UINT)(sizeof levels / sizeof levels[0]),
                               D3D11_SDK_VERSION, &g.dev, &got, &g.ctx);
    }
    if (FAILED(hr)) {
        gb_fail(forced ? "D3D11CreateDevice (WARP)" : "D3D11CreateDevice (hardware and WARP)", hr);
        return -1;
    }
    if (SUCCEEDED(ID3D11Device_QueryInterface(g.dev, &IID_IDXGIDevice, (void **)&dxgi))) {
        IDXGIAdapter *ad = NULL;
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &ad))
            && SUCCEEDED(IDXGIAdapter_GetDesc(ad, &desc)))
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name,
                                (int)sizeof name, NULL, NULL);
        GB_RELEASE(ad);
        GB_RELEASE(dxgi);
    }
    fprintf(stderr, "[GPUBE] device: %s%s (%s, feature level %X)\n",
            warp ? "warp" : "hardware", forced ? " forced by RECOMP_GPU_WARP=1" : "", name,
            (unsigned)got);
    fflush(stderr);
    return 0;
}

static int gb_create_pipeline(void)
{
    static const D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Nv2aVertex, x),
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, offsetof(Nv2aVertex, rhw),
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(Nv2aVertex, diffuse),
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Nv2aVertex, u),
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    HMODULE dll;
    GbCompileFn compile;
    ID3DBlob *vsb, *psb;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr;
    UINT stride = sizeof(Nv2aVertex), offset = 0;

    /* Loaded at run time so nothing links against d3dcompiler. */
    dll = LoadLibraryA("d3dcompiler_47.dll");
    compile = dll ? (GbCompileFn)(void (*)(void))GetProcAddress(dll, "D3DCompile") : NULL;
    if (!compile) {
        fprintf(stderr, "[GPUBE] d3dcompiler_47.dll / D3DCompile not available\n");
        return -1;
    }
    vsb = gb_compile(compile, "vs_main", "vs_4_0");
    psb = gb_compile(compile, "ps_main", "ps_4_0");
    if (!vsb || !psb) {
        GB_RELEASE(vsb);
        GB_RELEASE(psb);
        return -1;
    }
    hr = ID3D11Device_CreateVertexShader(g.dev, ID3D10Blob_GetBufferPointer(vsb),
                                         ID3D10Blob_GetBufferSize(vsb), NULL, &g.vs);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateInputLayout(g.dev, layout, 4,
                                            ID3D10Blob_GetBufferPointer(vsb),
                                            ID3D10Blob_GetBufferSize(vsb), &g.il);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreatePixelShader(g.dev, ID3D10Blob_GetBufferPointer(psb),
                                            ID3D10Blob_GetBufferSize(psb), NULL, &g.ps);
    GB_RELEASE(vsb);
    GB_RELEASE(psb);
    if (FAILED(hr)) {
        gb_fail("shader objects", hr);
        return -1;
    }

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = GB_VB_BYTES;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(g.dev, &bd, NULL, &g.vb);
    if (FAILED(hr)) {
        gb_fail("vertex buffer", hr);
        return -1;
    }
    g.vb_cap = GB_VB_BYTES / sizeof(Nv2aVertex);
    g.vb_pos = g.vb_cap;                  /* the first write discards */

    bd.ByteWidth = sizeof(GbConstants);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = ID3D11Device_CreateBuffer(g.dev, &bd, NULL, &g.cb);
    if (FAILED(hr)) {
        gb_fail("constant buffer", hr);
        return -1;
    }

    /* No culling: the CPU rasteriser this replaces draws both windings, and
     * depth clamping rather than clipping matches its unclipped z. */
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    rd.ScissorEnable = TRUE;
    hr = ID3D11Device_CreateRasterizerState(g.dev, &rd, &g.rs);
    if (FAILED(hr)) {
        gb_fail("rasterizer state", hr);
        return -1;
    }

    /* The context is private to this file, so the fixed state is set once. */
    ID3D11DeviceContext_IASetInputLayout(g.ctx, g.il);
    ID3D11DeviceContext_IASetPrimitiveTopology(g.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(g.ctx, 0, 1, &g.vb, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(g.ctx, g.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g.ctx, 0, 1, &g.cb);
    ID3D11DeviceContext_PSSetShader(g.ctx, g.ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g.ctx, 0, 1, &g.cb);
    ID3D11DeviceContext_RSSetState(g.ctx, g.rs);
    return 0;
}

/* NV2A (GL) blend factor to D3D11; the alpha slot cannot take *_COLOR factors. */
static D3D11_BLEND gb_blend_factor(uint32_t f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return D3D11_BLEND_ONE;    /* as the CPU path's default */
    }
}

static D3D11_BLEND_OP gb_blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static ID3D11BlendState *gb_blend_state(uint32_t enable, uint32_t src, uint32_t dst,
                                        uint32_t eq, uint32_t mask)
{
    D3D11_BLEND_DESC bd;
    GbBlend *e;
    unsigned i;
    HRESULT hr;

    if (!enable)
        src = dst = eq = 0;
    for (i = 0; i < g.nblends; i++) {
        e = &g.blends[i];
        if (e->enable == enable && e->src == src && e->dst == dst && e->eq == eq
            && e->mask == mask)
            return e->bs;
    }
    if (g.nblends < GB_MAX_BLENDS) {
        e = &g.blends[g.nblends++];
    } else {
        e = &g.blends[g.next_blend++ % GB_MAX_BLENDS];
        GB_RELEASE(e->bs);
    }
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].BlendEnable = enable ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend = gb_blend_factor(src, 0);
    bd.RenderTarget[0].DestBlend = gb_blend_factor(dst, 0);
    bd.RenderTarget[0].BlendOp = gb_blend_op(eq);
    bd.RenderTarget[0].SrcBlendAlpha = gb_blend_factor(src, 1);
    bd.RenderTarget[0].DestBlendAlpha = gb_blend_factor(dst, 1);
    bd.RenderTarget[0].BlendOpAlpha = gb_blend_op(eq);
    bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)mask;
    if (!enable) {
        bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    }
    e->enable = enable;
    e->src = src;
    e->dst = dst;
    e->eq = eq;
    e->mask = mask;
    e->bs = NULL;
    hr = ID3D11Device_CreateBlendState(g.dev, &bd, &e->bs);
    if (FAILED(hr))
        gb_fail("blend state", hr);
    return e->bs;
}

static ID3D11DepthStencilState *gb_depth_state(uint32_t enable, uint32_t func, uint32_t write)
{
    D3D11_DEPTH_STENCIL_DESC dd;
    GbDss *e;
    unsigned i;
    HRESULT hr;

    for (i = 0; i < g.ndss; i++) {
        e = &g.dsss[i];
        if (e->enable == enable && e->func == func && e->write == write)
            return e->dss;
    }
    if (g.ndss < GB_MAX_DSS) {
        e = &g.dsss[g.ndss++];
    } else {
        e = &g.dsss[g.next_dss++ % GB_MAX_DSS];
        GB_RELEASE(e->dss);
    }
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = enable ? TRUE : FALSE;
    dd.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    /* GL_NEVER..GL_ALWAYS (0x200..0x207) are in D3D11's order, from 1. */
    dd.DepthFunc = (func >= 0x200 && func <= 0x207)
                 ? (D3D11_COMPARISON_FUNC)(func - 0x200 + 1) : D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = FALSE;
    e->enable = enable;
    e->func = func;
    e->write = write;
    e->dss = NULL;
    hr = ID3D11Device_CreateDepthStencilState(g.dev, &dd, &e->dss);
    if (FAILED(hr))
        gb_fail("depth-stencil state", hr);
    return e->dss;
}

/* ── Render targets ──────────────────────────────────────────────────── */

static void gb_unbind_all(void)
{
    ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 0, NULL, NULL);
    g.bound_rtv = NULL;
    g.bound_dsv = NULL;
}

static void gb_target_release(GbTarget *t)
{
    if (t->rtv && t->rtv == g.bound_rtv)
        gb_unbind_all();
    GB_RELEASE(t->copy_srv);
    GB_RELEASE(t->copy);
    GB_RELEASE(t->srv);
    GB_RELEASE(t->rtv);
    GB_RELEASE(t->staging);
    GB_RELEASE(t->tex);
    memset(t, 0, sizeof *t);
}

/* Write the target's unflushed rectangle into guest memory in the guest format. */
static void gb_target_flush(GbTarget *t)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BOX box;
    LARGE_INTEGER t0, t1;
    uint8_t *dst;
    uint32_t x, y;
    HRESULT hr;

    if (!t->dirty)
        return;
    QueryPerformanceCounter(&t0);
    if (!t->staging) {
        ID3D11Texture2D_GetDesc(t->tex, &td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = ID3D11Device_CreateTexture2D(g.dev, &td, NULL, &t->staging);
        if (FAILED(hr)) {
            gb_fail("staging texture", hr);
            t->dirty = 0;
            return;
        }
    }
    box.left = t->dx0;
    box.top = t->dy0;
    box.right = t->dx1;
    box.bottom = t->dy1;
    box.front = 0;
    box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(g.ctx, (ID3D11Resource *)t->staging, 0,
                                              t->dx0, t->dy0, 0,
                                              (ID3D11Resource *)t->tex, 0, &box);
    hr = ID3D11DeviceContext_Map(g.ctx, (ID3D11Resource *)t->staging, 0, D3D11_MAP_READ,
                                 0, &m);
    if (FAILED(hr)) {
        gb_fail("readback Map", hr);
        t->dirty = 0;
        return;
    }
    dst = gb_guest(t->va);
    for (y = t->dy0; y < t->dy1; y++) {
        const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData
                                                 + (size_t)y * m.RowPitch) + t->dx0;
        uint8_t *row = dst + (size_t)y * t->pitch;
        if (t->bpp == 4) {
            memcpy(row + (size_t)t->dx0 * 4, src, (size_t)(t->dx1 - t->dx0) * 4);
        } else {
            /* Truncated to 5:6:5, as the CPU rasteriser stores it. */
            uint16_t *p = (uint16_t *)row + t->dx0;
            for (x = 0; x < t->dx1 - t->dx0; x++) {
                uint32_t c = src[x];
                p[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0)
                                | ((c >> 3) & 0x001F));
            }
        }
    }
    ID3D11DeviceContext_Unmap(g.ctx, (ID3D11Resource *)t->staging, 0);
    t->dirty = 0;
    QueryPerformanceCounter(&t1);
    g.st.readbacks++;
    g.st.readback_ticks += (uint64_t)(t1.QuadPart - t0.QuadPart);
}

/* Bring the GPU copy up to date with guest memory if a flip made it stale. */
static void gb_target_load(GbTarget *t)
{
    const uint8_t *src;
    uint32_t x, y;

    if (!t->stale)
        return;
    src = gb_guest(t->va);
    if (t->bpp == 4) {
        /* A8R8G8B8 in memory is B8G8R8A8 byte for byte. */
        ID3D11DeviceContext_UpdateSubresource(g.ctx, (ID3D11Resource *)t->tex, 0, NULL,
                                              src, t->pitch, 0);
    } else {
        uint32_t *buf = gb_scratch((size_t)t->w * t->h);
        if (!buf)
            return;
        /* Expanded by shifting, as the CPU blender reads 5:6:5, so a round trip is exact. */
        for (y = 0; y < t->h; y++) {
            const uint16_t *row = (const uint16_t *)(src + (size_t)y * t->pitch);
            uint32_t *out = buf + (size_t)y * t->w;
            for (x = 0; x < t->w; x++) {
                uint32_t v = row[x];
                out[x] = 0xFF000000u | ((v & 0xF800u) << 8) | ((v & 0x07E0u) << 5)
                       | ((v & 0x001Fu) << 3);
            }
        }
        ID3D11DeviceContext_UpdateSubresource(g.ctx, (ID3D11Resource *)t->tex, 0, NULL,
                                              buf, t->w * 4, 0);
    }
    t->stale = 0;
    g.st.rt_loads++;
}

static int gb_target_create(GbTarget *t, uint32_t va, uint32_t pitch, uint32_t bpp,
                            uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;

    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(g.dev, &td, NULL, &t->tex);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateRenderTargetView(g.dev, (ID3D11Resource *)t->tex, NULL,
                                                 &t->rtv);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)t->tex, NULL,
                                                   &t->srv);
    if (FAILED(hr)) {
        gb_fail("render target", hr);
        gb_target_release(t);
        return 0;
    }
    t->used = 1;
    t->va = va;
    t->pitch = pitch;
    t->bpp = bpp;
    t->w = w;
    t->h = h;
    t->stale = 1;
    return 1;
}

/* The cached target for a surface, created or grown as needed; NULL if unusable.
 * The backing spans whole rows (pitch / pixel size) from row 0 to the clip's
 * bottom, because vertices are in surface pixels from (0,0). */
static GbTarget *gb_target_for(const Nv2aSurface *s)
{
    uint32_t bpp = s->bytes_per_pixel, w, h, i;
    GbTarget *t = NULL, *lru = NULL;

    if ((bpp != 2 && bpp != 4) || !s->color_va || !s->pitch || !s->width || !s->height)
        return NULL;
    w = s->pitch / bpp;
    h = s->clip_y + s->height;
    if (!w || w > GB_MAX_DIM || h > GB_MAX_DIM)
        return NULL;
    for (i = 0; i < GB_MAX_TARGETS; i++) {
        GbTarget *c = &g.targets[i];
        if (c->used && c->va == s->color_va) {
            t = c;
            break;
        }
        if (!c->used) {
            if (!lru || lru->used)
                lru = c;
        } else if (!lru || (lru->used && c->last_use < lru->last_use)) {
            lru = c;
        }
    }
    if (t && t->pitch == s->pitch && t->bpp == bpp && t->h >= h) {
        t->last_use = ++g.use_clock;
        return t;
    }
    if (t) {
        /* Same address, new shape: keep the taller backing, rebuilt from guest memory. */
        if (t->pitch == s->pitch && t->bpp == bpp && t->h > h)
            h = t->h;
        gb_target_flush(t);
        gb_target_release(t);
    } else {
        t = lru;
        if (t->used) {
            gb_target_flush(t);
            gb_target_release(t);
        }
    }
    if (!gb_target_create(t, s->color_va, s->pitch, bpp, w, h))
        return NULL;
    t->last_use = ++g.use_clock;
    return t;
}

static void gb_target_mark(GbTarget *t, const D3D11_RECT *r)
{
    if (!t->dirty) {
        t->dirty = 1;
        t->dx0 = (uint32_t)r->left;
        t->dy0 = (uint32_t)r->top;
        t->dx1 = (uint32_t)r->right;
        t->dy1 = (uint32_t)r->bottom;
        return;
    }
    if ((uint32_t)r->left < t->dx0) t->dx0 = (uint32_t)r->left;
    if ((uint32_t)r->top < t->dy0) t->dy0 = (uint32_t)r->top;
    if ((uint32_t)r->right > t->dx1) t->dx1 = (uint32_t)r->right;
    if ((uint32_t)r->bottom > t->dy1) t->dy1 = (uint32_t)r->bottom;
}

/* ── Depth buffers ───────────────────────────────────────────────────── */

static void gb_depth_release(GbDepth *d)
{
    if (d->dsv && d->dsv == g.bound_dsv)
        gb_unbind_all();
    GB_RELEASE(d->dsv);
    GB_RELEASE(d->tex);
    memset(d, 0, sizeof *d);
}

/* D3D11 needs the depth view the size of the target, so the key includes it. */
static GbDepth *gb_depth_for(uint32_t va, uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC td;
    GbDepth *d = NULL, *lru = NULL;
    HRESULT hr;
    unsigned i;

    for (i = 0; i < GB_MAX_DEPTHS; i++) {
        GbDepth *c = &g.depths[i];
        if (c->used && c->va == va && c->w == w && c->h == h) {
            c->last_use = ++g.use_clock;
            return c;
        }
        if (c->used && c->va == va && !d)
            d = c;                          /* same zeta, other size: replaced */
        if (!c->used) {
            if (!lru || lru->used)
                lru = c;
        } else if (!lru || (lru->used && c->last_use < lru->last_use)) {
            lru = c;
        }
    }
    if (!d)
        d = lru;
    if (d->used)
        gb_depth_release(d);
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr = ID3D11Device_CreateTexture2D(g.dev, &td, NULL, &d->tex);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateDepthStencilView(g.dev, (ID3D11Resource *)d->tex, NULL,
                                                 &d->dsv);
    if (FAILED(hr)) {
        gb_fail("depth buffer", hr);
        gb_depth_release(d);
        return NULL;
    }
    /* Far, as the CPU path's fresh buffers are, until the title clears it. */
    ID3D11DeviceContext_ClearDepthStencilView(g.ctx, d->dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    d->used = 1;
    d->va = va;
    d->w = w;
    d->h = h;
    d->last_use = ++g.use_clock;
    return d;
}

/* What the title's z is measured in. XDK D3D sets SET_CLIP_MAX to the depth
 * format's full scale (2^24-1 for Z24S8, 2^16-1 for Z16), which is also the
 * scale its viewport transform gives z; Z24 is assumed when it says nothing. */
static float gb_z_scale(const Nv2aRenderState *rs)
{
    if (rs->depth_max > 65535.5f)
        return 16777215.0f;
    if (rs->depth_max > 1.5f)
        return 65535.0f;
    return 16777215.0f;
}

/* ── Textures ────────────────────────────────────────────────────────── */

static void gb_tex_release(GbTex *e)
{
    GB_RELEASE(e->srv);
    GB_RELEASE(e->tex);
}

/* Decode a texture into the entry; a format the decoder rejects leaves srv NULL. */
static void gb_tex_upload(GbTex *e, const Nv2aTexture *tx)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init;
    uint32_t *buf = gb_scratch((size_t)tx->width * tx->height);
    HRESULT hr;

    gb_tex_release(e);
    if (!buf || !nv2a_backend_decode_texture(tx, buf)) {
        g.st.tex_undecodable++;
        return;
    }
    memset(&td, 0, sizeof td);
    td.Width = tx->width;
    td.Height = tx->height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;    /* 0xAARRGGBB words, as decoded */
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    init.pSysMem = buf;
    init.SysMemPitch = tx->width * 4;
    init.SysMemSlicePitch = 0;
    hr = ID3D11Device_CreateTexture2D(g.dev, &td, &init, &e->tex);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)e->tex, NULL,
                                                   &e->srv);
    if (FAILED(hr)) {
        gb_fail("texture", hr);
        gb_tex_release(e);
        return;
    }
    g.st.tex_uploads++;
}

/* Is this texture the pixels of a live render target, laid out as we store it? */
static int gb_tex_aliases(const Nv2aTexture *tx, const GbTarget *t)
{
    if (!t->used || t->va != tx->offset || t->pitch != tx->pitch)
        return 0;
    if (t->bpp == 2)
        return tx->color == 0x11;                         /* LIN_R5G6B5 */
    return tx->color == 0x12 || tx->color == 0x1E;        /* LIN_A8R8G8B8 / X8R8G8B8 */
}

/* The view to sample for stage 0, and the texel-to-normalised scale for it.
 * `out` is the target being drawn into, which must not also be sampled. */
static ID3D11ShaderResourceView *gb_texture(const Nv2aTexture *tx, GbTarget *out,
                                            float *inv_w, float *inv_h)
{
    uint8_t *mem;
    uint32_t span, i;
    GbTex *e = NULL, *lru = NULL;

    if (!tx->width || !tx->height || tx->width > GB_MAX_DIM || tx->height > GB_MAX_DIM)
        return NULL;

    for (i = 0; i < GB_MAX_TARGETS; i++) {
        GbTarget *t = &g.targets[i];
        if (!gb_tex_aliases(tx, t))
            continue;
        gb_target_load(t);
        g.st.rt_aliases++;
        *inv_w = 1.0f / (float)t->w;
        *inv_h = 1.0f / (float)t->h;
        if (t != out)
            return t->srv;
        /* Sampling the target being drawn: read a snapshot of it instead. */
        if (!t->copy) {
            D3D11_TEXTURE2D_DESC td;
            HRESULT hr;
            ID3D11Texture2D_GetDesc(t->tex, &td);
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            hr = ID3D11Device_CreateTexture2D(g.dev, &td, NULL, &t->copy);
            if (SUCCEEDED(hr))
                hr = ID3D11Device_CreateShaderResourceView(g.dev, (ID3D11Resource *)t->copy,
                                                           NULL, &t->copy_srv);
            if (FAILED(hr)) {
                gb_fail("target snapshot", hr);
                GB_RELEASE(t->copy_srv);
                GB_RELEASE(t->copy);
                return NULL;
            }
        }
        ID3D11DeviceContext_CopyResource(g.ctx, (ID3D11Resource *)t->copy,
                                         (ID3D11Resource *)t->tex);
        g.st.rt_copies++;
        return t->copy_srv;
    }

    span = nv2a_backend_texture_span(tx);
    if (!span)
        return NULL;
    /* Guest bytes that a target has overwritten on the GPU must land first. */
    for (i = 0; i < GB_MAX_TARGETS; i++) {
        GbTarget *t = &g.targets[i];
        uint64_t lo = t->va, hi = (uint64_t)t->va + (uint64_t)t->pitch * t->h;
        if (t->used && t->dirty && lo < (uint64_t)tx->offset + span && tx->offset < hi)
            gb_target_flush(t);
    }

    for (i = 0; i < GB_MAX_TEXTURES; i++) {
        GbTex *c = &g.texs[i];
        if (c->used && c->offset == tx->offset && c->color == tx->color
            && c->w == tx->width && c->h == tx->height && c->pitch == tx->pitch) {
            e = c;
            break;
        }
        if (!c->used) {
            if (!lru || lru->used)
                lru = c;
        } else if (!lru || (lru->used && c->last_use < lru->last_use)) {
            lru = c;
        }
    }
    mem = gb_guest(tx->offset);
    if (e) {
        if (e->frame != g.frame) {
            uint64_t h = gb_hash(mem, span);
            if (h != e->hash) {
                gb_tex_upload(e, tx);
                e->hash = h;
            } else {
                g.st.tex_hits++;
            }
            e->frame = g.frame;
        } else {
            g.st.tex_hits++;
        }
    } else {
        e = lru;
        gb_tex_release(e);
        memset(e, 0, sizeof *e);
        e->used = 1;
        e->offset = tx->offset;
        e->color = tx->color;
        e->w = tx->width;
        e->h = tx->height;
        e->pitch = tx->pitch;
        e->span = span;
        e->hash = gb_hash(mem, span);
        e->frame = g.frame;
        gb_tex_upload(e, tx);
    }
    e->last_use = ++g.use_clock;
    *inv_w = 1.0f / (float)tx->width;
    *inv_h = 1.0f / (float)tx->height;
    return e->srv;
}

/* ── Drawing ─────────────────────────────────────────────────────────── */

typedef struct {
    GbTarget *t;
    GbDepth *d;
    ID3D11ShaderResourceView *srv;
    ID3D11SamplerState *samp;
    ID3D11BlendState *bs;
    FLOAT blend_factor[4];
    ID3D11DepthStencilState *dss;
    GbConstants k;
    D3D11_RECT scissor;
} GbDraw;

static int gb_vertex_ok(const Nv2aVertex *v)
{
    return isfinite(v->x) && isfinite(v->y) && isfinite(v->z) && isfinite(v->rhw)
        && v->rhw != 0.0f;
}

/* Upload the usable triangles of `v` and draw them; returns how many were drawn. */
static uint32_t gb_submit(const GbDraw *dr, const Nv2aVertex *v, uint32_t n)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    ID3D11ShaderResourceView *none = NULL;
    Nv2aVertex *dst;
    uint32_t i, out = 0;
    HRESULT hr;

    if (n > g.vb_cap)
        n = g.vb_cap - g.vb_cap % 3;
    if (g.vb_pos + n > g.vb_cap) {
        mode = D3D11_MAP_WRITE_DISCARD;
        g.vb_pos = 0;
    }
    hr = ID3D11DeviceContext_Map(g.ctx, (ID3D11Resource *)g.vb, 0, mode, 0, &m);
    if (FAILED(hr)) {
        gb_fail("vertex buffer Map", hr);
        return 0;
    }
    dst = (Nv2aVertex *)m.pData + g.vb_pos;
    for (i = 0; i + 3 <= n; i += 3) {
        if (gb_vertex_ok(&v[i]) && gb_vertex_ok(&v[i + 1]) && gb_vertex_ok(&v[i + 2])) {
            memcpy(dst + out, v + i, 3 * sizeof *v);
            out += 3;
        } else {
            g.st.dropped_tris++;
        }
    }
    ID3D11DeviceContext_Unmap(g.ctx, (ID3D11Resource *)g.vb, 0);
    if (!out)
        return 0;

    hr = ID3D11DeviceContext_Map(g.ctx, (ID3D11Resource *)g.cb, 0, D3D11_MAP_WRITE_DISCARD,
                                 0, &m);
    if (FAILED(hr)) {
        gb_fail("constant buffer Map", hr);
        return 0;
    }
    memcpy(m.pData, &dr->k, sizeof dr->k);
    ID3D11DeviceContext_Unmap(g.ctx, (ID3D11Resource *)g.cb, 0);

    /* Unbind the texture first, so a target last sampled is never bound twice. */
    ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 1, &none);
    {
        ID3D11DepthStencilView *dsv = dr->d ? dr->d->dsv : NULL;
        if (g.bound_rtv != dr->t->rtv || g.bound_dsv != dsv) {
            D3D11_VIEWPORT vp;
            ID3D11DeviceContext_OMSetRenderTargets(g.ctx, 1, &dr->t->rtv, dsv);
            g.bound_rtv = dr->t->rtv;
            g.bound_dsv = dsv;
            vp.TopLeftX = 0.0f;
            vp.TopLeftY = 0.0f;
            vp.Width = (FLOAT)dr->t->w;
            vp.Height = (FLOAT)dr->t->h;
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            ID3D11DeviceContext_RSSetViewports(g.ctx, 1, &vp);
        }
    }
    ID3D11DeviceContext_RSSetScissorRects(g.ctx, 1, &dr->scissor);
    ID3D11DeviceContext_OMSetBlendState(g.ctx, dr->bs, dr->blend_factor, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g.ctx, dr->dss, 0);
    if (dr->srv) {
        ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 1, &dr->srv);
        ID3D11DeviceContext_PSSetSamplers(g.ctx, 0, 1, &dr->samp);
    }
    ID3D11DeviceContext_Draw(g.ctx, out, g.vb_pos);
    g.vb_pos += out;
    return out;
}

/* The clip rectangle, in target pixels; 0 if it is empty. */
static int gb_scissor(const Nv2aSurface *s, const GbTarget *t, D3D11_RECT *r)
{
    uint32_t x1 = s->clip_x + s->width, y1 = s->clip_y + s->height;

    if (x1 > t->w) x1 = t->w;
    if (y1 > t->h) y1 = t->h;
    if (s->clip_x >= x1 || s->clip_y >= y1)
        return 0;
    r->left = (LONG)s->clip_x;
    r->top = (LONG)s->clip_y;
    r->right = (LONG)x1;
    r->bottom = (LONG)y1;
    return 1;
}

static UINT8 gb_write_mask(uint32_t color_mask, uint32_t bpp)
{
    UINT8 m = 0;

    if (color_mask & 0x00010000u) m |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (color_mask & 0x00000100u) m |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (color_mask & 0x00000001u) m |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    /* A 5:6:5 target has no alpha; keeping ours at 1 makes DST_ALPHA read 1, as on the CPU path. */
    if ((color_mask & 0x01000000u) && bpp == 4) m |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    return m;
}

static uint32_t gb_sampler_mode(uint32_t addr)
{
    return addr == 1 ? 0u : addr == 2 ? 1u : 2u;    /* wrap, mirror, else clamp */
}

/* Is one half of SET_TEXTURE_FILTER (nv2a_regs.h NV097_SET_TEXTURE_FILTER_MIN
 * 0x00FF0000, _MAG 0x0F000000) a linear (tent) filter? 1 is box, 2 tent; the
 * MIN mip modes 3-6 (BOX/TENT_NEARESTLOD, BOX/TENT_TENT_LOD) keep their in-level
 * choice in the low bit, since only level 0 is uploaded. Anything else is linear. */
static int gb_filter_linear(uint32_t mode, int is_min)
{
    if (mode == 1 || (is_min && (mode == 3 || mode == 5)))
        return 0;
    return 1;
}

/* The sampler for a texture's filter and address modes, created on first use. */
static ID3D11SamplerState *gb_sampler(const Nv2aTexture *tx)
{
    static const D3D11_TEXTURE_ADDRESS_MODE m[3] = {
        D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_MIRROR, D3D11_TEXTURE_ADDRESS_CLAMP
    };
    static const D3D11_FILTER f[4] = {                 /* [min_linear * 2 + mag_linear] */
        D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT,
        D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT, D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT
    };
    uint32_t fi = (uint32_t)gb_filter_linear((tx->filter >> 16) & 0xFF, 1) * 2
                + (uint32_t)gb_filter_linear((tx->filter >> 24) & 0xF, 0);
    uint32_t ai = gb_sampler_mode(tx->addr_u) * 3 + gb_sampler_mode(tx->addr_v);
    D3D11_SAMPLER_DESC sd;
    HRESULT hr;

    if (!g.samp[fi][ai]) {
        memset(&sd, 0, sizeof sd);
        sd.Filter = f[fi];
        sd.AddressU = m[ai / 3];
        sd.AddressV = m[ai % 3];
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = ID3D11Device_CreateSamplerState(g.dev, &sd, &g.samp[fi][ai]);
        if (FAILED(hr))
            gb_fail("sampler state", hr);
    }
    return g.samp[fi][ai];
}

static void gb_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRenderState *rs = b->state;
    GbDraw dr;
    uint32_t drawn, bc = rs->blend_color;
    float zs;

    if (!g.ok || b->count < 3)
        return;
    memset(&dr, 0, sizeof dr);
    dr.t = gb_target_for(s);
    if (!dr.t || !gb_scissor(s, dr.t, &dr.scissor)) {
        g.st.skipped++;
        return;
    }
    gb_target_load(dr.t);
    if (rs->zeta_va)
        dr.d = gb_depth_for(rs->zeta_va, dr.t->w, dr.t->h);

    zs = gb_z_scale(rs);
    dr.k.xf[0] = 2.0f / (float)dr.t->w;
    dr.k.xf[1] = 2.0f / (float)dr.t->h;
    dr.k.xf[2] = 1.0f / zs;
    if (b->texture) {
        dr.srv = gb_texture(b->texture, dr.t, &dr.k.tx[0], &dr.k.tx[1]);
        dr.samp = gb_sampler(b->texture);
        if (!dr.samp)
            dr.srv = NULL;
    }
    dr.k.xf[3] = dr.srv ? 1.0f : 0.0f;
    dr.k.tx[2] = (rs->alpha_test_enable && rs->alpha_func >= 0x200 && rs->alpha_func < 0x207)
               ? (float)(rs->alpha_func - 0x200) : -1.0f;
    dr.k.tx[3] = (float)(rs->alpha_ref & 0xFF);

    dr.bs = gb_blend_state(rs->blend_enable ? 1u : 0u, rs->blend_src, rs->blend_dst,
                           rs->blend_eq, gb_write_mask(rs->color_mask, dr.t->bpp));
    if (rs->blend_src == 0x8003 || rs->blend_src == 0x8004
        || rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004) {
        /* D3D11 has no constant-alpha factor: every channel of the factor is the alpha. */
        dr.blend_factor[0] = dr.blend_factor[1] = dr.blend_factor[2] =
        dr.blend_factor[3] = (float)(bc >> 24) / 255.0f;
    } else {
        dr.blend_factor[0] = (float)((bc >> 16) & 0xFF) / 255.0f;
        dr.blend_factor[1] = (float)((bc >> 8) & 0xFF) / 255.0f;
        dr.blend_factor[2] = (float)(bc & 0xFF) / 255.0f;
        dr.blend_factor[3] = (float)(bc >> 24) / 255.0f;
    }
    if (!dr.d)
        dr.dss = gb_depth_state(0, 0x207, 0);
    else if (rs->depth_test_enable)
        dr.dss = gb_depth_state(1, rs->depth_func, rs->depth_write ? 1u : 0u);
    else if (rs->depth_write)
        dr.dss = gb_depth_state(1, 0x207, 1);    /* D3D11 cannot write depth with the test off */
    else
        dr.dss = gb_depth_state(0, 0x207, 0);
    if (!dr.bs || !dr.dss)
        return;

    drawn = gb_submit(&dr, b->vertices, b->count);
    if (!drawn)
        return;
    gb_target_mark(dr.t, &dr.scissor);
    g.st.draws++;
    g.st.tris += drawn / 3;
}

static void gb_clear(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags,
                     uint32_t argb, uint32_t zstencil)
{
    GbDraw dr;
    Nv2aVertex q[6];
    UINT8 mask = 0, all;
    float x0, y0, x1, y1, z;
    int i;

    if (!g.ok)
        return;
    memset(&dr, 0, sizeof dr);
    dr.t = gb_target_for(s);
    if (!dr.t || !gb_scissor(s, dr.t, &dr.scissor)) {
        g.st.skipped++;
        return;
    }
    /* CLEAR_SURFACE colour bits: R 0x10, G 0x20, B 0x40, A 0x80. */
    if (flags & 0x10) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (flags & 0x20) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (flags & 0x40) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if ((flags & 0x80) && dr.t->bpp == 4) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    all = dr.t->bpp == 4 ? D3D11_COLOR_WRITE_ENABLE_ALL
                         : (UINT8)(D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN
                                   | D3D11_COLOR_WRITE_ENABLE_BLUE);
    if (dr.t->stale) {
        /* A clear of every pixel and channel needs nothing from guest memory. */
        if (mask == all && dr.scissor.left == 0 && dr.scissor.top == 0
            && (uint32_t)dr.scissor.right == dr.t->w && (uint32_t)dr.scissor.bottom == dr.t->h)
            dr.t->stale = 0;
        else
            gb_target_load(dr.t);
    }
    if ((flags & 0x1) && rs->zeta_va)
        dr.d = gb_depth_for(rs->zeta_va, dr.t->w, dr.t->h);
    if (!mask && !dr.d)
        return;

    /* The same depth units the draws use, from the top 24 (Z24S8) or low 16 (Z16) bits. */
    z = gb_z_scale(rs) == 65535.0f ? (float)(zstencil & 0xFFFF) / 65535.0f
                                   : (float)(zstencil >> 8) / 16777215.0f;
    x0 = (float)dr.scissor.left;
    y0 = (float)dr.scissor.top;
    x1 = (float)dr.scissor.right;
    y1 = (float)dr.scissor.bottom;
    for (i = 0; i < 6; i++) {
        static const int cx[6] = { 0, 1, 0, 0, 1, 1 }, cy[6] = { 0, 0, 1, 1, 0, 1 };
        q[i].x = cx[i] ? x1 : x0;
        q[i].y = cy[i] ? y1 : y0;
        q[i].z = z;
        q[i].rhw = 1.0f;
        q[i].diffuse = argb;
        q[i].u = q[i].v = 0.0f;
    }
    dr.k.xf[0] = 2.0f / (float)dr.t->w;
    dr.k.xf[1] = 2.0f / (float)dr.t->h;
    dr.k.xf[2] = 1.0f;
    dr.k.xf[3] = 0.0f;
    dr.k.tx[2] = -1.0f;
    dr.bs = gb_blend_state(0, 0, 0, 0, mask);
    dr.dss = dr.d ? gb_depth_state(1, 0x207, 1) : gb_depth_state(0, 0x207, 0);
    if (!dr.bs || !dr.dss)
        return;
    if (!gb_submit(&dr, q, 6))
        return;
    if (mask)
        gb_target_mark(dr.t, &dr.scissor);
    g.st.clears++;
}

static void gb_stats(int force)
{
    LARGE_INTEGER now;
    double ms;

    QueryPerformanceCounter(&now);
    if (!force && (now.QuadPart - g.last_stats_qpc) * 1000 < (LONGLONG)GB_STATS_MS * g.qpf.QuadPart)
        return;
    g.last_stats_qpc = now.QuadPart;
    ms = g.qpf.QuadPart ? (double)g.st.readback_ticks * 1000.0 / (double)g.qpf.QuadPart : 0.0;
    fprintf(stderr, "[GPUBE] flips=%u draws=%llu tris=%llu clears=%llu tex_uploads=%llu"
                    " tex_hits=%llu tex_undecodable=%llu rt_aliases=%llu rt_copies=%llu"
                    " rt_loads=%llu readbacks=%llu readback_ms=%.1f dropped_tris=%llu"
                    " skipped=%llu\n",
            g.frame, (unsigned long long)g.st.draws, (unsigned long long)g.st.tris,
            (unsigned long long)g.st.clears, (unsigned long long)g.st.tex_uploads,
            (unsigned long long)g.st.tex_hits, (unsigned long long)g.st.tex_undecodable,
            (unsigned long long)g.st.rt_aliases, (unsigned long long)g.st.rt_copies,
            (unsigned long long)g.st.rt_loads, (unsigned long long)g.st.readbacks, ms,
            (unsigned long long)g.st.dropped_tris, (unsigned long long)g.st.skipped);
    fflush(stderr);
}

/* End of frame: every target drawn since the last flip goes back to guest
 * memory, and every target reloads from guest memory on its next use. */
static void gb_flip(void)
{
    unsigned i;

    if (!g.ok)
        return;
    for (i = 0; i < GB_MAX_TARGETS; i++) {
        if (!g.targets[i].used)
            continue;
        gb_target_flush(&g.targets[i]);
        g.targets[i].stale = 1;
    }
    g.frame++;
    gb_stats(!g.stats_said);
    g.stats_said = 1;
}

static const Nv2aBackend s_d3d11_backend = { gb_clear, gb_draw, gb_flip };

int nv2a_d3d11_backend_install(void)
{
    static int result = 1;                  /* 1: not attempted yet */

    if (result != 1)
        return result;
    result = -1;
    QueryPerformanceFrequency(&g.qpf);
    if (gb_create_device() != 0 || gb_create_pipeline() != 0) {
        fprintf(stderr, "[GPUBE] D3D11 back end not installed; the executor keeps its CPU path\n");
        fflush(stderr);
        return result;
    }
    g.ok = 1;
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        g.last_stats_qpc = now.QuadPart;
    }
    nv2a_backend_register(&s_d3d11_backend);
    fprintf(stderr, "[GPUBE] D3D11 back end registered%s\n",
            getenv("RECOMP_PB_EXEC") ? ""
                                     : " (RECOMP_PB_EXEC is not set, so nothing will call it)");
    fflush(stderr);
    result = 0;
    return result;
}

#endif /* _WIN32 */
