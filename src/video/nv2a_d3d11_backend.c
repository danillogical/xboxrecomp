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
 *   - When the batch carries register-combiner state, a pixel shader generated
 *     from it reproduces nv2a_rc_eval (src/kernel/nv2a_combiner.c) statement for
 *     statement, with all four texture stages. Shaders are cached by the
 *     registers that shape the program; the per-stage constants, fog colour and
 *     texture scales live in a constant buffer, so a fade is not a new shader.
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
#include <stdarg.h>
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
#define GB_MAX_RC        256              /* cached combiner pixel shaders */

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

/* The combiner path's interpolants, shared by its vertex shader and every
 * generated pixel shader. Colours and fog interpolate linearly in screen space
 * and texture coordinates perspective-correct, as the CPU program path does. */
#define GB_RC_VO \
    "struct VO2 { float4 p : SV_Position; noperspective float4 d0 : COLOR0;\n" \
    "  noperspective float4 d1 : COLOR1; noperspective float fog : TEXCOORD4;\n" \
    "  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2;\n" \
    "  float4 t3 : TEXCOORD3; };\n"

static const char s_hlsl_rc_vs[] =
    "cbuffer C : register(b0) { float4 g_xf; float4 g_tx; };\n"
    GB_RC_VO
    "struct VI2 { float3 p : POSITION; float rhw : TEXCOORD5; float4 d0 : COLOR0;\n"
    "  float4 d1 : COLOR1; float fog : TEXCOORD4; float4 t0 : TEXCOORD0;\n"
    "  float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
    "VO2 vs_rc(VI2 i) {\n"
    "  VO2 o;\n"
    "  float w = 1.0 / i.rhw;\n"
    "  o.p = float4((i.p.x * g_xf.x - 1.0) * w, (1.0 - i.p.y * g_xf.y) * w,\n"
    "               i.p.z * g_xf.z * w, w);\n"
    "  o.d0 = i.d0.zyxw; o.d1 = i.d1.zyxw; o.fog = i.fog;\n"
    "  o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;\n"
    "  return o;\n"
    "}\n";

/* What the combiner path uploads per vertex: Nv2aVertex and Nv2aVertexExtra merged. */
typedef struct {
    float x, y, z, rhw;
    uint32_t d0, d1;
    float fog;
    float t[4][4];
} GbRcVertex;

/* Pixel-shader constants of the combiner path (register b1). */
typedef struct {
    float c0[8][4], c1[8][4];           /* per-stage FACTOR0/1, r,g,b,a */
    float fc0[4], fc1[4];               /* SPECULAR_FOG_FACTOR0/1 */
    float fogc[4];                      /* fog colour */
    float ts[4][4];                     /* per stage: coordinate scale x,y; z 1 if bound */
    float at[4];                        /* alpha func index (-1 off), alpha ref */
} GbRcConstants;

/* The registers that shape a generated combiner program; unused stages are zero. */
typedef struct {
    uint32_t color_icw[8], alpha_icw[8], color_ocw[8], alpha_ocw[8];
    uint32_t stages, flags, final0, final1, stage_program, clip_plane_mode;
} GbRcKey;

typedef struct {
    int used, failed;
    uint64_t hash, last_use;
    GbRcKey key;
    ID3D11PixelShader *ps;
} GbRcShader;

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
    uint64_t rc_draws, rc_shaders, rc_compile_fail, rc_fallback, rc_compile_ticks;
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
    ID3D11VertexShader *vs_rc;          /* the combiner path's pipeline */
    ID3D11InputLayout *il_rc;
    ID3D11Buffer *vb_rc, *cb_rc;
    UINT vb_rc_cap, vb_rc_pos;
    int cur_kind;                       /* bound IA/VS: 0 none, 1 stage-0, 2 combiners */
    ID3D11PixelShader *cur_ps;
    GbRcShader rc[GB_MAX_RC];
    unsigned rc_fail_said;
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
    void *compile;                      /* D3DCompile, kept for combiner shaders */
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

static ID3DBlob *gb_compile_src(GbCompileFn compile, const char *src, size_t len,
                                const char *entry, const char *target, int quiet)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = compile(src, len, "nv2a_d3d11_backend", NULL, NULL,
                         entry, target, 0, 0, &code, &err);

    if (FAILED(hr)) {
        if (!quiet)
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

static ID3DBlob *gb_compile(GbCompileFn compile, const char *entry, const char *target)
{
    return gb_compile_src(compile, s_hlsl, sizeof s_hlsl - 1, entry, target, 0);
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
    static const D3D11_INPUT_ELEMENT_DESC layout_rc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GbRcVertex, x),
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 5, DXGI_FORMAT_R32_FLOAT, 0, offsetof(GbRcVertex, rhw),
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(GbRcVertex, d0),
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(GbRcVertex, d1),
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32_FLOAT, 0, offsetof(GbRcVertex, fog),
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(GbRcVertex, t[0]),
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(GbRcVertex, t[1]),
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(GbRcVertex, t[2]),
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(GbRcVertex, t[3]),
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    HMODULE dll;
    GbCompileFn compile;
    ID3DBlob *vsb, *psb, *rcb;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr;

    /* Loaded at run time so nothing links against d3dcompiler. */
    dll = LoadLibraryA("d3dcompiler_47.dll");
    compile = dll ? (GbCompileFn)(void (*)(void))GetProcAddress(dll, "D3DCompile") : NULL;
    if (!compile) {
        fprintf(stderr, "[GPUBE] d3dcompiler_47.dll / D3DCompile not available\n");
        return -1;
    }
    g.compile = (void *)compile;
    vsb = gb_compile(compile, "vs_main", "vs_4_0");
    psb = gb_compile(compile, "ps_main", "ps_4_0");
    rcb = gb_compile_src(compile, s_hlsl_rc_vs, sizeof s_hlsl_rc_vs - 1, "vs_rc", "vs_4_0", 0);
    if (!vsb || !psb || !rcb) {
        GB_RELEASE(vsb);
        GB_RELEASE(psb);
        GB_RELEASE(rcb);
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
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateVertexShader(g.dev, ID3D10Blob_GetBufferPointer(rcb),
                                             ID3D10Blob_GetBufferSize(rcb), NULL, &g.vs_rc);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateInputLayout(g.dev, layout_rc,
                                            (UINT)(sizeof layout_rc / sizeof layout_rc[0]),
                                            ID3D10Blob_GetBufferPointer(rcb),
                                            ID3D10Blob_GetBufferSize(rcb), &g.il_rc);
    GB_RELEASE(vsb);
    GB_RELEASE(psb);
    GB_RELEASE(rcb);
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
    bd.ByteWidth = GB_VB_BYTES * 4;       /* 92-byte vertices: still a full batch */
    hr = ID3D11Device_CreateBuffer(g.dev, &bd, NULL, &g.vb_rc);
    if (FAILED(hr)) {
        gb_fail("combiner vertex buffer", hr);
        return -1;
    }
    g.vb_rc_cap = (GB_VB_BYTES * 4) / sizeof(GbRcVertex);
    g.vb_rc_pos = g.vb_rc_cap;

    bd.ByteWidth = sizeof(GbConstants);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = ID3D11Device_CreateBuffer(g.dev, &bd, NULL, &g.cb);
    if (SUCCEEDED(hr)) {
        bd.ByteWidth = sizeof(GbRcConstants);
        hr = ID3D11Device_CreateBuffer(g.dev, &bd, NULL, &g.cb_rc);
    }
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

    /* The context is private to this file, so the fixed state is set once;
     * gb_pipeline switches the input layout, buffers and shaders per draw. */
    ID3D11DeviceContext_IASetPrimitiveTopology(g.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetConstantBuffers(g.ctx, 0, 1, &g.cb);
    ID3D11DeviceContext_PSSetConstantBuffers(g.ctx, 0, 1, &g.cb);
    ID3D11DeviceContext_PSSetConstantBuffers(g.ctx, 1, 1, &g.cb_rc);
    ID3D11DeviceContext_RSSetState(g.ctx, g.rs);
    return 0;
}

/* Bind the stage-0 pipeline (ps NULL) or the combiner pipeline with `ps`. */
static void gb_pipeline(ID3D11PixelShader *ps)
{
    int kind = ps ? 2 : 1;

    if (g.cur_kind != kind) {
        UINT stride = kind == 2 ? (UINT)sizeof(GbRcVertex) : (UINT)sizeof(Nv2aVertex);
        UINT offset = 0;
        ID3D11DeviceContext_IASetInputLayout(g.ctx, kind == 2 ? g.il_rc : g.il);
        ID3D11DeviceContext_IASetVertexBuffers(g.ctx, 0, 1, kind == 2 ? &g.vb_rc : &g.vb,
                                               &stride, &offset);
        ID3D11DeviceContext_VSSetShader(g.ctx, kind == 2 ? g.vs_rc : g.vs, NULL, 0);
        g.cur_kind = kind;
    }
    if (!ps)
        ps = g.ps;
    if (g.cur_ps != ps) {
        ID3D11DeviceContext_PSSetShader(g.ctx, ps, NULL, 0);
        g.cur_ps = ps;
    }
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
    /* Combiner path: the generated shader, all four stages, and its constants. */
    ID3D11PixelShader *rc_ps;
    ID3D11ShaderResourceView *srvs[4];
    ID3D11SamplerState *samps[4];
    GbRcConstants rck;
} GbDraw;

static int gb_vertex_ok(const Nv2aVertex *v)
{
    return isfinite(v->x) && isfinite(v->y) && isfinite(v->z) && isfinite(v->rhw)
        && v->rhw != 0.0f;
}

/* Map a dynamic ring buffer for `n` more vertices of `stride` bytes. */
static void *gb_ring_map(ID3D11Buffer *vb, UINT cap, UINT *pos, uint32_t n, UINT stride)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    HRESULT hr;

    if (*pos + n > cap) {
        mode = D3D11_MAP_WRITE_DISCARD;
        *pos = 0;
    }
    hr = ID3D11DeviceContext_Map(g.ctx, (ID3D11Resource *)vb, 0, mode, 0, &m);
    if (FAILED(hr)) {
        gb_fail("vertex buffer Map", hr);
        return NULL;
    }
    return (uint8_t *)m.pData + (size_t)*pos * stride;
}

static int gb_cb_write(ID3D11Buffer *cb, const void *data, size_t bytes)
{
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr = ID3D11DeviceContext_Map(g.ctx, (ID3D11Resource *)cb, 0,
                                         D3D11_MAP_WRITE_DISCARD, 0, &m);
    if (FAILED(hr)) {
        gb_fail("constant buffer Map", hr);
        return 0;
    }
    memcpy(m.pData, data, bytes);
    ID3D11DeviceContext_Unmap(g.ctx, (ID3D11Resource *)cb, 0);
    return 1;
}

/* Upload the usable triangles of `v` (with `x` on the combiner path) and draw
 * them; returns how many vertices were drawn. */
static uint32_t gb_submit(const GbDraw *dr, const Nv2aVertex *v, const Nv2aVertexExtra *x,
                          uint32_t n)
{
    ID3D11ShaderResourceView *none[4] = { NULL, NULL, NULL, NULL };
    int rc = dr->rc_ps != NULL;
    UINT cap = rc ? g.vb_rc_cap : g.vb_cap, *pos = rc ? &g.vb_rc_pos : &g.vb_pos;
    uint8_t *dst;
    uint32_t i, j, out = 0;

    if (n > cap)
        n = cap - cap % 3;
    dst = (uint8_t *)gb_ring_map(rc ? g.vb_rc : g.vb, cap, pos, n,
                                 rc ? (UINT)sizeof(GbRcVertex) : (UINT)sizeof(Nv2aVertex));
    if (!dst)
        return 0;
    for (i = 0; i + 3 <= n; i += 3) {
        if (!(gb_vertex_ok(&v[i]) && gb_vertex_ok(&v[i + 1]) && gb_vertex_ok(&v[i + 2]))) {
            g.st.dropped_tris++;
            continue;
        }
        if (!rc) {
            memcpy((Nv2aVertex *)dst + out, v + i, 3 * sizeof *v);
        } else {
            for (j = i; j < i + 3; j++) {
                GbRcVertex *o = (GbRcVertex *)dst + out + (j - i);
                o->x = v[j].x;
                o->y = v[j].y;
                o->z = v[j].z;
                o->rhw = v[j].rhw;
                o->d0 = v[j].diffuse;
                o->d1 = x[j].specular;
                o->fog = x[j].fog;
                memcpy(o->t, x[j].tex, sizeof o->t);
            }
        }
        out += 3;
    }
    ID3D11DeviceContext_Unmap(g.ctx, (ID3D11Resource *)(rc ? g.vb_rc : g.vb), 0);
    if (!out)
        return 0;
    if (!gb_cb_write(g.cb, &dr->k, sizeof dr->k))
        return 0;
    if (rc && !gb_cb_write(g.cb_rc, &dr->rck, sizeof dr->rck))
        return 0;

    /* Unbind textures first, so a target last sampled is never bound twice. */
    ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 4, none);
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
    gb_pipeline(dr->rc_ps);
    if (rc) {
        ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 4, dr->srvs);
        ID3D11DeviceContext_PSSetSamplers(g.ctx, 0, 4, dr->samps);
    } else if (dr->srv) {
        ID3D11DeviceContext_PSSetShaderResources(g.ctx, 0, 1, &dr->srv);
        ID3D11DeviceContext_PSSetSamplers(g.ctx, 0, 1, &dr->samp);
    }
    ID3D11DeviceContext_Draw(g.ctx, out, *pos);
    *pos += out;
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

/* ── Register combiners ──────────────────────────────────────────────
 *
 * The generated pixel shader is nv2a_rc_eval (src/kernel/nv2a_combiner.c)
 * transcribed: the same register file R0..R15, the same input mappings, the
 * same order of reads and writes, so a back-end pixel and a CPU pixel come from
 * the same arithmetic. Register numbers are an input byte's low nibble:
 *   0 zero  1 c0  2 c1  3 fog  4 v0  5 v1  8-11 t0-t3  12 r0  13 r1
 *   14 v1+r0 sum and 15 E*F (final combiner only). */

typedef struct { char *p; size_t n, cap; } GbStr;

static void gb_cat(GbStr *b, const char *fmt, ...)
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

/* PS_INPUTMAPPING of `e`, as nv2a_combiner.c map_in. */
static void gb_map_in(char *o, size_t cap, const char *e, uint32_t byte)
{
    static const char *const f[8] = {
        "max(%s, 0)", "(1 - saturate(%s))", "(2 * max(%s, 0) - 1)", "(-2 * max(%s, 0) + 1)",
        "(max(%s, 0) - 0.5)", "(-max(%s, 0) + 0.5)", "(%s)", "(-(%s))"
    };
    snprintf(o, cap, f[(byte >> 5) & 7], e);
}

/* An input byte read as RGB: the alpha bit replicates .a. */
static void gb_in_rgb(char *o, size_t cap, uint32_t byte)
{
    char e[16];
    snprintf(e, sizeof e, (byte & 0x10) ? "R%u.aaa" : "R%u.rgb", byte & 0xF);
    gb_map_in(o, cap, e, byte);
}

/* An input byte read as alpha: the alpha bit picks .a over .b. */
static void gb_in_a(char *o, size_t cap, uint32_t byte)
{
    char e[16];
    snprintf(e, sizeof e, (byte & 0x10) ? "R%u.a" : "R%u.b", byte & 0xF);
    gb_map_in(o, cap, e, byte);
}

/* PS_COMBINEROUTPUT scale/bias, clamped to [-1, 1], as map_out. */
static void gb_out(char *o, size_t cap, const char *e, uint32_t mapping)
{
    const char *f;
    switch (mapping) {
    case 0x08: f = "clamp(%s - 0.5, -1, 1)"; break;
    case 0x10: f = "clamp(%s * 2, -1, 1)"; break;
    case 0x18: f = "clamp((%s - 0.5) * 2, -1, 1)"; break;
    case 0x20: f = "clamp(%s * 4, -1, 1)"; break;
    case 0x30: f = "clamp(%s * 0.5, -1, 1)"; break;
    default:   f = "clamp(%s, -1, 1)"; break;
    }
    snprintf(o, cap, f, e);
}

static uint32_t gb_stage_mode(const GbRcKey *k, int st)
{
    return (k->stage_program >> (st * 5)) & 0x1F;
}

/* Fill `k` from the batch; 0 if a stage needs something the shader cannot do. */
static int gb_rc_key(const Nv2aBatch *b, GbRcKey *k)
{
    const Nv2aCombiner *rc = b->combiner;
    uint32_t i, n = rc->control & 0xFF;
    int st, clip = 0;

    memset(k, 0, sizeof *k);
    if (n > 8)
        n = 8;
    for (i = 0; i < n; i++) {
        k->color_icw[i] = rc->color_icw[i];
        k->alpha_icw[i] = rc->alpha_icw[i];
        k->color_ocw[i] = rc->color_ocw[i];
        k->alpha_ocw[i] = rc->alpha_ocw[i];
    }
    k->stages = n;
    k->flags = (rc->control >> 8) & 0x111;          /* mux MSB, per-stage c0, c1 */
    k->final0 = rc->final0;
    k->final1 = rc->final1;
    k->stage_program = rc->stage_program & 0xFFFFF;
    for (st = 0; st < 4; st++) {
        uint32_t m = gb_stage_mode(k, st);
        /* Bump-environment and dot-product modes, and true cube maps, are not translated. */
        if (m > 5)
            return 0;
        if (m == 3 && b->textures[st] && b->textures[st]->cube)
            return 0;
        if (m == 5)
            clip = 1;
    }
    if (clip)
        k->clip_plane_mode = b->clip_plane_mode & 0xFFFF;
    return 1;
}

/* What texture stage st contributes, as rc_stage_fetch in nv2a_pb_exec.c. A
 * stage with no usable texture samples white there; here its g_ts.z is 0. */
static void gb_rc_fetch(GbStr *b, const GbRcKey *k, int st)
{
    uint32_t m = gb_stage_mode(k, st), j;
    int r = 8 + st;

    switch (m) {
    case 0:                                         /* NONE */
        gb_cat(b, "  R%d = float4(0, 0, 0, 1);\n", r);
        break;
    case 1: case 2:                                 /* PROJECT2D / 3D */
        gb_cat(b, "  { float q = i.t%d.w != 0 ? i.t%d.w : 1;\n"
                  "    R%d = lerp(float4(1, 1, 1, 1), tx%d.SampleLevel(sm%d,"
                  " i.t%d.xy / q * g_ts[%d].xy, 0), g_ts[%d].z); }\n",
               st, st, r, st, st, st, st, st);
        break;
    case 3:                                         /* CUBEMAP on a 2D texture */
        gb_cat(b, "  R%d = lerp(float4(1, 1, 1, 1), tx%d.SampleLevel(sm%d,"
                  " i.t%d.xy * g_ts[%d].xy, 0), g_ts[%d].z);\n", r, st, st, st, st, st);
        break;
    case 4:                                         /* PASSTHRU */
        gb_cat(b, "  R%d = saturate(i.t%d);\n", r, st);
        break;
    default:                                        /* CLIPPLANE */
        for (j = 0; j < 4; j++) {
            int ge = (k->clip_plane_mode >> (st * 4 + j)) & 1;
            gb_cat(b, "  if (i.t%d.%c %s 0) discard;\n", st, "xyzw"[j], ge ? ">=" : "<");
        }
        gb_cat(b, "  R%d = 0;\n", r);
        break;
    }
}

/* One general combiner stage, as the stage loop of nv2a_rc_eval. */
static void gb_rc_stage(GbStr *b, const GbRcKey *k, uint32_t s)
{
    uint32_t icw = k->color_icw[s], ocw = k->color_ocw[s];
    uint32_t aicw = k->alpha_icw[s], aocw = k->alpha_ocw[s];
    uint32_t fl = ocw >> 12, afl = aocw >> 12;
    char A[64], B[64], C[64], D[64], aA[64], aB[64], aC[64], aD[64];
    char ab[64], cd[64], ms[64], aab[64], acd[64], ams[64];

    gb_cat(b, "  {\n    R1 = g_c0[%u]; R2 = g_c1[%u];\n",
           (k->flags & 0x010) ? s : 0u, (k->flags & 0x100) ? s : 0u);
    gb_cat(b, (k->flags & 1) ? "    bool mx = R12.a >= 0.5;\n"
                             : "    bool mx = (((int)(R12.a * 255.0)) & 1) != 0;\n");
    gb_in_rgb(A, sizeof A, icw >> 24); gb_in_rgb(B, sizeof B, icw >> 16);
    gb_in_rgb(C, sizeof C, icw >> 8);  gb_in_rgb(D, sizeof D, icw);
    gb_in_a(aA, sizeof aA, aicw >> 24); gb_in_a(aB, sizeof aB, aicw >> 16);
    gb_in_a(aC, sizeof aC, aicw >> 8);  gb_in_a(aD, sizeof aD, aicw);
    /* Both portions read before either writes. */
    gb_cat(b, "    float3 A = %s, B = %s, C = %s, D = %s;\n", A, B, C, D);
    gb_cat(b, "    float aA = %s, aB = %s, aC = %s, aD = %s;\n", aA, aB, aC, aD);
    gb_cat(b, "    float3 pab = %s, pcd = %s;\n",
           (fl & 2) ? "dot(A, B).xxx" : "A * B", (fl & 1) ? "dot(C, D).xxx" : "C * D");
    gb_cat(b, "    float3 psm = %s;\n", (fl & 4) ? "(mx ? pcd : pab)" : "pab + pcd");
    gb_out(ab, sizeof ab, "pab", fl & 0x38);
    gb_out(cd, sizeof cd, "pcd", fl & 0x38);
    gb_out(ms, sizeof ms, "psm", fl & 0x38);
    gb_cat(b, "    float3 oab = %s, ocd = %s, osm = %s;\n", ab, cd, ms);
    gb_cat(b, "    float qab = aA * aB, qcd = aC * aD;\n");
    gb_cat(b, "    float qsm = %s;\n", (afl & 4) ? "(mx ? qcd : qab)" : "qab + qcd");
    gb_out(aab, sizeof aab, "qab", afl & 0x38);
    gb_out(acd, sizeof acd, "qcd", afl & 0x38);
    gb_out(ams, sizeof ams, "qsm", afl & 0x38);
    gb_cat(b, "    float wab = %s, wcd = %s, wsm = %s;\n", aab, acd, ams);
    /* RGB destinations; blue-to-alpha also writes the destination's .a. */
    if ((ocw >> 4) & 0xF) {
        gb_cat(b, "    R%u.rgb = oab;\n", (ocw >> 4) & 0xF);
        if (fl & 0x80)
            gb_cat(b, "    R%u.a = oab.b;\n", (ocw >> 4) & 0xF);
    }
    if (ocw & 0xF) {
        gb_cat(b, "    R%u.rgb = ocd;\n", ocw & 0xF);
        if (fl & 0x40)
            gb_cat(b, "    R%u.a = ocd.b;\n", ocw & 0xF);
    }
    if ((ocw >> 8) & 0xF)
        gb_cat(b, "    R%u.rgb = osm;\n", (ocw >> 8) & 0xF);
    if ((aocw >> 4) & 0xF)
        gb_cat(b, "    R%u.a = wab;\n", (aocw >> 4) & 0xF);
    if (aocw & 0xF)
        gb_cat(b, "    R%u.a = wcd;\n", aocw & 0xF);
    if ((aocw >> 8) & 0xF)
        gb_cat(b, "    R%u.a = wsm;\n", (aocw >> 8) & 0xF);
    /* Register 0 is zero whatever was written to it. */
    gb_cat(b, "    R0 = 0;\n  }\n");
}

/* The whole pixel shader for key `k`; returns its length, 0 if it overflowed. */
static size_t gb_rc_source(const GbRcKey *k, char *buf, size_t cap)
{
    GbStr b = { buf, 0, cap };
    char A[64], B[64], C[64], D[64], E[64], F[64], G[64];
    uint32_t s;
    int st;

    gb_cat(&b, "cbuffer RC : register(b1) {\n"
               "  float4 g_c0[8]; float4 g_c1[8]; float4 g_fc0; float4 g_fc1;\n"
               "  float4 g_fogc; float4 g_ts[4]; float4 g_at; };\n");
    gb_cat(&b, "%s", GB_RC_VO);
    for (st = 0; st < 4; st++)
        gb_cat(&b, "Texture2D tx%d : register(t%d); SamplerState sm%d : register(s%d);\n",
               st, st, st, st);
    gb_cat(&b, "float4 ps_rc(VO2 i) : SV_Target {\n"
               "  float4 R0 = 0, R1 = 0, R2 = 0, R3 = float4(g_fogc.rgb, i.fog);\n"
               "  float4 R4 = i.d0, R5 = i.d1, R6 = 0, R7 = 0, R8, R9, R10, R11;\n"
               "  float4 R12 = 0, R13 = 0, R14 = 0, R15 = 0, o;\n");
    for (st = 0; st < 4; st++)
        gb_rc_fetch(&b, k, st);
    /* r0.a starts as texture 0's alpha, or 1 with stage 0 off. */
    gb_cat(&b, "  R12.a = %s;\n", gb_stage_mode(k, 0) ? "R8.a" : "1");
    for (s = 0; s < k->stages; s++)
        gb_rc_stage(&b, k, s);
    if (k->final0 || k->final1) {
        uint32_t f0 = k->final0, f1 = k->final1, ff = f1 & 0xFF;
        gb_cat(&b, "  R1 = g_fc0; R2 = g_fc1;\n");
        /* V1R0 sum: optional complements (0x40 v1, 0x20 r0), clamp (0x80). */
        gb_cat(&b, "  { float3 sa = %s, sb = %s; R14.rgb = %s; R14.a = 0; }\n",
               (ff & 0x40) ? "1 - R5.rgb" : "R5.rgb", (ff & 0x20) ? "1 - R12.rgb" : "R12.rgb",
               (ff & 0x80) ? "saturate(sa + sb)" : "sa + sb");
        gb_in_rgb(E, sizeof E, f1 >> 24);
        gb_in_rgb(F, sizeof F, f1 >> 16);
        gb_cat(&b, "  { float3 E = %s, F = %s; R15.rgb = E * F; R15.a = 0; }\n", E, F);
        gb_in_rgb(A, sizeof A, f0 >> 24); gb_in_rgb(B, sizeof B, f0 >> 16);
        gb_in_rgb(C, sizeof C, f0 >> 8);  gb_in_rgb(D, sizeof D, f0);
        gb_in_a(G, sizeof G, f1 >> 8);
        /* D + mix(C, B, A), alpha from G. */
        gb_cat(&b, "  { float3 A = %s, B = %s, C = %s, D = %s;\n"
                   "    o.rgb = D + C * (1 - A) + B * A; o.a = %s; }\n", A, B, C, D, G);
    } else {
        gb_cat(&b, "  o = R12;\n");
    }
    gb_cat(&b, "  o = saturate(o);\n"
               "  if (g_at.x >= 0.0) {\n"
               "    int v = (int)(o.a * 255.0 + 0.5), r = (int)g_at.y, f = (int)g_at.x;\n"
               "    bool keep = f == 7 || (f == 1 && v < r) || (f == 2 && v == r)\n"
               "             || (f == 3 && v <= r) || (f == 4 && v > r) || (f == 5 && v != r)\n"
               "             || (f == 6 && v >= r);\n"
               "    if (!keep) discard;\n"
               "  }\n"
               "  return o;\n}\n");
    return b.n < b.cap ? b.n : 0;
}

/* The compiled shader for key `k`, generated on first use; NULL if it failed. */
static ID3D11PixelShader *gb_rc_shader(const GbRcKey *k)
{
    static char src[32768];
    uint64_t h = gb_hash((const uint8_t *)k, sizeof *k);
    GbRcShader *e = NULL, *lru = NULL;
    LARGE_INTEGER t0, t1;
    ID3DBlob *code;
    size_t len;
    unsigned i;
    HRESULT hr;

    for (i = 0; i < GB_MAX_RC; i++) {
        GbRcShader *c = &g.rc[i];
        if (c->used && c->hash == h && memcmp(&c->key, k, sizeof *k) == 0) {
            c->last_use = ++g.use_clock;
            return c->ps;
        }
        if (!c->used) {
            if (!lru || lru->used)
                lru = c;
        } else if (!lru || (lru->used && c->last_use < lru->last_use)) {
            lru = c;
        }
    }
    e = lru;
    GB_RELEASE(e->ps);
    memset(e, 0, sizeof *e);
    e->used = 1;
    e->hash = h;
    e->key = *k;
    e->last_use = ++g.use_clock;
    QueryPerformanceCounter(&t0);
    len = gb_rc_source(k, src, sizeof src);
    code = len ? gb_compile_src((GbCompileFn)g.compile, src, len, "ps_rc", "ps_4_0", 1) : NULL;
    if (code) {
        hr = ID3D11Device_CreatePixelShader(g.dev, ID3D10Blob_GetBufferPointer(code),
                                            ID3D10Blob_GetBufferSize(code), NULL, &e->ps);
        GB_RELEASE(code);
        if (FAILED(hr)) {
            gb_fail("combiner pixel shader", hr);
            e->ps = NULL;
        }
    }
    QueryPerformanceCounter(&t1);
    g.st.rc_compile_ticks += (uint64_t)(t1.QuadPart - t0.QuadPart);
    if (e->ps) {
        g.st.rc_shaders++;
    } else {
        e->failed = 1;
        g.st.rc_compile_fail++;
        if (g.rc_fail_said++ < 2) {
            /* Recompile loudly once, so the error and the source are in the log. */
            fprintf(stderr, "[GPUBE] combiner shader failed (stages=%u final=%08X/%08X"
                            " program=%05X); source:\n%s\n",
                    k->stages, k->final0, k->final1, k->stage_program, len ? src : "(overflow)");
            if (len) {
                code = gb_compile_src((GbCompileFn)g.compile, src, len, "ps_rc", "ps_4_0", 0);
                GB_RELEASE(code);
            }
            fflush(stderr);
        }
    }
    return e->ps;
}

static void gb_unpack(uint32_t argb, float o[4])
{
    o[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(argb & 0xFF) / 255.0f;
    o[3] = (float)(argb >> 24) / 255.0f;
}

/* Set up the combiner half of a draw; 0 means draw it the stage-0 way instead. */
static int gb_rc_prepare(GbDraw *dr, const Nv2aBatch *b)
{
    const Nv2aCombiner *rc = b->combiner;
    GbRcKey key;
    int st, i;

    if (!gb_rc_key(b, &key))
        return 0;
    dr->rc_ps = gb_rc_shader(&key);
    if (!dr->rc_ps)
        return 0;
    for (i = 0; i < 8; i++) {
        gb_unpack(rc->factor0[i], dr->rck.c0[i]);
        gb_unpack(rc->factor1[i], dr->rck.c1[i]);
    }
    gb_unpack(rc->final_c0, dr->rck.fc0);
    gb_unpack(rc->final_c1, dr->rck.fc1);
    gb_unpack(b->fog_color, dr->rck.fogc);
    for (st = 0; st < 4; st++) {
        const Nv2aTexture *tx = b->textures[st];
        uint32_t m = gb_stage_mode(&key, st);
        float iw = 1.0f, ih = 1.0f;
        dr->rck.ts[st][0] = dr->rck.ts[st][1] = 1.0f;
        if (!tx || (m != 1 && m != 2 && m != 3))
            continue;
        dr->srvs[st] = gb_texture(tx, dr->t, &iw, &ih);
        dr->samps[st] = gb_sampler(tx);
        if (!dr->samps[st])
            dr->srvs[st] = NULL;
        /* Linear formats are addressed in texels, the rest in [0, 1]. */
        if (tx->linear) {
            dr->rck.ts[st][0] = iw;
            dr->rck.ts[st][1] = ih;
        }
        dr->rck.ts[st][2] = dr->srvs[st] ? 1.0f : 0.0f;
    }
    dr->rck.at[0] = dr->k.tx[2];
    dr->rck.at[1] = dr->k.tx[3];
    return 1;
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
    dr.k.tx[2] = (rs->alpha_test_enable && rs->alpha_func >= 0x200 && rs->alpha_func < 0x207)
               ? (float)(rs->alpha_func - 0x200) : -1.0f;
    dr.k.tx[3] = (float)(rs->alpha_ref & 0xFF);
    if (b->combiner && b->extra) {
        if (gb_rc_prepare(&dr, b)) {
            g.st.rc_draws++;
        } else {
            /* Not translatable: stage 0 modulated by diffuse, as before combiners. */
            g.st.rc_fallback++;
            dr.rc_ps = NULL;
            memset(dr.srvs, 0, sizeof dr.srvs);
            memset(dr.samps, 0, sizeof dr.samps);
        }
    }
    if (!dr.rc_ps && b->texture) {
        dr.srv = gb_texture(b->texture, dr.t, &dr.k.tx[0], &dr.k.tx[1]);
        dr.samp = gb_sampler(b->texture);
        if (!dr.samp)
            dr.srv = NULL;
    }
    dr.k.xf[3] = dr.srv ? 1.0f : 0.0f;

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

    drawn = gb_submit(&dr, b->vertices, dr.rc_ps ? b->extra : NULL, b->count);
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
    if (!gb_submit(&dr, q, NULL, 6))
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
                    " skipped=%llu rc_draws=%llu rc_shaders=%llu rc_compile_fail=%llu"
                    " rc_fallback=%llu rc_compile_ms=%.1f\n",
            g.frame, (unsigned long long)g.st.draws, (unsigned long long)g.st.tris,
            (unsigned long long)g.st.clears, (unsigned long long)g.st.tex_uploads,
            (unsigned long long)g.st.tex_hits, (unsigned long long)g.st.tex_undecodable,
            (unsigned long long)g.st.rt_aliases, (unsigned long long)g.st.rt_copies,
            (unsigned long long)g.st.rt_loads, (unsigned long long)g.st.readbacks, ms,
            (unsigned long long)g.st.dropped_tris, (unsigned long long)g.st.skipped,
            (unsigned long long)g.st.rc_draws, (unsigned long long)g.st.rc_shaders,
            (unsigned long long)g.st.rc_compile_fail, (unsigned long long)g.st.rc_fallback,
            g.qpf.QuadPart ? (double)g.st.rc_compile_ticks * 1000.0 / (double)g.qpf.QuadPart
                           : 0.0);
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
