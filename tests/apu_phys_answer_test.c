/* APU DSP scratch addresses: MmGetPhysicalAddress records, per 4 KB page, which
 * form its latest answer was for, and the APU translation honours it.
 *
 * THE BUG THIS REPRODUCES
 *   The guest asks MmGetPhysicalAddress for the physical addresses of ordinary
 *   (low) RAM pages and its sound library hands them to the APU DSP as scratch.
 *   A low-RAM VA is answered unchanged, a window VA less XBOX_CONTIG_BASE, so one
 *   page number can name a low-RAM page or a contiguous-window page. The APU
 *   translation used to send every address below the contiguous high-water mark
 *   to the window, which aliased the stage's vertex buffers.
 *
 * CONTRACT
 *   - xbox_MmGetPhysicalAddress(v) records, for the page it answers, whether v was
 *     an ordinary-RAM address (1) or a window address (0). The latest answer wins.
 *   - apu_guest_dma_ptr(addr, len) on a range whose every page was last answered
 *     for ordinary RAM resolves to ordinary RAM (va == addr), even when addr is
 *     below the contiguous high-water mark.
 *   - A range with any page not recorded as ordinary RAM keeps the high-water rule
 *     (below the mark: the window, va == XBOX_CONTIG_BASE + addr).
 *   - An address at or past XBOX_CONTIG_SIZE is never flagged.
 *   - The bitmap is safe to flip from one thread while another reads it: a reader
 *     sees the old answer or the new one, and neighbouring pages in the same
 *     32-bit word do not change.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "apu_watch.h"
#include "d3d/d3d8_xbox.h"   /* IDirect3DDevice8, for the inert D3D stub below */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* Defined by the APU core; the base host pointer for guest VAs. */
extern uint8_t *g_apu_ram_ptr;

/* Inert stubs: the real definitions live in the game, which this standalone
 * fixture does not link. Same list as vm_reuse_test.c. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }
void recomp_diag_thread_start(uint32_t start, uint32_t low, uint32_t high)
{ (void)start; (void)low; (void)high; }
void recomp_diag_thread_end(void) {}
void recomp_diag_record(uint32_t kind, uint32_t target, uint32_t site, uint32_t value)
{ (void)kind; (void)target; (void)site; (void)value; }
void jsrf_slot_latch_install(uint32_t raw_value, uint32_t installed_value)
{ (void)raw_value; (void)installed_value; }
void jsrf_slot_latch_sample(uint32_t tid, uint32_t call_index, uint32_t ordinal,
                            uint32_t before, uint32_t after)
{ (void)tid; (void)call_index; (void)ordinal; (void)before; (void)after; }
void jsrf_slot_watch_handshake(uint32_t slot_va) { (void)slot_va; }
void jsrf_slot_watch_alias_armed(uint32_t mapped_mask, uint32_t protect_mask, uint32_t alias_count)
{ (void)mapped_mask; (void)protect_mask; (void)alias_count; }
int jsrf_slot_watch_alias_touch(uint32_t alias_index, uint32_t fault_va, uint64_t rip,
                                uint32_t value, uint32_t published)
{ (void)alias_index; (void)fault_va; (void)rip; (void)value; (void)published; return 0; }
void jsrf_slot_watch_write(uint32_t provenance, uint32_t before, uint32_t after,
                           uint64_t rip, uint32_t ordinal)
{ (void)provenance; (void)before; (void)after; (void)rip; (void)ordinal; }

/* Video/framebuffer and D3D symbols referenced by kernel_bridge.c and
 * nv2a_pb_exec.c but defined by the GAME. They are only reached on paths this
 * fixture never takes, so inert stubs are correct here. */
int  xbox_VideoIsPlaying(void) { return 0; }
int  xbox_VideoPlayFile(const char *host_path) { (void)host_path; return 0; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowPresent(void) {}
uint32_t xbox_FramebufferPresentSerial(void) { return 0; }
unsigned long long xbox_FramebufferPresentHash(void) { return 0; }
void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars) { (void)name; (void)max_chars; }
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
IDirect3DDevice8 *xbox_GetD3DDevice(void) { return NULL; }

static unsigned checks;
static unsigned failures;

static int check(int condition, const char *name)
{
    ++checks;
    if (!condition) {
        ++failures;
        fprintf(stderr, "FAIL: %s\n", name);
    }
    return condition;
}

/* Pages used below. The ordinary-RAM scratch page is the measured one. */
#define SCRATCH_PA      0x01430000u   /* answered for ordinary RAM          */
#define NEXT_PA         0x01431000u   /* the page after it, never answered  */
#define CONTIG_ALLOC    0x01800000u   /* puts the high-water mark at 24 MB  */
#define PAGE_SIZE_4K    0x1000u

static ULONG_PTR phys_of(uint32_t va)
{
    return xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)va);
}

static uint8_t *dma(uint32_t addr, uint32_t len, uint32_t *va)
{
    *va = 0xDEADBEEFu;
    return apu_guest_dma_ptr(addr, len, va);
}

/* --- single-threaded contract ------------------------------------------ */

static void case_translation(void)
{
    uint32_t va;
    uint8_t *p;

    check(xbox_GetMappedSize() > NEXT_PA + 0x2000u, "low RAM maps past the scratch pages");
    check(!xbox_PhysIsLowRamAnswer(SCRATCH_PA, PAGE_SIZE_4K),
          "precondition: no answer is recorded before MmGetPhysicalAddress");
    check(!xbox_PhysIsLowRamAnswer(NEXT_PA, 1),
          "precondition: the neighbouring page is not recorded");

    /* 2. An ordinary-RAM address is answered unchanged and recorded. */
    check(phys_of(SCRATCH_PA) == SCRATCH_PA, "MmGetPhysicalAddress(0x01430000) == 0x01430000");
    check(xbox_PhysIsLowRamAnswer(SCRATCH_PA, PAGE_SIZE_4K),
          "the answered page is recorded as ordinary RAM");

    /* 3. Move the contiguous high-water mark past both pages. */
    check(xbox_ContiguousAlloc(CONTIG_ALLOC, 4096) != 0, "contiguous allocation succeeds");
    check(xbox_ContiguousAllocatedBytes() > NEXT_PA + PAGE_SIZE_4K,
          "high-water mark is past 0x01431000");

    /* 4. The recorded range resolves to ordinary RAM, not the window. */
    p = dma(SCRATCH_PA, PAGE_SIZE_4K, &va);
    check(va == SCRATCH_PA, "recorded page: translated va is ordinary RAM 0x01430000");
    check(p == g_apu_ram_ptr + SCRATCH_PA, "recorded page: host pointer is RAM base + 0x01430000");

    /* 5. A range running into an unrecorded page keeps the high-water rule. */
    p = dma(SCRATCH_PA + 0x800u, PAGE_SIZE_4K, &va);
    check(va == XBOX_CONTIG_BASE + SCRATCH_PA + 0x800u,
          "range crossing into an unrecorded page: translated va is the window");
    check(p == g_apu_ram_ptr + XBOX_CONTIG_BASE + SCRATCH_PA + 0x800u,
          "range crossing into an unrecorded page: host pointer is in the window");

    /* 6. The window VA's answer for the same page number wins. */
    check(phys_of(XBOX_CONTIG_BASE + SCRATCH_PA) == SCRATCH_PA,
          "MmGetPhysicalAddress(0x81430000) == 0x01430000");
    check(!xbox_PhysIsLowRamAnswer(SCRATCH_PA, PAGE_SIZE_4K),
          "a window answer clears the ordinary-RAM record");
    p = dma(SCRATCH_PA, PAGE_SIZE_4K, &va);
    check(va == XBOX_CONTIG_BASE + SCRATCH_PA, "after the window answer: translated va is the window");
    check(p == g_apu_ram_ptr + XBOX_CONTIG_BASE + SCRATCH_PA,
          "after the window answer: host pointer is in the window");

    /* And the most recent answer wins the other way too. */
    phys_of(SCRATCH_PA);
    p = dma(SCRATCH_PA, PAGE_SIZE_4K, &va);
    check(va == SCRATCH_PA && p == g_apu_ram_ptr + SCRATCH_PA,
          "a later ordinary-RAM answer restores ordinary RAM");
}

static void case_out_of_range(void)
{
    static const uint32_t probes[] = {
        0x00000000u, 0x01430000u, XBOX_CONTIG_SIZE - PAGE_SIZE_4K,
    };
    uint32_t before[sizeof probes / sizeof probes[0]];
    uint32_t addr[] = {
        XBOX_CONTIG_SIZE, XBOX_CONTIG_SIZE + PAGE_SIZE_4K, 0x7FFFF000u,
        0x80000000u, 0xFFFFF000u, 0xFFFFFFFFu,
    };
    size_t i;

    for (i = 0; i < sizeof probes / sizeof probes[0]; i++)
        before[i] = (uint32_t)xbox_PhysIsLowRamAnswer(probes[i], PAGE_SIZE_4K);

    for (i = 0; i < sizeof addr / sizeof addr[0]; i++) {
        char name[96];
        xbox_PhysNoteAnswer(addr[i], 1);
        snprintf(name, sizeof name, "0x%08X is never flagged (PhysNoteAnswer)", addr[i]);
        check(!xbox_PhysIsLowRamAnswer(addr[i], 1), name);
    }
    /* The same through the kernel entry point, with ordinary-RAM addresses. */
    phys_of(XBOX_CONTIG_SIZE);
    phys_of(0xFFFFF000u);
    check(!xbox_PhysIsLowRamAnswer(XBOX_CONTIG_SIZE, 1),
          "MmGetPhysicalAddress(window size) is never flagged");
    check(!xbox_PhysIsLowRamAnswer(0xFFFFF000u, PAGE_SIZE_4K),
          "MmGetPhysicalAddress(0xFFFFF000) is never flagged");

    /* A range that ends past the window is never flagged either, even when its
     * first page is. */
    phys_of(XBOX_CONTIG_SIZE - PAGE_SIZE_4K);
    check(xbox_PhysIsLowRamAnswer(XBOX_CONTIG_SIZE - PAGE_SIZE_4K, PAGE_SIZE_4K),
          "the last window-sized page can be flagged");
    check(!xbox_PhysIsLowRamAnswer(XBOX_CONTIG_SIZE - PAGE_SIZE_4K, 2 * PAGE_SIZE_4K),
          "a range running past the window size is not flagged");
    check(!xbox_PhysIsLowRamAnswer(0xFFFFF000u, 0x2000u),
          "a range wrapping past 4 GB is not flagged");
    xbox_PhysNoteAnswer(XBOX_CONTIG_SIZE - PAGE_SIZE_4K, 0);

    /* Out-of-range notes must not have disturbed any in-range page. */
    for (i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        char name[96];
        snprintf(name, sizeof name, "in-range page 0x%08X unchanged by out-of-range notes", probes[i]);
        check((uint32_t)xbox_PhysIsLowRamAnswer(probes[i], PAGE_SIZE_4K) == before[i], name);
    }
}

/* --- two-thread race ---------------------------------------------------- */

/* Three pages sharing one 32-bit bitmap word (page numbers 0x1420..0x143F). */
#define TOGGLE_PA    0x01435000u
#define NEIGHBOR_HI  0x01434000u   /* preset: ordinary RAM (1) */
#define NEIGHBOR_LO  0x01436000u   /* preset: not recorded (0) */
#define TOGGLES      1000000u

static volatile LONG race_done;
static volatile LONG bad_toggle_reads;
static volatile LONG bad_neighbor_reads;
static volatile LONG seen_zero, seen_one;
static volatile LONG reads;

static DWORD WINAPI toggler(LPVOID unused)
{
    uint32_t i;
    (void)unused;
    for (i = 0; i < TOGGLES; i++)
        xbox_PhysNoteAnswer(TOGGLE_PA, (int)(i & 1u) ^ 1);   /* 1,0,1,0,... */
    InterlockedExchange(&race_done, 1);
    return 0;
}

static DWORD WINAPI reader(LPVOID unused)
{
    (void)unused;
    while (!race_done) {
        int t = xbox_PhysIsLowRamAnswer(TOGGLE_PA, PAGE_SIZE_4K);
        int hi = xbox_PhysIsLowRamAnswer(NEIGHBOR_HI, PAGE_SIZE_4K);
        int lo = xbox_PhysIsLowRamAnswer(NEIGHBOR_LO, PAGE_SIZE_4K);
        if (t != 0 && t != 1)
            InterlockedIncrement(&bad_toggle_reads);
        else if (t)
            InterlockedIncrement(&seen_one);
        else
            InterlockedIncrement(&seen_zero);
        if (hi != 1 || lo != 0)
            InterlockedIncrement(&bad_neighbor_reads);
        InterlockedIncrement(&reads);
    }
    return 0;
}

static void case_race(void)
{
    HANDLE th[2];

    xbox_PhysNoteAnswer(NEIGHBOR_HI, 1);
    xbox_PhysNoteAnswer(NEIGHBOR_LO, 0);
    xbox_PhysNoteAnswer(TOGGLE_PA, 0);
    check(xbox_PhysIsLowRamAnswer(NEIGHBOR_HI, 1) && !xbox_PhysIsLowRamAnswer(NEIGHBOR_LO, 1),
          "race preset: neighbours hold 1 and 0");

    th[0] = CreateThread(NULL, 0, reader, NULL, 0, NULL);
    th[1] = CreateThread(NULL, 0, toggler, NULL, 0, NULL);
    if (!check(th[0] && th[1], "race threads start"))
        return;
    WaitForMultipleObjects(2, th, TRUE, 60000);
    CloseHandle(th[0]);
    CloseHandle(th[1]);

    printf("  race: %ld reads, toggled page seen 1 x%ld, 0 x%ld\n",
           (long)reads, (long)seen_one, (long)seen_zero);
    check(reads > 0, "the reader ran during the toggling");
    check(bad_toggle_reads == 0, "the toggled page only ever reads 0 or 1");
    check(bad_neighbor_reads == 0, "neighbouring pages in the same word never change");
    /* The last toggle (i = TOGGLES-1, odd) set the page to 0. */
    check(!xbox_PhysIsLowRamAnswer(TOGGLE_PA, 1), "the toggled page ends on the last answer");
}

/* No sections, no TLS directory: enough for xbox_MemoryLayoutInit to build the
 * guest mapping and heap, which is all this fixture needs. */
static uint8_t *make_synthetic_xbe(size_t *out_size)
{
    static uint8_t xbe[0x2000];
    memset(xbe, 0, sizeof(xbe));
    *(uint32_t *)(xbe + 0x0104) = XBOX_BASE_ADDRESS;
    *(uint32_t *)(xbe + 0x0108) = 0x1000;
    *(uint32_t *)(xbe + 0x011C) = 0;
    *(uint32_t *)(xbe + 0x0120) = XBOX_BASE_ADDRESS + 0x200;
    *(uint32_t *)(xbe + 0x012C) = 0;
    *(uint32_t *)(xbe + 0x0158) = 0;
    *out_size = sizeof(xbe);
    return xbe;
}

int main(void)
{
    size_t xbe_size = 0;
    uint8_t *xbe;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    xbe = make_synthetic_xbe(&xbe_size);
    if (!xbox_MemoryLayoutInit(xbe, xbe_size)) {
        fprintf(stderr, "xbox_MemoryLayoutInit failed\n");
        return 2;
    }
    g_apu_ram_ptr = (uint8_t *)xbox_GetMemoryBase();
    if (!g_apu_ram_ptr) {
        fprintf(stderr, "xbox_GetMemoryBase failed\n");
        return 2;
    }

    case_translation();
    case_out_of_range();
    case_race();

    printf("%s: %u apu-phys-answer checks run, %u failed\n",
           failures ? "FAIL" : "PASS", checks, failures);
    fflush(stdout);
    return failures ? 1 : 0;
}
