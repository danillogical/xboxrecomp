/* Regression: the per-walk page-validation cache must NOT weaken the guard.
 *
 * THE DEFECT THE CACHE FIXES. `submit_read_word` called `VirtualQuery` for every
 * pushbuffer word it read -- once per 4 bytes, under `g_mmio_owner_lock`.
 * Measured on this host at the base of a 64 MB mapped view (the shape of the
 * nv2a contiguous window) that costs 12 us per call untouched and **378 us**
 * once the pages are resident, because `VirtualQuery` walks the region's
 * page-descriptor chain. An 8144-word walk therefore spent ~1.5 s in pure
 * validity checking, and R1's measured maximum lock hold was 1636 ms -- within
 * 8 % of that, from two independent instruments.
 *
 * THE RISK THE CACHE INTRODUCES, AND WHAT THIS TEST PINS. Caching a validated
 * span means a word inside that span skips `VirtualQuery`. If the cache were
 * wrong about the span -- too wide, not clamped to the ring, or consulted before
 * the address bounds -- a word in an unmapped, guard or non-readable page could
 * be read without a check, and the walk would fault on a bad ring instead of
 * refusing it. So this test does not check that the cache is fast; it checks
 * that the guard still REFUSES what it always refused.
 *
 * WHAT IT DRIVES. The real `nv2a_submit_pending` through the real MMIO entry
 * point, with a pushbuffer window that deliberately contains a guard page in the
 * middle. It asserts:
 *
 *   1. POSITIVE CONTROL -- a ring entirely inside readable memory is consumed
 *      and commits (so the harness can observe a successful walk at all);
 *   2. a word that lands in a PAGE_GUARD page is REFUSED, and GET does not
 *      advance past it;
 *   3. the refusal survives REPETITION -- the same walk retried several times
 *      still refuses, which is the case a cache that wrongly validated once
 *      would break;
 *   4. a readable word BEFORE the guard page is still consumed, so the guard
 *      refuses at the right address rather than refusing the whole ring;
 *   5. a page validated during one walk and revoked before the next is
 *      refused by the next, so the validation cache does not outlive a walk.
 *
 * The window is built with VirtualAlloc so the guard page is a real one: this
 * test is Windows-only by construction, and it says so rather than silently
 * passing elsewhere. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "nv2a_state.h"

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

/* The D3D11 backend is stubbed: this test exercises the read guard, not the
 * executor. Same stub shape as the other submit tests. */
int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param)
{
    (void)subchannel; (void)method; (void)param;
    return 1;
}

#ifndef _WIN32
int main(void)
{
    fprintf(stderr, "nv2a_read_guard_test: skipped (Windows-only: needs a real "
                    "PAGE_GUARD page)\n");
    return 0;
}
#else
#include <windows.h>

/* A ring whose middle page is a guard page. */
#define RING_PAGES 3
#define RING_BYTES (RING_PAGES * 0x1000)

static uint8_t *g_ring;
static uint8_t g_vram[1 << 16];
static uint8_t g_ramin[1 << 12];

static void wr32(uint32_t off, uint32_t v) { memcpy(g_ring + off, &v, 4); }

/* A packet header: count in bits 18+, subchannel 13+, method low 13. */
static uint32_t hdr(uint32_t subchannel, uint32_t method, uint32_t count)
{
    return (count << 18) | (subchannel << 13) | method;
}

static NV2AState *fresh(void)
{
    NV2AState *d;
    nv2a_reset_standalone_for_test();
    d = nv2a_init_standalone(g_vram, sizeof(g_vram), g_ramin, sizeof(g_ramin));
    nv2a_set_pushbuffer_window(d, g_ring, 0, RING_BYTES);
    d->pfifo.regs[NV_PFIFO_RAMHT] = 0;
    return d;
}

int main(void)
{
    /* A 3-page region with the MIDDLE page guarded, so a readable prefix and a
     * readable suffix both exist and only the middle is refused. */
    g_ring = (uint8_t *)VirtualAlloc(NULL, RING_BYTES, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_READWRITE);
    if (!g_ring) {
        fprintf(stderr, "nv2a_read_guard_test: VirtualAlloc failed\n");
        return 1;
    }
    {
        DWORD old;
        CHECK(VirtualProtect(g_ring + 0x1000, 0x1000, PAGE_READWRITE | PAGE_GUARD,
                             &old) != 0,
              "could not install the guard page; the test would be vacuous");
    }

    /* 1. POSITIVE CONTROL: a packet that fits entirely in the FIRST (readable)
     *    page must be consumed. Without this arm, a guard that refused
     *    everything would look like a pass. */
    {
        NV2AState *d = fresh();
        uint32_t get_before, get_after;

        /* One 2-word packet at offset 0: header + one parameter. */
        wr32(0, hdr(0, 0x0100, 1));
        wr32(4, 0x12345678u);
        d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] = 0;
        d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT] = 8;
        get_before = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
        (void)nv2a_submit_pending(d);
        get_after = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
        CHECK(get_after > get_before,
              "a packet wholly inside a readable page was NOT consumed "
              "(get 0x%X -> 0x%X); the harness cannot observe a good walk, so "
              "the guard arm below proves nothing", get_before, get_after);
    }

    /* 2 + 3. The guard page. PUT extends into it, so the walk must refuse rather
     *        than read across. Retried several times, because a cache that
     *        wrongly validated the span once would succeed on a retry. */
    {
        NV2AState *d = fresh();
        uint32_t get_after_first = 0;
        int attempt;

        /* A one-parameter packet wholly in the readable first page: header at
         * 0x0FF8, parameter at 0x0FFC. PUT is 0x1004, so after the packet the
         * walk looks at 0x1000 for the next header (the yield peek), and that
         * word is in the guarded middle page. It is the peek that must be
         * refused, not the packet. The guard page cannot be written, so
         * nothing is planted there. */
        wr32(0x0FF8, hdr(0, 0x0100, 1));
        d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] = 0x0FF8;
        d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT] = 0x1004;

        for (attempt = 0; attempt < 4; ++attempt) {
            uint32_t before = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
            (void)nv2a_submit_pending(d);
            if (attempt == 0)
                get_after_first = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
            CHECK(d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] == before,
                  "attempt %d: GET advanced (0x%X -> 0x%X) across a PAGE_GUARD "
                  "page; the read guard was bypassed -- most likely by the "
                  "per-walk validation cache validating too wide a span",
                  attempt, before, d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET]);
        }
        CHECK(get_after_first == 0x0FF8,
              "GET moved to 0x%X on the first guarded attempt; expected it to "
              "stay at the packet start 0x0FF8", get_after_first);
    }

    /* 4. A readable word BEFORE the guard is still fine, so the refusal is
     *    located at the guard rather than applied to the whole ring. This is
     *    the control that stops arm 2/3 from passing vacuously. */
    {
        NV2AState *d = fresh();
        uint32_t before, after;

        wr32(0x0FF0, hdr(0, 0x0100, 1));
        wr32(0x0FF4, 0xDEADBEEFu);      /* parameter in the readable page */
        d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] = 0x0FF0;
        d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT] = 0x0FF8;
        before = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
        (void)nv2a_submit_pending(d);
        after = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
        CHECK(after > before,
              "a packet ending exactly at the guard boundary was NOT consumed "
              "(get 0x%X -> 0x%X); the guard is refusing readable memory too",
              before, after);
    }

    /* 5. The validated-page cache is per walk. Walk N reads page 0 and
     *    validates it; page 0 then loses its access; walk N+1 over the same
     *    words must refuse. A cache that outlived the walk would still call the
     *    page readable and fault on it. */
    {
        NV2AState *d = fresh();
        DWORD old_protect;
        uint32_t get_after;

        wr32(0x0100, hdr(0, 0x0100, 1));
        wr32(0x0104, 0x12345678u);
        d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] = 0x0100;
        d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT] = 0x0108;
        (void)nv2a_submit_pending(d);
        CHECK(d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] == 0x0108,
              "walk N did not consume the packet (get 0x%X, %s); the arm would "
              "prove nothing", d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET],
              nv2a_submit_diagnostic(d->pfifo.submit_diag));

        CHECK(VirtualProtect(g_ring, 0x1000, PAGE_NOACCESS, &old_protect) != 0,
              "could not revoke access to page 0");
        d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] = 0x0100;
        d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT] = 0x0108;
        (void)nv2a_submit_pending(d);
        get_after = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
        CHECK(strcmp(nv2a_submit_diagnostic(d->pfifo.submit_diag),
                     "unreadable_pushbuffer") == 0,
              "walk N+1 over a page revoked since walk N gave diag %s; the "
              "validation cache outlived its walk",
              nv2a_submit_diagnostic(d->pfifo.submit_diag));
        CHECK(get_after == 0x0100, "walk N+1 moved GET to 0x%X", get_after);

        /* Control: with access restored the same walk succeeds again. */
        CHECK(VirtualProtect(g_ring, 0x1000, PAGE_READWRITE, &old_protect) != 0,
              "could not restore page 0");
        (void)nv2a_submit_pending(d);
        CHECK(d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] == 0x0108,
              "walk after restoring access did not consume the packet (%s)",
              nv2a_submit_diagnostic(d->pfifo.submit_diag));
    }

    VirtualFree(g_ring, 0, MEM_RELEASE);
    g_ring = NULL;

    if (g_failures) {
        fprintf(stderr, "nv2a_read_guard_test: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "nv2a_read_guard_test: all checks passed\n");
    return 0;
}
#endif
