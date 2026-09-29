/*
 * NV2A model contract tests: the pushbuffer walk and the PGRAPH registers the
 * guest's interrupt handler touches, driven through the real MMIO entry
 * points of nv2a_core.c against a fake physical window and instance memory.
 *
 * The D3D11 translator is replaced by a recorder so the tests can see which
 * methods would have reached it.
 */
#include "nv2a_state.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            ++g_failures;                                                   \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

/* ── translator recorder ─────────────────────────────────────────────── */

static unsigned g_translator_calls;

int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param)
{
    (void)subchannel; (void)method; (void)param;
    ++g_translator_calls;
    return 1;
}

/* ── fixture ─────────────────────────────────────────────────────────── */

/* Guest physical 0.. as the contiguous window; pushbuffers sit below 64 KiB
 * so no offset carries the kick bit. */
static uint8_t g_window[1u << 20];
static uint8_t g_ramin[64u << 10];
static uint8_t g_vram[4096];

#define PB_BASE        0x1000u

#define H_KELVIN       0x0000000Du   /* small handles: the fold keeps them in a 4 KiB table */
#define H_MEMCPY       0x0000000Eu

#define INST_KELVIN    0x1000u
#define INST_MEMCPY    0x1010u

static void wr32(uint8_t *base, uint32_t off, uint32_t v)
{
    memcpy(base + off, &v, 4);
}

static uint32_t ramht_slot(uint32_t handle)
{
    uint32_t hash = 0;
    while (handle) {
        hash ^= handle & 0x7FFu;
        handle >>= 11;
    }
    return hash * 8u;
}

static void ramht_insert(uint32_t handle, uint32_t instance)
{
    uint32_t slot = ramht_slot(handle);
    wr32(g_ramin, slot, handle);
    wr32(g_ramin, slot + 4, NV_RAMHT_STATUS | (instance >> 4));
}

static NV2AState *fresh(void)
{
    NV2AState *d;
    memset(g_window, 0, sizeof(g_window));
    memset(g_ramin, 0, sizeof(g_ramin));
    g_translator_calls = 0;
    nv2a_reset_standalone_for_test();
    nv2a_bind_instance_memory(0x00F00000u, g_ramin, sizeof(g_ramin));
    d = nv2a_init_standalone(g_vram, sizeof(g_vram), NULL, 0);
    nv2a_set_pushbuffer_window(d, g_window, 0, sizeof(g_window));
    /* 4 KiB RAMHT at instance 0. */
    d->pfifo.regs[NV_PFIFO_RAMHT] = 0;
    ramht_insert(H_KELVIN, INST_KELVIN);
    wr32(g_ramin, INST_KELVIN, 0x97u);
    ramht_insert(H_MEMCPY, INST_MEMCPY);
    wr32(g_ramin, INST_MEMCPY, 0x39u);
    return d;
}

/* ── pushbuffer builder ──────────────────────────────────────────────── */

typedef struct {
    uint32_t start, at;
} Pb;

static uint32_t hdr(uint32_t subchannel, uint32_t method, uint32_t count)
{
    return (count << 18) | (subchannel << 13) | method;
}

static void pb_begin(Pb *pb, uint32_t at)
{
    pb->start = pb->at = at;
}

static void pb_word(Pb *pb, uint32_t w)
{
    wr32(g_window, pb->at, w);
    pb->at += 4;
}

static void pb_method(Pb *pb, uint32_t subchannel, uint32_t method, uint32_t param)
{
    pb_word(pb, hdr(subchannel, method, 1));
    pb_word(pb, param);
}

static void mmio_w(NV2AState *d, uint32_t addr, uint32_t v)
{
    nv2a_mmio_write(d, addr, v, 4);
}

static uint32_t mmio_r(NV2AState *d, uint32_t addr)
{
    return (uint32_t)nv2a_mmio_read(d, addr, 4);
}

#define USER(r)   (0x800000u + (r))

static uint32_t get_ptr(NV2AState *d)
{
    return mmio_r(d, USER(NV_USER_DMA_GET));
}

/* Start a stream at `get` and kick it at `put`, as the guest does. */
static void kick(NV2AState *d, uint32_t get, uint32_t put)
{
    mmio_w(d, USER(NV_USER_DMA_GET), get);
    mmio_w(d, USER(NV_USER_DMA_PUT), put);
}

/* ── tests ───────────────────────────────────────────────────────────── */

/* A subchannel bound to a non-Kelvin class never reaches the Kelvin
 * translator; Kelvin and never-bound subchannels still do. */
static void test_translator_reads_bound_class(void)
{
    NV2AState *d = fresh();
    Pb pb;
    pb_begin(&pb, PB_BASE);
    pb_method(&pb, 0, 0x0000, H_KELVIN);
    pb_method(&pb, 1, 0x0000, H_MEMCPY);
    kick(d, pb.start, pb.at);
    CHECK(get_ptr(d) == pb.at, "binding stream did not commit (get=%08X)", get_ptr(d));
    CHECK(d->pfifo.binding_class[1] == 0x39u, "subchannel 1 class %02X",
          d->pfifo.binding_class[1]);

    g_translator_calls = 0;
    pgraph_method(d, 1, NV097_CLEAR_SURFACE, 0xF0);
    CHECK(g_translator_calls == 0, "memcpy-class method reached the Kelvin translator");
    pgraph_method(d, 0, NV097_CLEAR_SURFACE, 0xF0);
    CHECK(g_translator_calls == 1, "Kelvin method did not reach the translator");
    pgraph_method(d, 5, NV097_CLEAR_SURFACE, 0xF0);
    CHECK(g_translator_calls == 2, "unbound subchannel lost the Kelvin reading");
}

int main(void)
{
    test_translator_reads_bound_class();

    if (g_failures) {
        fprintf(stderr, "nv2a_actions_test: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "nv2a_actions_test: all checks passed\n");
    return 0;
}
