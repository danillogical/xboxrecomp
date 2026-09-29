/**
 * Xbox Memory Layout Implementation
 *
 * Maps the XBE data sections to their expected virtual addresses on Windows.
 * This is critical for the recompiled code which references globals by
 * absolute address (e.g., mov eax, [0x004D532C]).
 *
 * Implementation:
 * 1. VirtualAlloc a contiguous region at XBOX_BASE_ADDRESS
 * 2. Copy .rdata and initialized .data from the XBE
 * 3. Zero-fill the BSS region
 * 4. Set memory protection (read-only for .rdata)
 */

#include "xbox_memory_layout.h"
#include "kernel.h"
#include "recomp_diagnostics.h"   /* jsrf_slot_watch_alias_* prototypes (A2h alias census) */
#include "kmem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#if defined(_WIN32)
/* WIN32_LEAN_AND_MEAN excludes the toolhelp API from windows.h, and the all-thread census needs
 * CreateToolhelp32Snapshot/Thread32First. Pulled in explicitly rather than by relaxing the lean
 * define, which every other translation unit in the toolkit depends on. */
#include <tlhelp32.h>
#else
#include <unistd.h>   /* _exit */
#endif

/* XBE header field offsets (per xboxdevwiki.net/Xbe) */
#define XBE_MAGIC_OFFSET        0x0000
#define XBE_BASE_ADDR_OFFSET    0x0104
#define XBE_HEADER_SIZE_OFFSET  0x0108
#define XBE_SECTION_COUNT_OFFSET 0x011C
#define XBE_SECTION_HEADERS_OFFSET 0x0120
#define XBE_TLS_ADDR_OFFSET     0x012C

/* XBE section header layout (56 bytes each) */
#define SECTHDR_FLAGS       0x00
#define SECTHDR_VA          0x04
#define SECTHDR_VSIZE       0x08
#define SECTHDR_RAW_OFFSET  0x0C
#define SECTHDR_RAW_SIZE    0x10
#define SECTHDR_NAME_ADDR   0x14
#define SECTHDR_SIZE        56

static void *g_memory_base = NULL;
static size_t g_memory_size = 0;
static ptrdiff_t g_memory_offset = 0;  /* actual_base - XBOX_BASE_ADDRESS */

/* Actual mapped RAM for this run; see the header. Default retail 64 MB. */
size_t g_xbox_total_ram = XBOX_TOTAL_RAM;
size_t g_xbox_map_size = 0;   /* 0 = same as RAM */

void xbox_SetTotalRam(size_t bytes)
{
    g_xbox_total_ram = bytes;
}


void xbox_SetMapSize(size_t bytes)
{
    g_xbox_map_size = bytes;
}

/* File mapping handle for the Xbox memory region.
 * Using CreateFileMapping + MapViewOfFileEx allows mirror views to alias
 * the same physical pages as the base region, so writes to mirror addresses
 * (which wrap modulo 64 MB on real Xbox hardware) correctly modify the
 * underlying data. */
static HANDLE g_mapping_handle = NULL;

/* Mirror view pointers for cleanup */
static void *g_mirror_views[XBOX_NUM_MIRRORS] = {0};

/* The base view and its 28 mirrors occupy one contiguous span. Reserving that
 * span up front is what makes the mirrors placeable at all: each one sits at
 * base + N * 64 MB, and on a host that chose the base for us, those addresses
 * run through whatever the loader already owns. Placing them one at a time
 * means ~3 of 28 collide, and *which* three changes with ASLR. Claiming the
 * whole range first, then carving views out of ground we hold, removes the
 * question. */
static void *g_span_base = NULL;
static size_t g_span_size = 0;
static void *g_tiled_view = NULL;

/* Contiguous / physical memory window (see MemoryLayoutInit).
 * XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE come from kernel.h - the bridges need
 * the same numbers for MmClaimGpuInstanceMemory. */
static void *g_contig_memory = NULL;

/* NV2A GPU register aperture (see MemoryLayoutInit). Backed as plain RAM so
 * that D3D8 code linked into the title can poke it without faulting. */
#define XBOX_NV2A_BASE 0xFD000000u
#define XBOX_NV2A_SIZE (16u * 1024u * 1024u)
static void *g_nv2a_memory = NULL;

/* MCPX southbridge register span: APU 0xFE800000 through NIC 0xFEF00000. */
#define XBOX_MCPX_BASE 0xFE800000u
#define XBOX_MCPX_SIZE (8u * 1024u * 1024u)
static void *g_mcpx_memory = NULL;

/* Flash ROM. The console's 256 KB flash is mirrored through the top of the
 * address space, and the MCPX span above stops one page short of it -- so a
 * title that touches it faulted on an address that is perfectly ordinary on
 * hardware.
 *
 * The Xbox Dashboard does, from two directions at once: its XIP workers hash
 * 64 KB from 0xFF000000 (it verifies archives against digests), and its render
 * path writes to 0xFF000040. Both are hard faults today, and they kill the
 * process a few dozen lines after its first frame clears.
 *
 * Plain memory, like the other two apertures, and mapped for the same stated
 * reason: a read of zero is survivable, a fault is not. Zeros are not the
 * console's BIOS, so a digest taken over this will not match one taken over
 * real flash -- that is a separate question from whether the access should
 * fault, and this is the half that has an obviously right answer. */
#define XBOX_FLASH_BASE 0xFF000000u
#define XBOX_FLASH_SIZE (1u * 1024u * 1024u)
static void *g_flash_memory = NULL;
/* The contiguous window's backing section. It is a file mapping rather than
 * plain committed memory for one reason: the tiled aperture has to be a
 * second view of the very same bytes, and only a mapping can be mapped
 * twice. See the tiled aperture below for why that matters.
 */
static HANDLE g_contig_mapping = NULL;
/* How much of the tiled aperture can exist.
 *
 * Two ceilings, both below the mapped RAM size once that is large:
 *
 *   - it starts at 0xF0000000 in a 32-bit guest address space, so it can
 *     never reach past 0x100000000; and
 *   - the NV2A register aperture sits at 0xFD000000, which is where the
 *     window really ends on hardware.
 *
 * Asking for the full RAM size overlapped both and MapViewOfFileEx failed
 * with ERROR_INVALID_ADDRESS -- a warning at startup and then a fault on the
 * title's first surface write, with nothing connecting the two. */
/* How much guest address space is mapped.
 *
 * For anything that dereferences an address it read out of guest memory --
 * a descriptor pointer a device model follows, say. Those are attacker-ish
 * input in the only sense that matters here: the title can leave one
 * uninitialised, and 0xCCCCCCCC dereferenced is a crash in the runtime
 * rather than a fault the title would have taken. */
size_t xbox_GetMappedSize(void)
{
    return g_memory_size;
}

static size_t xbox_TiledApertureSize(void)
{
    uint64_t end = XBOX_NV2A_BASE < 0x100000000ULL
                 ? XBOX_NV2A_BASE : 0x100000000ULL;
    size_t max = (size_t)(end - XBOX_TILED_BASE);
    return g_memory_size < max ? g_memory_size : max;
}

static HANDLE g_nv2a_ack_thread = NULL;
static volatile LONG g_nv2a_ack_stop = 0;
/* One-shot flag for the AC'97 reach witness: printed once, on the first tick
 * that observes GC bit 1 set. Keeps the witness bounded and makes it a reach
 * witness rather than a per-tick trace. */
static volatile LONG g_ac97_witness_done = 0;
/* Read at worker start; diagnostic fixtures may disable GPU mutations while
 * retaining the same worker's kernel/APU clock updates. Frozen collectors
 * read this exported value to identify the active model. */
volatile LONG g_nv2a_ack_enabled = 1;
static volatile LONG g_nv2a_ack_active = 0;

void xbox_Nv2aClaimRegisterOwner(void)
{
    InterlockedExchange(&g_nv2a_ack_enabled, 0);
    while (InterlockedCompareExchange(&g_nv2a_ack_active, 0, 0))
        SwitchToThread();
    MemoryBarrier();
}

/*
 * NV2A busy-bit acknowledgement.
 *
 * D3D8 talks to the GPU through set-a-bit / wait-for-hardware-to-clear-it
 * handshakes. Against plain RAM the bit is set and nothing ever clears it, so
 * the title spins forever. Halo hangs in the push-buffer kick at 0x001EF930:
 *
 *     mov  [eax+0x100410], edx     ; set 0x10000
 *   L: test [eax+0x100410], 0x10000
 *     jne  L                       ; wait for the GPU
 *
 * Clearing those bits from a thread is not a hack around the handshake, it is
 * the handshake: on hardware the GPU clears them asynchronously, which is
 * exactly what this does. Work that would have been submitted is being done by
 * the D3D11 layer instead, so acknowledging immediately is honest.
 *
 * Only registers listed here are touched. Blanket-zeroing the aperture would
 * also wipe registers holding real state.
 *
 * ponytail: table-driven, extend as more handshakes turn up. A spin on a bit
 * that is not listed still hangs -- run the title and the watchdog sample will
 * name the register.
 */
static const struct { uint32_t offset; uint32_t busy_mask; } NV2A_ACK[] = {
    { 0x100410, 0x00010000u },  /* PFB flush kick, Halo 0x001EF930 */

    /* Interrupt status registers. These are write-1-to-clear on hardware, so
     * an ISR "clearing" one writes the pending bit back -- against plain RAM
     * that sets it instead, the interrupt stays pending forever, and the
     * service routine re-enters until the stack is gone. Halo dies exactly
     * that way: CMiniport::ServiceGrInterrupt writes 0x1000 to PGRAPH_INTR to
     * acknowledge, reads it back still pending, and recurses into a native
     * stack overflow.
     *
     * Holding them at zero is correct rather than convenient: nothing here
     * ever raises a GPU interrupt, so "none pending" is the truth. */
    { 0x000100, 0xFFFFFFFFu },  /* PMC_INTR_0    */
    { 0x001100, 0xFFFFFFFFu },  /* PBUS_INTR_0   */
    { 0x002100, 0xFFFFFFFFu },  /* PFIFO_INTR_0  */
    { 0x400100, 0xFFFFFFFFu },  /* PGRAPH_INTR   */
    { 0x600100, 0xFFFFFFFFu },  /* PCRTC_INTR_0  */
};

/*
 * Bits that must always read as SET. The mirror image of the table above:
 * where an interrupt-pending bit is false because nothing raises interrupts,
 * a queue-empty bit is true because nothing is queued.
 *
 * Halo's CMiniport::TilingUpdateIdle spins until the PFIFO caches report
 * empty (0x001F5CD1). Zeroed RAM says "not empty" forever, so tile setup
 * during CDevice::InitializeFrameBuffers never completes.
 *
 * Note 0x003220 is deliberately absent -- that one exits on the bit being
 * CLEAR, which zeroed memory already gives.
 */
static const struct { uint32_t offset; uint32_t idle_mask; } NV2A_IDLE[] = {
    { 0x002400, 0x00000010u },  /* PFIFO_RUNOUT_STATUS  LOW_MARK (empty) */
    { 0x003214, 0x00000010u },  /* PFIFO_CACHE1_STATUS  LOW_MARK (empty) */
};

/*
 * PFIFO channel DMA pointers. Software writes DMA_PUT and spins until the GPU
 * advances DMA_GET to match -- "you have consumed everything I submitted".
 * Halo's wait is at 0x001F3948:
 *
 *   L: call BusyLoop
 *      ecx = [[dev+0x2304] + 0x44]   ; DMA_GET
 *      edx = [dev]                   ; DMA_PUT
 *      test (edx ^ ecx), 0xfffffff
 *      jne L
 *
 * [dev+0x2304] is 0xFD800000, so the channel's USER area sits at aperture
 * offset 0x800000 and the two pointers are at +0x40 / +0x44. Copying PUT to
 * GET is the acknowledgement; the commands are not executed from the push
 * buffer here -- the D3D11 layer draws -- so reporting them consumed is the
 * truthful answer.
 *
 * This was written once, removed, and restored. It was removed because
 * [dev+0x2304] read as 0x0080F7FF, i.e. no register to acknowledge -- but that
 * garbage was a downstream symptom of ordinal 47 having no stdcall arg size,
 * which walked esp 8 bytes off and made D3D initialise the DMA channel with
 * `this` = 1. With that fixed the pointer is correct and so is this.
 */
#define NV2A_USER_DMA_PUT 0x800040u
#define NV2A_USER_DMA_GET 0x800044u

/*
 * The same channel's pointers on the PFIFO side of the aperture.
 *
 * The USER area above is the window software writes through; PFIFO holds the
 * engine's own copy, and D3D reads it back on the path where the USER pointer
 * is not usable. The title's channel context switch saves and restores all
 * four of these as one block (DDS9 0x002FE2xx), which is what identifies them:
 *
 *   0x3240 CACHE1_DMA_PUT          0x3248 CACHE1_REF
 *   0x3244 CACHE1_DMA_GET          0x324C CACHE1_DMA_SUBROUTINE
 *
 * DMA_SUBROUTINE matters because it is not a flag: bits 31:1 are the offset
 * the engine returns to when a pushbuffer subroutine ends, and bit 0 says
 * whether one is running. DDS9's free-space calculation (sub_002F6CC0) reads
 * the USER GET first and falls back to this register's return offset when
 * that lands outside the ring -- i.e. "the GPU is off in a subroutine, so ask
 * where it will come back to". Zeroed RAM answers 0 to both, which is below
 * the ring base, and the free-space subtraction then goes negative and is
 * clamped to zero. The reserve wants 0x2000 bytes, gets 0, and spins.
 *
 * Not acknowledged here, only reported. DDS9 reads the USER pair and never
 * reaches the fallback, so every value in this block is zero for the one
 * title that was traced -- mirroring PUT to GET would be a guess dressed as
 * a handshake. The watchdog prints them so the next title to spin here is
 * diagnosed from data instead.
 */
#define NV2A_PFIFO_DMA_PUT        0x003240u
#define NV2A_PFIFO_DMA_GET        0x003244u
#define NV2A_PFIFO_REF            0x003248u
#define NV2A_PFIFO_DMA_SUBROUTINE 0x00324Cu

/*
 * Free-running counters in the MCPX aperture.
 *
 * Some hardware registers are clocks, not flags: software reads them and waits
 * until the value passes a target. Against zeroed RAM the value never moves and
 * the wait is forever. DirectSound's CMcpxCore::SetupVoiceProcessor spins on
 * the APU sample counter at 0xFE820010 exactly this way, which is where Halo
 * stopped once input initialisation started working.
 *
 * Ticking it is the honest model: on hardware this counter advances on its own
 * whether or not anything is listening.
 *
 * ponytail: the rate is "as fast as this thread loops", not 48 kHz. Nothing
 * paces audio off it yet. Derive it from a real clock if timing starts to
 * matter.
 */
/*
 * AC'97 bus-master reset, modelled by trapping the write rather than by
 * clearing the bit afterwards.
 *
 * Each of the three DMA channels -- PCM In, PCM Out, Mic In -- has a one-byte
 * control register at NABM + 0x0B, and bit 1 is RR, "Reset Registers".
 * Software sets it and waits for the controller to clear it. DDS9 does that
 * inside DirectSoundCreate, and the wait is worth quoting because it is not a
 * poll:
 *
 *     mov  cl, [eax+0xFEC0010B]
 *     and  cl, 2
 *   L: test cl, cl
 *     jne  L
 *
 * MSVC hoisted the load out of the loop -- the pointer was not volatile -- so
 * the title reads the register exactly ONCE, a few instructions after writing
 * it, and spins forever on whatever that single read returned. On hardware
 * the reset has long completed by then.
 *
 * That rules out the NV2A_ACK approach. A thread that clears the bit
 * afterwards is racing a window a few instructions wide and gets only one
 * attempt; measured, it loses, and the title sits on a stale cl = 2 while the
 * register itself reads 0. The bit has to be clear at the moment of the read,
 * which means the write must never deposit it.
 *
 * So the page is PAGE_READONLY: reads run at full speed and see plain memory,
 * writes fault. The fault handler makes the page writable, single-steps the
 * faulting instruction, then masks RR out of the three control bytes and
 * re-protects. No instruction decoding, which matters because the write forms
 * a compiler emits here are not worth enumerating -- and being wrong about
 * one would corrupt a register rather than fail visibly.
 *
 * Only RR. Bit 0 is RPBM, run/pause bus master, which software owns.
 */
#define AC97_NABM_OFFSET  0x400000u   /* 0xFEC00000 within the MCPX aperture */
#define AC97_TRAP_BYTES   0x1000u
#define AC97_RR           0x02u

static void *g_ac97_page = NULL;      /* host address of the trapped page */
static void *g_ac97_veh  = NULL;
static RECOMP_TLS int s_ac97_stepping = 0;

/* ── A2h LIVE slot-write watch: the SHARED per-thread pending-ownership state ───────────────────
 *
 * The POSIX host layer (platform/win32_compat.h) provides the 32-bit interlocked primitives and the
 * page constants but not the 64-bit ones, and it has no PAGE_GUARD. The loss counters are 64-bit
 * because a bounded run can still exceed 2^32 writes on a hot page in principle, so they are routed
 * through one pair of macros rather than spelled per host. The POSIX build is the conformance test,
 * not a live title, so a plain read-modify-write is adequate there. The macros live HERE, above the
 * AC'97 handler, because that handler now accounts through them too.
 *
 * Declared here, ABOVE ac97_write_veh(), because the AC'97 trap is a SECOND owner of the same
 * single-step machinery and must record its own pending bit through the same protocol. Putting this
 * state below the AC'97 handler is what would force the AC'97 path to keep its own private flag --
 * and two private flags on one thread is precisely the collision the packet requires be made
 * bit-exact instead of assumed away.
 *
 *   own-TF  (0x1) -- stepping a faulting write on a page the SLOT watch owns.
 *   AC97-TF (0x2) -- stepping a faulting write on the AC'97 bus-master page.
 *
 * The bits are INDEPENDENT because a single thread can hold both at once: a fault can arrive while
 * the other owner's step is still pending. Servicing is therefore per-bit and idempotent, and the
 * TF bit is restored from the value that was present BEFORE THE FIRST owner armed -- not from
 * whichever handler happens to finish last. */
#define A2H_SLOTW_PEND_OWN   0x1u
#define A2H_SLOTW_PEND_AC97  0x2u

#if defined(_WIN32)
#  define A2H_SLOTW_INC64(p)     InterlockedIncrement64((volatile LONG64 *)(p))
#  define A2H_SLOTW_SET64(p, v)  InterlockedExchange64((volatile LONG64 *)(p), (LONG64)(v))
#else
#  define A2H_SLOTW_INC64(p)     ((uint64_t)(++(*(uint64_t *)(p))))
#  define A2H_SLOTW_SET64(p, v)  ((void)(*(uint64_t *)(p) = (uint64_t)(v)))
#endif
#ifndef PAGE_GUARD
#  define PAGE_GUARD 0x100u
#endif

static volatile LONG g_a2h_slotw_armed = 0;
static RECOMP_TLS uint32_t s_a2h_slotw_pending = 0;
static RECOMP_TLS uint32_t s_a2h_slotw_saved_tf = 0;

/* Take ownership of one pending bit for the CURRENT thread, capturing the pre-entry TF exactly once
 * -- at the 0 -> non-zero transition, so a nested second owner cannot overwrite the value the
 * outermost owner is going to restore. */
static void a2h_slotw_take_pending(uint32_t bit, DWORD eflags)
{
    if (s_a2h_slotw_pending == 0)
        s_a2h_slotw_saved_tf = (uint32_t)eflags & 0x100u;
    s_a2h_slotw_pending |= bit;
}

/* Release one pending bit and return the TF state the thread must now have: still set while any
 * owner remains, otherwise the pre-entry TF bit EXACTLY (cleared only if it was originally 0).
 *
 * THE DUAL-OWNER COUNT IS TAKEN HERE, AND THAT PLACEMENT IS THE WHOLE POINT. "This #DB had two
 * pending owners" is only observable at the FIRST release, because that is the only moment at which
 * both bits are known to have been pending together -- and it is symmetric: whichever handler runs
 * first, releasing its bit leaves the other still set. Counting it in either handler instead made
 * the number a record of "which handler ran first", which is exactly what the synthetic-overlap
 * fixture caught: the slot-first order reported dual=0 for a #DB that had plainly been dual. */
static DWORD a2h_slotw_release_pending(uint32_t bit, DWORD eflags)
{
    s_a2h_slotw_pending &= ~bit;
    if (s_a2h_slotw_pending) {
        A2H_SLOTW_INC64(&g_xbox_a2h_slotw.loss.db_dual_serviced);
        return (DWORD)(eflags | 0x100u);
    }
    return (DWORD)((eflags & ~0x100u) | s_a2h_slotw_saved_tf);
}

static void ac97_clear_reset_bits(void)
{
    /* Every bus-master channel, not the three a PC AC'97 has.
     *
     * The generic controller has PCM In, PCM Out and Mic In at NABM +0x00,
     * +0x10 and +0x20; the MCPX has more, and DDS9 walks a table of channel
     * offsets rather than naming them. It reset the channel at +0x00 first
     * and then one at +0x60 -- which a three-entry list did not cover, so it
     * spun on the second exactly as it had on the first. Sweeping the whole
     * NABM block is both simpler and right: +0x0B is the control byte of
     * whatever channel lives there, and RR is the same bit in all of them. */
    uint32_t off;

    for (off = 0x10B; off < 0x180; off += 0x10) {
        volatile uint8_t *r = (volatile uint8_t *)((char *)g_ac97_page + off);
        if (*r & AC97_RR)
            *r = (uint8_t)(*r & ~AC97_RR);
    }
}

static LONG CALLBACK ac97_write_veh(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    DWORD old;

    if (!g_ac97_page)
        return EXCEPTION_CONTINUE_SEARCH;

    /* Second half: the faulting write has now executed. Apply what the
     * controller would have done and close the page again. Thread-local,
     * because another thread must not mistake its own single-step for this
     * one -- and re-protecting from the wrong thread would strand this one
     * mid-step.
     *
     * THIS IS THE SHARED OWNERSHIP WORD, NOT A PRIVATE FLAG. The AC'97 trap is one of TWO owners of
     * this thread's single-step, and both record through a2h_slotw_take_pending/release_pending so a
     * #DB with both bits pending is serviced once per owner instead of being consumed by whichever
     * handler sees it first. With the slot watch unarmed the word only ever holds this bit, so the
     * behaviour is exactly what it was -- except that TF is now restored to the value present BEFORE
     * this owner armed rather than being cleared unconditionally, which is what the packet requires
     * and what keeps a guest's own single-step from being silently cancelled. */
    if (code == EXCEPTION_SINGLE_STEP && (s_a2h_slotw_pending & A2H_SLOTW_PEND_AC97)) {
        XboxA2hSlotwLedger *L = &g_xbox_a2h_slotw;
        /* ACCOUNTED HERE, WHERE THE AC'97 WORK IS ACTUALLY DONE.
         *
         * MEASURED, NOT PREFERRED. The first version incremented this in the SLOT handler's "pass it
         * on to AC'97" branch, which made it a record of "the slot handler happened to run first"
         * rather than of "the AC'97 owner was serviced". Under the opposite registration order the
         * AC'97 handler runs first and the slot handler then sees only its own bit, so the AC'97
         * count stayed 0 for a step that had plainly been serviced -- and the synthetic-overlap
         * fixture failed on the ORDER rather than on the protocol. Counting at the point of work
         * makes both orders report the same thing, which is what "serviced exactly once" has to mean
         * if it is to be independent of handler order. The dual-owner count is taken in the shared
         * release helper for the same reason. */
        ac97_clear_reset_bits();
        VirtualProtect(g_ac97_page, AC97_TRAP_BYTES, PAGE_READONLY, &old);
        ep->ContextRecord->EFlags = a2h_slotw_release_pending(A2H_SLOTW_PEND_AC97,
                                                             ep->ContextRecord->EFlags);
        A2H_SLOTW_INC64(&L->loss.db_ac97_serviced);
        /* ⚠ THIS RETURN IS ORDER-DEPENDENT AND MUST NOT SWALLOW THE OTHER OWNER.
         *
         * The two handlers are both registered at priority 1, and Windows calls equal-priority
         * vectored handlers in the order they were added -- this one FIRST, because the AC'97 trap
         * arms during memory-layout init and the slot watch arms later. So on a #DB with BOTH bits
         * pending, this handler runs first, and returning EXCEPTION_CONTINUE_EXECUTION here would
         * resume the thread with the slot watch's own-TF still pending and its page left open: the
         * slot watch would never see its #DB, never read the post-value, and never re-protect.
         *
         * So when the other owner is still pending the #DB is RELEASED down the chain instead, with
         * TF left set (a2h_slotw_release_pending did that because the word is non-zero). The slot
         * watch then services its own bit and is the one that finally resumes the thread. Under the
         * opposite registration order the same two steps happen in the other sequence and converge
         * on the same state -- which is what the synthetic-overlap fixture asserts for BOTH orders
         * rather than assuming the one this build happens to install. */
        if (s_a2h_slotw_pending & A2H_SLOTW_PEND_OWN)
            return EXCEPTION_CONTINUE_SEARCH;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_ACCESS_VIOLATION
            && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];

        if (fault >= (uintptr_t)g_ac97_page
                && fault < (uintptr_t)g_ac97_page + AC97_TRAP_BYTES) {
            if (!VirtualProtect(g_ac97_page, AC97_TRAP_BYTES,
                                PAGE_READWRITE, &old))
                return EXCEPTION_CONTINUE_SEARCH;
            a2h_slotw_take_pending(A2H_SLOTW_PEND_AC97, ep->ContextRecord->EFlags);
            ep->ContextRecord->EFlags |= 0x100u;   /* TF: step the write */
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Arm the trap. Called once the MCPX aperture exists, and only alongside the
 * rest of RECOMP_AC97_READY: a title that never gets as far as resetting a
 * channel has nothing to gain from it, and the page fault costs something. */
static void ac97_arm_write_trap(void)
{
    DWORD old;

    if (!g_mcpx_memory || g_ac97_page)
        return;
    g_ac97_page = (char *)g_mcpx_memory + AC97_NABM_OFFSET;
    /* First, so it runs before the game target's own crash reporter, which
     * would otherwise print the write as an access violation. */
    g_ac97_veh = AddVectoredExceptionHandler(1, ac97_write_veh);
    if (!g_ac97_veh
            || !VirtualProtect(g_ac97_page, AC97_TRAP_BYTES,
                               PAGE_READONLY, &old)) {
        if (g_ac97_veh) {
            RemoveVectoredExceptionHandler(g_ac97_veh);
            g_ac97_veh = NULL;
        }
        g_ac97_page = NULL;
        fprintf(stderr, "  AC97: could not arm the bus-master write trap;"
                        " a channel reset will spin\n");
        return;
    }
    fprintf(stderr, "  AC97: bus-master writes trapped at 0x%08X"
                    " (channel reset completes on write)\n",
            XBOX_MCPX_BASE + AC97_NABM_OFFSET);
}

/*
 * Command words the DSP stub completes instantly. A bring-up probe, not a
 * model.
 *
 * The GP and EP DSPs in src/apu are stubs -- effects bypass, encode
 * passthrough -- and a stub that never completes is worse for a title than
 * one that completes at once, because the title cannot get past it at all.
 * DDS9 posts a command and waits for the DSP to clear it:
 *
 *     mov  [ebx], 3            ; ebx = scratch + 0x810
 *   L: cmp  dword [ebx], 0
 *     jne  L
 *
 * Unlike the AC'97 reset bit, this one re-reads every iteration, so clearing
 * it from here is a race this side wins rather than loses.
 *
 * Why an environment variable rather than a registration API: the word's
 * address is reached as *(*(*(this+8)+0x10)) + 0x810 from an object with no
 * global anchor, and the APU never sees that address directly -- it reaches
 * the block through the scatter-gather descriptors the title programmed. So
 * the honest fix is for the GP stub to follow those descriptors, which is DSP
 * work. This exists to answer, in one run and without that work, whether
 * completing the command is in fact all the title is waiting for.
 *
 * RECOMP_DSP_ACK=0x804A8810[,...] -- up to 8 words, zeroed whenever non-zero.
 */
#define XBOX_MAX_DSP_ACK 8
static uint32_t g_dsp_ack[XBOX_MAX_DSP_ACK];
static int g_dsp_ack_count = 0;

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_DSP_ACK");
    char buf[128], *q, *end;

    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (q = buf; *q && g_dsp_ack_count < XBOX_MAX_DSP_ACK; ) {
        unsigned long va = strtoul(q, &end, 0);
        if (end == q)
            break;
        g_dsp_ack[g_dsp_ack_count++] = (uint32_t)va;
        q = (*end == ',') ? end + 1 : end;
    }
    if (g_dsp_ack_count)
        fprintf(stderr, "  DSP ack: %d command word(s) will be completed"
                        " immediately\n", g_dsp_ack_count);
}

static int fence_readable(uint32_t va, uint32_t bytes);  /* defined below */

/* Hold a guest global at a value. A bring-up probe, like the DSP ack.
 *
 * There is exactly one reason this exists: to answer "is the title waiting on
 * this?" in one run, before spending a day making the thing that would set it
 * honestly. It is not a fix and must not be mistaken for one -- whatever it
 * holds, nothing in the guest is producing, so the state it fakes is
 * inconsistent with everything downstream of it by construction.
 *
 * RECOMP_POKE=0x30F234:1,0x30F238:1
 */
#define XBOX_MAX_POKE 8
static struct { uint32_t va, value; } g_poke[XBOX_MAX_POKE];
static int g_poke_count;

static void poke_init(void)
{
    const char *spec = getenv("RECOMP_POKE");
    char buf[192], *q, *end;

    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (q = buf; *q && g_poke_count < XBOX_MAX_POKE; ) {
        unsigned long va = strtoul(q, &end, 0);
        unsigned long val = 0;
        if (end == q)
            break;
        if (*end == ':')
            val = strtoul(end + 1, &end, 0);
        g_poke[g_poke_count].va    = (uint32_t)va;
        g_poke[g_poke_count].value = (uint32_t)val;
        g_poke_count++;
        q = (*end == ',') ? end + 1 : end;
    }
    if (g_poke_count)
        fprintf(stderr, "  POKE: holding %d guest global(s) -- bring-up probe,"
                        " not a fix\n", g_poke_count);
}

static void poke_tick(void)
{
    int i;

    for (i = 0; i < g_poke_count; i++) {
        if (!fence_readable(g_poke[i].va, 4))
            continue;
        {
            volatile uint32_t *w = (volatile uint32_t *)
                ((uintptr_t)g_poke[i].va + g_memory_offset);
            if (*w != g_poke[i].value)
                *w = g_poke[i].value;
        }
    }
}

static void dsp_ack_tick(void)
{
    int i;

    for (i = 0; i < g_dsp_ack_count; i++) {
        /* fence_readable rather than a bare bounds test: these land in the
         * contiguous window, which a plain size check against the main map
         * rejects. */
        if (!fence_readable(g_dsp_ack[i], 4))
            continue;
        {
            volatile uint32_t *w = (volatile uint32_t *)
                ((uintptr_t)g_dsp_ack[i] + g_memory_offset);
            if (*w)
                *w = 0;
        }
    }
}

static const uint32_t MCPX_COUNTERS[] = {
    0x020010,   /* APU GP sample counter, DirectSound SetupVoiceProcessor */
};

/* AC'97 register offsets and bits, used by the always-on codec model in
 * nv2a_ack_thread below. File scope because the worker is defined far above
 * the layout code that documents them. */
#define MCPX_AC97_GLOB_CNT     0x0040012Cu   /* 0xFEC0012C: GC, guest-written */
#define MCPX_AC97_CODEC_STATUS 0x00400130u   /* 0xFEC00130: GS, guest-polled */
#define MCPX_AC97_COLD_RESET   0x00000002u   /* GC bit 1, active-low Cold Reset# */
#define MCPX_AC97_CODEC_READY  0x00000100u   /* GS bit 8, primary codec ready */

static void *g_mcpx_regs = NULL;
/* Set when the APU's registers are unmapped so they can be routed to the
 * emulated APU. Once that happens they are no longer plain memory, and the
 * counter ticking below must leave them alone -- writing through the pointer
 * faults, and the emulated APU owns those registers anyway. */
static int g_apu_mmio_trapped = 0;

/*
 * GPU completion fences the title waits on in guest memory rather than in the
 * aperture. See xbox_Nv2aMirrorFence in the header for why this is the same
 * acknowledgement the NV2A_ACK table makes, and why the address has to be
 * followed through the device struct instead of being a constant.
 */
#define XBOX_MAX_FENCE_MIRRORS 4

static struct {
    uint32_t device_ptr_va;
    uint32_t src_off;
    uint32_t ptr_off;
} g_fence_mirrors[XBOX_MAX_FENCE_MIRRORS];
static int g_fence_mirror_count = 0;

int xbox_Nv2aMirrorFence(uint32_t device_ptr_va,
                         uint32_t src_off, uint32_t ptr_off)
{
    if (g_fence_mirror_count >= XBOX_MAX_FENCE_MIRRORS)
        return -1;
    g_fence_mirrors[g_fence_mirror_count].device_ptr_va = device_ptr_va;
    g_fence_mirrors[g_fence_mirror_count].src_off = src_off;
    g_fence_mirrors[g_fence_mirror_count].ptr_off = ptr_off;
    g_fence_mirror_count++;
    fprintf(stderr, "  NV2A fence mirror: device at 0x%08X,"
            " +0x%X -> *(+0x%X)\n",
            device_ptr_va, src_off, ptr_off);
    return 0;
}
/* A guest address is usable only once the window is mapped and it lands
 * inside it; the chain is followed fresh every poll because the title may not
 * have built it yet. */
static int fence_readable(uint32_t va, uint32_t bytes)
{
    /* Page zero is unmapped, so the bound is the first mapped page rather than
     * just "not null": the device pointer is zero until the title creates the
     * device, and this thread polls from before that. Rejecting only 0 let
     * dev + get_ptr_off through as 0x34 and faulted on the very first tick. */
    if (g_memory_base == NULL || va < XBOX_FS_BASE)
        return 0;
    /* The contiguous window is mapped separately and sits far above the main
     * range, so a size check against g_memory_size rejects it. The fence a
     * title waits on is exactly the kind of block that lives there --
     * MmAllocateContiguousMemory is where a GPU-written semaphore comes
     * from -- so a chain ending in that window has to be followed, not
     * discarded. */
    if (va >= XBOX_CONTIG_BASE
            && (uint64_t)va + bytes <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
        return g_contig_memory != NULL;
    return (size_t)va + bytes <= g_memory_size;
}

/*
 * Two counters inside the device, one of which the GPU owns.
 *
 * D3D's swap throttle is a pair: the title bumps "frames submitted" itself and
 * waits for "frames completed", which on hardware only the GPU moves. The Xbox
 * dashboard's is exactly that, at guest 0x000AF121 --
 *
 *     eax = [esi+0x2518]        ; completed
 *     ecx = [esi+0x2B60]        ; submitted
 *     ecx = ecx - eax
 *     if (ecx < 2) proceed      ; else spin on a 400-iteration delay loop
 *
 * -- and with nothing moving completed it spins there forever once two frames
 * are outstanding. That delay loop was 99.8 million of the dashboard's calls,
 * against 35 thousand for the next function down.
 *
 * This differs from xbox_Nv2aFrameCounter, which advances a counter on a 60 Hz
 * clock, in the way that matters for this pair: a free-running counter can
 * pass submitted, and then submitted - completed underflows to about four
 * billion, which is >= 2, and the spin never ends again. Mirroring cannot do
 * that, because completed is only ever whatever submitted already is.
 *
 * It is also simply true here. The pushbuffer is executed at submit, so by the
 * time the title asks whether the frame is finished, it is.
 */
#define XBOX_MAX_COUNTER_MIRRORS 4

static struct {
    uint32_t device_ptr_va;
    uint32_t src_off, dst_off;
} g_counter_mirrors[XBOX_MAX_COUNTER_MIRRORS];
static int g_counter_mirror_count = 0;

int xbox_Nv2aMirrorCounter(uint32_t device_ptr_va,
                           uint32_t src_off, uint32_t dst_off)
{
    if (g_counter_mirror_count >= XBOX_MAX_COUNTER_MIRRORS)
        return -1;
    g_counter_mirrors[g_counter_mirror_count].device_ptr_va = device_ptr_va;
    g_counter_mirrors[g_counter_mirror_count].src_off = src_off;
    g_counter_mirrors[g_counter_mirror_count].dst_off = dst_off;
    g_counter_mirror_count++;
    fprintf(stderr, "  NV2A counter mirror: device at 0x%08X,"
            " +0x%X -> +0x%X\n", device_ptr_va, src_off, dst_off);
    return 0;
}

static void counter_mirrors_tick(void)
{
    for (int i = 0; i < g_counter_mirror_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_counter_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_counter_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_counter_mirrors[i].src_off, 4)
                || !fence_readable(dev + g_counter_mirrors[i].dst_off, 4))
            continue;
        {
            volatile uint32_t *dst =
                (volatile uint32_t *)((uintptr_t)(dev + g_counter_mirrors[i].dst_off)
                                      + g_memory_offset);
            uint32_t src =
                *(volatile uint32_t *)((uintptr_t)(dev + g_counter_mirrors[i].src_off)
                                       + g_memory_offset);
            if (*dst != src)
                *dst = src;
        }
    }
}

/*
 * Frame counters the title polls to pace itself.
 *
 * D3D keeps a swap count inside the device and bumps it once per presented
 * frame; a title that wants to wait a frame reads it and spins until it moves.
 * Wreckless does exactly that at guest 0x000DC5E0 -- "loop while the counter
 * has advanced by less than 2" -- so a counter that never moves is not a
 * dropped frame, it is a hang with a full asset load behind it.
 *
 * Nothing here presents, so nothing would ever move it. Advancing it on a
 * clock is what makes the wait terminate, and 60 Hz is the rate the title
 * expects the display to run at. Followed through the device pointer for the
 * same reason the fence is: the device is allocated at runtime.
 */
#define XBOX_MAX_FRAME_COUNTERS 4
#define XBOX_FRAME_PERIOD_MS    16      /* ~60 Hz */

static struct {
    uint32_t device_ptr_va;
    uint32_t counter_off;
} g_frame_counters[XBOX_MAX_FRAME_COUNTERS];
static int   g_frame_counter_count = 0;
static DWORD g_frame_counter_last_ms = 0;

int xbox_Nv2aFrameCounter(uint32_t device_ptr_va, uint32_t counter_off)
{
    if (g_frame_counter_count >= XBOX_MAX_FRAME_COUNTERS)
        return -1;
    g_frame_counters[g_frame_counter_count].device_ptr_va = device_ptr_va;
    g_frame_counters[g_frame_counter_count].counter_off   = counter_off;
    g_frame_counter_count++;
    fprintf(stderr, "  Frame counter: device at 0x%08X, count +0x%X @ %d Hz\n",
            device_ptr_va, counter_off, 1000 / XBOX_FRAME_PERIOD_MS);
    return 0;
}

/* A real swap happened: advance every registered counter, and remember when.
 *
 * The timer below exists for a title nothing presents for. Once the
 * pushbuffer executor is actually running flips, the timer is the wrong
 * clock and an actively harmful one: Half-Life 2's loader paces its intro on
 * this count, so a 62 Hz timer against an executor managing a fraction of a
 * frame per second ran the video forward in virtual time far faster than it
 * could be drawn. Only every few hundredth frame was ever presented, each one
 * sampled part way through its own decode -- which looks exactly like a
 * stalling, blocky video rather than a clock running away.
 */
static DWORD g_frame_counter_flip_ms;

void xbox_Nv2aFrameCounterFlip(void)
{
    int i;

    g_frame_counter_flip_ms = GetTickCount();
    if (!g_frame_counter_flip_ms)
        g_frame_counter_flip_ms = 1;          /* 0 means "never" */
    for (i = 0; i < g_frame_counter_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_frame_counters[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_frame_counters[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_frame_counters[i].counter_off, 4))
            continue;
        *(volatile uint32_t *)((uintptr_t)(dev + g_frame_counters[i].counter_off)
                               + g_memory_offset) += 1;
    }
}

static void frame_counters_tick(void)
{
    DWORD now = GetTickCount();
    int i;

    if (!g_frame_counter_count)
        return;
    if (g_frame_counter_last_ms
            && (now - g_frame_counter_last_ms) < XBOX_FRAME_PERIOD_MS)
        return;
    /* Something is presenting: let it drive the count instead. Two seconds,
     * because the executor's flips are not evenly spaced and a title that
     * genuinely stops presenting still has to be got moving again. */
    if (g_frame_counter_flip_ms && (now - g_frame_counter_flip_ms) < 2000) {
        g_frame_counter_last_ms = now;
        return;
    }
    g_frame_counter_last_ms = now;

    for (i = 0; i < g_frame_counter_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_frame_counters[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_frame_counters[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_frame_counters[i].counter_off, 4))
            continue;
        *(volatile uint32_t *)((uintptr_t)(dev + g_frame_counters[i].counter_off)
                               + g_memory_offset) += 1;
    }
}

static void fence_mirrors_tick(void)
{
    for (int i = 0; i < g_fence_mirror_count; i++) {
        uint32_t dev, get_ptr;

        if (!fence_readable(g_fence_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_fence_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_fence_mirrors[i].ptr_off, 4)
                || !fence_readable(dev + g_fence_mirrors[i].src_off, 4))
            continue;
        get_ptr = *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].ptr_off)
                                         + g_memory_offset);
        if (!fence_readable(get_ptr, 4))
            continue;
        {
            volatile uint32_t *fence =
                (volatile uint32_t *)((uintptr_t)get_ptr + g_memory_offset);
            uint32_t value =
                *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].src_off)
                                       + g_memory_offset);
            if (*fence != value)
                *fence = value;
        }
    }
}

static int s_nv2a_trace = 0;

/* The display framebuffer, as reported by AvSetDisplayMode. Checksummed once a
 * second so a run can answer the only question that matters before building a
 * presenter: is the guest putting pixels anywhere at all, and do they change
 * from frame to frame. */
static uint32_t s_fb_va, s_fb_pitch, s_fb_height = 480;

void xbox_SetDisplayFramebuffer(uint32_t fb_va, uint32_t pitch)
{
    s_fb_va = fb_va;
    s_fb_pitch = pitch;
}

/* Where the title last said its framebuffer is, for anything that wants to read
 * guest pixels directly. There was only a setter, so every such reader carried
 * its own hardcoded address instead -- and a title that moves its framebuffer
 * (which is most of them, once it owns one) left that constant pointing at
 * uninitialised memory. A dump taken there is not empty, it is noise, which
 * reads as "the title drew garbage" rather than "you read the wrong page".
 *
 * This is the resolved address, not the physical one AvSetDisplayMode states:
 * the caller resolves before storing, because a physical framebuffer address
 * read directly lands in the loaded image. Getting that wrong is the same bug
 * twice over -- once in the probe below, once in a caller of this. */
uint32_t xbox_GetDisplayFramebuffer(uint32_t *pitch)
{
    if (pitch)
        *pitch = s_fb_pitch;
    return s_fb_va;
}

static void framebuffer_probe_tick(void)
{
    static DWORD last_ms;
    static uint32_t last_sum;
    DWORD now = GetTickCount();
    uint32_t sum = 0, nonzero = 0, i, n;
    const uint32_t *p;

    if (!s_nv2a_trace || !s_fb_va || !s_fb_pitch)
        return;
    if (last_ms && (now - last_ms) < 1000)
        return;
    last_ms = now;
    if ((size_t)s_fb_va + s_fb_pitch * s_fb_height > g_memory_size)
        return;
    p = (const uint32_t *)((uintptr_t)s_fb_va + g_memory_offset);
    n = (s_fb_pitch * s_fb_height) / 4;
    for (i = 0; i < n; i++) {
        sum = sum * 33u + p[i];
        if (p[i]) nonzero++;
    }
    fprintf(stderr, "  [FB] 0x%08X sum=%08X nonzero=%u/%u %s\n",
            s_fb_va, sum, nonzero, n,
            sum != last_sum ? "CHANGED" : "same");
    last_sum = sum;
    fflush(stderr);
}

/* Clear the request bits "hardware" would clear on its own.
 *
 * Also called by the pushbuffer walker between commands. This thread executes
 * the pushbuffer below, and under RECOMP_PB_EXEC one pass can take a whole
 * rendered frame; with the bits cleared only here, every XDK D3D kickoff --
 * which sets 0x100410 bit 16 and spins until it clears -- waited that long,
 * and a level load, thousands of kickoffs long, looked frozen. Clearing them
 * from inside the walk answers the kickoff within a few hundred commands,
 * without a thread of its own spinning on a host core. */
void xbox_Nv2aAckBusyBits(void)
{
    volatile uint32_t *regs = (volatile uint32_t *)g_nv2a_memory;

    /* These are register mutations, so the register-owner gate applies here too:
     * RECOMP_GPU_ACK=0 or a claimed aperture means nothing clears busy bits. */
    if (!regs || !InterlockedCompareExchange(&g_nv2a_ack_enabled, 0, 0))
        return;
    for (size_t i = 0; i < sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]); i++) {
        volatile uint32_t *r =
            (volatile uint32_t *)((char *)regs + NV2A_ACK[i].offset);
        if (*r & NV2A_ACK[i].busy_mask) {
            *r &= ~NV2A_ACK[i].busy_mask;
        }
    }
}

static DWORD WINAPI nv2a_ack_thread(LPVOID param)
{
    volatile uint32_t *regs = (volatile uint32_t *)param;
    while (!InterlockedCompareExchange(&g_nv2a_ack_stop, 0, 0)) {
        if (InterlockedCompareExchange(&g_nv2a_ack_enabled, 0, 0)) {
        InterlockedIncrement(&g_nv2a_ack_active);
        if (InterlockedCompareExchange(&g_nv2a_ack_enabled, 0, 0)) {
        xbox_Nv2aAckBusyBits();
        for (size_t i = 0; i < sizeof(NV2A_IDLE) / sizeof(NV2A_IDLE[0]); i++) {
            volatile uint32_t *r =
                (volatile uint32_t *)((char *)regs + NV2A_IDLE[i].offset);
            if ((*r & NV2A_IDLE[i].idle_mask) != NV2A_IDLE[i].idle_mask) {
                *r |= NV2A_IDLE[i].idle_mask;
            }
        }
        /* DMA_GET used to be set to DMA_PUT here, at the top of the tick,
         * before the scan below had executed anything.
         *
         * GET is what tells the title how far the GPU has consumed, and D3D
         * waits on it before reusing the ring. Reporting "all consumed"
         * while the commands were still unread gave the title permission to
         * overwrite them, and it took it: the executor then read whatever
         * part of the segment had survived, so each pass drew a different
         * subset of the frame. It looks like unstable geometry and is a
         * lost-command race.
         *
         * The advance now happens after the scan, further down, which is
         * also the only ordering that gives the title real back-pressure. */
        frame_counters_tick();
        framebuffer_probe_tick();

        /* Which framebuffer the display would be scanning out.
         *
         * PCRTC_START holds the address the CRTC reads pixels from, so
         * whatever the title last set there is the frame it believes is on
         * screen. Nothing here scans out, so this is the one place that says
         * whether the guest is producing an image at all -- and where it is.
         * Gated, because it is a bring-up question, not a runtime one. */
        /* Not gated on the trace flag: nv2a_pb_scan is what drives the
         * executor, and it already returns unless RECOMP_PB_SCAN or
         * RECOMP_PB_EXEC asked for it. Gating the call as well meant
         * RECOMP_PB_EXEC on its own did nothing at all, and the executor
         * only ran when someone happened to also be tracing. */
        {
            /* Is the title submitting GPU work at all? PUT is where the
             * title's pushbuffer writer has got to; if it never moves, nothing
             * is being drawn and the missing piece is upstream of the GPU. */
            static DWORD  last_put_ms;
            static uint32_t last_put;
            static uint32_t get_written = 0xFFFFFFFFu;  /* GET as we last left it */
            DWORD now_ms = GetTickCount();
            uint32_t put = *(volatile uint32_t *)((char *)regs + NV2A_USER_DMA_PUT);
            if (put != last_put || (now_ms - last_put_ms) > 2000) {
                /* Survey the segment the title just submitted, once. */
                {
                    extern void nv2a_pb_scan(uint32_t);
                    extern void nv2a_pb_scan_report(void);
                    static DWORD last_report;

                    /* DMA_PUT holds a PHYSICAL address -- Xbox D3D writes
                     * `VA & 0x0FFFFFFF` and reads the GPU's position back as
                     * `GET | 0x80000000`. nv2a_pb_scan reads guest VAs, so
                     * handing it the raw register value pointed it at low
                     * memory: for the Xbox Dashboard, whose pushbuffer is at
                     * 0x80001000, PUT reads 0x1000 and the survey walked the
                     * fake TIB. It reported a plausible-looking inventory of
                     * nothing, which is worse than reporting none -- the
                     * conclusion drawn was "the title submits no methods"
                     * while it was submitting them the whole time.
                     *
                     * The contiguous window IS the physical-address view, so
                     * OR-ing its base is the documented round trip, not a
                     * guess. */
                    extern void nv2a_pb_resync(uint32_t);
                    uint32_t get_now = *(volatile uint32_t *)
                                       ((char *)regs + NV2A_USER_DMA_GET);
                    /* GET is ours to advance; if it is not what we last
                     * wrote, the title reset the ring. The first time, it is
                     * where D3D started the ring: walking from there rather
                     * than from the first PUT keeps the one-time device state
                     * (depth function, and so on) sent before it. */
                    if (get_written == 0xFFFFFFFFu || get_now != get_written)
                        nv2a_pb_resync(get_now);
                    if (put != last_put)
                        nv2a_pb_scan(put);
                    /* Periodic, because what the title submits at init is not
                     * what it submits once it is drawing a menu, and the
                     * question the survey answers is about the latter. */
                    if (s_nv2a_trace && now_ms - last_report > 10000) {
                        last_report = now_ms;
                        nv2a_pb_scan_report();
                    }
                }
                /* Consumed, now that it has actually been executed. */
                {
                    volatile uint32_t *get =
                        (volatile uint32_t *)((char *)regs
                                              + NV2A_USER_DMA_GET);
                    *get = put;
                    get_written = put;
                }
                last_put = put; last_put_ms = now_ms;
                /* GET as well as PUT. A title that stops submitting has either
                 * finished or is spinning on the GPU catching up, and only GET
                 * tells those apart -- D3D waits for GET to reach PUT before it
                 * reuses the buffer, so GET stuck behind PUT is the shape of a
                 * pushbuffer-full hang. Also show the same pair as the Xbox
                 * Dashboard reads them: its D3D holds a register-block pointer
                 * in its device struct rather than assuming 0xFD800000, and
                 * mirroring the wrong block leaves it spinning on a GET that
                 * never moves. */
                if (s_nv2a_trace) {
                    uint32_t g = *(volatile uint32_t *)
                                 ((char *)regs + NV2A_USER_DMA_GET);
                    fprintf(stderr, "  [NV2A] DMA_PUT = 0x%08X  DMA_GET = "
                            "0x%08X%s\n", put, g,
                            g == put ? "" : "  (GPU behind)");
                }
                fflush(stderr);
            }
        }
        if (s_nv2a_trace) {
            static uint32_t last_start = 0xFFFFFFFFu;
            uint32_t start = *(volatile uint32_t *)((char *)regs + 0x600800);
            if (start != last_start) {
                last_start = start;
                fprintf(stderr, "  [NV2A] PCRTC_START = 0x%08X\n", start);
                fflush(stderr);
            }
        }

        } /* ownership recheck */
        InterlockedDecrement(&g_nv2a_ack_active);
        } /* GPU acknowledgement, mirrors and optional executor */
        if (g_mcpx_regs && !g_apu_mmio_trapped) {
            for (size_t i = 0; i < sizeof(MCPX_COUNTERS) / sizeof(MCPX_COUNTERS[0]); i++) {
                volatile uint32_t *c =
                    (volatile uint32_t *)((char *)g_mcpx_regs + MCPX_COUNTERS[i]);
                *c += 1;
            }
        }

        /* AC'97 codec presence. GS(0xFEC00130).bit8 := GC(0xFEC0012C).bit1.
         *
         * Level-triggered every tick: the guest's own reset write drives the
         * ready bit, so this models device state rather than asserting a
         * constant. See the derivation and its citations at the definition of
         * MCPX_AC97_CODEC_STATUS above.
         *
         * OUTSIDE the g_apu_mmio_trapped gate on purpose, like the mirrors
         * below: this writes GUEST MEMORY in the AC'97 aperture, which is above
         * the APU's 512K window, so who owns the APU registers is irrelevant.
         * Gating it would let RECOMP_APU_TRAP silently stop the model.
         *
         * Atomic read-modify-write: sub_001A71B3 performs full-dword writes to
         * this register, so a plain |= from this thread could lose a guest
         * update. No other bit of GLOB_STA is touched, and GC is never written
         * from the host -- it is read only, so the guest's own value is what
         * drives the model. */
        if (g_mcpx_regs) {
            volatile uint32_t *gc =
                (volatile uint32_t *)((char *)g_mcpx_regs + MCPX_AC97_GLOB_CNT);
            volatile uint32_t *gs =
                (volatile uint32_t *)((char *)g_mcpx_regs + MCPX_AC97_CODEC_STATUS);
            uint32_t gc_now = *gc;

            if (gc_now & MCPX_AC97_COLD_RESET)
                InterlockedOr((volatile LONG *)gs, (LONG)MCPX_AC97_CODEC_READY);
            else
                InterlockedAnd((volatile LONG *)gs, (LONG)~MCPX_AC97_CODEC_READY);

            /* Reach witness: print once, on the first tick that observes GC bit
             * 1 set, and read BOTH registers back through the guest mapping --
             * not from locals -- so a wrong-address model is visible rather
             * than silent. Printed AFTER the atomic set on this same tick: the
             * reverse order would report GS bit 8 clear while GC bit 1 is set,
             * and the packet's AC4 would then FAIL a correct model.
             *
             * Emitted with a bare fprintf + fflush, NOT through the
             * KERNEL_LOG_ON budget gate, so it survives log truncation -- the
             * same property that makes the KeConnectInterrupt line usable as
             * AC4-POLL. */
            if (!g_ac97_witness_done && (gc_now & MCPX_AC97_COLD_RESET)) {
                g_ac97_witness_done = 1;
                fprintf(stderr,
                        "[A3A] ac97 witness: gc=0x%08X gs=0x%08X\n",
                        (unsigned)*gc, (unsigned)*gs);
                fflush(stderr);
            }
        }

        /* Advance KeTickCount. It was written once at init and left frozen,
         * which silently breaks every timeout that polls it: Halo's DHCP setup
         * waits on a tick deadline that never arrives and spins forever bringing
         * up XNet. A live clock is also just the truth -- KeTickCount ticks on
         * hardware whether or not anyone is asleep. GetTickCount() shares the
         * millisecond unit, so the rate matches. */
        *(volatile uint32_t *)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT)
                               + g_memory_offset) = GetTickCount();

        /* Publish model state into guest memory. Outside the register gate on
         * purpose: these write GUEST MEMORY, not MMIO, so who owns the
         * registers is irrelevant to them. Leaving them inside meant claiming
         * the register owner silently stopped every mirror -- which is why
         * nothing ever wrote the notify word a title waits on. The two
         * bring-up probes also write guest memory and belong here; both do
         * nothing unless RECOMP_DSP_ACK / RECOMP_POKE are set. */
        fence_mirrors_tick();
        dsp_ack_tick();
        poke_tick();
        counter_mirrors_tick();

        Sleep(0);  /* yield; the waiter is spinning on another core */
    }
    return 0;
}

static void xbox_Nv2aAckStart(void)
{
    const char *ack = getenv("RECOMP_GPU_ACK");
    InterlockedExchange(&g_nv2a_ack_enabled,
                        !(ack && strcmp(ack, "0") == 0));
    fprintf(stderr, "  NV2A GPU acknowledgement mutations: %s (kernel/APU clock worker retained)\n",
            g_nv2a_ack_enabled ? "enabled" : "disabled");
    dsp_ack_init();
    poke_init();
    g_nv2a_ack_stop = 0;
    g_nv2a_ack_thread = CreateThread(NULL, 0, nv2a_ack_thread,
                                     g_nv2a_memory, 0, NULL);
    if (g_nv2a_ack_thread) {
        fprintf(stderr, "  NV2A busy-bit ack table: %zu register(s) (%s)\n",
                sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]),
                g_nv2a_ack_enabled ? "enabled" : "disabled");
    }
}

/* Separate allocation for Xbox kernel address space (0x80010000+).
 * Some RenderWare code reads the kernel PE header to detect features. */
static void *g_kernel_memory = NULL;

/* Global offset accessible by recompiled code (via recomp_types.h) */
ptrdiff_t g_xbox_mem_offset = 0;

/* Bounds of the title's executable sections, from its own XBE section table.
 *
 * RECOMP_ICALL uses these to decide whether an indirect-call target is code
 * before dispatching it. This used to be a hardcoded "0x00400000..0xFE000000 is
 * not code" test, which is true for Burnout 3 -- its .text ends at 0x002CC200,
 * so everything above 0x400000 really is data -- and false for any title with
 * more code than that. Half-Life 2's .text runs to 0x005F4A6C, so the constant
 * silently discarded every indirect call into the top two thirds of the game,
 * including the one that enters its main. No log, no crash: eax = 0 and carry
 * on, which looks exactly like a function that returned early.
 *
 * Zero until the layout is initialised, which the macro treats as "allow" so
 * nothing breaks before the title is loaded. */
uint32_t g_xbox_image_lo = 0;
uint32_t g_xbox_image_hi = 0;
uint32_t g_xbox_code_lo = 0;
uint32_t g_xbox_code_hi = 0;

/* Global registers for recompiled code (via recomp_types.h) */
/* Each guest thread's TIB. The first thread uses the one the loader built;
 * a spawned thread gets its own from xbox_AllocThreadTib(). */
RECOMP_TLS uint32_t g_fs_base = XBOX_TIB_MAIN;

/* The shape of the TLS block the loader built, so a new thread can be
 * given one just like it: where the initialised image data starts, how
 * big the block is, and how big the per-thread structure slot 0 points
 * at is. Zero total means the image had no TLS directory. */
static uint32_t g_tls_template_va, g_tls_total, g_tls_thread_size = 64;

RECOMP_TLS uint32_t g_eax = 0, g_ecx = 0, g_edx = 0, g_esp = 0;
RECOMP_TLS uint32_t g_ebx = 0, g_esi = 0, g_edi = 0;

#ifdef RECOMP_ABI_CHECK
/* Report a lifted function that returned without restoring ebx/esi/edi.
 *
 * Those are callee-saved on x86, and the recompiler keeps them in globals, so
 * a function whose epilogue was never lifted corrupts its caller rather than
 * itself -- an error with no crash and no message, just less work silently
 * done. Ranked by hit count so the routine breaking a hot loop stands out from
 * the one-offs; -DRECOMP_ABI_CHECK only, since it costs three compares on
 * every indirect call.
 */
extern RECOMP_TLS volatile uint32_t g_icall_trace[16];
extern RECOMP_TLS volatile uint32_t g_icall_trace_idx;

void recomp_abi_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0,
                              uint32_t edi0, uint32_t esp0)
{
    enum { SLOTS = 32 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
        fprintf(stderr, "[ABI] sub_%08X:%s%s%s%s\n"
                        "      ebx %08X->%08X esi %08X->%08X"
                        " edi %08X->%08X esp %08X->%08X\n",
                va,
                g_ebx != ebx0 ? " ebx" : "",
                g_esi != esi0 ? " esi" : "",
                g_edi != edi0 ? " edi" : "",
                g_esp < esp0 + 4 ? " esp(epilogue never ran)" : "",
                ebx0, g_ebx, esi0, g_esi, edi0, g_edi, esp0, g_esp);
        /* esp coming back too HIGH means some callee popped arguments that
         * were never pushed -- a convention mismatch the one-sided invariant
         * above cannot see. The most recent indirect targets are the usual
         * suspects, so name them. */
        {
            int t;
            fprintf(stderr, "      esp delta %+d, recent icall targets:",
                    (int)(g_esp - esp0));
            for (t = 4; t >= 1; t--)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx - t) & 15]);
            fputc('\n', stderr);
        }
        fflush(stderr);
    }
    hits[i]++;
}
#endif

/* SEH frame pointer bridge (see recomp_types.h for explanation) */
RECOMP_TLS uint32_t g_seh_ebp = 0;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top = 0;

/* Set once at startup. The generated code reads it at the ret of every
 * --force-return function, so it has to be cheap and it has to default to
 * off: a build carrying forced functions behaves normally until the
 * variable is set. */
int g_force_return = 0;
/* x87 control and status. The reset default masks every exception and
 * rounds to nearest, which is what the CRT expects before _control87. */
RECOMP_TLS uint16_t g_fp_control_word = 0x037Fu;
RECOMP_TLS int g_fp_cmp = 0;
RECOMP_TLS uint16_t g_fp_cc = 0x4000;

/* Defined below, with the other guest registers. */
extern RECOMP_TLS uint32_t g_ebp;
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi;

/* ---- non-local jumps ---------------------------------------------------
 *
 * The native half of the guest's setjmp/longjmp. See recomp_types.h for why a
 * guest-only longjmp is not enough; in short, the recompiled frames are C
 * frames and something has to unwind them.
 *
 * Keyed by guest buffer address, per thread. Buffers nest, so jumping to an
 * outer one discards every inner entry -- those frames are gone.
 */
#define RECOMP_JMPBUF_SLOTS 32

typedef struct {
    uint32_t buf_va;
    jmp_buf  native;
} recomp_jmp_slot;

static RECOMP_TLS recomp_jmp_slot s_jmp[RECOMP_JMPBUF_SLOTS];
static RECOMP_TLS int             s_jmp_used;

jmp_buf *recomp_setjmp_slot(uint32_t buf_va)
{
    int i;

    for (i = 0; i < s_jmp_used; i++)
        if (s_jmp[i].buf_va == buf_va)
            return &s_jmp[i].native;      /* the same buffer, re-armed */
    if (s_jmp_used >= RECOMP_JMPBUF_SLOTS)
        s_jmp_used = RECOMP_JMPBUF_SLOTS - 1;   /* keep the deepest */
    s_jmp[s_jmp_used].buf_va = buf_va;
    return &s_jmp[s_jmp_used++].native;
}

int recomp_guest_longjmp(uint32_t buf_va, uint32_t value)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    int i;

    for (i = s_jmp_used - 1; i >= 0; i--) {
        if (s_jmp[i].buf_va != buf_va)
            continue;

        /* The callee-saved registers and the stack, exactly as the CRT's
         * longjmp restores them: esp is the setjmp-time esp plus the return
         * address that setjmp's own ret would have popped. */
        g_ebx = *(const uint32_t *)(mem + buf_va + 0x04);
        g_edi = *(const uint32_t *)(mem + buf_va + 0x08);
        g_esi = *(const uint32_t *)(mem + buf_va + 0x0C);
        g_esp = *(const uint32_t *)(mem + buf_va + 0x10) + 4;

        /* ebp is a C local in every translated function, and a local modified
         * after setjmp is indeterminate once longjmp lands. Hand the resumed
         * frame its saved value back through the globals it already reads. */
        g_seh_ebp = *(const uint32_t *)(mem + buf_va + 0x00);
        g_ebp     = g_seh_ebp;

        s_jmp_used = i + 1;   /* the inner buffers died with their frames */
        longjmp(s_jmp[i].native, value ? (int)value : 1);
    }
    return 0;
}

/* Watchdog: dump the guest call stack if the title stops making progress.
 *
 * A hang gives nothing to work from -- no crash, no last log line, no native
 * stack that means anything, because the guest frames live in guest memory and
 * the native one only shows whichever translated function is spinning. Sampling
 * the guest stack from a second thread is the one view that says where the
 * title actually is. Same GS format the crash handler uses, so tools/
 * stackwalk.py reads either.
 *
 * Off unless RECOMP_WATCHDOG_SECS is set, so it costs a getenv in normal runs.
 */
/* Defined below, after the watchdog. */
extern RECOMP_TLS volatile uint32_t g_icall_trace[16];
extern RECOMP_TLS volatile uint32_t g_icall_trace_idx;
extern RECOMP_TLS volatile uint64_t g_icall_count;

static uint32_t *s_watchdog_esp;
/* The other guest registers are thread-local too, so the watchdog has to be
 * handed the guest thread's copies rather than reading its own -- which are
 * always zero, and read as "every register is null" at exactly the moment the
 * registers are the thing being asked about. */
static uint32_t *s_watchdog_regs[6];
static unsigned  s_watchdog_secs;
static volatile uint32_t *s_watchdog_trace, *s_watchdog_trace_idx;
static volatile uint64_t *s_watchdog_count;

/* Can RECOMP_PEEK dereference this guest address?
 *
 * It used to accept only the first 64 MB, which reads as "RAM" but is not the
 * question -- every window this file maps is mapped at va + g_memory_offset,
 * so the register apertures are just as dereferenceable as RAM is. Rejecting
 * them silently printed nothing for an address that was perfectly readable,
 * and a hang spinning on a GPU register is exactly the case where the value
 * that matters lives at 0xFD......  Peeking one is how the busy-wait in
 * DDS9's pushbuffer reserve was pinned to a DMA pointer rather than a flag.
 *
 * Every window is checked against its own pointer, because they are mapped
 * independently and any of them can be absent for this run. The 4 is the
 * width of the read below: an address one or two bytes short of the end is
 * inside the window and still faults. */
static int peek_readable(uint32_t va)
{
    struct { const void *mapped; uint32_t base; uint64_t size; } win[] = {
        { g_memory_base,   XBOX_BASE_ADDRESS, (uint64_t)g_memory_size },
        { g_contig_memory, XBOX_CONTIG_BASE,  XBOX_CONTIG_SIZE },
        { g_nv2a_memory,   XBOX_NV2A_BASE,    XBOX_NV2A_SIZE },
        { g_mcpx_memory,   XBOX_MCPX_BASE,    XBOX_MCPX_SIZE },
        { g_flash_memory,  XBOX_FLASH_BASE,   XBOX_FLASH_SIZE },
    };
    size_t i;

    for (i = 0; i < sizeof(win) / sizeof(win[0]); i++) {
        if (!win[i].mapped || !win[i].size)
            continue;
        if (va >= win[i].base
                && (uint64_t)va + 4 <= (uint64_t)win[i].base + win[i].size)
            return 1;
    }
    return 0;
}

/* ---- RECOMP_WATCH: name the guest code that changes a guest dword -------
 *
 * A peek says a value changed between two samples. It does not say who
 * changed it, and for a value produced deep inside a middleware layer that
 * is the only question that matters -- reading the lifted C outwards from
 * the write is guesswork, and reading it inwards from the caller is worse.
 *
 * Same mechanism as the AC'97 trap above: make the page read-only, catch the
 * write, single-step it, then report. What it adds is the guest call chain,
 * scanned off the guest stack the way the watchdog does, which turns "the
 * mask became 4" into a list of addresses to go and read.
 *
 * Off unless RECOMP_WATCH is set. Costs a page fault per write to that page,
 * so it is a bring-up tool and says so.
 */
static uint32_t g_watch_va;
static void    *g_watch_page;
static uint32_t g_watch_last;
static void    *g_watch_veh;
static RECOMP_TLS int s_watch_stepping;

/* A plausible guest code address: inside the image's executable sections,
 * as recorded from the section headers at load. */
static int watch_is_code(uint32_t va)
{
    return va >= g_xbox_code_lo && va < g_xbox_code_hi;
}

static void watch_report(void)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    uint32_t now = *(const uint32_t *)(mem + g_watch_va);
    uint32_t esp = g_esp, i, shown = 0;

    if (now == g_watch_last)
        return;
    fprintf(stderr, "[WATCH] [%08X] %08X -> %08X  (esp=%08X)\n",
            g_watch_va, g_watch_last, now, esp);
    g_watch_last = now;

    /* Return addresses the recompiled code pushed, innermost first. Values
     * that merely look like code get printed too -- the chain is a lead, not
     * a proof, and saying so is cheaper than a stack walk that cannot be
     * done without frame information the lift does not keep. */
    for (i = 0; esp && i < 256u && shown < 12u; i++) {
        uint32_t slot = esp + i * 4u;
        uint32_t v;
        if (!peek_readable(slot))
            break;
        v = *(const uint32_t *)(mem + slot);
        if (watch_is_code(v)) {
            fprintf(stderr, "         [esp+%-4u] %08X\n", i * 4u, v);
            shown++;
        }
    }

    /* RECOMP_WATCH_RAW also prints the frame unfiltered. The filtered chain
     * answers "who wrote this"; the raw frame answers "to what object", which
     * is the next question every time -- saved registers and pointer
     * arguments live there and look nothing like code. */
    if (getenv("RECOMP_WATCH_RAW")) {
        for (i = 0; esp && i < 24u; i++) {
            uint32_t slot = esp + i * 4u;
            if (!peek_readable(slot))
                break;
            fprintf(stderr, "         raw[esp+%-4u] %08X\n", i * 4u,
                    *(const uint32_t *)(mem + slot));
        }
    }
    fflush(stderr);
}

static LONG CALLBACK watch_veh(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    DWORD old;

    if (!g_watch_page)
        return EXCEPTION_CONTINUE_SEARCH;

    if (code == EXCEPTION_SINGLE_STEP && s_watch_stepping) {
        s_watch_stepping = 0;
        watch_report();
        VirtualProtect(g_watch_page, 4096, PAGE_READONLY, &old);
        ep->ContextRecord->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_ACCESS_VIOLATION
            && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];

        if (fault >= (uintptr_t)g_watch_page
                && fault < (uintptr_t)g_watch_page + 4096) {
            if (!VirtualProtect(g_watch_page, 4096, PAGE_READWRITE, &old))
                return EXCEPTION_CONTINUE_SEARCH;
            s_watch_stepping = 1;
            ep->ContextRecord->EFlags |= 0x100u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* The target, which may be reached through pointers that do not exist yet.
 *
 * "[[0x006DF414]]+0x14" is two dereferences and an offset: the interesting
 * field of a heap object whose address changes run to run, but which is
 * always reachable from a static one. Without this the only way to watch such
 * a field is to learn its address from one run and hope the allocator repeats
 * it, which it does not. */
static unsigned g_watch_derefs;
static uint32_t g_watch_root;
static uint32_t g_watch_off;

static int watch_resolve(uint32_t *out)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    uint32_t a = g_watch_root;
    unsigned k;

    for (k = 0; k < g_watch_derefs; k++) {
        if (!peek_readable(a))
            return 0;
        a = *(const uint32_t *)(mem + a);
        if (!a)
            return 0;
    }
    a += g_watch_off;
    if (!peek_readable(a))
        return 0;
    *out = a;
    return 1;
}

static int watch_arm(uint32_t va);

/* Poll until the chain resolves, then arm. Twenty milliseconds, because the
 * object appears once during bring-up and never again -- this thread exists
 * for a few seconds and then does nothing for the rest of the run. */
static DWORD WINAPI watch_resolver(LPVOID unused)
{
    unsigned tries;

    (void)unused;
    for (tries = 0; tries < 15000u && !g_watch_page; tries++) {
        uint32_t va;
        if (watch_resolve(&va) && watch_arm(va))
            return 0;
        Sleep(20);
    }
    if (!g_watch_page)
        fprintf(stderr, "  WATCH: %u-deep chain from 0x%08X never resolved\n",
                g_watch_derefs, g_watch_root);
    return 0;
}

void xbox_WatchInit(void)
{
    const char *spec = getenv("RECOMP_WATCH");
    const char *q;
    char *endp;

    if (!spec || !*spec || g_memory_base == NULL || g_watch_page)
        return;

    for (q = spec; *q == '['; q++)
        g_watch_derefs++;
    g_watch_root = (uint32_t)strtoul(q, &endp, 0);
    while (*endp == ']')
        endp++;
    if (*endp == '+')
        g_watch_off = (uint32_t)strtoul(endp + 1, NULL, 0);

    if (g_watch_derefs) {
        fprintf(stderr, "  WATCH: resolving %u-deep chain from 0x%08X "
                        "+0x%X\n", g_watch_derefs, g_watch_root, g_watch_off);
        CloseHandle(CreateThread(NULL, 0, watch_resolver, NULL, 0, NULL));
        return;
    }
    watch_arm(g_watch_root + g_watch_off);
}

static int watch_arm(uint32_t va)
{
    DWORD old;

    g_watch_va = va;
    if (!peek_readable(g_watch_va)) {
        fprintf(stderr, "  WATCH: 0x%08X is not in a mapped window; "
                        "not armed\n", g_watch_va);
        return 0;
    }
    g_watch_last = *(const uint32_t *)((const uint8_t *)g_memory_offset
                                       + g_watch_va);
    /* The page holding the guest dword, in host terms. */
    g_watch_page = (void *)(((uintptr_t)((const uint8_t *)g_memory_offset
                                         + g_watch_va)) & ~(uintptr_t)4095);
    g_watch_veh = AddVectoredExceptionHandler(1, watch_veh);
    if (!g_watch_veh
            || !VirtualProtect(g_watch_page, 4096, PAGE_READONLY, &old)) {
        if (g_watch_veh) {
            RemoveVectoredExceptionHandler(g_watch_veh);
            g_watch_veh = NULL;
        }
        g_watch_page = NULL;
        fprintf(stderr, "  WATCH: cannot trap 0x%08X; not armed\n",
                g_watch_va);
        return 0;
    }
    fprintf(stderr, "  WATCH: writes to the page of 0x%08X are trapped "
                    "(current %08X)\n", g_watch_va, g_watch_last);
    fflush(stderr);
    return 1;
}

/* Print the RECOMP_PEEK globals. Shared, because the two moments worth
 * sampling are a hang and an early exit, and only the first had it: a title
 * whose main() returns during init never reaches the watchdog, so the one
 * question that mattered -- which of its init calls failed -- was the one the
 * tooling could not answer. Silent unless RECOMP_PEEK is set. */
void xbox_PeekSample(const char *label)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    const char *spec = getenv("RECOMP_PEEK");
    char buf[256], *q, *end;

    if (!spec || !*spec || g_memory_base == NULL)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    fprintf(stderr, "  %s:", label ? label : "peek");
    for (q = buf; *q; ) {
        /* "[[0x006DF414]]+0x14" follows two pointers and adds an offset.
         * The fields worth watching during bring-up are usually inside heap
         * objects whose addresses change run to run but which are always
         * reachable from a static one, and a peek that cannot follow a
         * pointer cannot see them at all. */
        unsigned derefs = 0, k;
        unsigned long va;
        uint32_t a;
        int ok = 1;

        while (*q == '[') { derefs++; q++; }
        va = strtoul(q, &end, 0);
        if (end == q)
            break;
        while (*end == ']')
            end++;
        a = (uint32_t)va;
        for (k = 0; k < derefs && ok; k++) {
            if (!peek_readable(a) || !(a = *(const uint32_t *)(mem + a)))
                ok = 0;
        }
        if (*end == '+')
            a += (uint32_t)strtoul(end + 1, &end, 0);
        if (ok && peek_readable(a))
            fprintf(stderr, " [%08X]=%08X", a, *(const uint32_t *)(mem + a));
        else
            fprintf(stderr, " [%08X]=??", a);
        q = (*end == ',') ? end + 1 : end;
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

static DWORD WINAPI xbox_watchdog_thread(LPVOID unused)
{
    const uint8_t *mem;
    uint32_t esp, i;

    (void)unused;
    Sleep(s_watchdog_secs * 1000u);

    mem = (const uint8_t *)g_memory_offset;
    esp = s_watchdog_esp ? *s_watchdog_esp : 0;
    fprintf(stderr, "[WATCHDOG] no exit after %us; guest esp=0x%08X\n"
            "  regs: eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X\n",
            s_watchdog_secs, esp,
            s_watchdog_regs[0] ? *s_watchdog_regs[0] : 0,
            s_watchdog_regs[1] ? *s_watchdog_regs[1] : 0,
            s_watchdog_regs[2] ? *s_watchdog_regs[2] : 0,
            s_watchdog_regs[3] ? *s_watchdog_regs[3] : 0,
            s_watchdog_regs[4] ? *s_watchdog_regs[4] : 0,
            s_watchdog_regs[5] ? *s_watchdog_regs[5] : 0);
    /* The recent indirect-call targets name whatever is spinning: a stuck loop
     * inside a function reached through a pointer leaves no clue on the stack
     * beyond the return address of the call that entered it. */
    {
        uint32_t k;
        /* The running indirect-call total separates a hang from mere
         * slowness. Kernel calls cannot: a pure CPU loop makes none, so
         * "same count at 20s and 60s" proves nothing about it. */
        fprintf(stderr, "  icalls so far: %llu\n",
                (unsigned long long)(*s_watchdog_count));
        fprintf(stderr, "  recent ICALL targets:");
        for (k = 0; k < 16; k++)
            fprintf(stderr, " %08X",
                    s_watchdog_trace[((*s_watchdog_trace_idx) + k) & 15]);
        fprintf(stderr, "\n");
    }
    /* Guest globals worth seeing at the moment of the hang.
     *
     * RECOMP_PEEK is otherwise only sampled by the pushbuffer reporter, which
     * a title that hangs before rendering never reaches -- and a spin that
     * makes no kernel calls is invisible to RECOMP_KERNEL_WATCH too. A pure
     * CPU loop polling a global is exactly the case neither of those covers.
     */
    xbox_PeekSample("peek");
    /* The pushbuffer pointers, unconditionally.
     *
     * "Extend the table as more handshakes turn up -- run the title and the
     * watchdog sample will name the register" is only true if the sample
     * actually shows them. It did not: a title spinning on a DMA pointer made
     * no kernel calls and no indirect calls, so every other line the watchdog
     * prints was identical between two samples taken 40 seconds apart, and the
     * register that was stuck did not appear at all.
     *
     * Both sides of the channel, because which one the title consults is a
     * property of its D3D and not of the hardware: Halo waits on the USER
     * pair, DDS9 reads USER first and falls back to PFIFO's DMA_SUBROUTINE.
     * Printing only the pair that some other title used is how this stayed
     * invisible. */
    if (g_nv2a_memory) {
        const char *r = (const char *)g_nv2a_memory;
#define WD_NV2A(off) (*(const volatile uint32_t *)(r + (off)))
        fprintf(stderr, "  NV2A USER  PUT=%08X GET=%08X\n"
                        "  NV2A PFIFO PUT=%08X GET=%08X REF=%08X SUBR=%08X\n",
                WD_NV2A(NV2A_USER_DMA_PUT), WD_NV2A(NV2A_USER_DMA_GET),
                WD_NV2A(NV2A_PFIFO_DMA_PUT), WD_NV2A(NV2A_PFIFO_DMA_GET),
                WD_NV2A(NV2A_PFIFO_REF), WD_NV2A(NV2A_PFIFO_DMA_SUBROUTINE));
#undef WD_NV2A
    }

    for (i = 0; i < 400 && esp; i++) {
        uint32_t a = esp + i * 4;
        if (a < XBOX_STACK_BASE || a >= XBOX_STACK_TOP) break;
        fprintf(stderr, "    GS %08X %08X\n", a,
                *(const uint32_t *)(mem + a));
    }
    fflush(stderr);
    _exit(3);
    return 0;
}

void xbox_WatchdogStart(void)
{
    const char *secs = getenv("RECOMP_WATCHDOG_SECS");
    HANDLE h;

    if (!secs || !*secs)
        return;
    s_watchdog_secs = (unsigned)atoi(secs);
    if (!s_watchdog_secs)
        return;

    /* Taken on the guest thread: g_esp is thread-local, so the watchdog has to
     * be handed the address of the one that matters rather than reading its
     * own, which is always zero. */
    s_watchdog_esp = &g_esp;
    s_watchdog_trace = g_icall_trace;
    s_watchdog_trace_idx = &g_icall_trace_idx;
    s_watchdog_count = &g_icall_count;
    s_watchdog_regs[0] = &g_eax; s_watchdog_regs[1] = &g_ecx;
    s_watchdog_regs[2] = &g_edx; s_watchdog_regs[3] = &g_ebx;
    s_watchdog_regs[4] = &g_esi; s_watchdog_regs[5] = &g_edi;
    h = CreateThread(NULL, 0, xbox_watchdog_thread, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
}

/* SSE. 128 bits of architectural state, per-thread like the rest. */
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
/* Last frame established by `mov ebp, esp`. Read by frameless functions
 * that address their caller's frame through ebp. */
RECOMP_TLS uint32_t g_ebp = 0;

/* EFLAGS.DF. Zero means the string instructions walk forwards, which is the
 * ABI's resting state and what almost every one of them does -- so this is
 * almost always 0 and costs a predictable branch. The exceptions are the ones
 * that matter: MSVC's strrchr/wcsrchr scan backwards from the terminator with
 * `std; repne scasb`, and memmove goes backwards when its regions overlap the
 * wrong way. Thread-local, because `std` and the `cld` that undoes it can land
 * in different lifted bodies of the same guest routine. */
RECOMP_TLS int g_df = 0;

/* ICALL trace ring buffer */
RECOMP_TLS volatile uint32_t g_icall_trace[16] = {0};
RECOMP_TLS volatile uint32_t g_icall_trace_idx = 0;
RECOMP_TLS volatile uint64_t g_icall_count = 0;

BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size)
{
    g_force_return = getenv("RECOMP_FORCE_RETURN") != NULL;
    DWORD old_protect;
    const uint8_t *xbe = (const uint8_t *)xbe_data;

    if (g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: already initialized\n");
        return FALSE;
    }

    /*
     * Calculate the full range we need to map.
     * From XBOX_MAP_START (0x0) to the end of the furthest section.
     * This includes low memory (KPCR at 0x0-0xFF) which game code reads
     * from, the XBE sections, and the simulated stack.
     */
    /* Map the full Xbox address space (covers all sections + stack + heap).
     * Size is runtime-configurable: retail 64 MB, devkit debug builds 128 MB. */
    /* The mapped range, which is not necessarily RAM. Mirrors are placed
     * at multiples of this, so growing it is what stops a title's
     * above-RAM allocations from aliasing low memory. */
    g_memory_size = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;

    /*
     * Create a file mapping backed by the page file.
     *
     * Using file mapping instead of VirtualAlloc allows us to map the same
     * physical pages at multiple virtual addresses via MapViewOfFileEx.
     * This is critical for the Xbox RAM mirror: the Xbox memory controller
     * uses a 26-bit address bus, so ALL addresses wrap modulo 64 MB.
     * Code that writes to address 0x20000448 is really writing to 0x00000448.
     * With file mapping views, we create aliased mappings at 64 MB intervals
     * that all point to the same physical memory.
     */
    g_mapping_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,   /* page file backed */
        NULL,                   /* default security */
        PAGE_READWRITE,         /* read-write access */
        0,                      /* high DWORD of size */
        (DWORD)g_memory_size,   /* low DWORD of size (64 MB) */
        NULL                    /* unnamed mapping */
    );
    if (!g_mapping_handle) {
        fprintf(stderr, "xbox_MemoryLayoutInit: CreateFileMapping failed (error %lu)\n",
                GetLastError());
        return FALSE;
    }

    /*
     * Map the base view at the desired virtual address.
     * Try the original Xbox base address first. If that fails (common on
     * Windows 11 where low addresses are often reserved), try page-aligned
     * addresses upward until we find a free region.
     */
    {
        static const uintptr_t try_bases[] = {
            XBOX_BASE_ADDRESS,      /* 0x00010000 - original Xbox address */
            0x00800000,             /* 8 MB - above typical PEB/TEB region */
            0x01000000,             /* 16 MB */
            0x02000000,             /* 32 MB */
            0x10000000,             /* 256 MB */
            0,                      /* sentinel - let OS choose */
        };

        /* Iterate the whole array, sentinel included. The old condition
         * (try_bases[i] != 0 || i == 0) stopped *at* the zero rather than
         * using it, so the "let the OS choose" fallback never ran: the loop
         * tried the fixed addresses and gave up. Invisible on Windows, where
         * one of the low bases succeeds -- fatal on arm64 macOS, where all of
         * them sit inside the 4 GB __PAGEZERO segment and none can. */
        /* Reserve base + mirrors as one range, and map the base at its head.
         * VirtualFree releases just the slice about to be used, so each view
         * replaces our own reservation rather than racing for free space. */
        g_span_size = g_memory_size * (size_t)(1 + XBOX_NUM_MIRRORS);
        g_span_base = VirtualAlloc(NULL, g_span_size, MEM_RESERVE, PAGE_NOACCESS);
        if (g_span_base) {
            VirtualFree(g_span_base, g_memory_size, MEM_RELEASE);
            g_memory_base = MapViewOfFileEx(g_mapping_handle,
                                            FILE_MAP_ALL_ACCESS, 0, 0,
                                            g_memory_size, g_span_base);
            if (!g_memory_base) {
                VirtualFree(g_span_base, g_span_size, MEM_RELEASE);
                g_span_base = NULL;
                g_span_size = 0;
            }
        }

        const size_t n_bases = sizeof(try_bases) / sizeof(try_bases[0]);
        for (size_t i = 0; !g_memory_base && i < n_bases; i++) {
            LPVOID hint = try_bases[i] ? (LPVOID)try_bases[i] : NULL;
            g_memory_base = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,           /* offset into mapping */
                g_memory_size,  /* size */
                hint            /* desired base address */
            );
            if (g_memory_base) {
                if (try_bases[i] != 0 && (uintptr_t)g_memory_base != try_bases[i]) {
                    /* OS gave us a different address, retry */
                    UnmapViewOfFile(g_memory_base);
                    g_memory_base = NULL;
                    continue;
                }
                break;
            }
        }
    }

    if (!g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: failed to map base view (%zu KB)\n",
                g_memory_size / 1024);
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
        return FALSE;
    }

    g_memory_offset = (uintptr_t)g_memory_base - XBOX_MAP_START;

    /* Guest page zero: no access.
     *
     * Nothing legitimate lives there -- every XBE's image base is 0x00010000
     * and the TIB now sits at XBOX_FS_BASE -- so any access is a null pointer
     * the title dereferenced. Left readable it did quiet damage: a null check
     * of the form `cmp byte [ecx], 0` read whatever happened to be at 0 and
     * decided the pointer was fine, and a store through a null pointer landed
     * on real memory and surfaced as corruption somewhere unrelated. Faulting
     * here turns both into one access violation at the instruction that made
     * the mistake, which the crash handler can name.
     *
     * Opt-in through RECOMP_TRAP_NULL, because it converts a class of bug the
     * title currently survives into a hard stop: a guest that dereferences null
     * and ignores the result keeps running while page zero reads as zero, and
     * stops dead once it faults. That is the right default for hunting one of
     * these and the wrong one for making progress past the rest, so it is a
     * switch rather than a policy.
     *
     * Note this is separate from moving the TIB off page zero, which is not
     * optional: with the TIB gone, address 0 reads as plain zero, so a null
     * check written as a load through the pointer now gets the answer it
     * expects whether or not the page is trapped.
     *
     * Best-effort: failing to protect it costs only the diagnostic. */
    if (XBOX_MAP_START == 0 && getenv("RECOMP_TRAP_NULL")) {
        DWORD old_protect;
        /* Protection is applied at host page granularity, and the host page
         * is not always the guest's 4 KB -- Apple Silicon uses 16 KB, so this
         * 0x1000 request actually covers guest 0..0x3FFF. That is why
         * XBOX_TIB_MAIN sits at 0x4000: the widest page any supported host
         * uses fits below the TIB, so the rounding costs nothing and the
         * guard installs everywhere.
         *
         * The check below is what remains of an earlier bug rather than dead
         * code. With the TIB at 0x1000 the rounding reached it, init wrote the
         * TIB moments later, and every run that asked for the guard died at
         * startup -- so the guard disabled itself on all of Apple Silicon and
         * the diagnostic silently did nothing. It stays as a floor for a host
         * with pages wider than the TIB offset, where skipping really is
         * better than breaking the run. */
#if defined(_WIN32)
        SYSTEM_INFO si;
        long host_page;
        GetSystemInfo(&si);
        host_page = (long)si.dwPageSize;
#else
        long host_page = sysconf(_SC_PAGESIZE);
#endif
        if (host_page > 0 && (uint32_t)host_page > XBOX_TIB_MAIN) {
            fprintf(stderr, "  RECOMP_TRAP_NULL: not available -- the host page "
                    "is %ld bytes, so trapping guest page zero would also trap "
                    "the TIB at 0x%08X\n", host_page, XBOX_TIB_MAIN);
        } else {
            if (VirtualProtect(g_memory_base, 0x1000, PAGE_NOACCESS, &old_protect)) {
                fprintf(stderr, "  guest page 0 is PAGE_NOACCESS"
                                " (null dereferences fault)\n");
            }
        }
    }

    if (g_memory_offset == 0) {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%08X (original Xbox address)\n",
                g_memory_size / 1024, XBOX_MAP_START);
    } else {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%p (offset %+td from Xbox base)\n",
                g_memory_size / 1024, g_memory_base, g_memory_offset);
    }

    /*
     * Helper macro: convert Xbox VA to actual mapped address.
     * When g_memory_offset == 0 (ideal case), this is identity.
     */
    #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))

    /*
     * Copy XBE header to base address.
     * The Xbox kernel maps the XBE image header at 0x00010000.
     * Game code reads kernel thunk table, certificate data, and
     * section info from this region.
     */
    {
        /* XBE header size is at file offset 0x0108 (SizeOfImageHeader) */
        DWORD header_size = 0;
        if (xbe_size >= 0x10C) {
            header_size = *(const DWORD *)(xbe + 0x0108);
        }
        if (header_size == 0 || header_size > 0x10000)
            header_size = 0x1000;  /* fallback: 4KB */
        if (header_size > xbe_size)
            header_size = (DWORD)xbe_size;
        memcpy(XBOX_VA(XBOX_BASE_ADDRESS), xbe, header_size);
        fprintf(stderr, "  XBE header: %u bytes at %p (Xbox VA 0x%08X)\n",
                header_size, XBOX_VA(XBOX_BASE_ADDRESS), XBOX_BASE_ADDRESS);
    }

    /*
     * Dynamically load ALL XBE sections by parsing the section headers.
     *
     * This replaces the old approach of hardcoding section addresses for
     * a specific game (Burnout 3). By reading the section table from the
     * XBE header, any game's sections are loaded automatically.
     *
     * Every section is copied to its original Xbox VA:
     * - .text: needed because memory walkers may scan code pages
     * - .rdata: constants, vtables, kernel thunk table
     * - .data: global variables (initialized portion from XBE, BSS zeroed)
     * - XDK library sections (D3D, DSOUND, WMADEC, XPP, etc.)
     * - DOLBY, BINK, XTIMAGE, etc.
     */
    {
        DWORD base_addr = *(const DWORD *)(xbe + XBE_BASE_ADDR_OFFSET);
        DWORD num_sections = *(const DWORD *)(xbe + XBE_SECTION_COUNT_OFFSET);
        DWORD sect_headers_va = *(const DWORD *)(xbe + XBE_SECTION_HEADERS_OFFSET);
        DWORD sect_headers_off = sect_headers_va - base_addr;
        int sections_loaded = 0;
        int sections_short = 0;
        size_t total_bytes = 0;

        if (num_sections > 64) num_sections = 64;  /* sanity cap */

        fprintf(stderr, "  XBE sections: %u (headers at file offset 0x%08X)\n",
                num_sections, sect_headers_off);

        for (DWORD si = 0; si < num_sections; si++) {
            if (sect_headers_off + (si + 1) * SECTHDR_SIZE > xbe_size) break;

            const uint8_t *sh = xbe + sect_headers_off + si * SECTHDR_SIZE;
            DWORD sec_va       = *(const DWORD *)(sh + SECTHDR_VA);
            DWORD sec_vsize    = *(const DWORD *)(sh + SECTHDR_VSIZE);
            DWORD sec_raw_off  = *(const DWORD *)(sh + SECTHDR_RAW_OFFSET);
            DWORD sec_raw_size = *(const DWORD *)(sh + SECTHDR_RAW_SIZE);
            DWORD sec_name_va  = *(const DWORD *)(sh + SECTHDR_NAME_ADDR);

            /* Read section name from XBE header */
            const char *sec_name = "?";
            DWORD name_off = sec_name_va - base_addr;
            if (name_off < xbe_size && name_off + 8 <= xbe_size)
                sec_name = (const char *)(xbe + name_off);

            /* Validate: section must fit within our 64MB mapped region */
            if (sec_va < XBOX_BASE_ADDRESS || sec_va + sec_vsize > XBOX_TOTAL_RAM)
                continue;

            /* Determine copy size (raw_size may exceed vsize due to alignment) */
            DWORD copy_size = (sec_raw_size < sec_vsize) ? sec_raw_size : sec_vsize;

            /* Zero the full virtual size first (handles BSS) */
            memset(XBOX_VA(sec_va), 0, sec_vsize);

            /*
             * Copy initialized data from XBE.
             *
             * A section whose raw data runs past the end of the buffer is a
             * truncated or corrupt image, not a BSS section, and it must not
             * be counted among the sections loaded. Reporting it as loaded is
             * how a 4MB title read through a 1MB buffer produced "Loaded
             * 17/17 sections" with every byte of every section still zero --
             * including the kernel thunk table, which then resolved 0 imports
             * and looked like a title that calls no kernel functions.
             */
            int have_data = (copy_size == 0) ||
                            (sec_raw_off + copy_size <= xbe_size);
            if (copy_size > 0 && have_data) {
                memcpy(XBOX_VA(sec_va), xbe + sec_raw_off, copy_size);
            } else if (!have_data) {
                fprintf(stderr,
                        "  WARNING: section %u (%s) raw data 0x%08X+%u runs past "
                        "the %zu-byte image -- left zeroed\n",
                        si, sec_name, sec_raw_off, copy_size, xbe_size);
                sections_short++;
            }

            /* Every loaded section, executable or not. Anything that writes
             * guest memory from outside the title -- the pushbuffer executor
             * clearing a surface, say -- needs to know where the title itself
             * lives, because scribbling on it is not a rendering artefact, it
             * is the title's code and globals gone. */
            if (!g_xbox_image_lo || sec_va < g_xbox_image_lo)
                g_xbox_image_lo = sec_va;
            if (sec_va + sec_vsize > g_xbox_image_hi)
                g_xbox_image_hi = sec_va + sec_vsize;

            /* Executable sections define the range indirect calls may target.
             * XBE section flag 0x04 is EXECUTABLE. */
            if (*(const DWORD *)(sh + SECTHDR_FLAGS) & 0x00000004u) {
                if (!g_xbox_code_lo || sec_va < g_xbox_code_lo)
                    g_xbox_code_lo = sec_va;
                if (sec_va + sec_vsize > g_xbox_code_hi)
                    g_xbox_code_hi = sec_va + sec_vsize;
            }

            if (have_data) {
                sections_loaded++;
                total_bytes += copy_size;
            }

            fprintf(stderr, "  [%2u] %-12s VA=0x%08X vsize=%-8u raw=0x%08X rsize=%-8u%s\n",
                    si, sec_name, sec_va, sec_vsize, sec_raw_off, sec_raw_size,
                    (sec_raw_size < sec_vsize) ? " (BSS)" : "");
        }

        fprintf(stderr, "  Loaded %d/%u sections (%zu bytes total)\n",
                sections_loaded, num_sections, total_bytes);
        if (sections_short) {
            fprintf(stderr,
                    "  ERROR: %d section(s) had no data in the image -- the XBE "
                    "is truncated or was read short; the title will not run\n",
                    sections_short);
        }
    }

    /*
     * Parse the kernel thunk table address from the XBE header.
     * The XBE stores KernelImageThunkAddress at offset 0x0158, XOR-encrypted.
     * The key differs between retail and debug XBEs, and there is no flag
     * saying which was used -- decode with both and keep whichever lands in
     * the mapped address range (this is what tools/xbe_parser does).
     *
     * Debug XBEs are not an edge case here: they are the builds most worth
     * recompiling, since they still carry assert strings and symbols. Halo's
     * cachebeta.xbe is one, and assuming the retail key decoded its thunk
     * table to 0xB4F98174 instead of 0x00253090, which silently fell back to
     * the compile-time default and resolved 0 of 378 kernel imports.
     */
    if (xbe_size >= 0x015C) {
        uint32_t thunk_raw = *(const uint32_t *)(xbe + 0x0158);
        uint32_t thunk_retail = thunk_raw ^ 0x5B6D40B6;  /* retail XOR key */
        uint32_t thunk_debug  = thunk_raw ^ 0xEFB1F152;  /* debug XOR key  */
        uint32_t thunk_va;

        if (thunk_retail >= XBOX_BASE_ADDRESS && thunk_retail < XBOX_TOTAL_RAM) {
            thunk_va = thunk_retail;
        } else {
            thunk_va = thunk_debug;
        }

        /* Validate: thunk VA should be within our mapped region */
        if (thunk_va >= XBOX_BASE_ADDRESS && thunk_va < XBOX_TOTAL_RAM) {
            /* Count thunk entries by scanning until we hit 0 */
            uint32_t thunk_count = 0;
            /* XBOX_KERNEL_THUNK_TABLE_SIZE, not 366: the kernel exports 378
             * slots, and kernel.h notes 366 is short by 12. A title importing
             * a high ordinal would have had its table truncated here. */
            for (uint32_t t = 0; t < XBOX_KERNEL_THUNK_TABLE_SIZE; t++) {
                uint32_t entry = *(volatile uint32_t *)((uintptr_t)(thunk_va + t * 4) + g_memory_offset);
                if (entry == 0) break;
                thunk_count++;
            }
            xbox_kernel_set_thunk_address(thunk_va, thunk_count);
            fprintf(stderr, "  Kernel thunks: %u entries at Xbox VA 0x%08X\n",
                    thunk_count, thunk_va);
        } else {
            fprintf(stderr, "  WARNING: kernel thunk VA 0x%08X out of range (raw=0x%08X)\n",
                    thunk_va, thunk_raw);
        }
    }

    /*
     * NOTE: .rdata is NOT set read-only.
     * VirtualProtect rounds to page boundaries, and the .rdata end (0x003B2454)
     * and .data start (0x003B2360) share the same 4KB page (0x003B2000-0x003B2FFF).
     * Making .rdata read-only also makes the first ~0xCA0 bytes of .data read-only,
     * which causes game initialization code to fault when writing to .data globals
     * in that overlap range.
     */
    (void)old_protect;

    #undef XBOX_VA

    /* Set the global offset for recompiled code MEM macros */
    g_xbox_mem_offset = g_memory_offset;

    /*
     * Initialize the Xbox stack for recompiled code.
     * The stack area lives at XBOX_STACK_BASE in Xbox address space.
     * g_esp is the global stack pointer shared by all translated functions.
     */
    g_esp = XBOX_STACK_TOP;
    fprintf(stderr, "  Stack: %u KB at Xbox VA 0x%08X (ESP = 0x%08X)\n",
            XBOX_STACK_SIZE / 1024, XBOX_STACK_BASE, g_esp);

    /*
     * Populate the fake Thread Information Block (TIB) at Xbox VA 0x0.
     *
     * The original Xbox code uses fs:[offset] to read per-thread data,
     * but the recompiler drops the fs: segment prefix and generates
     * MEM32(offset) instead. Since we mapped low memory (0x0-0xFFFF),
     * we populate the TIB fields that game code accesses:
     *
     *   fs:[0x00] = SEH exception list (-1 = end of chain)
     *   fs:[0x04] = stack base (top of stack)
     *   fs:[0x08] = stack limit (bottom of stack)
     *   fs:[0x18] = self pointer (TIB address)
     *   fs:[0x20] = KPCR Prcb pointer (→ fake structure)
     *   fs:[0x28] = TLS / RW engine context pointer
     *
     * We use free space in the BSS area for the fake structures.
     */
    {
        #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
        #define MEM32_INIT(va, val) (*(uint32_t *)XBOX_VA(va) = (uint32_t)(val))

        /* Fake TIB at address 0x0 */
        MEM32_INIT(XBOX_FS_BASE + 0x00, 0xFFFFFFFF);       /* SEH: end of chain */
        MEM32_INIT(XBOX_FS_BASE + 0x04, XBOX_STACK_TOP);   /* Stack base (high address) */
        MEM32_INIT(XBOX_FS_BASE + 0x08, XBOX_STACK_BASE);  /* Stack limit (low address) */
        MEM32_INIT(XBOX_FS_BASE + 0x18, XBOX_FS_BASE);     /* Self pointer */

        /*
         * fs:[0x20] - On Xbox KPCR, this is the Prcb pointer.
         * Game code reads [fs:[0x20] + 0x250] which on the real Xbox
         * accesses a D3D cache structure. We set it to 0 so the read
         * at offset 0x250 returns 0, causing the cache init to be skipped.
         */
        /* A zeroed block rather than a null pointer. The read is
         * [fs:[0x20] + 0x250], and this used to be left at 0 so that read
         * landed on guest address 0x250 and returned zero by accident -- which
         * only worked while page zero was mapped. Pointing at real zeroed
         * memory says the same thing to the title and survives that page being
         * unmapped, which is what makes a genuine null dereference visible. */
        #define FAKE_PRCB_VA 0x00761000  /* zeroed KPCR Prcb stand-in */
        memset(XBOX_VA(FAKE_PRCB_VA), 0, 0x400);
        MEM32_INIT(XBOX_FS_BASE + 0x20, FAKE_PRCB_VA);
        #undef FAKE_PRCB_VA

        /*
         * fs:[0x28] - Thread local storage / RW engine context.
         * The RW engine reads [fs:[0x28] + 0x28] to get a pointer
         * to its data area. We allocate a fake structure at 0x00760000
         * (in the BSS area) and a data buffer at 0x00700000.
         */
        #define FAKE_TLS_VA     0x00760000  /* Fake TLS structure (in BSS) */
        #define FAKE_RWDATA_VA  0x00700000  /* RW engine data area (in BSS) */

        MEM32_INIT(XBOX_FS_BASE + 0x28, FAKE_TLS_VA);
        /* TLS[0x28] = pointer to RW data area */
        MEM32_INIT(FAKE_TLS_VA + 0x28, FAKE_RWDATA_VA);

        /*
         * XBE TLS directory.
         *
         * An image with __declspec(thread) data carries one, and on hardware
         * the loader acts on it. Nothing here did, so thread-local access read
         * whatever memory happened to be under fs:[4].
         *
         * Xbox reaches thread-local data through NtTib.StackBase -- fs:[4] --
         * not Win32's fs:[0x2C], and the block sits BELOW that pointer: the
         * image's entry point computes its own index, negative, as
         * -(blocksize/4). Wreckless does this at guest 0x000EB57E and arrives
         * at -5 for its 20-byte block, so [fs:[4] + index*4] is the block's
         * first dword. The rounding below mirrors that arithmetic exactly,
         * because fs:[4] has to land where the title's own index says it is.
         *
         * The index itself is deliberately NOT written here: the title
         * computes and stores it. What the loader owes it is a block in the
         * right place.
         *
         * Slot 0 holds a pointer to per-thread data -- XAPI's SetLastError is
         * [[fs:[4] + index*4] + 4] = err -- so it gets a zeroed block rather
         * than being left NULL, which had SetLastError writing the error code
         * over fs:[4] itself and the next call faulting at guest 0xFFFFFFEF.
         *
         * ponytail: one block for the whole process, not one per thread.
         * Every guest thread therefore shares LastError. Give this a per-thread
         * allocation when a title is observed to care.
         */
        #define FAKE_TLS_BLOCK_VA  0x00770000  /* image TLS data          */
        #define FAKE_TLS_THREAD_VA 0x00770200  /* what slot 0 points at   */
        {
            DWORD tls_dir_va = *(const DWORD *)(xbe + XBE_TLS_ADDR_OFFSET);

            if (tls_dir_va) {
                const uint32_t *tls = (const uint32_t *)XBOX_VA(tls_dir_va);
                uint32_t data_start = tls[0];
                uint32_t data_end   = tls[1];
                uint32_t zero_fill  = tls[4];
                uint32_t init_size  = (data_end > data_start)
                                    ? data_end - data_start : 0;
                uint32_t total      = ((init_size + zero_fill + 0xF) & ~0xFu) + 4;

                memset(XBOX_VA(FAKE_TLS_BLOCK_VA), 0, total);
                memset(XBOX_VA(FAKE_TLS_THREAD_VA), 0, 64);
                if (init_size)
                    memcpy(XBOX_VA(FAKE_TLS_BLOCK_VA),
                           XBOX_VA(data_start), init_size);

                MEM32_INIT(FAKE_TLS_BLOCK_VA, FAKE_TLS_THREAD_VA);
                MEM32_INIT(XBOX_FS_BASE + 0x04, FAKE_TLS_BLOCK_VA + total);

                g_tls_template_va = FAKE_TLS_BLOCK_VA;
                g_tls_total       = total;

                fprintf(stderr, "  TLS: %u-byte block at 0x%08X,"
                        " fs:[4] = 0x%08X (index will be %d)\n",
                        total, FAKE_TLS_BLOCK_VA, FAKE_TLS_BLOCK_VA + total,
                        -(int)(total / 4));
            }
        }
        #undef FAKE_TLS_BLOCK_VA
        #undef FAKE_TLS_THREAD_VA

        fprintf(stderr, "  TIB: fake TIB at VA 0x%X, TLS at 0x%08X, RW data at 0x%08X\n",
                XBOX_FS_BASE, FAKE_TLS_VA, FAKE_RWDATA_VA);

        #undef FAKE_TLS_VA
        #undef FAKE_RWDATA_VA
        #undef MEM32_INIT
        #undef XBOX_VA
    }

    /*
     * Contiguous / physical memory window at 0x80000000.
     *
     * MmAllocateContiguousMemory hands back addresses in this window: physical
     * page P is visible at 0x80000000 + P. Titles that pin buffers at fixed
     * physical addresses then use the whole range, so it has to be backed for
     * its full length - Halo pins 3.4 MB at 0x61000 and 22 MB at 0x3A6000, and
     * with only the fake kernel page mapped here a write walked off the end of
     * it a few pages in.
     *
     * Deliberately NOT a view of the 64 MB RAM mapping. On hardware this window
     * aliases physical RAM, but we load the XBE image into the low addresses of
     * that same region, so aliasing would put a title's pinned pools on top of
     * its own code. Separate storage costs an extra mapping and behaves
     * correctly; nothing here depends on the aliasing.
     *
     * Reserved before the kernel page below, which lives inside it.
     */
    {
        uintptr_t contig_native = XBOX_CONTIG_BASE + g_memory_offset;
        g_contig_mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
            0, (DWORD)XBOX_CONTIG_SIZE, NULL);
        g_contig_memory = g_contig_mapping
            ? MapViewOfFileEx(g_contig_mapping, FILE_MAP_ALL_ACCESS,
                              0, 0, XBOX_CONTIG_SIZE, (LPVOID)contig_native)
            : NULL;
        if (!g_contig_memory)
            g_contig_memory = VirtualAlloc(
                (LPVOID)contig_native,
                XBOX_CONTIG_SIZE,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE
            );
        if (g_contig_memory) {
            fprintf(stderr, "  Contiguous window: %u MB at Xbox VA 0x%08X\n",
                    XBOX_CONTIG_SIZE / (1024 * 1024), XBOX_CONTIG_BASE);
        } else {
            fprintf(stderr, "  WARNING: contiguous window at 0x%08X failed "
                    "(error %lu); pinned physical allocations will fault\n",
                    XBOX_CONTIG_BASE, GetLastError());
        }
    }

    /*
     * NV2A hardware register aperture at 0xFD000000 (16 MB).
     *
     * The GPU's registers are memory-mapped here on real hardware. A title
     * that only calls D3D never notices, but the D3D8 library is linked into
     * the XBE rather than provided by the kernel, so once execution is inside
     * it the register pokes are just loads and stores in recompiled code.
     * Halo faults reading 0xFD001804 during rasterizer_preinitialize, a few
     * instructions after Direct3DCreate8 returns.
     *
     * Backed as ordinary zeroed RAM. That is enough to get through
     * initialisation, and reads returning zero are the benign answer for the
     * status and capability registers touched here.
     *
     * ponytail: plain memory, no register semantics. A spin loop waiting for
     * a bit to *set* would hang rather than fault -- if that shows up, the fix
     * is to bridge the D3D8 entry point that owns the loop, not to start
     * emulating NV2A. Nothing has needed that yet.
     */
    {
        uintptr_t nv2a_native = XBOX_NV2A_BASE + g_memory_offset;
        g_nv2a_memory = VirtualAlloc(
            (LPVOID)nv2a_native,
            XBOX_NV2A_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        /* The pushbuffer survey rides on the same poll, so either
         * variable arms it. */
        s_nv2a_trace = getenv("RECOMP_NV2A_TRACE") != NULL
                    || getenv("RECOMP_PB_SCAN") != NULL
                    || getenv("RECOMP_PB_EXEC") != NULL;
        if (g_nv2a_memory) {
            fprintf(stderr, "  NV2A register aperture: %u MB at Xbox VA "
                    "0x%08X (backing allocated; register owner pending)\n",
                    XBOX_NV2A_SIZE / (1024 * 1024), XBOX_NV2A_BASE);
        } else {
            fprintf(stderr, "  WARNING: NV2A aperture at 0x%08X failed "
                    "(error %lu); D3D register access will fault\n",
                    XBOX_NV2A_BASE, GetLastError());
        }
    }

    /*
     * MCPX device apertures.
     *
     * The NV2A block above is not the only hardware the title touches
     * directly. The southbridge devices live higher up:
     *
     *   0xFE800000  APU (audio processing unit)
     *   0xFEC00000  AC97
     *   0xFED00000  USB0 / USB1
     *   0xFEF00000  NIC
     *
     * Halo faults reading 0xFED00000 during input initialisation -- the XDK's
     * USB code talks to the host controller's registers rather than going
     * through a driver. Back the whole span as plain RAM for the same reason
     * the NV2A aperture is backed: a read of zero is survivable, a fault is
     * not.
     *
     * ponytail: no register semantics anywhere in here. If something spins
     * waiting for a bit to set, extend the NV2A ack thread's table rather than
     * emulating the device.
     */
    {
        uintptr_t mcpx_native = XBOX_MCPX_BASE + g_memory_offset;
        g_mcpx_memory = VirtualAlloc(
            (LPVOID)mcpx_native,
            XBOX_MCPX_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        g_mcpx_regs = g_mcpx_memory;
        if (g_mcpx_memory) {
            /* AC'97 codec ready -- modelled, always on.
             *
             * DirectSound resets the codec by setting bit 1 of 0xFEC0012C and
             * then polls 0xFEC00130 for bit 8 a thousand times waiting for the
             * codec to come up. On zeroed registers that bit never appears, so
             * the wait times out and DirectSoundCreate returns DSERR_NODRIVER
             * (0x88780078).
             *
             * That failure is not confined to audio. Wreckless initialises its
             * whole engine object behind `if (DirectSoundCreate() >= 0)`, so a
             * failed create skips the initialisation, leaves the object's table
             * pointer null, and the null propagates: a null-derived divisor
             * produces a NaN transform matrix, which produces a garbage index,
             * which crashes. Reporting the codec as present is what lets the
             * engine initialise at all.
             *
             * THE RULE, and it is a MODEL rather than an assertion:
             *
             *     GS(0xFEC00130).bit8 := GC(0xFEC0012C).bit1
             *
             * Bit 1 of GC is ICH_AC97COLD, the ACTIVE-LOW Cold Reset# line:
             * writing 1 RELEASES the codec from reset, and 0 holds it in reset.
             * So bit 8 of GS -- "primary (AC_SDIN0) codec ready" -- is set
             * exactly when bit 1 of GC is set, and cleared when it is clear.
             * Evaluated level-triggered on every tick of this worker, so it
             * tracks the guest's own reset writes rather than latching once.
             *
             * Polarity, cited (Linux v6.6, sound/pci/intel8x0.c, SHA-256
             * F5F1AE46661C848CCD29C2B1368DB38A159EC8650209E54FB2974996F50B5FFC).
             * The identifiers below are quoted WITHOUT their hash sign so this
             * comment cannot be mistaken for preprocessor directives:
             *   L140  "define ICH_AC97COLD 0x00000002"  -- AC'97 cold reset
             *   L163  "define ICH_PCR 0x00000100"       -- primary (AC_SDIN0) codec ready
             *   L2360-2361 in snd_intel8x0_ich_chip_reset is decisive:
             *         finish cold or do warm reset
             *         cnt |= (cnt and ICH_AC97COLD) == 0 ? ICH_AC97COLD : ICH_AC97WARM;
             *     -- when bit 1 reads 0 the driver SETS it to finish the reset,
             *     so 0 = reset in progress and 1 = released/running.
             *   L2295-2298 (error path) clears the bit "for the next chance",
             *     i.e. clearing RE-ARMS the reset.
             *
             * Corroborated by xemu, the reference Xbox emulator: hw/audio/ac97.c
             * (commit 2799183ecc5119269be01c340d0c9465dbb04d02) defines
             * GS_S0CR (1 << 8) as read-only and returns `glob_sta | GS_S0CR`
             * unconditionally (L785); hw/xbox/mcpx/aci.c (commit
             * 704ece9ac661f325aa51bb0b28d326063633227b) maps NAM at +0x0 and
             * NABM at +0x100, which puts GLOB_STA at 0xFEC00130, and
             * hw/xbox/xbox.c:334 instantiates that device.
             *
             * ADMITTED under docs/jsrf-run-profiles.md section
             * "Unconditional modeled hardware causes" (commit
             * 73eee970a2d3e22a4301879e00d0125d27ee0498), class 1: device state
             * from modeled prior state. The evidence is the secondary-source
             * path -- xemu plus Linux intel8x0, two independent sources of
             * different provenance, at least one Xbox/MCPX-specific -- and no
             * public MCPX/ACI datasheet exists. That limitation is recorded
             * rather than papered over: deriving GS.bit8 from GC.bit1 is a
             * STATED MODELLING ASSUMPTION WITH A NAMED FALSIFIER (write GC bit 1
             * clear and read GS bit 8), not a documented device behaviour, and
             * the falsifier cannot occur in this title.
             *
             * WHAT IS NOT MODELLED, deliberately: W1C on this register. The
             * second consumer, sub_001A71B3, does a full-dword read-modify-write
             * of 0xFEC00130 and would clear bit 8 if it ran -- but it is only
             * reachable from the vector-6 ISR, which this runtime never raises,
             * so its write-back path is unreachable while only bit 8 is set.
             *
             * The register sits ABOVE the APU's 512K window
             * (src/apu/README.md:73 scopes the VEH-hooked window to
             * 0xFE800000-0xFE87FFFF), so this is plain memory here.
             *
             * This replaces an environment-gated override: the answer is now a
             * modelled cause rather than a shortcut, and it is unconditional.
             * The register constants live at file scope, beside MCPX_COUNTERS,
             * because the worker that uses them is defined above. */
            /* The APU's own 512K can be unmapped so its register traffic can be
             * routed to the emulated APU, which is the half that answers the
             * DSP handshake. Only the APU's 512K is unmapped: AC'97 above it
             * stays plain memory, which is what the codec-ready bit needs.
             *
             * RECOMP_APU_TRAP is now independent of the codec model. They used
             * to share a variable, so asking for the codec-ready bit also
             * unmapped the APU -- and nothing in either repository ever called
             * apu_hook_handle_mmio, so the first APU register access faulted and
             * killed the process. JSRF reaches 0xFE811100 within two seconds
             * under that combined switch for exactly that reason
             * (logs/runs/20260922-055053-100-p4-ac97). The codec bit is now
             * modelled unconditionally and does NOT unmap anything, so this
             * trap is only asked for when a handler is wired. */
            if (getenv("RECOMP_APU_TRAP")) {
                DWORD old_protect;
                if (VirtualProtect((char *)g_mcpx_memory, 0x00080000u,
                                   PAGE_NOACCESS, &old_protect))
                    g_apu_mmio_trapped = 1;
                if (g_apu_mmio_trapped)
                    fprintf(stderr, "  APU: 0x%08X..0x%08X trapped for MMIO\n",
                            XBOX_MCPX_BASE, XBOX_MCPX_BASE + 0x00080000u);
                else
                    fprintf(stderr, "  APU: trap requested but VirtualProtect"
                                    " failed (error %lu); registers stay"
                                    " plain memory\n", GetLastError());
            }
            fprintf(stderr, "  MCPX device aperture: %u MB at Xbox VA "
                    "0x%08X (APU/AC97/USB/NIC, zeroed)\n",
                    XBOX_MCPX_SIZE / (1024 * 1024), XBOX_MCPX_BASE);
        } else {
            fprintf(stderr, "  WARNING: MCPX aperture at 0x%08X failed "
                    "(error %lu); USB/audio register access will fault\n",
                    XBOX_MCPX_BASE, GetLastError());
        }
    }

    /* Flash ROM aperture -- see XBOX_FLASH_BASE for why. */
    {
        uintptr_t flash_native = XBOX_FLASH_BASE + g_memory_offset;

        g_flash_memory = VirtualAlloc(
            (LPVOID)flash_native,
            XBOX_FLASH_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        if (g_flash_memory) {
            fprintf(stderr, "  Flash ROM aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, not a real BIOS image)\n",
                    XBOX_FLASH_SIZE / (1024 * 1024), XBOX_FLASH_BASE);
        } else {
            fprintf(stderr, "  WARNING: flash aperture at 0x%08X failed "
                    "(error %lu); a title reading flash will fault\n",
                    XBOX_FLASH_BASE, GetLastError());
        }
    }

    if (g_nv2a_memory) {
        xbox_Nv2aAckStart();
    }

    /*
     * Allocate a page at Xbox kernel address space (0x80010000).
     *
     * RenderWare's Xbox driver code (xbcache.c) reads MEM32(0x8001003C)
     * to parse the Xbox kernel's PE header and find the INIT section for
     * CPU cache line sizing. On PC, we provide a minimal fake PE header
     * with 0 sections so the function gracefully skips the cache init.
     *
     * The actual native address is 0x80010000 + g_memory_offset.
     */
    {
        #define XBOX_KERNEL_BASE 0x80010000u
        #define KERNEL_PAGE_SIZE 4096
        uintptr_t kernel_native = XBOX_KERNEL_BASE + g_memory_offset;
        /* Already committed if the contiguous window above succeeded -
         * 0x80010000 sits inside it - so just use that storage. */
        g_kernel_memory = g_contig_memory
            ? (void *)kernel_native
            : VirtualAlloc((LPVOID)kernel_native, KERNEL_PAGE_SIZE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (g_kernel_memory) {
            /* Zero-fill then set e_lfanew = 0x80 (offset to PE header).
             * With the rest zeroed, NumberOfSections = 0 and the INIT
             * section search finds nothing, which is the safe path. */
            memset(g_kernel_memory, 0, KERNEL_PAGE_SIZE);
            *(uint32_t *)((uint8_t *)g_kernel_memory + 0x3C) = 0x80;  /* e_lfanew */
            fprintf(stderr, "  Kernel: fake PE header at Xbox VA 0x%08X (native %p)\n",
                    XBOX_KERNEL_BASE, g_kernel_memory);
        } else {
            fprintf(stderr, "  WARNING: could not map Xbox kernel VA 0x%08X\n",
                    XBOX_KERNEL_BASE);
        }
        #undef XBOX_KERNEL_BASE
        #undef KERNEL_PAGE_SIZE
    }

    /* Initialize the dynamic heap. */
    fprintf(stderr, "  Heap: %u MB at Xbox VA 0x%08X-0x%08X\n",
            (unsigned)((XBOX_HEAP_TOP - XBOX_HEAP_BASE) / (1024 * 1024)),
            XBOX_HEAP_BASE, XBOX_HEAP_TOP);

    /*
     * Map mirror views of the 64 MB region.
     *
     * On retail Xbox, physical RAM wraps at 64 MB due to the 26-bit
     * address bus. Address 0x04070000 reads the same data as 0x00070000.
     * The RenderWare engine's memory walker crosses 64 MB and accesses
     * mirrored data for an extended walk covering 256+ MB of virtual
     * addresses. Game init code also writes large data structures past
     * 64 MB that on real hardware wrap into physical RAM.
     *
     * We map additional views of the SAME file mapping section at 64 MB
     * intervals. All views alias the same physical pages, so reads and
     * writes at any mirror address correctly access the base data.
     */
    {
        int mirrors_ok = 0;
        /* The tiled aperture is a specific architectural alias -- physical RAM
         * a second time at 0xF0000000, which is where titles render -- while
         * these mirrors are a generic emulation of the address wrap. When the
         * mapped size is large enough that a mirror would cover 0xF0000000,
         * the mirror wins the address and the tiled mapping fails with
         * ERROR_INVALID_ADDRESS; Half-Life 2 then faults on its first surface
         * write. The specific alias is worth more than one wrap mirror, so
         * skip any that would overlap it.
         *
         * Guest addresses, not host: mirror m covers guest
         * (m + 1) * g_memory_size. */
        uint64_t tiled_lo = XBOX_TILED_BASE;
        uint64_t tiled_hi = tiled_lo + xbox_TiledApertureSize();

        for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
            uintptr_t mirror_base = (uintptr_t)g_memory_base +
                                    (uintptr_t)(m + 1) * g_memory_size;
            uint64_t guest_lo = (uint64_t)(m + 1) * g_memory_size;
            uint64_t guest_hi = guest_lo + g_memory_size;

            if (guest_lo < tiled_hi && tiled_lo < guest_hi) {
                fprintf(stderr, "  Mirror %d: skipped, overlaps the tiled"
                                " aperture at 0x%08X\n",
                        m + 1, (unsigned)XBOX_TILED_BASE);
                continue;
            }
            /* Inside the reservation this hands back the slice we are about
             * to use; outside it (no reservation) this is a no-op on an
             * address we never held. */
            if (g_span_base)
                VirtualFree((LPVOID)mirror_base, g_memory_size, MEM_RELEASE);
            g_mirror_views[m] = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                g_memory_size,
                (LPVOID)mirror_base
            );
            if (g_mirror_views[m]) {
                mirrors_ok++;
            } else {
                fprintf(stderr, "  Mirror %d: FAILED at %p (error %lu)\n",
                        m + 1, (void *)mirror_base, GetLastError());
            }
        }
        fprintf(stderr, "  RAM mirror: %d/%d views mapped (covers %d MB)\n",
                mirrors_ok, XBOX_NUM_MIRRORS,
                (int)((mirrors_ok + 1) * g_memory_size / (1024 * 1024)));
    }

    /*
     * Tiled / write-combined aperture at 0xF0000000.
     *
     * The NV2A exposes physical RAM a second time here and titles render
     * through it. Wreckless's first surface write goes to guest 0xF1954000 --
     * the tiled alias of physical 0x01954000, already inside our RAM -- and
     * faulted because nothing was mapped there.
     *
     * A view of the same section rather than fresh storage: the title writes a
     * surface through the tiled address and reads it back through the normal
     * one, so the two have to be the same bytes. That is the whole reason the
     * RAM lives in a file mapping.
     */
    {
        uintptr_t tiled_native = XBOX_TILED_BASE + g_memory_offset;
        size_t tiled_size = xbox_TiledApertureSize();
        /* A view of the CONTIGUOUS window, not of RAM.
         *
         * On hardware all three -- physical P, 0x80000000+P and 0xF0000000+P
         * -- are one and the same memory. Here they cannot be: the XBE image
         * is loaded at its own VA in the RAM mapping, so aliasing the
         * contiguous window onto RAM would drop a title's pinned physical
         * pools on top of its own code (Halo pins 3.4 MB at 0x61000, which is
         * inside its image). The contiguous window therefore has separate
         * storage, and the question becomes which of the two the tiled
         * aperture should be a view of.
         *
         * It is the contiguous one. A tiled address is a GPU surface address
         * by construction, and GPU surfaces come from
         * MmAllocateContiguousMemory -- so the pairing that has to hold is
         * tiled to contiguous. Against RAM instead, Half-Life 2's loader wrote
         * every decoded video frame through 0xF1C63000 while D3D sampled the
         * texture at 0x81C63000, and the sampler read zeros: 1.8 billion black
         * pixels rasterised, perfectly, from an empty texture.
         */
        if (tiled_size > XBOX_CONTIG_SIZE)
            tiled_size = XBOX_CONTIG_SIZE;
        g_tiled_view = g_contig_mapping
            ? MapViewOfFileEx(
                g_contig_mapping,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                tiled_size,
                (LPVOID)tiled_native)
            : NULL;
        if (g_tiled_view) {
            /* Prove the alias rather than assert it. Everything the title
             * renders goes through this window and is read back through the
             * physical address, so if the two are not the same bytes the GPU
             * sees empty buffers and the screen stays black -- with nothing
             * anywhere to say why. One write and one read turns that into a
             * startup line. */
            {
                volatile uint32_t *via_tiled =
                    (volatile uint32_t *)((uintptr_t)(XBOX_TILED_BASE + 0x1000)
                                          + g_memory_offset);
                volatile uint32_t *via_contig =
                    (volatile uint32_t *)((uintptr_t)(XBOX_CONTIG_BASE + 0x1000)
                                          + g_memory_offset);
                uint32_t saved = *via_contig;

                *via_tiled = 0xA5C30F17u;
                if (*via_contig != 0xA5C30F17u)
                    fprintf(stderr, "  WARNING: tiled aperture does NOT alias"
                            " the contiguous window (wrote A5C30F17, read"
                            " %08X) -- the GPU will sample empty textures\n",
                            *via_contig);
                else
                    fprintf(stderr, "  Tiled aperture alias verified"
                            " (tiled 0x%08X == contiguous 0x%08X)\n",
                            XBOX_TILED_BASE, XBOX_CONTIG_BASE);
                *via_contig = saved;
            }
            fprintf(stderr, "  Tiled aperture: %u MB at Xbox VA 0x%08X"
                    " (aliases the contiguous window)\n",
                    (unsigned)(g_memory_size / (1024 * 1024)),
                    XBOX_TILED_BASE);
        } else {
            fprintf(stderr, "  WARNING: tiled aperture at 0x%08X failed"
                    " (error %lu); rendering writes will fault\n",
                    XBOX_TILED_BASE, GetLastError());
        }
    }

    xbox_WatchInit();
    fprintf(stderr, "xbox_MemoryLayoutInit: complete\n");
    return TRUE;
}

/*
 * Make every RAM mirror read-only, for finding writes that reach low memory
 * through an alias.
 *
 * Xbox RAM is visible at 28 virtual addresses that alias the same pages, so a
 * store to 0x04000004 changes Xbox VA 4 without ever touching VA 4. Both a
 * page-protection watchpoint and a DR0 hardware watchpoint on VA 4 therefore
 * report nothing while the memory demonstrably changes -- which is exactly
 * what happened chasing Halo's fs:[4] corruption.
 *
 * Debug aid, not part of normal startup: a title that legitimately writes
 * through a mirror will fault here too, and the fault address names the alias
 * and the code.
 */
void xbox_ProtectMirrorsForDebug(void)
{
    int n = 0;
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        DWORD old;
        if (g_mirror_views[m] &&
            VirtualProtect(g_mirror_views[m], g_memory_size,
                           PAGE_READONLY, &old)) {
            n++;
        }
    }
    fprintf(stderr, "  Mirrors: %d/%d made read-only (debug)\n",
            n, XBOX_NUM_MIRRORS);
}

/* ── A2h alias first-touch census (gated, observation only) ──────────────────────────────────
 *
 * WHY THIS EXISTS. The kernel thunk slot at guest VA 0x001C4064 is reachable at 29 linear host
 * addresses: the canonical mapping plus 28 mirror views that alias the SAME file region. A native
 * DR0 watch matches a LINEAR address, so it watches the canonical one and is structurally blind to
 * the other 28 -- a store through a mirror changes the memory the slot reads without ever touching
 * the watched address. The packet is explicit that this is not hypothetical: it is the mechanism
 * that hid Halo's fs:[4] corruption (see the comment above xbox_ProtectMirrorsForDebug).
 *
 * THIS IS A FIRST-TOUCH CENSUS, NOT A WRITE HISTORY. A page can only absorb one first write before
 * this handler sees it; after that the page is reopened and further writes to it escape. That is
 * sufficient for the question actually being asked -- whether ANY alias was written at all -- and
 * it is deliberately not more: a per-write history is unbounded, and the packet forbids sizing any
 * decision record by run length. One touch disqualifies every absence/attribution row.
 *
 * RECORD-BEFORE-OPEN. The AC'97 handler above opens the page first and records nothing at fault
 * time, so it is a functional precedent but NOT a forensic one and its ordering is deliberately
 * not copied. Here the touch record is published BEFORE VirtualProtect opens the page, because a
 * concurrent writer landing in an unprotected interval would otherwise be neither recorded nor the
 * first touch -- and an unrecorded write is exactly the error this census exists to prevent. If
 * the record cannot be published, the page is NOT opened: the fault is left to propagate rather
 * than converted into a silent hole in the census.
 *
 * OFF BY DEFAULT. JSRF_TRACE_A2H_DR is read once and cached; with it unset no handler is
 * registered, no page is protected and nothing is printed. */
#define A2H_ALIAS_SLOT_VA   0x001C4064u
#define A2H_ALIAS_PAGE_SIZE 4096u
#define A2H_ALIAS_GATE      "JSRF_TRACE_A2H_DR"

static void *g_a2h_alias_veh = NULL;
static void *g_a2h_alias_pages[XBOX_NUM_MIRRORS] = {0};
static uint32_t g_a2h_alias_mapped_mask = 0;
static uint32_t g_a2h_alias_protect_mask = 0;
static uint32_t g_a2h_alias_armed = 0;
static int g_a2h_alias_gate = -1;
static RECOMP_TLS int s_a2h_alias_stepping = 0;   /* 1-based index of the page this thread opened */

static int a2h_alias_on(void)
{
    if (g_a2h_alias_gate < 0)
        g_a2h_alias_gate = getenv(A2H_ALIAS_GATE) ? 1 : 0;
    return g_a2h_alias_gate;
}

static LONG CALLBACK a2h_alias_veh(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    DWORD old;

    /* Second half: the faulting write has executed. Close the page again and stop stepping.
     * Thread-local, exactly as AC'97 does it and for the same reason -- another thread must not
     * mistake its own single-step for this one, and re-protecting from the wrong thread would
     * strand this one mid-step. */
    if (code == EXCEPTION_SINGLE_STEP && s_a2h_alias_stepping) {
        int index = s_a2h_alias_stepping - 1;
        s_a2h_alias_stepping = 0;
        if (index >= 0 && index < XBOX_NUM_MIRRORS && g_a2h_alias_pages[index])
            VirtualProtect(g_a2h_alias_pages[index], A2H_ALIAS_PAGE_SIZE, PAGE_READONLY, &old);
        ep->ContextRecord->EFlags &= ~0x100u;   /* clear TF */
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_ACCESS_VIOLATION
            && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];

        /* ONLY OUR EXACT PAGES. Everything else is somebody else's fault: the game's own VEH
         * (src/main.c:212) handles the NV2A and APU apertures and this must not shadow it. */
        for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
            uintptr_t base = (uintptr_t)g_a2h_alias_pages[m];
            uint32_t live;
            if (!base || fault < base || fault >= base + A2H_ALIAS_PAGE_SIZE)
                continue;

            /* PUBLISH FIRST. Read the live slot through this alias and hand the record over
             * BEFORE the page is opened; only a confirmed publication may open it. */
            live = *(volatile uint32_t *)(base + (A2H_ALIAS_SLOT_VA & (A2H_ALIAS_PAGE_SIZE - 1)));
            if (!jsrf_slot_watch_alias_touch((uint32_t)m, (uint32_t)fault,
                                             (uint64_t)ep->ContextRecord->Rip, live, 1)) {
                /* Publication failed. Do NOT open: a page that is open without a record is a hole
                 * in the census, and an unrecorded write is the one outcome that would make a
                 * zero-touch conclusion false. The failure is recorded inside the game's registry
                 * (publish_failed) and the fault is left to propagate rather than being converted
                 * into a silent absence. */
                fprintf(stderr, "  [A2HSLOT] alias touch NOT published mirror=%d fault=%p --"
                                " leaving the page closed (coverage failure)\n",
                        m + 1, (void *)fault);
                fflush(stderr);
                return EXCEPTION_CONTINUE_SEARCH;
            }
            if (!VirtualProtect((LPVOID)base, A2H_ALIAS_PAGE_SIZE, PAGE_READWRITE, &old))
                return EXCEPTION_CONTINUE_SEARCH;
            s_a2h_alias_stepping = m + 1;
            ep->ContextRecord->EFlags |= 0x100u;   /* TF: step the write, then close again */
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Arm the census. Called before the toolkit installs its thunk table, so every mirror page is
 * read-only before the install store can execute. Returns 1 when EVERY mapped view is protected.
 *
 * A MAPPED VIEW THAT CANNOT BE PROTECTED IS A COVERAGE FAILURE, NOT A WARNING: an unwatched page
 * can absorb a write that the census would then report as absent. The masks are handed to the game
 * so the archive carries the distinction rather than an inference. */
uint32_t xbox_A2hAliasCensusArm(void)
{
    int mapped = 0, protected_ = 0;

    if (!a2h_alias_on() || g_a2h_alias_armed) return g_a2h_alias_armed;
    g_a2h_alias_mapped_mask = 0;
    g_a2h_alias_protect_mask = 0;

    /* Registered at the same priority as the game's own VEH and AC'97's (both 1). This handler
     * claims only its own pages and returns EXCEPTION_CONTINUE_SEARCH for everything else, so it
     * neither shadows nor reorders their handling. */
    g_a2h_alias_veh = AddVectoredExceptionHandler(1, a2h_alias_veh);
    if (!g_a2h_alias_veh) {
        fprintf(stderr, "  [A2HSLOT] alias census FAILED: no VEH\n");
        fflush(stderr);
        return 0;
    }

    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        DWORD old;
        uintptr_t base;
        if (!g_mirror_views[m]) continue;      /* not mapped: accounted for, not assumed */
        base = (uintptr_t)g_mirror_views[m] + (A2H_ALIAS_SLOT_VA & ~(A2H_ALIAS_PAGE_SIZE - 1));
        g_a2h_alias_pages[m] = (void *)base;
        g_a2h_alias_mapped_mask |= (1u << m);
        mapped++;
        if (VirtualProtect((LPVOID)base, A2H_ALIAS_PAGE_SIZE, PAGE_READONLY, &old)) {
            g_a2h_alias_protect_mask |= (1u << m);
            protected_++;
        } else {
            fprintf(stderr, "  [A2HSLOT] alias census: mirror %d page at %p NOT protected"
                            " (error %lu) -- coverage failure\n", m + 1, (void *)base, GetLastError());
            g_a2h_alias_pages[m] = NULL;
        }
    }
    g_a2h_alias_armed = (mapped > 0 && mapped == protected_) ? 1u : 0u;
    jsrf_slot_watch_alias_armed(g_a2h_alias_mapped_mask, g_a2h_alias_protect_mask,
                                (uint32_t)protected_);
    fprintf(stderr, "  [A2HSLOT] alias census armed=%u mapped=%d protected=%d mask=%08X/%08X"
                    " slot_page=%08X\n",
            g_a2h_alias_armed, mapped, protected_, g_a2h_alias_mapped_mask,
            g_a2h_alias_protect_mask, A2H_ALIAS_SLOT_VA & ~(A2H_ALIAS_PAGE_SIZE - 1));
    fflush(stderr);
    return g_a2h_alias_armed;
}

/* Restore the original protections and drop the handler. The packet's closure rule requires the
 * mirror-page protections to be restored, not merely reported. */
void xbox_A2hAliasCensusDisarm(void)
{
    int restored = 0;
    if (!g_a2h_alias_armed && !g_a2h_alias_veh) return;
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        DWORD old;
        if (!g_a2h_alias_pages[m]) continue;
        if (VirtualProtect(g_a2h_alias_pages[m], A2H_ALIAS_PAGE_SIZE, PAGE_READWRITE, &old)) {
            restored++;
        } else {
            fprintf(stderr, "  [A2HSLOT] alias census: mirror %d page NOT restored (error %lu)\n",
                    m + 1, GetLastError());
        }
        g_a2h_alias_pages[m] = NULL;
    }
    if (g_a2h_alias_veh) {
        RemoveVectoredExceptionHandler(g_a2h_alias_veh);
        g_a2h_alias_veh = NULL;
    }
    fprintf(stderr, "  [A2HSLOT] alias census disarmed restored=%d\n", restored);
    fflush(stderr);
    g_a2h_alias_armed = 0;
}

/* ── A2h LIVE slot-write watch: page protection ONLY, no debug registers ────────────────────────
 *
 * WHY THIS EXISTS. `software_device+0x242C` (== `context+0x1C4`) is read at 0x00193E62, NULL-tested,
 * and called at 0x00193EB5. It is installed with the code pointer 0x0015F9D0 by the store at
 * 0x0018CE3A, but the observed terminal target is the packed colour word 0x001D5078, which is what
 * the store at 0x00199F45 (`mov [esi+ebp*4+0x3ec],eax`) produces. The question is WHO wrote the
 * slot last, and with WHAT VALUE. That is a question about a STORE, and a store is the one thing a
 * read-only page refuses.
 *
 * MECHANISM: PAGE PROTECTION, AND NOTHING ELSE. The slot's 4 KiB page is made PAGE_READONLY in the
 * canonical view and in every mapped mirror view of the same physical page. An RO page faults on
 * WRITE and never on READ, so the poll loop's read volume costs nothing and no read filter is
 * needed -- the protection IS the discrimination. There is no DR0/DR6/DR7 anywhere in this
 * facility and none may be added: the DR channel is EXCLUDED, not demoted.
 *
 * PAGE GRANULARITY IS LOAD-BEARING. The slot sits at page offset 0x62C of page 0x0019D000, so the
 * watch necessarily catches EVERY write to that page. A fault is a SLOT HIT only when its effective
 * address equals the re-derived slot VA for that alias; every other fault on the page is TRAFFIC
 * and is counted as such. Conflating the two would attribute an unrelated page write to the slot.
 *
 * THE ADDRESS IS NEVER PRESELECTED. `MEM32(0x19DCE0)` is a POINTER to the device object, allocated
 * at runtime, so the slot VA does not exist until the title allocates. It is read at ARM, the slot
 * is derived by CHECKED 32-bit addition, and both are re-derived at TERMINAL; a moved base is a
 * RE-SCOPE and is recorded as such rather than silently compared against the old VA.
 *
 * OBSERVATION ONLY. The handler's single change is the faulting thread's own TF bit, so the store
 * the guest was already executing can complete and be read back. The store itself is executed by
 * the guest, unmodified; no guest register, memory, allocation, cleanup or device state is altered,
 * and nothing here can suppress a guest trap or fabricate a result. */
#define A2H_SLOTW_EV_WRITE      1u
#define A2H_SLOTW_EV_STEP       2u
#define A2H_SLOTW_EV_READ       3u
#define A2H_SLOTW_EV_CONTROL    4u

#define A2H_SLOTW_MAGIC 0x57533241u   /* 'A2SW' */

XboxA2hSlotwLedger g_xbox_a2h_slotw;

static void *g_a2h_slotw_veh = NULL;
static void *g_a2h_slotw_pages[XBOX_A2H_SLOTW_PAGES_MAX] = {0};
static int g_a2h_slotw_gate = -1;
static volatile LONG g_a2h_slotw_seq = 0;
static volatile LONG g_a2h_slotw_step_owner = 0;   /* native tid of the thread mid-step, else 0 */

/* `g_a2h_slotw_armed`, `s_a2h_slotw_pending` and `s_a2h_slotw_saved_tf` are declared ABOVE
 * ac97_write_veh(), because the AC'97 trap is a second owner of the same pending word. The
 * per-OWNER payload below is this watch's alone: AC'97's step carries its own reset-bit work and
 * needs none of it. */
static RECOMP_TLS uint32_t s_a2h_slotw_pending_alias = 0;
static RECOMP_TLS uint32_t s_a2h_slotw_pending_slot = 0;
static RECOMP_TLS uint32_t s_a2h_slotw_pending_pre = 0;
static RECOMP_TLS uint32_t s_a2h_slotw_pending_seq = 0;
/* THE ADDRESS THE OPERATING SYSTEM REPORTED FOR THIS FAULT. Kept so the step handler can compare the
 * OS's own report against the address this facility computes for its post-value read: for a slot
 * store the two must be the SAME HOST ADDRESS, and that agreement is the Q3(c) cross-validation. */
static RECOMP_TLS uint32_t s_a2h_slotw_pending_fault = 0;
/* THE FAULTING RIP'S RANGE CLASS AND ITS OPTIONAL NATIVE-DISASM CORROBORATION, carried from the
 * fault to its step so the STEP record carries the same classification the WRITE record did. They
 * are carried rather than recomputed because the step's RIP is the NEXT instruction, and
 * reclassifying it would label the step with a different instruction's class. */
static RECOMP_TLS uint32_t s_a2h_slotw_pending_range = 0;
static RECOMP_TLS uint32_t s_a2h_slotw_pending_form = 0;

static int a2h_slotw_on(void)
{
    if (g_a2h_slotw_gate < 0)
        g_a2h_slotw_gate = getenv(XBOX_A2H_SLOTW_GATE) ? 1 : 0;
    return g_a2h_slotw_gate;
}

static uint32_t a2h_slotw_next_seq(void)
{
    return (uint32_t)InterlockedIncrement(&g_a2h_slotw_seq);
}

/* Publish one FULL record. Returns 1 when the record is in the ledger, 0 when the bounded array is
 * full -- in which case the overflow latch is set and the caller must NOT treat the write as
 * unobserved. A full record per event, never a sample and never first-N.
 *
 * ⚠ WHAT CALLS THIS, AND WHAT NO LONGER DOES. The packet's design admits detailed records for
 * exactly two things: SLOT BYTE writes (the fault record and its step record, which together carry
 * the slot's before/after values) and the FIRST-TOUCH census. A write to the watched page whose
 * effective address is not the slot CANNOT CHANGE THE SLOT; the packet calls it TRAFFIC and requires
 * it COUNTED, not transcribed. ON trial 1 recorded every page write and its step, so 129 writes
 * consumed 258 of 256 records and the fail-closed path ended the run on ordinary volume. Traffic now
 * takes a2h_slotw_note_traffic() below and consumes no record at all after the page is first walked.
 *
 * THE FAIL-CLOSED PATH IS UNCHANGED AND IS NOT WEAKENED: this still returns 0 when the array is
 * full, and the caller still leaves the page CLOSED and lets the fault propagate. What changed is
 * that ordinary page traffic can no longer reach the array. */
static int a2h_slotw_publish(uint32_t kind, uint32_t alias_index, uint32_t slot_hit,
                             uint32_t fault_va, uint32_t pre_value, uint32_t post_value,
                             uint64_t rip, uint32_t range_class, uint32_t form)
{
    XboxA2hSlotwLedger *L = &g_xbox_a2h_slotw;
    uint32_t seq = a2h_slotw_next_seq();
    LONG slot;

    slot = InterlockedIncrement((volatile LONG *)&L->event_count) - 1;
    if (slot < 0 || slot >= XBOX_A2H_SLOTW_EVENTS_MAX) {
        A2H_SLOTW_INC64(&L->loss.dropped_events);
        InterlockedExchange((volatile LONG *)&L->event_overflow, 1);
        InterlockedExchange((volatile LONG *)&L->loss.overflow, 1);
        return 0;
    }
    /* Every field is written before the record is reachable: a reader that walks event_count can
     * only see records whose seq is non-zero, and seq is written LAST. */
    L->events[slot].alias_index = alias_index;
    L->events[slot].slot_hit = slot_hit;
    L->events[slot].fault_va = fault_va;
    L->events[slot].pre_value = pre_value;
    L->events[slot].post_value = post_value;
    L->events[slot].tid = (uint32_t)GetCurrentThreadId();
    L->events[slot].range_class = range_class;
    L->events[slot].form = form;
    L->events[slot].rip = rip;
    L->events[slot].ticks = (uint64_t)GetTickCount64();
    L->events[slot].kind = kind;
    L->events[slot].seq = seq;          /* LAST: the record becomes visible here */
    return 1;
}

/* ── TRAFFIC: COUNTED, NOT RECORDED ────────────────────────────────────────────────────────────
 *
 * A write to the watched page at an address that is NOT the slot cannot change the slot. The packet
 * is explicit that these are TRAFFIC and that they are counted; retaining them as records was the
 * implementation's violation of its own design and the direct cause of the ON-trial-1 overflow.
 *
 * WHAT IS KEPT, AND WHY IT IS NOT "NOTHING". The packet also requires a FIRST-TOUCH CENSUS: the SET
 * of addresses the page's traffic has touched, each recorded ONCE, with the value that was there
 * before the first touch. That is what makes the census informative about the fill without being a
 * transcript of it -- and it is why a linear fill costs ONE record per DISTINCT dword and ZERO for
 * every repeat. `loss.nonslot_writes` remains the uncapped multiplicity.
 *
 * ⚠ "FIRST TOUCH" MEANS FIRST TOUCH **OF THAT ADDRESS**, AND THE FIRST VERSION GOT THIS WRONG. It
 * claimed a census entry unconditionally, so 64 writes to one address produced 64 entries: a census
 * of WRITES wearing the name of a census of ADDRESSES. The fixture's repeat arm caught it. The
 * address must therefore be LOOKED UP before an entry is claimed -- and the lookup is a linear scan
 * because the table is small, is written once per address, and is only ever walked by a thread that
 * has just taken a fault. A hash would add a failure mode (collisions, resizing) to save time on a
 * path that has already paid for an exception.
 *
 * CONCURRENCY: the scan and the claim are not one atomic step, so two threads first-touching the
 * SAME address concurrently can each add an entry. That is a bounded, benign duplication -- the
 * census stays a SET for the reader (duplicates carry the same offset) and the count is reported
 * alongside `nonslot_distinct` so a reader can see it. It never loses a touch, which is the property
 * that matters: a missing entry would make a touched address look untouched. */
static void a2h_slotw_note_traffic(uint32_t alias_index, uint32_t fault_va, uint32_t offset,
                                   uint32_t pre_at_fault, uint32_t slot_value, uint64_t rip,
                                   uint32_t range_class, uint32_t form)
{
    XboxA2hSlotwLedger *L = &g_xbox_a2h_slotw;
    LONG slot;
    uint32_t i, seen = L->first_touch_count;

    /* `loss.nonslot_writes` is NOT incremented here: the fault handler counts it alongside
     * `slot_hits`, so every relevant AV is classified exactly once. Incrementing it in both places
     * would double every traffic write, and the fixture's `+N` assertion is what would catch it. */
    /* ALREADY CENSUSED? Then this is a repeat touch of a known address: it costs NOTHING -- no
     * entry, no allocation, no overflow risk. This is the arm that makes a long linear fill
     * harmless rather than merely survivable. */
    for (i = 0; i < seen && i < XBOX_A2H_SLOTW_FIRST_TOUCH_MAX; i++) {
        if (L->first_touch[i].valid && L->first_touch[i].offset == offset
                && L->first_touch[i].alias_index == alias_index)
            return;
    }
    A2H_SLOTW_INC64(&L->loss.nonslot_distinct);
    slot = InterlockedIncrement((volatile LONG *)&L->first_touch_count) - 1;
    if (slot < 0 || slot >= XBOX_A2H_SLOTW_FIRST_TOUCH_MAX) {
        /* A LATCH, and only for the census -- a first touch beyond the census capacity costs the
         * CENSUS's completeness, never the write's coverage: the write was still protected, still
         * faulted, and is still counted in `nonslot_writes`. It is not silently dropped. */
        InterlockedExchange((volatile LONG *)&L->first_touch_overflow, 1);
        A2H_SLOTW_INC64(&L->loss.first_touch_overflow);
        A2H_SLOTW_INC64(&L->loss.first_touch_dropped);
        A2H_SLOTW_SET64(&L->loss.overflow, 1);
        return;
    }
    L->first_touch[slot].offset = offset;
    L->first_touch[slot].alias_index = alias_index;
    L->first_touch[slot].tid = (uint32_t)GetCurrentThreadId();
    L->first_touch[slot].pre_value = pre_at_fault;
    L->first_touch[slot].slot_value_at_touch = slot_value;
    L->first_touch[slot].range_class = range_class;
    L->first_touch[slot].form = form;
    L->first_touch[slot].rip = rip;
    L->first_touch[slot].ticks = (uint64_t)GetTickCount64();
    InterlockedExchange((volatile LONG *)&L->first_touch[slot].valid, 1);   /* LAST */
}

/* ── THE RANGE CLASSIFIER: NATIVE DOMAIN. THIS REPLACES A VOID PARADIGM ──────────────────────────
 *
 * ⚠⚠ WHAT WAS HERE, AND WHY IT COULD NEVER HAVE WORKED.
 *
 * The previous body read the NATIVE instruction bytes at the faulting RIP and tested them for GUEST
 * ENCODINGS: `p[0] == 0x89`, `(modrm & 0xC0) == 0x80`, then `disp32 == 0x242C` (the installer) or
 * `disp32 == 0x3EC` with SIB (the candidate). Its comment asserted that "the bytes AT the recorded
 * RIP name the instruction exactly".
 *
 * THAT PREMISE IS FALSE IN RECOMPILED CODE. A faulting RIP inside this program points at GENERATED
 * C compiled by the host toolchain -- MSVC's own instruction selection for `MEM32(ecx + 0x242C) =
 * eax` -- and not at the guest's `mov [ecx+0x242c],eax`. The guest encoding `89 81 2C 24 00 00` is
 * not merely unlikely to appear there; there is no mechanism by which it could. The classifier was
 * therefore structurally incapable of returning MODRM, and EVERY encoding classification this
 * facility ever emitted from a fault RIP was UNSOUND -- ON-3's `enc=3` on the slot event and the
 * `enc` fields on ON-1/ON-2 among them. The Advisor's ruling makes that categorical, not
 * site-specific, so no encoding classification from a fault RIP may be cited as evidence anywhere.
 *
 * ⚠ THE REPLACEMENT CLASSIFIES BY ADDRESS RANGE, AND GUEST-BYTE EXPECTATIONS ARE FORBIDDEN HERE.
 * There is no byte test left in this file and none may be reintroduced:
 *
 *     RIP in [recomp_lo, recomp_hi)   -> GAME_MODULE   (the recompiled module's own code)
 *     RIP in [image_lo,  image_hi)    -> TOOLKIT_HOST  (this image, but not recompiled code)
 *     otherwise                       -> UNKNOWN       (=> INFRA FAILURE; never attributed)
 *
 * BOTH BOUNDS COME FROM THE REAL ARTIFACT:
 *   * the recompiled bound is PUBLISHED BY THE EMBEDDER through
 *     xbox_A2hSlotWatchSetRecompBounds(), derived from addresses the generated dispatch
 *     (`recomp_lookup`) actually returns -- the toolkit cannot enumerate the game's own generated
 *     translation units, and an invented bound would reintroduce exactly the arithmetic this
 *     replacement removes;
 *   * the image bound is read from THIS PROCESS'S OWN PE HEADERS (`__ImageBase` plus the optional
 *     header's SizeOfImage), i.e. the module the RIP actually lives in.
 *
 * ⚠ AND THE ORDER MATTERS. The recompiled test is made FIRST because the recompiled module is
 * LINKED INTO this image: its addresses are inside [image_lo, image_hi) too. Testing the image
 * bound first would classify every recompiled RIP as TOOLKIT_HOST and the control could never fire.
 * The two ranges are therefore not disjoint by construction, and this ordering is the whole
 * classification. */
static uint64_t s_a2h_slotw_recomp_lo = 0;
static uint64_t s_a2h_slotw_recomp_hi = 0;
static uint32_t s_a2h_slotw_recomp_valid = 0;
static uint32_t s_a2h_slotw_recomp_probes = 0;
/* ⚠ THE SORTED SET OF RECOMPILED FUNCTION STARTS -- THE CLASSIFIER'S ACTUAL TEST.
 *
 * A single interval was tried first and MEASUREMENT against the real image rejected it: the game's
 * own probe objects (`harness_probes`/`video_probes`/`gpu_probes`) are linked into the MIDDLE of the
 * recompiled extent as one 0x1690-byte run of 14 host functions, and a store from `probe_worker_fault`
 * or `jsrf_probe_gpu` -- both of which run during a probe run -- would have classified GAME_MODULE
 * and been counted as the installer control. Membership of this set is the test instead.
 *
 * SORTED ASCENDING, and the sort is done by the PUBLISHER before `valid` is set, so the classifier
 * can binary-search it without a lock and without ever observing a half-written set. */
static uint64_t s_a2h_slotw_recomp_starts[XBOX_A2H_SLOTW_RECOMP_MAX];
static uint32_t s_a2h_slotw_recomp_count = 0;
/* The embedder's own overflow report. Latched, never cleared, and published into the ledger at ARM
 * so the archive distinguishes "the embedder could not enumerate the module" from "the toolkit
 * rejected what it was given" -- both are INFRA FAILURE, but they are different defects. */
static uint32_t s_a2h_slotw_recomp_overflow = 0;

/* This process's OWN image bounds, from its PE headers. Read once and cached: the loader does not
 * move a module after it is mapped, so a per-fault read would buy nothing and cost an exception
 * handler's time. Returns 1 when the bounds are known. */
static uint64_t s_a2h_slotw_image_lo = 0;
static uint64_t s_a2h_slotw_image_hi = 0;
static int s_a2h_slotw_image_read = 0;

static void a2h_slotw_read_image_bounds(void)
{
    /* ⚠ `__ImageBase` IS A LINKER-PROVIDED SYMBOL AND IS NOT DECLARED BY <windows.h>. MSVC emits it
     * for every image (EXE or DLL) and it names THIS module's own PE header, which is the one address
     * that cannot be wrong about which image a RIP belongs to -- no API call, no symbol lookup and no
     * debugger involved. The declaration is the documented MSVC form. */
    extern IMAGE_DOS_HEADER __ImageBase;
    const uint8_t *base;
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS *nt;

    if (s_a2h_slotw_image_read)
        return;
    s_a2h_slotw_image_read = 1;

    /* `__ImageBase` is the linker-provided address of this module's own PE header. It is the one
     * address that cannot be wrong about which image the RIP belongs to, and it needs no API call,
     * no symbol lookup and no debugger. */
    base = (const uint8_t *)&__ImageBase;
    dos = (const IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    s_a2h_slotw_image_lo = (uint64_t)(uintptr_t)base;
    s_a2h_slotw_image_hi = s_a2h_slotw_image_lo + (uint64_t)nt->OptionalHeader.SizeOfImage;
}

void xbox_A2hSlotWatchSetRecompBounds(uint64_t lo, uint64_t hi, uint32_t probes, uint32_t valid)
{
    /* A bound is accepted only when the embedder says it came from real `recomp_lookup` answers AND
     * it is a non-empty, correctly ordered interval. Anything else CLEARS the bound: the classifier
     * then refuses to classify, which is the fail-closed behaviour the packet requires. Accepting a
     * degenerate range would make every RIP "inside the recompiled module" and hand the control to
     * an unrelated writer -- the exact misattribution this whole fix exists to prevent. */
    if (!valid || hi <= lo) {
        s_a2h_slotw_recomp_lo = 0;
        s_a2h_slotw_recomp_hi = 0;
        s_a2h_slotw_recomp_valid = 0;
        s_a2h_slotw_recomp_probes = 0;
        s_a2h_slotw_recomp_count = 0;
        return;
    }
    s_a2h_slotw_recomp_lo = lo;
    s_a2h_slotw_recomp_hi = hi;
    s_a2h_slotw_recomp_valid = 1;
    s_a2h_slotw_recomp_probes = probes;
    /* A publication with no starts is a set the classifier cannot test, so it is treated as no
     * publication at all rather than as an empty (and therefore never-matching) set. */
    s_a2h_slotw_recomp_count = 0;
}

/* ── THE FULL PUBLICATION: EVERY RECOMPILED FUNCTION START, SORTED ───────────────────────────────
 *
 * ⚠ THIS IS THE ONE THE GAME CALLS, AND IT REPLACES THE SINGLE-INTERVAL PUBLICATION ABOVE.
 *
 * `starts` are the native addresses the generated dispatch returned for real guest VAs. They are
 * COPIED and SORTED here, and `valid` is set LAST, so a classifier running concurrently either sees
 * the previous set whole or the new set whole -- never a partially written one.
 *
 * ⚠ AN OVERFLOW IS REFUSED WHOLE, NOT TRUNCATED. If the embedder offers more starts than the array
 * holds, the publication is REJECTED and the previous state is cleared, so the classifier reports
 * UNKNOWN and `range_unavailable` moves -- INFRA FAILURE. Keeping the first N would classify the
 * functions that happened to fit and silently misplace every other one, which is strictly worse than
 * refusing: a wrong GAME_MODULE is a misattribution, an UNKNOWN is a stop. */
uint32_t xbox_A2hSlotWatchSetRecompStarts(const uint64_t *starts, uint32_t count, uint32_t probes)
{
    uint32_t i, j;

    if (!starts || count == 0 || count > XBOX_A2H_SLOTW_RECOMP_MAX) {
        /* REFUSE WHOLE. The caller's overflow flag is set by the GAME side; here the bound is simply
         * cleared so nothing classifies against a set this facility could not record. */
        s_a2h_slotw_recomp_lo = 0;
        s_a2h_slotw_recomp_hi = 0;
        s_a2h_slotw_recomp_valid = 0;
        s_a2h_slotw_recomp_count = 0;
        s_a2h_slotw_recomp_probes = probes;
        return 0;
    }

    for (i = 0; i < count; i++)
        s_a2h_slotw_recomp_starts[i] = starts[i];

    /* INSERTION SORT: the array is published once, at ARM, by one thread, and the input arrives in
     * GUEST-VA order which is NOT native order -- so it must be sorted, and a simple insertion sort
     * on a few thousand entries that runs once is the honest choice over a library call whose
     * failure mode would be a silently unsorted set (which would break the binary search). */
    for (i = 1; i < count; i++) {
        uint64_t key = s_a2h_slotw_recomp_starts[i];
        j = i;
        while (j > 0 && s_a2h_slotw_recomp_starts[j - 1] > key) {
            s_a2h_slotw_recomp_starts[j] = s_a2h_slotw_recomp_starts[j - 1];
            j--;
        }
        s_a2h_slotw_recomp_starts[j] = key;
    }

    s_a2h_slotw_recomp_lo = s_a2h_slotw_recomp_starts[0];
    s_a2h_slotw_recomp_hi = s_a2h_slotw_recomp_starts[count - 1];
    s_a2h_slotw_recomp_probes = probes;
    s_a2h_slotw_recomp_count = count;
    s_a2h_slotw_recomp_valid = 1;     /* LAST: the set becomes visible whole */
    return 1;
}

/* ⚠ THE CLASSIFICATION ITSELF, AND IT CONTAINS NO ENCODING TEST. See the block comment above for
 * why the recompiled test must come first and why the bounds must come from the artifact.
 *
 * THE RECOMPILED TEST IS SET MEMBERSHIP: the RIP is GAME_MODULE when it lies in `[start_k, start_{k+1})`
 * for consecutive PUBLISHED RECOMPILED STARTS, or at/after the last one.
 *
 * ⚠ WHAT THIS DOES AND DOES NOT BUY, MEASURED RATHER THAN ASSUMED. Within a generated translation
 * unit the recompiler emits one C function per guest function, back to back, so `[start_k, start_{k+1})`
 * IS that function's body and the test is exact there. It is NOT exact across a host run linked
 * between two recompiled functions: the function below the run has its interval stretched over it.
 * The fixture's discriminating arm MEASURED that the set test and a bare interval test have
 * EQUIVALENT COVERAGE on that case -- both attribute such a run to the function below -- so the set
 * is chosen for what it genuinely provides (the real published data, and an explicit extent
 * definition), NOT because it closes that residual. Closing it would need function ENDS, which the
 * generated dispatch cannot answer. */
static uint32_t a2h_slotw_classify_rip(uint64_t rip)
{
    uint32_t lo_i, hi_i, mid;

    a2h_slotw_read_image_bounds();

    /* THE RECOMPILED TEST FIRST: the recompiled module is LINKED INTO this image, so its addresses
     * are inside the image bound too. Testing the image bound first would classify every recompiled
     * RIP as TOOLKIT_HOST and the control could never fire. */
    if (s_a2h_slotw_recomp_valid && s_a2h_slotw_recomp_count) {
        uint64_t first = s_a2h_slotw_recomp_starts[0];
        uint64_t last = s_a2h_slotw_recomp_starts[s_a2h_slotw_recomp_count - 1u];
        if (rip >= first && rip <= last) {
            /* The greatest published start <= rip, by binary search over the sorted set. */
            lo_i = 0;
            hi_i = s_a2h_slotw_recomp_count;
            while (lo_i < hi_i) {
                mid = lo_i + (hi_i - lo_i) / 2u;
                if (s_a2h_slotw_recomp_starts[mid] <= rip)
                    lo_i = mid + 1u;
                else
                    hi_i = mid;
            }
            if (lo_i > 0 && s_a2h_slotw_recomp_starts[lo_i - 1u] == rip)
                return XBOX_A2H_SLOTW_RANGE_GAME_MODULE;   /* EXACTLY a function's entry */
            if (lo_i > 0) {
                /* STRICTLY INSIDE the interval [start_{k}, start_{k+1}) whose lower end is the
                 * nearest published start at or below the RIP. The upper end is the NEXT published
                 * start, or one past the highest start when this is the last function. */
                uint64_t next = (lo_i < s_a2h_slotw_recomp_count)
                                    ? s_a2h_slotw_recomp_starts[lo_i]
                                    : (s_a2h_slotw_recomp_hi + 1u);
                if (rip < next)
                    return XBOX_A2H_SLOTW_RANGE_GAME_MODULE;
            }
        }
    }

    if (s_a2h_slotw_image_hi && rip >= s_a2h_slotw_image_lo && rip < s_a2h_slotw_image_hi)
        return XBOX_A2H_SLOTW_RANGE_TOOLKIT_HOST;

    return XBOX_A2H_SLOTW_RANGE_UNKNOWN;
}

/* ── OPTIONAL NATIVE-DISASSEMBLY CORROBORATION, AND IT NEVER GATES ──────────────────────────────
 *
 * This decodes the NATIVE bytes at the RIP by NATIVE x86-64 semantics and answers one question: is
 * this instruction a STORE TO MEMORY? That is a statement about the HOST's instruction set and
 * carries no guest-byte expectation, which is what makes it admissible where the old classifier was
 * not.
 *
 * ⚠ IT IS CORROBORATION ONLY. The range class above is the classification; the control and every
 * record key on the RANGE CLASS alone. A RIP that does not decode, or decodes to something this
 * decoder does not recognise, keeps its range class and is reported as UNDECODED. That separation
 * is deliberate: a hand-written partial decoder must never be able to fail a run the range
 * classifier placed correctly, and it must never be able to promote a RIP the range classifier did
 * not place.
 *
 * The decoder is deliberately PARTIAL and says so. It handles the forms a compiled store actually
 * takes -- REX prefixes, the 0x88/0x89 (mov r/m,r) and 0xC6/0xC7 (mov r/m,imm) families, the
 * 0x00-0x3B ALU group with /0 and /1 (add/or), 0xFF /0,/1,/2 (inc/dec/call), and the
 * 0x80/0x81/0x83 group -- and it must consume a ModRM byte whose mod field is NOT 0b11 (a register
 * operand is not a memory store). Anything else returns UNDECODED rather than guessing. */
static uint32_t a2h_slotw_corroborate_form(uint64_t rip)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)rip;
    uint8_t op, modrm;
    size_t i = 0;

    /* A RIP is only readable if it is inside this process's own image; guard rather than fault
     * inside the fault handler. MEMORY_BASIC_INFORMATION is the cheapest sound test. */
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)(uintptr_t)rip, &mbi, sizeof(mbi)) == 0)
            return XBOX_A2H_SLOTW_FORM_UNDECODED;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS))
            return XBOX_A2H_SLOTW_FORM_UNDECODED;
    }

    /* Legacy prefixes a store may carry: operand/address size and the segment overrides. Bounded, so
     * a pathological prefix run cannot walk off the page. */
    while (i < 8) {
        uint8_t b = p[i];
        if (b == 0x66 || b == 0x67 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26
                || b == 0x64 || b == 0x65 || b == 0xF0 || b == 0xF2 || b == 0xF3) {
            i++;
            continue;
        }
        break;
    }
    /* REX, if present. Its presence is exactly one of the four causes the corrected question
     * listed, and this decoder simply consumes it. */
    if (i < 8 && (p[i] & 0xF0u) == 0x40u)
        i++;
    if (i >= 8)
        return XBOX_A2H_SLOTW_FORM_UNDECODED;

    op = p[i];
    modrm = p[i + 1];

    /* mov r/m8, r8 (0x88) and mov r/m32/64, r32/64 (0x89): a store exactly when mod != 0b11. */
    if (op == 0x88 || op == 0x89)
        return ((modrm & 0xC0u) != 0xC0u) ? XBOX_A2H_SLOTW_FORM_STORE
                                          : XBOX_A2H_SLOTW_FORM_NOT_STORE;
    /* mov r/m8, imm8 (0xC6) and mov r/m32, imm32 (0xC7): the same test. */
    if (op == 0xC6 || op == 0xC7)
        return ((modrm & 0xC0u) != 0xC0u) ? XBOX_A2H_SLOTW_FORM_STORE
                                          : XBOX_A2H_SLOTW_FORM_NOT_STORE;
    /* The ALU group 0x00-0x3B: /0 is `add`, /1 is `or` -- both write their destination, so a memory
     * destination is a store. Other /n values in this range are compares or test-like and do not. */
    if (op <= 0x3Bu) {
        uint8_t reg = (uint8_t)((modrm >> 3) & 0x07u);
        if ((modrm & 0xC0u) == 0xC0u)
            return XBOX_A2H_SLOTW_FORM_NOT_STORE;
        return (reg == 0u || reg == 1u) ? XBOX_A2H_SLOTW_FORM_STORE : XBOX_A2H_SLOTW_FORM_NOT_STORE;
    }
    /* The immediate group 0x80/0x81/0x83: /0 add and /1 or store; /7 cmp does not. */
    if (op == 0x80 || op == 0x81 || op == 0x83) {
        uint8_t reg = (uint8_t)((modrm >> 3) & 0x07u);
        if ((modrm & 0xC0u) == 0xC0u)
            return XBOX_A2H_SLOTW_FORM_NOT_STORE;
        return (reg == 0u || reg == 1u) ? XBOX_A2H_SLOTW_FORM_STORE : XBOX_A2H_SLOTW_FORM_NOT_STORE;
    }
    /* 0xFF /0 inc and /1 dec write their operand; /2 call and /3 callf do not write memory. */
    if (op == 0xFF) {
        uint8_t reg = (uint8_t)((modrm >> 3) & 0x07u);
        if (reg > 1u)
            return XBOX_A2H_SLOTW_FORM_NOT_STORE;
        return ((modrm & 0xC0u) != 0xC0u) ? XBOX_A2H_SLOTW_FORM_STORE
                                          : XBOX_A2H_SLOTW_FORM_NOT_STORE;
    }
    return XBOX_A2H_SLOTW_FORM_UNDECODED;
}

/* Which of OUR pages, if any, holds this fault address? Returns the alias index + 1, or 0. */
/* ⚠ RECORD ONE UNPLACEABLE RIP, ONCE. `range_unknown` counts them all; this samples the first few
 * DISTINCT ones so a reader can tell an instrument bug inside the recompiled extent from a writer
 * that is genuinely outside the image -- two findings that demand OPPOSITE responses and that a bare
 * count cannot distinguish.
 *
 * The distinctness scan is bounded by the sample size, so a hot loop of unplaceable faults costs a
 * comparison against at most 16 addresses rather than an unbounded table. Duplicates are NOT
 * re-recorded: the sample is a SET of RIPs, and `range_unknown` already carries the multiplicity. */
static void a2h_slotw_note_unknown_rip(uint64_t rip)
{
    XboxA2hSlotwLedger *L = &g_xbox_a2h_slotw;
    uint32_t n, i;

    n = L->unknown_rip_count;
    if (n >= XBOX_A2H_SLOTW_UNKNOWN_SAMPLE_MAX)
        return;
    for (i = 0; i < n && i < XBOX_A2H_SLOTW_UNKNOWN_SAMPLE_MAX; i++) {
        if (L->unknown_rips[i] == rip)
            return;                        /* already sampled: the counter has the multiplicity */
    }
    /* A benign race: two threads can claim the same index and one write wins. Both RIPs are
     * unplaceable, so the sample stays a set of unplaceable RIPs either way -- it can lose one entry
     * under contention, which is why `unknown_rip_count` is reported as the sample's size and
     * `range_unknown` remains the authoritative count. */
    L->unknown_rips[n] = rip;
    /* ⚠ AND WHICH REGION IT LIVES IN, FROM THE OS. The first question about an unplaceable RIP is
     * "which mapped region is this?", and the two answers demand OPPOSITE responses: a RIP inside
     * THIS image means the classifier failed on its own code (an instrument bug), while a RIP in
     * another module means the writer is genuinely outside anything this facility can place. Asking
     * the OS once per sampled RIP makes that answerable from the archive instead of by a re-run.
     * This is an OBSERVATION about the address; it changes no classification. */
    {
        MEMORY_BASIC_INFORMATION mbi;
        uint64_t base = 0;
        if (VirtualQuery((LPCVOID)(uintptr_t)rip, &mbi, sizeof(mbi)) != 0)
            base = (uint64_t)(uintptr_t)mbi.AllocationBase;
        L->unknown_rip_bases[n] = base;
        L->unknown_same_image[n] = (base && s_a2h_slotw_image_lo
                                    && base == s_a2h_slotw_image_lo) ? 1u : 0u;
        L->unknown_reserved[n] = 0;
    }
    L->unknown_rip_count = n + 1;
}

static uint32_t a2h_slotw_owning_alias(uintptr_t fault)
{
    uint32_t i;
    for (i = 0; i < XBOX_A2H_SLOTW_PAGES_MAX; i++) {
        uintptr_t base = (uintptr_t)g_a2h_slotw_pages[i];
        if (!base) continue;
        if (fault >= base && fault < base + XBOX_A2H_SLOTW_PAGE_SIZE)
            return i + 1;
    }
    return 0;
}

/* The canonical slot VA for the current arm, re-derived rather than cached by the caller. */
static uint32_t a2h_slotw_slot_va(void)
{
    return g_xbox_a2h_slotw.arm_slot;
}

/* Service ONE owner bit exactly once, then clear it. `bit` is 0x1 (own) or 0x2 (AC97). Returns
 * nothing: the caller decides the TF state from what REMAINS pending, which is the whole point of
 * keeping the bits separate. */
static void a2h_slotw_service_own(PEXCEPTION_POINTERS ep)
{
    XboxA2hSlotwLedger *L = &g_xbox_a2h_slotw;
    DWORD old;
    uint32_t alias = s_a2h_slotw_pending_alias;
    uint32_t post;
    uintptr_t base;

    /* The faulting store has now executed. Read the POST value through the SAME alias the store
     * used, so a store that landed through a mirror is read back through that mirror -- an alias
     * whose write is read back canonically would report the wrong value if the views ever diverged
     * (they are asserted to alias, but the packet forbids relying on that here). */
    base = (uintptr_t)g_a2h_slotw_pages[alias ? alias - 1 : 0];
    post = *(volatile uint32_t *)(base + (a2h_slotw_slot_va() & (XBOX_A2H_SLOTW_PAGE_SIZE - 1)));

    A2H_SLOTW_INC64(&L->loss.steps);

    /* ── A STEP RECORD IS A SLOT-BYTE RECORD, AND ONLY THAT ──────────────────────────────────────
     *
     * The step of a TRAFFIC write is housekeeping: the page must be re-protected and the step
     * counted, but no slot byte changed, so the packet's design gives it no record. Publishing one
     * anyway is what made a page write cost TWO records on ON trial 1 and exhausted the buffer.
     * `loss.steps` and `loss.rearm_ok` still count every step, so the re-arm coverage claim is
     * unchanged and remains uncapped. */
    if (s_a2h_slotw_pending_slot) {
        /* ── Q3(c), HALF ONE: THE STRUCTURAL ADDRESS INVARIANT ───────────────────────────────────
         *
         * The fault handler was handed `fault` BY THE OPERATING SYSTEM; the step handler computes
         * where to read the post value ITSELF. For a slot store both must name the SAME HOST
         * ADDRESS, so this asserts that the pending state round-tripped the OS's report intact and
         * that `a2h_slotw_slot_va()` did not move between the fault and its step.
         *
         * ⚠ THIS HALF IS STRUCTURAL AND NEARLY TAUTOLOGICAL, AND SAYING SO IS THE POINT. Both sides
         * derive from `g_a2h_slotw_pages[alias-1] + (slot_va & 0xFFF)`, so it can only fail if the
         * pending state or the arm moved underneath the step. It is kept because that failure mode
         * is real (a re-arm, a re-scope, a corrupted TLS word) and cheap to exclude -- NOT because it
         * is the overlap control.
         *
         * ⚠ THE OVERLAP CONTROL IS HALF TWO, AND IT IS IN xbox_A2hSlotWatchTerminal(): the
         * instrument's read of the slot through its CACHED RAW ALIAS POINTER against the guest's own
         * read of the same dword through `g_xbox_mem_offset`. Those are two genuinely different
         * address computations for one physical dword, which is the property that caught the
         * byte-order trap. A disagreement there is an instrument bug and fails closed. */
        uint32_t read_va = (uint32_t)(base + (a2h_slotw_slot_va()
                                              & (XBOX_A2H_SLOTW_PAGE_SIZE - 1)));
        A2H_SLOTW_INC64(&L->loss.cross_checks);
        if (read_va != s_a2h_slotw_pending_fault) {
            InterlockedIncrement((volatile LONG *)&L->cross_mismatch);
            A2H_SLOTW_INC64(&L->loss.cross_mismatch);
            A2H_SLOTW_SET64(&L->loss.overflow, 1);
            fprintf(stderr, "  [A2HSLOTW] CROSS-VALIDATION MISMATCH: the OS reported the fault at"
                            " %08X but this facility read the post-value at %08X (alias=%u"
                            " slot=%08X) -- INSTRUMENT BUG, failing closed\n",
                    s_a2h_slotw_pending_fault, read_va, alias, a2h_slotw_slot_va());
            fflush(stderr);
        } else {
            InterlockedIncrement((volatile LONG *)&L->cross_checks);
        }
        L->last_slot_read_value = post;
        L->last_slot_read_seq = (uint32_t)g_a2h_slotw_seq;
        L->last_slot_read_alias = alias;
        /* The positive half of value fidelity: a facility that reported every store as a no-op would
         * leave this at 0, and a reader would then see that the value fields were never exercised
         * rather than trusting them. */
        if (post != s_a2h_slotw_pending_pre)
            L->last_slot_change_seen = 1;
        /* THE GUARD THAT MAKES HALF TWO SOUND: the slot-hit count AT the moment of this read. A
         * later reader that finds the count unchanged knows no slot write landed between this read
         * and its own, so the two cover the same value and must agree. */
        L->last_slot_read_hits = (uint32_t)L->loss.slot_hits;
        if (!a2h_slotw_publish(A2H_SLOTW_EV_STEP, alias, s_a2h_slotw_pending_slot,
                               read_va, s_a2h_slotw_pending_pre, post,
                               (uint64_t)ep->ContextRecord->Rip,
                               s_a2h_slotw_pending_range, s_a2h_slotw_pending_form)) {
            /* The step record is lost. The page is STILL closed below, because leaving it open would
             * convert a bounded loss into an unbounded one -- and the overflow latch already marks
             * every absence/order row invalid. */
        }
        /* ⚠ THE LAST-RECORDED SLOT WRITE VALUE, KEPT FOR THE COHERENCE GATE. It is updated ONLY
         * here, on a real slot-hit step, so it is the post-value of the LAST RECORDED slot write --
         * exactly the operand the gate compares against the terminal read. Recording it anywhere
         * else (or from a terminal read) would make the gate compare a value with itself. */
        L->coherence_last_write_value = post;
        L->coherence_last_write_seq = s_a2h_slotw_pending_seq;
    }

    /* RE-PROTECT. A failure here leaves the page OPEN, which is a coverage hole and is counted as
     * one: it is never silently treated as "no further writes".
     *
     * ⚠ THIS RUNS AFTER EVERY WRITE, SLOT OR NOT, AND THAT IS THE ACCEPTED SHAPE. `VirtualProtect`
     * is PAGE-GRANULAR and the slot sits at offset 0x62C of its page, so opening the page for a
     * non-slot write also makes the SLOT writable. The narrowing that would "leave RW after a
     * non-slot write" is therefore NOT implemented and must never be: ON-3 had 9077 non-slot writes,
     * so that optimization would have blinded the instrument after the first one. Every window is
     * exactly one instruction wide, and each one is TICK-LOGGED here so the archive carries how
     * much unprotected time the run contained instead of a reader having to assume it was zero. */
    if (VirtualProtect((LPVOID)base, XBOX_A2H_SLOTW_PAGE_SIZE, PAGE_READONLY, &old)) {
        A2H_SLOTW_INC64(&L->loss.rearm_ok);
        A2H_SLOTW_INC64(&L->loss.protected_intervals);
        L->window_close_ticks_last = (uint64_t)GetTickCount64();
        L->window_open = 0;
    } else {
        A2H_SLOTW_INC64(&L->loss.rearm_failed);
        InterlockedExchange((volatile LONG *)&L->loss.overflow, 1);
        /* THE WINDOW STAYS OPEN AND SAYS SO. Leaving `window_open` set is the difference between a
         * stated coverage hole and a silent one. */
    }
    InterlockedExchange((volatile LONG *)&g_a2h_slotw_step_owner, 0);
    s_a2h_slotw_pending_alias = 0;
    s_a2h_slotw_pending_slot = 0;
    s_a2h_slotw_pending_pre = 0;
    s_a2h_slotw_pending_seq = 0;
    s_a2h_slotw_pending_fault = 0;
    s_a2h_slotw_pending_range = 0;
    s_a2h_slotw_pending_form = 0;
    A2H_SLOTW_INC64(&L->loss.db_own_serviced);
}

static LONG CALLBACK a2h_slotw_veh(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    XboxA2hSlotwLedger *L = &g_xbox_a2h_slotw;

    if (!g_a2h_slotw_armed)
        return EXCEPTION_CONTINUE_SEARCH;

    /* ── THE #DB, BIT-EXACT ────────────────────────────────────────────────────────────────────
     *
     * Service EVERY pending owner exactly once, then clear each serviced bit. TF stays set while a
     * pending owner remains; when none remains, the pre-entry TF bit is restored EXACTLY -- cleared
     * only if it was originally 0, set only if it was originally 1. An unowned #DB is NEVER
     * consumed: this handler did not ask for it, and swallowing it would rob whichever component
     * did (AC'97's own trap among them). */
    if (code == EXCEPTION_SINGLE_STEP) {
        uint32_t pending = s_a2h_slotw_pending;

        if (!pending) {
            A2H_SLOTW_INC64(&L->loss.db_unowned);
            return EXCEPTION_CONTINUE_SEARCH;     /* NOT OURS. Do not consume it. */
        }

        /* OWN FIRST: post-value + RO re-protect, then release the own bit. The release recomputes TF
         * from what REMAINS pending, so a still-pending AC'97 owner keeps TF set. */
        if (pending & A2H_SLOTW_PEND_OWN) {
            a2h_slotw_service_own(ep);
            ep->ContextRecord->EFlags =
                a2h_slotw_release_pending(A2H_SLOTW_PEND_OWN, ep->ContextRecord->EFlags);
        }

        if (s_a2h_slotw_pending & A2H_SLOTW_PEND_AC97) {
            /* THE EXISTING AC97 INTERLOCK, INTEGRATED RATHER THAN DUPLICATED.
             *
             * ac97_write_veh() above owns the reset-bit work and the re-protection of its own page,
             * and it is reached through the SAME vectored chain. Its bit is deliberately NOT
             * cleared here: this handler passes the #DB on, ac97_write_veh() claims it (the pending
             * word is per-thread, so it is this thread's own step it sees), performs its reset-bit
             * work, re-protects, releases ITS bit and restores TF. Clearing the bit here would make
             * the AC'97 handler skip its work and strand its page open -- so "service every pending
             * owner exactly once" is enforced by each owner releasing only its OWN bit, in the
             * handler that actually does that owner's work. The AC'97 accounting is done THERE for
             * the same reason -- see the comment in ac97_write_veh. */
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* Only the own bit was pending; its release already restored the pre-entry TF exactly. */
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    /* ── THE WRITE FAULT ─────────────────────────────────────────────────────────────────────── */
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault;
        uint32_t alias, range_class, form, pre, pre_at_fault, slot_hit, slot_va, off, fault_off;
        DWORD old;

        /* `ExceptionInformation[0] == 1` is the belt-and-suspenders filter the preflight requires to
         * be PROVEN rather than assumed: 0 means read, 1 means write, 8 means execute. A read can
         * only fault here if something else removed the protection, and treating it as a write would
         * single-step a load and corrupt the ledger. It is proven by fixture, not trusted. */
        if (ep->ExceptionRecord->ExceptionInformation[0] != 1) {
            A2H_SLOTW_INC64(&L->loss.unexpected_exception);
            return EXCEPTION_CONTINUE_SEARCH;
        }
        fault = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        alias = a2h_slotw_owning_alias(fault);
        if (!alias)
            return EXCEPTION_CONTINUE_SEARCH;      /* not our page: never shadow another handler */

        A2H_SLOTW_INC64(&L->loss.relevant_av);

        slot_va = a2h_slotw_slot_va();
        off = slot_va & (XBOX_A2H_SLOTW_PAGE_SIZE - 1);
        slot_hit = (fault == (uintptr_t)g_a2h_slotw_pages[alias - 1] + off) ? 1u : 0u;
        if (slot_hit)
            A2H_SLOTW_INC64(&L->loss.slot_hits);
        else
            A2H_SLOTW_INC64(&L->loss.nonslot_writes);

        /* A SECOND THREAD faulting on an owned page while another is mid-step is a concurrent
         * overlapping writer: the page is open for the stepping thread, so this fault can only
         * happen because the protection was temporarily off. That is a coverage hole for any
         * absence row and is counted, never ignored. */
        if (InterlockedCompareExchange(&g_a2h_slotw_step_owner, 0, 0) != 0
                && (uint32_t)InterlockedCompareExchange(&g_a2h_slotw_step_owner, 0, 0)
                   != (uint32_t)GetCurrentThreadId()) {
            A2H_SLOTW_INC64(&L->loss.concurrent_overlap);
        }

        /* TWO DIFFERENT READS, TWO DIFFERENT MEANINGS -- AND BOTH ARE NEEDED.
         *
         * `pre` is THE SLOT'S value at the fault, read at the faulting alias's page base plus the
         * SLOT's offset. `off` derives from `slot_va`, NOT from `fault`, so `pre` is alias-consistent
         * and is the slot's pre-write value whichever alias the store arrived through. That is by
         * design (Q3(a)) and is not changed here.
         *
         * `pre_at_fault` is the value at the FAULTING ADDRESS. It is a different field with a
         * different meaning, and it is what the first-touch census records -- "what was at the
         * address this fill touched, before the fill touched it". Conflating the two is the naming
         * ambiguity the Q3(a) record had to resolve, so they are kept apart by name and by record. */
        pre = *(volatile uint32_t *)((uintptr_t)g_a2h_slotw_pages[alias - 1] + off);
        fault_off = (uint32_t)(fault & (XBOX_A2H_SLOTW_PAGE_SIZE - 1));
        pre_at_fault = *(volatile uint32_t *)((uintptr_t)g_a2h_slotw_pages[alias - 1] + fault_off);
        /* ⚠ THE CLASSIFICATION IS BY RANGE, IN THE NATIVE DOMAIN, AND IT READS NO INSTRUCTION
         * BYTES TO DECIDE. See a2h_slotw_classify_rip(). The optional native-disasm corroboration
         * is taken here too, and it NEVER gates anything -- it is recorded beside the range class so
         * a reader can check the two against each other. */
        range_class = a2h_slotw_classify_rip((uint64_t)ep->ContextRecord->Rip);
        form = a2h_slotw_corroborate_form((uint64_t)ep->ContextRecord->Rip);
        switch (range_class) {
        case XBOX_A2H_SLOTW_RANGE_GAME_MODULE:  A2H_SLOTW_INC64(&L->loss.range_game); break;
        case XBOX_A2H_SLOTW_RANGE_TOOLKIT_HOST: A2H_SLOTW_INC64(&L->loss.range_host); break;
        default:
            /* ⚠ UNKNOWN IS INFRA FAILURE, NOT A WARNING. A RIP this facility cannot place must stop
             * the packet rather than be attributed, so it is counted, latched, and printed.
             *
             * ⚠ AND THE RIP ITSELF IS SAMPLED, because the count alone cannot tell an instrument bug
             * inside the recompiled extent from a writer that is genuinely outside the image -- and
             * those demand opposite responses. See XBOX_A2H_SLOTW_UNKNOWN_SAMPLE_MAX. */
            A2H_SLOTW_INC64(&L->loss.range_unknown);
            InterlockedExchange((volatile LONG *)&L->loss.overflow, 1);
            a2h_slotw_note_unknown_rip((uint64_t)ep->ContextRecord->Rip);
            fprintf(stderr, "  [A2HSLOTW] RIP %016llX is in NO known range (recomp_valid=%u"
                            " recomp_count=%u recomp=%016llX..%016llX image=%016llX..%016llX)"
                            " -- INFRA FAILURE\n",
                    (unsigned long long)ep->ContextRecord->Rip, s_a2h_slotw_recomp_valid,
                    s_a2h_slotw_recomp_count,
                    (unsigned long long)s_a2h_slotw_recomp_lo,
                    (unsigned long long)s_a2h_slotw_recomp_hi,
                    (unsigned long long)s_a2h_slotw_image_lo,
                    (unsigned long long)s_a2h_slotw_image_hi);
            fflush(stderr);
            break;
        }
        if (!s_a2h_slotw_recomp_valid) {
            A2H_SLOTW_INC64(&L->loss.range_unavailable);
        }
        switch (form) {
        case XBOX_A2H_SLOTW_FORM_STORE:     A2H_SLOTW_INC64(&L->loss.form_store); break;
        case XBOX_A2H_SLOTW_FORM_NOT_STORE: A2H_SLOTW_INC64(&L->loss.form_not_store); break;
        default:                            A2H_SLOTW_INC64(&L->loss.form_undecoded); break;
        }

        /* ── THE PACKET'S OWN DESIGN: TRAFFIC IS COUNTED, SLOT BYTES ARE RECORDED ────────────────
         *
         * A write to the watched page that is NOT the slot cannot change the slot. ON trial 1
         * recorded every such write AND its step, so 129 page writes consumed 258 of 256 records,
         * the bounded array overflowed, and the fail-closed path -- correctly refusing to convert a
         * lost write into a silent absence -- ended the run on ordinary traffic volume.
         *
         * The repair is the packet's design, not a larger buffer: non-slot writes move the uncapped
         * counters and at most ONE first-touch census record per distinct address, and publish no
         * per-write record at all. A page-write stream of any length now consumes no records once
         * the page has been walked. */
        if (slot_hit) {
            /* PUBLISH FIRST. The record is written BEFORE the page is opened: a page opened without
             * a published record could absorb a concurrent write that is then neither recorded nor
             * the first touch. If publication fails the page is left CLOSED and the fault
             * propagates. THIS IS THE FAIL-CLOSED PATH, AND IT IS UNCHANGED -- what changed is that
             * only writes that can actually change the slot can reach it. */
            if (!a2h_slotw_publish(A2H_SLOTW_EV_WRITE, alias, slot_hit, (uint32_t)fault, pre, 0,
                                   (uint64_t)ep->ContextRecord->Rip, range_class, form)) {
                A2H_SLOTW_INC64(&L->loss.publish_failed);
                fprintf(stderr, "  [A2HSLOTW] write NOT published alias=%u fault=%p -- page left"
                                " CLOSED (coverage failure)\n", alias, (void *)fault);
                fflush(stderr);
                return EXCEPTION_CONTINUE_SEARCH;
            }
        } else {
            a2h_slotw_note_traffic(alias, (uint32_t)fault, fault_off, pre_at_fault, pre,
                                   (uint64_t)ep->ContextRecord->Rip, range_class, form);
        }

        /* ── THE REQUIRED POSITIVE CONTROL, RE-EXPRESSED IN THE NATIVE DOMAIN ────────────────────
         *
         * IT CAN NO LONGER TEST AN ENCODING, because there is no encoding to test: in recompiled
         * code the faulting instruction is generated C, and the guest's `89 81 2C 24 00 00` can
         * never appear at a native RIP. The old test `enc == ENC_MODRM` was therefore unsatisfiable
         * by construction, which is why ON-3's slot event recorded `enc=3` -- the control's
         * condition was simply never true, on any run, for any writer.
         *
         * THE CONTROL IS NOW: "a store LANDED ON THE DERIVED SLOT, and the faulting RIP is INSIDE
         * THE RECOMPILED MODULE." That is the same claim in the only domain where it can be checked:
         * the store reached the re-derived slot address (the slot_hit test, which is an ADDRESS
         * comparison against `arm_slot` and has not changed), and it came from the title's own
         * recompiled code rather than from the toolkit's runtime, a host callback, or anywhere else.
         *
         * ⚠ THE PRE-VALUE IS STILL NOT PART OF THE TEST. Requiring `pre == 0` was a defect: it made
         * the control depend on nothing having touched the slot earlier, so a run in which the
         * control DID fire could still be reported as INFRA FAILURE. The VALUE is recorded in the
         * step record's post_value and compared against 0x0015F9D0 OFFLINE.
         *
         * ⚠ AND THE NATIVE-DISASM CORROBORATION IS DELIBERATELY NOT PART OF IT EITHER. The control
         * keys on the RANGE CLASS alone, so a partial hand-written decoder can never fail a run the
         * range classifier placed correctly. The corroboration is recorded beside the class for a
         * reader to check, which is what "optional corroboration" means. */
        if (slot_hit && range_class == XBOX_A2H_SLOTW_RANGE_GAME_MODULE) {
            A2H_SLOTW_INC64(&L->loss.installer_control_hits);
        }

        if (!VirtualProtect((LPVOID)g_a2h_slotw_pages[alias - 1], XBOX_A2H_SLOTW_PAGE_SIZE,
                            PAGE_READWRITE, &old)) {
            A2H_SLOTW_INC64(&L->loss.protect_failed);
            InterlockedExchange((volatile LONG *)&L->loss.overflow, 1);
            return EXCEPTION_CONTINUE_SEARCH;
        }
        A2H_SLOTW_INC64(&L->loss.unprotected_intervals);
        /* THE WINDOW IS OPEN, AND IT IS LOGGED RATHER THAN ASSUMED AWAY. It closes in the step
         * handler below; `window_open` is what makes "a window is open right now" visible to a
         * reader instead of inferable only from the counters. */
        L->window_open_count++;
        L->window_open_ticks_last = (uint64_t)GetTickCount64();
        L->window_open = 1;

        a2h_slotw_take_pending(A2H_SLOTW_PEND_OWN, ep->ContextRecord->EFlags);
        s_a2h_slotw_pending_alias = alias;
        s_a2h_slotw_pending_slot = slot_hit;
        s_a2h_slotw_pending_pre = pre;
        s_a2h_slotw_pending_seq = (uint32_t)g_a2h_slotw_seq;
        s_a2h_slotw_pending_fault = (uint32_t)fault;
        s_a2h_slotw_pending_range = range_class;
        s_a2h_slotw_pending_form = form;
        InterlockedExchange(&g_a2h_slotw_step_owner, (LONG)GetCurrentThreadId());
        ep->ContextRecord->EFlags |= 0x100u;      /* step exactly the one faulting store */
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    /* Everything else -- including a #DB from a component this watch does not own -- is somebody
     * else's. Counted so a reader can see the stream was read, then passed on untouched. */
    A2H_SLOTW_INC64(&L->loss.unexpected_exception);
    return EXCEPTION_CONTINUE_SEARCH;
}

uint32_t xbox_A2hSlotWatchArm(void)
{
    XboxA2hSlotwLedger *L = &g_xbox_a2h_slotw;
    uint32_t base, slot, off, mapped = 0, protected_ = 0;
    uint32_t i;

    if (!a2h_slotw_on() || g_a2h_slotw_armed)
        return g_a2h_slotw_armed;
    if (!g_memory_base)
        return 0;

    memset(L, 0, sizeof(*L));
    L->magic = A2H_SLOTW_MAGIC;
    L->version = XBOX_A2H_SLOTW_VERSION;
    L->size = (uint32_t)sizeof(*L);

    /* ── PUBLISH THE RANGES THE CLASSIFIER WILL USE, SO THE ARCHIVE CARRIES THEM ─────────────────
     *
     * ⚠ THE BOUNDS ARE PUBLISHED HERE, BEFORE ANY FAULT CAN BE CLASSIFIED, because a classification
     * whose inputs are not in the archive cannot be checked by a reader. They come from the REAL
     * artifact: the recompiled bound from the embedder's `recomp_lookup` probes and the image bound
     * from this process's own PE headers.
     *
     * ⚠ A MISSING RECOMPILED BOUND IS RECORDED, NOT SUBSTITUTED. If the embedder never published
     * one, every RIP will classify UNKNOWN and `loss.range_unavailable` will move -- which is the
     * packet's INFRA FAILURE, and is exactly right: a bound this facility invented would make the
     * control satisfiable by an unrelated writer. */
    a2h_slotw_read_image_bounds();
    L->recomp_lo = s_a2h_slotw_recomp_lo;
    L->recomp_hi = s_a2h_slotw_recomp_hi;
    L->recomp_bound_valid = s_a2h_slotw_recomp_valid;
    L->recomp_bound_probes = s_a2h_slotw_recomp_probes;
    L->recomp_start_count = s_a2h_slotw_recomp_count;
    L->recomp_start_overflow = s_a2h_slotw_recomp_overflow;
    L->unknown_rip_count = 0;
    memset(L->unknown_rips, 0, sizeof(L->unknown_rips));
    memset(L->unknown_rip_bases, 0, sizeof(L->unknown_rip_bases));
    memset(L->unknown_same_image, 0, sizeof(L->unknown_same_image));
    memset(L->unknown_reserved, 0, sizeof(L->unknown_reserved));
    L->image_lo = s_a2h_slotw_image_lo;
    L->image_hi = s_a2h_slotw_image_hi;
    /* ⚠ THE SET ITSELF GOES INTO THE LEDGER, so the archive carries the classifier's ACTUAL input and
     * a reader can re-run any classification by hand instead of trusting the counts. Without this a
     * reader could see "game=1 host=9077" and have no way to check a single one of them. */
    {
        uint32_t i;
        for (i = 0; i < s_a2h_slotw_recomp_count && i < XBOX_A2H_SLOTW_RECOMP_ARCHIVE; i++)
            L->recomp_starts[i] = s_a2h_slotw_recomp_starts[i];
    }

    /* READ THE POINTER. It is a pointer, not the object: the device is allocated at runtime, so the
     * slot VA does not exist before this read and MUST NOT be preselected. */
    base = *(volatile uint32_t *)((uintptr_t)XBOX_A2H_SLOTW_DEVICE_PTR + g_xbox_mem_offset);

    /* CHECKED 32-bit addition. A base that is not inside the mapped RAM range cannot name the
     * device, and wrapping the addition would silently protect an unrelated page -- the packet's
     * "changed base => re-scope, never silently compare" rule applied at the ARM step. */
    {
        size_t ram = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;
        if (base < XBOX_BASE_ADDRESS || (size_t)base >= ram)
            base = 0;
    }
    if (!base) {
        L->arm_reason = XBOX_A2H_SLOTW_ARM_NOT_YET;
        return 0;
    }
    slot = base + XBOX_A2H_SLOTW_SLOT_OFFSET;
    if (slot < base) {   /* 32-bit overflow of the checked addition */
        fprintf(stderr, "  [A2HSLOTW] ARM refused: base 0x%08X + 0x%X wraps 32 bits\n",
                base, XBOX_A2H_SLOTW_SLOT_OFFSET);
        fflush(stderr);
        L->arm_reason = XBOX_A2H_SLOTW_ARM_COVERAGE;
        return 0;
    }
    off = slot & (XBOX_A2H_SLOTW_PAGE_SIZE - 1);

    L->arm_base = base;
    L->arm_slot = slot;
    L->page_offset = off;
    L->arm_ticks = (uint64_t)GetTickCount64();

    /* Registered at the same priority as the game's own VEH and AC'97's (both 1). This handler
     * claims only its own pages and its own #DB bits and returns EXCEPTION_CONTINUE_SEARCH for
     * everything else, so it neither shadows nor reorders them. */
    g_a2h_slotw_veh = AddVectoredExceptionHandler(1, a2h_slotw_veh);
    if (!g_a2h_slotw_veh) {
        fprintf(stderr, "  [A2HSLOTW] ARM FAILED: no VEH; nothing protected\n");
        fflush(stderr);
        L->arm_base = 0; L->arm_slot = 0;
        return 0;
    }

    /* Alias 0 is the canonical view: guest VA V is at g_memory_base + (V - 0x10000). Alias m+1 is
     * mirror view m, whose guest window is (m+1)*map_size. ALL of them are the SAME physical page,
     * so a store through any one of them changes the slot -- protecting only the canonical page
     * would be structurally blind to the other 28, which is the exact mechanism that hid Halo's
     * fs:[4] corruption. */
    for (i = 0; i < XBOX_A2H_SLOTW_PAGES_MAX; i++) {
        uintptr_t host;
        DWORD old;
        uint64_t guest_lo, guest_hi, gva;
        size_t map_size = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;

        if (i == 0) {
            /* ⚠⚠ THE CANONICAL ALIAS IS `va + g_memory_offset`, AND THE OLD FORMULA HERE WAS WRONG.
             *
             * This used to read `g_memory_base + (slot - XBOX_BASE_ADDRESS)`, i.e. it assumed
             * g_memory_base was the host address OF GUEST VA 0x10000. It is not: g_memory_base is the
             * host address of XBOX_MAP_START (0), because `g_memory_offset = g_memory_base -
             * XBOX_MAP_START`. The loader's own log states the ground truth unambiguously --
             * `XBE header: 2440 bytes at 0x0000000000020000 (Xbox VA 0x00010000)` -- so guest VA V
             * lives at host `V + g_memory_offset`, which is exactly what `XBOX_PTR` computes for the
             * guest's own `MEM32()`.
             *
             * MEASURED CONSEQUENCE OF THE OLD FORMULA: with g_memory_offset = 0x10000 the canonical
             * alias was protected 64 KiB BELOW the slot -- guest page 0x0018D000 instead of
             * 0x0019D000. The watch therefore faulted on, read, and recorded a page the title never
             * writes the slot to. It could not have reported a slot hit, and its `pre`/`post` values
             * described a different dword. The 28 mirror aliases were already correct, which is why
             * the coverage claim looked healthy while the canonical view -- the one a sub-64 MB guest
             * VA actually uses -- was pointed at the wrong page.
             *
             * The Q3(c) address-identity fixture is what caught this, and it is the reason that
             * fixture exists: the instrument and the guest must be shown to read the SAME dword, not
             * assumed to. Nothing about the TARGET changes -- the slot is still
             * `MEM32(0x19DCE0) + 0x242C`, still derived by checked addition, still slot-keyed. This
             * only makes the page the watch protects the page that VA is actually in. */
            gva = (uint64_t)slot;
            host = (uintptr_t)slot + (uintptr_t)g_memory_offset;
        } else {
            if (!g_mirror_views[i - 1]) continue;      /* not mapped: accounted, not assumed */
            guest_lo = (uint64_t)i * map_size;
            guest_hi = guest_lo + map_size;
            gva = guest_lo + ((uint64_t)slot % map_size);
            if (gva < guest_lo || gva >= guest_hi) continue;
            host = (uintptr_t)g_mirror_views[i - 1] + (uintptr_t)(gva - guest_lo);
        }
        host &= ~(uintptr_t)(XBOX_A2H_SLOTW_PAGE_SIZE - 1);

        g_a2h_slotw_pages[i] = (void *)host;
        L->mapped_mask |= (1u << i);
        mapped++;
        if (VirtualProtect((LPVOID)host, XBOX_A2H_SLOTW_PAGE_SIZE, PAGE_READONLY, &old)) {
            L->protect_mask |= (1u << i);
            protected_++;
            A2H_SLOTW_INC64(&L->loss.protected_intervals);
        } else {
            /* A MAPPED ALIAS THAT CANNOT BE PROTECTED IS A COVERAGE FAILURE, NOT A WARNING: a
             * store through it would change the slot invisibly. */
            fprintf(stderr, "  [A2HSLOTW] alias %u page at %p NOT protected (error %lu)"
                            " -- coverage failure\n", i, (void *)host, GetLastError());
            g_a2h_slotw_pages[i] = NULL;
            A2H_SLOTW_INC64(&L->loss.protect_failed);
            InterlockedExchange((volatile LONG *)&L->loss.overflow, 1);
        }
    }
    L->alias_count = mapped;
    L->protected_count = protected_;

    /* REFUSE TO OPEN ON FAILED PUBLICATION. Not every mapped alias protected means the watch is
     * structurally incomplete, so it is disarmed rather than left half-armed and read as if whole. */
    if (mapped == 0 || mapped != protected_) {
        fprintf(stderr, "  [A2HSLOTW] ARM REFUSED: mapped=%u protected=%u -- disarming\n",
                mapped, protected_);
        fflush(stderr);
        xbox_A2hSlotWatchDisarm();
        return 0;
    }

    g_a2h_slotw_armed = 1;
    L->armed = 1;
    L->arm_reason = XBOX_A2H_SLOTW_ARM_OK;
    fprintf(stderr, "  [A2HSLOTW] armed base=%08X slot=%08X page=%08X off=%03X aliases=%u/%u"
                    " mask=%08X\n",
            base, slot, slot & ~(XBOX_A2H_SLOTW_PAGE_SIZE - 1), off, protected_, mapped,
            L->protect_mask);
    fflush(stderr);
    return 1;
}

void xbox_A2hSlotWatchDisarm(void)
{
    uint32_t i;
    int restored = 0;

    if (!g_a2h_slotw_armed && !g_a2h_slotw_veh)
        return;
    g_a2h_slotw_armed = 0;
    for (i = 0; i < XBOX_A2H_SLOTW_PAGES_MAX; i++) {
        DWORD old;
        if (!g_a2h_slotw_pages[i]) continue;
        if (VirtualProtect(g_a2h_slotw_pages[i], XBOX_A2H_SLOTW_PAGE_SIZE,
                           PAGE_READWRITE, &old)) {
            restored++;
        } else {
            fprintf(stderr, "  [A2HSLOTW] alias %u page NOT restored (error %lu)\n",
                    i, GetLastError());
        }
        g_a2h_slotw_pages[i] = NULL;
    }
    if (g_a2h_slotw_veh) {
        RemoveVectoredExceptionHandler(g_a2h_slotw_veh);
        g_a2h_slotw_veh = NULL;
    }
    fprintf(stderr, "  [A2HSLOTW] disarmed restored=%d\n", restored);
    fflush(stderr);
}

/* ── THE TERMINAL-VALUE COHERENCE GATE'S DECISION RULE, IN ONE PLACE ─────────────────────────────
 *
 * ⚠ THIS IS THE ONLY PLACE THE VERDICT IS DECIDED, AND IT IS A FUNCTION SO THE FIXTURE CAN DRIVE IT
 * RATHER THAN REIMPLEMENT IT. A fixture that restated this rule would prove that the fixture and the
 * handler agree about a rule neither of them had tested -- which is the same class of error as the
 * void encoding classifier: a test asserting a property of an input the production path never sees.
 * Extracting the rule makes the fixture's arms assertions about PRODUCTION behaviour.
 *
 * The three-way outcome is the whole gate:
 *
 *   no terminal read at all          -> NO_TERMINAL     (nothing to compare; never a claim)
 *   no RECORDED slot write (seq==0)  -> NOT_COMPARABLE  (the terminal stands ALONE; never a claim)
 *   values DIFFER                    -> MISMATCH        (=> UNKNOWN, fail closed; latch set)
 *   values MATCH                     -> COHERENT        (removes the mismatch objection from a
 *                                                        RECORDED positive ONLY -- never an
 *                                                        absence proof, because a same-value
 *                                                        racing write in an open window would be
 *                                                        invisible and a match cannot exclude it)
 *
 * ⚠ `coherence_mismatch` IS A CUMULATIVE LATCH, NOT A CURRENT-STATE FLAG. It is never cleared, so a
 * run that mismatched once and later matched cannot be read as coherent throughout. */
static void a2h_slotw_decide_coherence(XboxA2hSlotwLedger *L)
{
    if (!L->terminal_seen) {
        L->coherence_verdict = XBOX_A2H_SLOTW_COH_NO_TERMINAL;
    } else if (L->coherence_last_write_seq == 0) {
        L->coherence_verdict = XBOX_A2H_SLOTW_COH_NOT_COMPARABLE;
    } else if (L->coherence_last_write_value != L->coherence_terminal_value) {
        L->coherence_verdict = XBOX_A2H_SLOTW_COH_MISMATCH;
        A2H_SLOTW_INC64(&L->loss.coherence_mismatch);
    } else {
        L->coherence_verdict = XBOX_A2H_SLOTW_COH_COHERENT;
    }
}

/* TERMINAL: re-derive the slot from a FRESH read of the pointer. A moved base is a RE-SCOPE -- the
 * terminal slot is reported, the difference is latched, and nothing compares the new VA against the
 * old one as though they were the same object. */
void xbox_A2hSlotWatchTerminal(uint32_t target)
{
    XboxA2hSlotwLedger *L = &g_xbox_a2h_slotw;
    uint32_t base, slot, base_ok;
    size_t ram = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;

    if (!L->magic)
        return;
    base = *(volatile uint32_t *)((uintptr_t)XBOX_A2H_SLOTW_DEVICE_PTR + g_xbox_mem_offset);
    /* IS THE BASE STILL A PLAUSIBLE GUEST POINTER? The same checked test ARM applies, so a base
     * that is no longer a pointer is reported as such instead of being run through the addition and
     * compared as though it named an object. */
    base_ok = (base >= XBOX_BASE_ADDRESS && (size_t)base < ram) ? 1u : 0u;
    slot = base_ok ? base + XBOX_A2H_SLOTW_SLOT_OFFSET : 0;

    L->term_base = base;
    L->term_base_ok = base_ok;
    L->term_slot = slot;
    L->slot_stable = (base_ok && slot == L->arm_slot) ? 1u : 0u;
    /* A base that MOVED and a base that STOPPED BEING A POINTER are both re-scope conditions -- the
     * old VA is not compared against the new one either way -- but only the first is a device move,
     * and `base_changed` is set for both so a reader cannot read the second as a clean comparison. */
    if (!L->slot_stable)
        A2H_SLOTW_SET64(&L->loss.base_changed, 1);
    L->terminal_target = target;
    L->terminal_seen = 1;
    L->terminal_ticks = (uint64_t)GetTickCount64();

    /* ── Q3(c), HALF TWO: THE OVERLAP CONTROL, AND IT IS THE ONE THAT MATTERS ─────────────────────
     *
     * TWO INDEPENDENT ADDRESS COMPUTATIONS, ONE PHYSICAL DWORD, READ BACK TO BACK:
     *
     *   (a) THIS FACILITY'S CACHED ALIAS READ -- `g_a2h_slotw_pages[0] + (arm_slot & 0xFFF)`. That
     *       base was captured ONCE at ARM and has been used for every fault record and every step
     *       read since. If it is stale, wrong, or was computed from the wrong alias, every value the
     *       instrument published is wrong and nothing else in the ledger would show it.
     *   (b) THE GUEST'S OWN TRANSLATION -- `arm_slot + g_memory_offset`, which is exactly what
     *       `MEM32(slot_va)` computes in the generated code (`XBOX_PTR`), re-derived here rather
     *       than cached.
     *
     * THESE MUST AGREE. A disagreement is an INSTRUMENT BUG: it is counted, the overflow latch is
     * set (invalidating absence/order rows), and it is printed. This is the check the Advisor
     * requires, and it is the same shape as the control that caught the byte-order trap.
     *
     * THE QUIET-WINDOW GUARD APPLIES HERE TOO. A slot write landing between the two reads would make
     * them legitimately differ, so the slot-hit count is sampled either side and the comparison is
     * made only when it did not move; otherwise the pair is counted SKIPPED, never as agreement. */
    if (g_a2h_slotw_armed && g_a2h_slotw_pages[0] && L->slot_stable) {
        uint32_t hits_before = (uint32_t)L->loss.slot_hits;
        uint32_t via_alias = *(volatile uint32_t *)((uintptr_t)g_a2h_slotw_pages[0]
                                  + (L->arm_slot & (XBOX_A2H_SLOTW_PAGE_SIZE - 1)));
        uint32_t via_guest = *(volatile uint32_t *)((uintptr_t)L->arm_slot
                                  + (uintptr_t)g_memory_offset);
        uint32_t hits_after = (uint32_t)L->loss.slot_hits;

        L->terminal_alias_value = via_alias;
        L->terminal_guest_value = via_guest;
        if (hits_before != hits_after) {
            A2H_SLOTW_INC64(&L->loss.cross_skipped);
            L->terminal_cross_ok = 0;      /* NOT compared: the window was not quiet */
        } else {
            A2H_SLOTW_INC64(&L->loss.cross_checks);
            InterlockedIncrement((volatile LONG *)&L->cross_checks);
            if (via_alias != via_guest) {
                InterlockedIncrement((volatile LONG *)&L->cross_mismatch);
                A2H_SLOTW_INC64(&L->loss.cross_mismatch);
                A2H_SLOTW_SET64(&L->loss.overflow, 1);
                L->terminal_cross_ok = 0;
                fprintf(stderr, "  [A2HSLOTW] CROSS-VALIDATION MISMATCH at TERMINAL: the cached"
                                " alias read gives %08X but the guest's own translation of %08X gives"
                                " %08X -- INSTRUMENT BUG, failing closed\n",
                        via_alias, L->arm_slot, via_guest);
                fflush(stderr);
            } else {
                L->terminal_cross_ok = 1;
            }
        }
        fprintf(stderr, "  [A2HSLOTW] cross-validation terminal slot=%08X alias=%08X guest=%08X"
                        " agree=%u hits_quiet=%u\n",
                L->arm_slot, via_alias, via_guest, L->terminal_cross_ok,
                hits_before == hits_after ? 1u : 0u);
        fflush(stderr);
    }

    /* ── THE TERMINAL-VALUE COHERENCE GATE (REQUIRED) ─────────────────────────────────────────────
     *
     * ⚠ THIS IS THE GATE THAT MAKES THE ACCEPTED SHAPE SOUND, AND IT IS NOT OPTIONAL.
     *
     * `VirtualProtect` is PAGE-GRANULAR and the slot sits at offset 0x62C of its page, so opening the
     * page to single-step a NON-SLOT write also makes the SLOT writable for that one instruction.
     * A racing write inside such a window would be invisible to this facility. Nothing in scope can
     * close that window -- which is exactly why the accepted shape is "re-arm after EVERY write and
     * accept the window as a stated limit" rather than the leave-RW narrowing that would blind the
     * instrument after the first traffic write.
     *
     * SO THE WINDOW IS MADE TO FAIL CLOSED:
     *
     *     LAST-RECORDED slot write value   vs   TERMINAL slot read
     *     MISMATCH  =>  UNKNOWN.   Never a claim.
     *
     * "Windows threaten only unrecorded writes; recorded positives stand; coherence converts the
     * residual same-value race to fail-closed."
     *
     * ⚠ THE WORKED EXAMPLE IS ON-3'S OWN NUMBERS: last-recorded `0x0015F9D0` against terminal
     * `0x001D5078` MISMATCHES, so that run is `UNKNOWN`. The mismatch is the GATE WORKING -- it is
     * not a defect in the instrument and it must not be "repaired".
     *
     * ⚠ AND THE GATE IS ONE-DIRECTIONAL, WHICH IS THE HALF THAT IS EASY TO GET WRONG. A MATCH
     * removes the mismatch objection from a RECORDED positive, and NOTHING MORE: it cannot prove
     * the absence of an unrecorded same-value racing write, so a coherent pair never yields an
     * absence or exclusivity row. `coherence_verdict` says COHERENT, never "clean".
     *
     * ⚠ AND "NO RECORDED WRITE" IS NOT COHERENCE EITHER. With `coherence_last_write_seq == 0` there
     * is nothing to compare, so the verdict is NOT_COMPARABLE and the terminal read stands alone --
     * which is precisely the state in which the packet forbids attributing anything from the
     * terminal. */
    L->coherence_terminal_value = L->terminal_guest_value;
    a2h_slotw_decide_coherence(L);
    fprintf(stderr, "  [A2HSLOTW] coherence verdict=%u last_write=%08X(seq=%u) terminal=%08X"
                    " %s\n", L->coherence_verdict, L->coherence_last_write_value,
            L->coherence_last_write_seq, L->coherence_terminal_value,
            L->coherence_verdict == XBOX_A2H_SLOTW_COH_MISMATCH
                ? "MISMATCH => UNKNOWN (the gate working, never a claim)"
                : (L->coherence_verdict == XBOX_A2H_SLOTW_COH_COHERENT
                       ? "match (removes the mismatch objection from a RECORDED positive only;"
                         " NOT an absence proof)"
                       : "not comparable: no recorded slot write to compare"));
    fflush(stderr);

    fprintf(stderr, "  [A2HSLOTW] terminal base=%08X base_ok=%u slot=%08X target=%08X stable=%u"
                    " reads=%u fourth=%u(%08X)\n",
            base, base_ok, slot, target, L->slot_stable, L->read_count, L->fourth_reached,
            L->fourth_value);
    fprintf(stderr, "  [A2HSLOTW] ranges recomp=%016llX..%016llX(valid=%u probes=%u)"
                    " image=%016llX..%016llX game=%llu host=%llu unknown=%llu unavailable=%llu\n",
            (unsigned long long)L->recomp_lo, (unsigned long long)L->recomp_hi,
            L->recomp_bound_valid, L->recomp_bound_probes,
            (unsigned long long)L->image_lo, (unsigned long long)L->image_hi,
            (unsigned long long)L->loss.range_game, (unsigned long long)L->loss.range_host,
            (unsigned long long)L->loss.range_unknown,
            (unsigned long long)L->loss.range_unavailable);
    fflush(stderr);
}

/* ── THE PUBLIC RANGE / COHERENCE SEAMS ──────────────────────────────────────────────────────────
 *
 * These exist so Exp0 can be run OFFLINE against the REAL loaded modules rather than only from
 * inside a live fault. Exp0 requires known RIPs to be classified against real image bounds; a proof
 * that could only run during a fault would not be offline, and a proof run against a synthetic range
 * would prove nothing about the ranges the classifier actually uses. */
uint32_t xbox_A2hSlotWatchClassifyRip(uint64_t rip)
{
    return a2h_slotw_classify_rip(rip);
}

void xbox_A2hSlotWatchRangeBounds(uint64_t *recomp_lo, uint64_t *recomp_hi, uint32_t *valid,
                                  uint64_t *image_lo, uint64_t *image_hi)
{
    a2h_slotw_read_image_bounds();
    if (recomp_lo) *recomp_lo = s_a2h_slotw_recomp_lo;
    if (recomp_hi) *recomp_hi = s_a2h_slotw_recomp_hi;
    if (valid) *valid = s_a2h_slotw_recomp_valid;
    if (image_lo) *image_lo = s_a2h_slotw_image_lo;
    if (image_hi) *image_hi = s_a2h_slotw_image_hi;
}

/* The published recompiled-function-start COUNT, so a fixture can assert the set was recorded whole
 * rather than truncated. */
uint32_t xbox_A2hSlotWatchRecompStartCount(void)
{
    return s_a2h_slotw_recomp_count;
}

/* Record an unplaceable RIP through the REAL sampler, so a fixture can prove the sample is a SET of
 * distinct RIPs and that its allocation-base attribution works -- rather than leaving new
 * diagnostic code untested. */
void xbox_A2hSlotWatchFixtureNoteUnknownRip(uint64_t rip)
{
    a2h_slotw_note_unknown_rip(rip);
}

/* The embedder's own overflow report. See the declaration above for why it is a separate latch. */
void xbox_A2hSlotWatchNoteRecompOverflow(void)
{
    s_a2h_slotw_recomp_overflow = 1;
}

int xbox_A2hSlotWatchCoherence(uint32_t *verdict, uint32_t *last_write_value,
                               uint32_t *last_write_seq, uint32_t *terminal_value)
{
    XboxA2hSlotwLedger *L = &g_xbox_a2h_slotw;
    if (!L->magic || !L->terminal_seen)
        return 0;
    if (verdict) *verdict = L->coherence_verdict;
    if (last_write_value) *last_write_value = L->coherence_last_write_value;
    if (last_write_seq) *last_write_seq = L->coherence_last_write_seq;
    if (terminal_value) *terminal_value = L->coherence_terminal_value;
    return 1;
}

/* THE FOURTH READ. `0x00193E62 mov eax,[esi+0x1C4]` produces the value that `0x00193EB5 call eax`
 * consumes. Counting is UNCAPPED, so "the fourth read" is a fact about the stream and not about a
 * bounded sample. At read #4 the value is latched together with the ORDERED EVENT ID of the last
 * slot-hit write at or before it -- an event id, not a log timestamp, so the tie to the installer
 * and to every slot write survives log truncation and interleaving. */
void xbox_A2hSlotWatchNoteFourthRead(uint32_t value, uint32_t read_index)
{
    XboxA2hSlotwLedger *L = &g_xbox_a2h_slotw;
    uint32_t i, best = 0;
    uint32_t seq;

    if (!L->magic || !g_a2h_slotw_armed)
        return;
    /* The INDEX is supplied by the caller from the guest's own counter and is authoritative; this
     * handler's own count is a cross-check, not the source, because the read site is reached from a
     * poll loop whose iterations this side cannot bound. */
    L->read_count = read_index ? read_index : (L->read_count + 1);
    /* EVERY instrumented read is counted, not only the fourth: `read_samples` is the read-side
     * coverage denominator, and a counter that only moved on the one read it latched could not
     * distinguish "one read was instrumented" from "the path was never reached". */
    A2H_SLOTW_INC64(&L->loss.read_samples);
    seq = a2h_slotw_next_seq();
    a2h_slotw_publish(A2H_SLOTW_EV_READ, 0, 0, 0, value, 0, 0,
                      XBOX_A2H_SLOTW_RANGE_UNKNOWN, XBOX_A2H_SLOTW_FORM_UNKNOWN);
    if (L->read_count != 4 || L->fourth_reached)
        return;

    /* The last slot-hit write at or before this read, by EVENT ID. A write that arrives after the
     * read has a larger seq and is excluded by construction rather than by timestamp comparison. */
    for (i = 0; i < XBOX_A2H_SLOTW_EVENTS_MAX; i++) {
        if (!L->events[i].seq || L->events[i].seq >= seq) continue;
        if (L->events[i].kind != A2H_SLOTW_EV_WRITE || !L->events[i].slot_hit) continue;
        if (L->events[i].seq > best) {
            best = L->events[i].seq;
            L->last_write_seq = L->events[i].seq;
            L->last_write_range = L->events[i].range_class;
            L->last_write_alias = L->events[i].alias_index;
            L->last_write_rip = L->events[i].rip;
            L->last_write_ticks = L->events[i].ticks;
            L->last_write_value = L->events[i].pre_value;
        }
    }
    L->fourth_value = value;
    L->fourth_seq = seq;
    L->fourth_reached = 1;
    L->loss.read_samples++;
    fprintf(stderr, "  [A2HSLOTW] fourth-read value=%08X index=%u seq=%u last_write_seq=%u"
                    " range=%u alias=%u\n", value, L->read_count, seq, L->last_write_seq,
            L->last_write_range, L->last_write_alias);
    fflush(stderr);
}

int xbox_A2hSlotWatchEnabled(void)
{
    return a2h_slotw_on();
}

/* ── Q3(c): CROSS-VALIDATION WHERE THE FAULT RECORD AND A HOOK READ OVERLAP ──────────────────────
 *
 * THE OVERLAP, STATED PRECISELY. Two INDEPENDENT address computations cover the same physical dword:
 *
 *   (1) THE FAULT RECORD'S READ -- `a2h_slotw_service_own()` reads the slot through the faulting
 *       alias's RAW HOST PAGE BASE, `g_a2h_slotw_pages[alias-1] + (slot_va & 0xFFF)`, and publishes
 *       it as the STEP record's post_value and as `last_slot_read_value`. That base was captured at
 *       ARM and cached; nothing re-derives it per event.
 *   (2) THE GUEST'S OWN READ -- `MEM32(slot_va)` in the game's generated code, which goes through
 *       `g_xbox_mem_offset` and the canonical view, re-derived on every access.
 *
 * Those are different arithmetic on different bases for one dword. If they disagree, one of them is
 * reading the wrong place, and every value the instrument publishes is suspect -- which is exactly
 * the class of defect that the byte-order trap was, and why the Advisor requires this structurally.
 *
 * ⚠ WHAT MAKES THE COMPARISON SOUND RATHER THAN RACY. A concurrent slot write would make the two
 * readers legitimately disagree. So the guard is `last_slot_read_hits`: the instrument's slot-hit
 * count at the moment it published `last_slot_read_value`. A caller reads the pair, performs its own
 * read, then reads the count again:
 *
 *   * count UNCHANGED -> no slot write landed across the caller's window, so both readers cover the
 *     same value at the same time. The values MUST agree, and a disagreement is an INSTRUMENT BUG.
 *   * count MOVED -> the window was not quiet. The pair is counted as SKIPPED, never as agreement,
 *     so "no comparison" can never be read as "agreement".
 *
 * ⚠ A DISAGREEMENT IS FAIL-CLOSED: `cross_mismatch` is incremented, `loss.overflow` is latched
 * (which invalidates absence/order rows), it is printed, and this returns 0. The caller must treat 0
 * as a failure. Nothing here changes guest state.
 *
 * `guest_va` must be the slot the caller actually read. A caller that read some other address is
 * comparing two different things, so it is SKIPPED and counted as such rather than passed. */
int xbox_A2hSlotWatchCrossCheck(uint32_t guest_va, uint32_t fault_record_value, uint32_t hook_value)
{
    XboxA2hSlotwLedger *L = &g_xbox_a2h_slotw;

    if (!L->magic || !g_a2h_slotw_armed)
        return 1;                    /* nothing armed: nothing to cross-validate */

    if (guest_va != L->arm_slot) {
        A2H_SLOTW_INC64(&L->loss.cross_skipped);
        return 1;
    }
    /* ⚠ ONLY CANONICAL-ALIAS READS ARE COMPARABLE, AND THIS IS A MEASURED HOST PROPERTY RATHER THAN
     * A CONVENIENCE. A slot write through a MIRROR view is read back through that mirror by design
     * (the step handler's own comment explains why), and the toolkit's fixture MEASURED that on this
     * host a store through mirror view 1 does NOT become visible in the canonical view. The guest's
     * own read is canonical. So comparing a mirror read against a canonical one would report a host
     * mapping property as an instrument disagreement -- and a check that fails closed on correct
     * behaviour is worse than no check. Every other alias is counted SKIPPED, so the archive shows
     * how many pairs were NOT comparable instead of implying they agreed. */
    if (L->last_slot_read_alias != 1u) {
        A2H_SLOTW_INC64(&L->loss.cross_skipped);
        return 1;
    }
    if (fault_record_value != hook_value) {
        InterlockedIncrement((volatile LONG *)&L->cross_mismatch);
        A2H_SLOTW_INC64(&L->loss.cross_mismatch);
        A2H_SLOTW_SET64(&L->loss.overflow, 1);
        fprintf(stderr, "  [A2HSLOTW] CROSS-VALIDATION MISMATCH slot=%08X fault_record=%08X"
                        " hook=%08X seq=%u -- the instrument's cached alias read and the guest's"
                        " own translation DISAGREE: INSTRUMENT BUG, failing closed\n",
                guest_va, fault_record_value, hook_value, L->last_slot_read_seq);
        fflush(stderr);
        return 0;
    }
    A2H_SLOTW_INC64(&L->loss.cross_checks);
    InterlockedIncrement((volatile LONG *)&L->cross_checks);
    return 1;
}

/* The slot VA this arm derived, for a caller that must read the SAME address the instrument reads.
 * Returns 0 when unarmed, so a caller cannot accidentally cross-check against address zero. */
uint32_t xbox_A2hSlotWatchSlotVa(void)
{
    return g_a2h_slotw_armed ? g_xbox_a2h_slotw.arm_slot : 0u;
}

/* The instrument's last published slot read, as a (value, seq, hit_count) triple. A caller reads the
 * triple, performs its own read of the same address, then reads the hit count again: an unchanged
 * count proves no slot write landed across its window, which is what makes the comparison a
 * same-address same-time one rather than a race. Returns 1 when a read has been published. */
int xbox_A2hSlotWatchLastSlotRead(uint32_t *out_value, uint32_t *out_seq, uint32_t *out_hits)
{
    XboxA2hSlotwLedger *L = &g_xbox_a2h_slotw;
    if (!g_a2h_slotw_armed)
        return 0;
    if (out_value) *out_value = L->last_slot_read_value;
    if (out_seq) *out_seq = L->last_slot_read_seq;
    if (out_hits) *out_hits = L->last_slot_read_hits;
    return 1;
}

/* The alias the instrument last read the slot through: 1 = canonical, m+1 = mirror m, 0 = none yet.
 * The hook cross-validates only canonical reads; see xbox_A2hSlotWatchCrossCheck(). */
uint32_t xbox_A2hSlotWatchLastSlotReadAlias(void)
{
    return g_a2h_slotw_armed ? g_xbox_a2h_slotw.last_slot_read_alias : 0u;
}

/* The instrument's slot-hit count right now. The caller reads it after its own read to decide
 * whether its window was quiet; see xbox_A2hSlotWatchCrossCheck(). */
uint32_t xbox_A2hSlotWatchSlotHits(void)
{
    return (uint32_t)g_xbox_a2h_slotw.loss.slot_hits;
}

/* ── THE FIXTURE SEAM ──────────────────────────────────────────────────────────────────────────
 *
 * The fixtures must prove properties of the REAL handlers, not of a reimplementation -- a fixture
 * that re-derives the protocol tests itself. These two entry points therefore call the very same
 * ac97_write_veh() and a2h_slotw_veh() bodies that the OS calls, with a real EXCEPTION_POINTERS and
 * a real CONTEXT, so the fixture observes the actual return values, the actual EFlags.TF the
 * handlers leave behind, and the actual ledger the handlers write.
 *
 * They are OBSERVATION-ONLY SEAMS FOR TESTS: they are declared only here, are not in any public
 * header, and are reachable only from the fixture translation unit that declares them extern. */
typedef struct {
    uint32_t armed;
    uint32_t page_offset;
    uint32_t slot_va;
    uint32_t alias_count;
    uint32_t protected_count;
    void    *page0;          /* the canonical protected page, for the fixtures' own stores */
    void    *mirror0;        /* mirror view 1's protected page */
} XboxA2hSlotwFixtureState;

int xbox_A2hSlotWatchFixtureArm(uint32_t device_va, XboxA2hSlotwFixtureState *out)
{
    uint32_t rc;

    if (!a2h_slotw_on())
        return 0;                    /* the gate is the gate: fixtures do not bypass it */
    if (!g_memory_base)
        return 0;

    /* Point the device global at a scratch object inside guest RAM, so the derivation runs exactly
     * as it does live (read the pointer, checked add, protect that page) rather than against a
     * hardcoded address. */
    *(volatile uint32_t *)((uintptr_t)XBOX_A2H_SLOTW_DEVICE_PTR + g_xbox_mem_offset) = device_va;

    rc = xbox_A2hSlotWatchArm();
    if (out) {
        memset(out, 0, sizeof(*out));
        out->armed = rc;
        out->page_offset = g_xbox_a2h_slotw.page_offset;
        out->slot_va = g_xbox_a2h_slotw.arm_slot;
        out->alias_count = g_xbox_a2h_slotw.alias_count;
        out->protected_count = g_xbox_a2h_slotw.protected_count;
        out->page0 = g_a2h_slotw_pages[0];
        out->mirror0 = g_a2h_slotw_pages[1];
    }
    return (int)rc;
}

/* Instrument the fourth-read latch for a fixture, with a controlled read index, so the read-side
 * latch can be exercised without a live guest poll loop. It calls the REAL xbox_A2hSlotWatchNoteFourthRead
 * and therefore exercises the real event-id tie, not a reimplementation of it. */
void xbox_A2hSlotWatchFixtureNoteRead(uint32_t value, uint32_t read_index)
{
    xbox_A2hSlotWatchNoteFourthRead(value, read_index);
}

/* Publish a synthetic SLOT-HIT write record through the REAL publisher, so a fixture can prove the
 * fourth-read latch's ORDERED-EVENT-ID tie without waiting for a live store. `slot_hit` and
 * `range_class` are the caller's, so the fixture can drive both the installer's class and an
 * unrelated one. */
int xbox_A2hSlotWatchFixturePublishWrite(uint32_t slot_hit, uint32_t range_class, uint32_t pre_value)
{
    if (!g_a2h_slotw_armed)
        return 0;
    return a2h_slotw_publish(A2H_SLOTW_EV_WRITE, 1u, slot_hit, 0u, pre_value, 0u, 0u,
                             range_class, XBOX_A2H_SLOTW_FORM_UNKNOWN);
}

/* Deliver one exception to the REAL slot-watch handler. Returns its return value verbatim. */
LONG xbox_A2hSlotWatchFixtureDeliver(DWORD code, ULONG_PTR info0, ULONG_PTR info1,
                                     void *context, ULONG_PTR *params, DWORD nparams)
{
    EXCEPTION_RECORD rec;
    EXCEPTION_POINTERS ep;

    memset(&rec, 0, sizeof(rec));
    rec.ExceptionCode = code;
    rec.ExceptionAddress = (PVOID)context;
    rec.NumberParameters = nparams;
    for (DWORD i = 0; i < nparams && i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
        rec.ExceptionInformation[i] = params[i];
    if (nparams >= 1) rec.ExceptionInformation[0] = info0;
    if (nparams >= 2) rec.ExceptionInformation[1] = info1;
    ep.ExceptionRecord = &rec;
    ep.ContextRecord = (CONTEXT *)context;
    return a2h_slotw_veh(&ep);
}

/* The same, for the REAL AC'97 handler: the fixture needs both halves of the overlap. */
LONG xbox_A2hSlotWatchFixtureDeliverAc97(DWORD code, ULONG_PTR info0, ULONG_PTR info1,
                                         void *context, DWORD nparams)
{
    EXCEPTION_RECORD rec;
    EXCEPTION_POINTERS ep;

    memset(&rec, 0, sizeof(rec));
    rec.ExceptionCode = code;
    rec.ExceptionAddress = (PVOID)context;
    rec.NumberParameters = nparams;
    if (nparams >= 1) rec.ExceptionInformation[0] = info0;
    if (nparams >= 2) rec.ExceptionInformation[1] = info1;
    ep.ExceptionRecord = &rec;
    ep.ContextRecord = (CONTEXT *)context;
    return ac97_write_veh(&ep);
}

/* Point the AC'97 trap at a scratch page inside guest RAM so the overlap fixture can arm BOTH
 * owners without the MCPX aperture existing. Returns 1 when the page is RO and the VEH is live. */
int xbox_A2hSlotWatchFixtureArmAc97(uint32_t page_va, void **out_page)
{
    DWORD old;
    void *host;

    if (!a2h_slotw_on() || !g_memory_base)
        return 0;
    host = (void *)((uintptr_t)page_va + (uintptr_t)g_memory_offset);
    if (!VirtualProtect(host, AC97_TRAP_BYTES, PAGE_READONLY, &old))
        return 0;
    g_ac97_page = host;
    if (out_page) *out_page = host;
    return 1;
}

void xbox_A2hSlotWatchFixtureDisarmAc97(void)
{
    DWORD old;
    if (g_ac97_page)
        VirtualProtect(g_ac97_page, AC97_TRAP_BYTES, PAGE_READWRITE, &old);
    g_ac97_page = NULL;
    s_a2h_slotw_pending = 0;
    s_a2h_slotw_saved_tf = 0;
}

/* Read/write the per-thread pending word directly, so the fixture can construct an OVERLAP that a
 * single fault cannot produce on its own, and can assert exactly what each handler left behind. */
void xbox_A2hSlotWatchFixtureSetPending(uint32_t bits, uint32_t saved_tf)
{
    s_a2h_slotw_pending = bits;
    s_a2h_slotw_saved_tf = saved_tf;
}

uint32_t xbox_A2hSlotWatchFixturePending(void)
{
    return s_a2h_slotw_pending;
}

uint32_t xbox_A2hSlotWatchFixtureSavedTf(void)
{
    return s_a2h_slotw_saved_tf;
}

/* Clear the coherence gate's two operands so a fixture can construct the NO-RECORDED-WRITE state.
 * That state is a real one -- it is what an arm whose slot was never written looks like -- but a
 * single live sequence cannot produce it once any slot write has happened, and the fixture must be
 * able to assert the NOT_COMPARABLE verdict rather than only the two that a written slot can reach.
 * This is the same kind of seam as xbox_A2hSlotWatchFixtureSetPending: it constructs a state the
 * live path cannot, so the handler's behaviour in that state is PROVEN rather than assumed. */
void xbox_A2hSlotWatchFixtureResetCoherence(void)
{
    g_xbox_a2h_slotw.coherence_last_write_value = 0;
    g_xbox_a2h_slotw.coherence_last_write_seq = 0;
}

/* Set the gate's two operands and run the REAL decision rule over them, returning the verdict. The
 * fixture must drive PRODUCTION code, not a restatement of its rule: a fixture that reimplemented
 * the comparison would only prove that the fixture and the handler agree about an untested rule,
 * which is the same defect class as the void encoding classifier. */
uint32_t xbox_A2hSlotWatchFixtureDecideCoherence(uint32_t last_write_value, uint32_t last_write_seq,
                                                 uint32_t terminal_value, uint32_t terminal_seen)
{
    XboxA2hSlotwLedger *L = &g_xbox_a2h_slotw;
    L->coherence_last_write_value = last_write_value;
    L->coherence_last_write_seq = last_write_seq;
    L->coherence_terminal_value = terminal_value;
    L->terminal_seen = terminal_seen;
    a2h_slotw_decide_coherence(L);
    return L->coherence_verdict;
}

XboxA2hSlotwLedger *xbox_A2hSlotWatchFixtureLedger(void)
{
    return &g_xbox_a2h_slotw;
}

/* ⚠ THE CLASSIFIER SEAM, IN THE NATIVE DOMAIN. The old seam classified the BYTES at an address and
 * is gone with the paradigm it belonged to: there is nothing left to classify from bytes. This
 * exposes the RANGE classifier instead, so a fixture can point it at the ACTUAL addresses of the
 * loaded image -- its own functions, the recompiled module's published bound, and an address outside
 * both -- and prove the classification against the real modules rather than a synthetic range. */
uint32_t xbox_A2hSlotWatchFixtureClassify(uint64_t rip)
{
    return a2h_slotw_classify_rip(rip);
}

/* The optional native-disasm corroboration, exposed for the same reason: Exp0 must show it agrees
 * with the range class on real code and that it never overrides it. */
uint32_t xbox_A2hSlotWatchFixtureForm(uint64_t rip)
{
    return a2h_slotw_corroborate_form(rip);
}

/* ── THE ALL-THREAD CENSUS, AND WHY IT IS A POLLING THREAD RATHER THAN A THREAD CALLBACK ────────
 *
 * "No thread exclusion, ever" means the census must be able to NAME a writer on any thread,
 * including one that did not exist when the watch armed. The page protection is process-wide and
 * therefore already covers a new thread's stores the instant it is created -- what a new thread
 * costs is not coverage of the WRITE but knowledge of the THREAD, and that is what this census adds.
 *
 * A thread-birth callback (CreateToolhelp32Snapshot diffing at a fixed cadence) is used rather than
 * a per-thread arming loop, because the mechanism has no per-thread state to arm: there are no debug
 * registers to program, which is exactly why the DR channel's arming problem does not recur here.
 * Arrivals and exits are both counted, and a tid that exits between two snapshots is reconciled by
 * the exit count rather than being reported as never having existed.
 *
 * This thread is a CENSUS, not a guard: it never protects or opens a page, never touches a guest
 * register and never calls into guest code. It is created only when the gate is set and it is
 * stopped and joined on disarm/shutdown. */
#define A2H_SLOTW_CENSUS_MS 50

static HANDLE g_a2h_slotw_census_thread = NULL;
static volatile LONG g_a2h_slotw_census_stop = 0;

static DWORD WINAPI a2h_slotw_census_thread(LPVOID param)
{
    XboxA2hSlotwLedger *L = &g_xbox_a2h_slotw;
    DWORD seen[XBOX_A2H_SLOTW_THREADS_MAX];
    uint32_t seen_count = 0;
    (void)param;

    /* The arming thread is the first member of the census by construction. */
    seen[seen_count++] = GetCurrentThreadId();
    L->thread_count = seen_count;

    while (!InterlockedCompareExchange(&g_a2h_slotw_census_stop, 0, 0)) {
        HANDLE snap;
        /* DEFERRED ARM. The device is allocated at runtime, so the pointer is polled here until it
         * names a plausible object; the slot is then derived from THAT read. Retried at the census
         * cadence rather than once, because a single early attempt would simply miss.
         *
         * ⚠ THE ARM MEMSETS THE LEDGER, so the census counts are re-published immediately after it:
         * leaving them to be restored only when the next thread arrives would report a live census
         * of one thread as zero, which is the kind of quiet undercount this whole facility exists to
         * avoid. */
        if (!g_a2h_slotw_armed && xbox_A2hSlotWatchArm())
            fprintf(stderr, "  [A2HSLOTW] deferred ARM completed base=%08X slot=%08X\n",
                    g_xbox_a2h_slotw.arm_base, g_xbox_a2h_slotw.arm_slot);
        L->thread_count = seen_count;

        snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te;
            DWORD live[XBOX_A2H_SLOTW_THREADS_MAX];
            uint32_t live_count = 0;
            uint32_t i, j;

            te.dwSize = sizeof(te);
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                    if (live_count < XBOX_A2H_SLOTW_THREADS_MAX)
                        live[live_count++] = te.th32ThreadID;
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);

            /* ARRIVALS: in the live set, not in the seen set. */
            for (i = 0; i < live_count; i++) {
                int known = 0;
                for (j = 0; j < seen_count; j++)
                    if (seen[j] == live[i]) { known = 1; break; }
                if (known) continue;
                if (seen_count < XBOX_A2H_SLOTW_THREADS_MAX) {
                    seen[seen_count++] = live[i];
                    L->thread_count = seen_count;
                    A2H_SLOTW_INC64(&L->loss.threads_new);
                } else {
                    /* The census array is bounded; an arrival beyond it is a coverage failure for
                     * the THREAD list and is latched rather than silently dropped. The WRITE is
                     * still covered -- the page is protected process-wide -- so this invalidates a
                     * per-thread claim, never a positive writer record. */
                    L->thread_overflow = 1;
                    A2H_SLOTW_SET64(&L->loss.overflow, 1);
                }
            }
            /* EXITS: in the seen set, not in the live set. A tid that arrives and exits between two
             * snapshots is counted as BOTH a new thread and a gone thread when it is next seen,
             * which is why both counters are reported instead of a net difference. */
            for (j = 0; j < seen_count; j++) {
                int alive = 0;
                for (i = 0; i < live_count; i++)
                    if (live[i] == seen[j]) { alive = 1; break; }
                if (!alive) {
                    A2H_SLOTW_INC64(&L->loss.threads_gone);
                    /* Remove it so a recycled tid is not mistaken for the old thread. */
                    seen[j] = seen[seen_count - 1];
                    seen_count--;
                    L->thread_count = seen_count;
                    j--;
                }
            }
        }
        Sleep(A2H_SLOTW_CENSUS_MS);
    }
    return 0;
}

void xbox_A2hSlotWatchStart(void)
{
    if (!a2h_slotw_on() || g_a2h_slotw_census_thread)
        return;

    /* ARM FIRST, SYNCHRONOUSLY, ON THIS THREAD. Two reasons, both measured rather than preferred:
     *
     *   (1) The census thread below must not be created while the game's own VEH chain is being
     *       registered from another thread -- the handler order is a carry-forward gate, and the
     *       order a reader re-verifies must be the order that was actually installed.
     *   (2) If MEM32(0x19DCE0) already names the device at this point, the pages are protected
     *       BEFORE the poll thread starts, so the very first write is caught. If it does not, the
     *       ARM is retried from the poll thread until it does, which is the deferred case. */
    if (xbox_A2hSlotWatchArm())
        fprintf(stderr, "  [A2HSLOTW] armed synchronously at start (base=%08X)\n",
                g_xbox_a2h_slotw.arm_base);
    else
        fprintf(stderr, "  [A2HSLOTW] device not yet allocated (MEM32(0x%08X)=%08X);"
                        " ARM deferred to the census poll\n",
                XBOX_A2H_SLOTW_DEVICE_PTR,
                *(volatile uint32_t *)((uintptr_t)XBOX_A2H_SLOTW_DEVICE_PTR + g_xbox_mem_offset));
    fflush(stderr);

    g_a2h_slotw_census_stop = 0;
    g_a2h_slotw_census_thread = CreateThread(NULL, 0, a2h_slotw_census_thread, NULL, 0, NULL);
    if (!g_a2h_slotw_census_thread) {
        fprintf(stderr, "  [A2HSLOTW] census thread NOT created (error %lu) -- arrivals and exits"
                        " are NOT accounted for\n", GetLastError());
        fflush(stderr);
        A2H_SLOTW_SET64(&g_xbox_a2h_slotw.loss.overflow, 1);
    }
}

void xbox_A2hSlotWatchStop(void)
{
    if (g_a2h_slotw_census_thread) {
        InterlockedExchange(&g_a2h_slotw_census_stop, 1);
        WaitForSingleObject(g_a2h_slotw_census_thread, 1000);
        CloseHandle(g_a2h_slotw_census_thread);
        g_a2h_slotw_census_thread = NULL;
    }
    xbox_A2hSlotWatchDisarm();
}

void xbox_MemoryLayoutShutdown(void)
{
    /* Restore mirror-page protections and drop the census handler before the views go away.
     * With the gate unset this is a no-op: no page was protected and no handler was registered. */
    xbox_A2hAliasCensusDisarm();
    xbox_A2hSlotWatchStop();
    if (g_kernel_memory) {
        VirtualFree(g_kernel_memory, 0, MEM_RELEASE);
        g_kernel_memory = NULL;
    }
    if (g_nv2a_ack_thread) {
        InterlockedExchange(&g_nv2a_ack_stop, 1);
        WaitForSingleObject(g_nv2a_ack_thread, 1000);
        CloseHandle(g_nv2a_ack_thread);
        g_nv2a_ack_thread = NULL;
    }
    if (g_nv2a_memory) {
        VirtualFree(g_nv2a_memory, 0, MEM_RELEASE);
        g_nv2a_memory = NULL;
    }
    /* Unmap mirror views first */
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        if (g_mirror_views[m]) {
            UnmapViewOfFile(g_mirror_views[m]);
            g_mirror_views[m] = NULL;
        }
    }
    /* Unmap base view */
    if (g_memory_base) {
        UnmapViewOfFile(g_memory_base);
        g_memory_base = NULL;
        g_memory_size = 0;
    }
    /* The apertures. Left mapped, a second init cannot place them: the first
     * run still owns 0x80000000, 0xFD000000, 0xFE800000, 0xFF000000 and the
     * tiled alias, and every one of those comes back as "failed" while init
     * still returns TRUE because they are best-effort. The result is a layout
     * that looks initialised and has no device apertures at all. */
    if (g_tiled_view) {
        UnmapViewOfFile(g_tiled_view);
        g_tiled_view = NULL;
    }
    if (g_contig_memory) {
        VirtualFree(g_contig_memory, 0, MEM_RELEASE);
        g_contig_memory = NULL;
    }
    if (g_mcpx_memory) {
        VirtualFree(g_mcpx_memory, 0, MEM_RELEASE);
        g_mcpx_memory = NULL;
    }
    if (g_flash_memory) {
        VirtualFree(g_flash_memory, 0, MEM_RELEASE);
        g_flash_memory = NULL;
    }

    /* Close file mapping handle */
    if (g_mapping_handle) {
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
    }

    /* Whatever is left of the base+mirrors reservation. The views carved out
     * of it are already unmapped above; this releases the range itself. */
    if (g_span_base) {
        VirtualFree(g_span_base, g_span_size, MEM_RELEASE);
        g_span_base = NULL;
        g_span_size = 0;
    }
    fprintf(stderr, "xbox_MemoryLayoutShutdown: released\n");
}

/* Bump allocator for pure address-space reservations, above RAM.
 *
 * A MEM_RESERVE costs no memory on real hardware -- it takes address space out
 * of a 4 GB range, not pages out of the 64 MB the console has -- so titles
 * reserve far more than exists and commit a fraction. Satisfying that out of
 * the RAM heap does not work: Half-Life 2 asks for 128 MB and then 200 MB, and
 * clamping those to what the heap can back left it sub-allocating across a
 * range it believed it owned, walking past the top of RAM and aliasing low
 * memory through the mirrors.
 *
 * So reservations come from the mapped space *above* RAM instead. Those pages
 * are already backed and distinct, nothing else hands them out, and a commit
 * inside one is a no-op because it is real memory already.
 *
 * Returns 0 when the mapping is no larger than RAM -- the default for titles
 * that never call xbox_SetMapSize -- which leaves the old behaviour untouched.
 *
 * ponytail: a bump allocator with no free. A reservation is address space, the
 * range is large, and a title that reserves and releases repeatedly would need
 * a real allocator; none has yet.
 */
static uint32_t g_reserve_next;

uint32_t xbox_ReserveAlloc(uint32_t size, uint32_t align)
{
    uint32_t base;

    if (g_memory_size <= g_xbox_total_ram || size == 0)
        return 0;
    if (!align)
        align = 4096;
    if (!g_reserve_next)
        g_reserve_next = (uint32_t)g_xbox_total_ram;

    base = (g_reserve_next + align - 1) & ~(align - 1);
    if ((size_t)base + size > g_memory_size)
        return 0;
    g_reserve_next = base + size;
    return base;
}

BOOL xbox_IsXboxAddress(uintptr_t address)
{
    return (address >= XBOX_BASE_ADDRESS &&
            address < XBOX_BASE_ADDRESS + g_memory_size);
}

void *xbox_GetMemoryBase(void)
{
    return g_memory_base;
}

ptrdiff_t xbox_GetMemoryOffset(void)
{
    return g_memory_offset;
}

/* ── Dynamic heap allocator ────────────────────────────────
 *
 * Simple bump allocator for MmAllocateContiguousMemory and similar.
 * Returns Xbox VAs within the mapped region so MEM32() works correctly.
 * No free support (bump-only for now).
 */
static uint32_t g_heap_next = XBOX_HEAP_BASE;

static int g_heap_alloc_count = 0;

/* Block table backing xbox_HeapFree. A bump pointer alone never reclaims,
 * which is fine for a title that allocates once and fatal for a debug build
 * that churns. Flat array rather than an intrusive list: allocations come back
 * in bump order, so index order is address order and coalescing is a
 * neighbour check. */
#define XBOX_HEAP_MAX_BLOCKS 65536
static struct kmem_block g_heap_blocks[XBOX_HEAP_MAX_BLOCKS];
static int g_heap_block_count = 0;
/* Guest threads, DPCs and bridges allocate and free concurrently; every heap
 * entry point takes this, and none calls another while holding it. */
static SRWLOCK g_heap_lock = SRWLOCK_INIT;

static struct kmem_alloc_counters g_kmem_alloc;

/* RECOMP_KMEM_LEGACY=1 puts back the allocator and kernel memory behaviour
 * that preceded the region registry, for A/B runs. Read once. */
int xbox_KmemLegacy(void)
{
    static int legacy = -1;

    if (legacy < 0) {
        const char *v = getenv("RECOMP_KMEM_LEGACY");
        legacy = (v && v[0] == '1') ? 1 : 0;
    }
    return legacy;
}


/*
 * Simulated stacks for spawned threads.
 *
 * The main thread owns the top of the XBOX_STACK region and grows down; worker
 * stacks are carved from the bottom upward so the two cannot meet until the
 * whole 8 MB is gone. Xbox VAs, not host memory: recompiled code addresses its
 * stack through MEM32() like any other Xbox pointer.
 */
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

#define XBOX_THREAD_STACK_SIZE  (512 * 1024)
#define XBOX_MAX_THREAD_STACKS  8

static int g_thread_stacks_used = 0;

/* A TIB and TLS block for a newly spawned guest thread.
 *
 * A TIB is per-thread on the console and was per-process here: one address,
 * 0x1000, for everyone. Two things live in it that must not be shared. fs:[0]
 * is the SEH chain head, so two threads unwinding at once walk each other's
 * frames. fs:[4] points at the image's TLS block, whose slot 0 is the CRT's
 * per-thread data -- errno, the locale, and the bookkeeping _lock() uses to
 * decide who owns which lock.
 *
 * Half-Life 2 deadlocked on the last of those: two threads inside _lock(),
 * each holding the CRT lock the other was waiting for, because "which thread
 * am I" was a single shared answer.
 *
 * The new block is a copy of the template the loader built, so a thread starts
 * with the image's initialised thread-local data rather than zeros, and its
 * own per-thread structure behind slot 0.
 */
uint32_t xbox_AllocThreadTib(void)
{
    /* XBOX_VA is scoped to the loader; the same arithmetic, spelled here. */
    #define TIB_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
    const uint32_t tib_size = 0x40;
    uint32_t tib, block, thread_data, total;

    if (!g_tls_total)
        return 0;                    /* image has no TLS; nothing to copy */

    total = g_tls_total;
    tib = xbox_HeapAlloc(tib_size + total + g_tls_thread_size, 16);
    if (!tib)
        return 0;
    block       = tib + tib_size;
    thread_data = block + total;

    /* The TIB itself, copied so stack bounds and the fields the title filled
     * in are inherited, then the two that must not be. */
    memcpy(TIB_VA(tib), TIB_VA(XBOX_TIB_MAIN), tib_size);
    memcpy(TIB_VA(block), TIB_VA(g_tls_template_va), total);
    memset(TIB_VA(thread_data), 0, g_tls_thread_size);

    *(uint32_t *)TIB_VA(tib + 0x00) = 0xFFFFFFFFu;   /* own SEH chain    */
    *(uint32_t *)TIB_VA(block)      = thread_data;   /* slot 0           */
    *(uint32_t *)TIB_VA(tib + 0x04) = block + total; /* fs:[4], see above*/

    return tib;
    #undef TIB_VA
}

uint32_t xbox_AllocThreadStack(void)
{
    uint32_t base;

    if (g_thread_stacks_used >= XBOX_MAX_THREAD_STACKS) {
        return 0;
    }

    /* From the heap, not from XBOX_STACK_BASE.
     *
     * The stack region begins at 0x00780000, which is fine only while the
     * title's image ends below that. Half-Life 2's image runs to 0x009B68C0,
     * so the first thread stack (0x00780000..0x00800000) landed inside its
     * .rdata and .data: the worker spawned during engine init wrote its
     * frames over the game's own static data. Nothing faults -- the pages are
     * mapped and writable -- so it shows up later as globals that were
     * correct when written and wrong when read.
     *
     * The heap already starts above the image and knows how big it is, so
     * taking slices from it is correct for any image size instead of only
     * for small ones.
     */
    base = xbox_HeapAlloc(XBOX_THREAD_STACK_SIZE, 4096);
    if (!base)
        return 0;
    g_thread_stacks_used++;

    /* Top of the slice, 16-byte aligned, growing down. */
    return base + XBOX_THREAD_STACK_SIZE - 16;
}

/* Give a worker's stack back when the worker ends.
 *
 * The counter used to only ever go up, so a title that creates and destroys
 * threads ran the pool dry no matter how few were alive at once. The Xbox
 * Dashboard spawns one worker per ambient WAV and terminates it before loading
 * the next; after XBOX_MAX_THREAD_STACKS files the pool was empty and
 * PsCreateSystemThreadEx fell back to running the worker inline. That fallback
 * is a deadlock here rather than a slowdown: the worker ran to completion
 * before the caller reached its wait, so the main thread then waited forever on
 * events whose only signaller had already finished. It looked like an audio
 * hang, three layers away from the cause.
 *
 * Takes the value AllocThreadStack returned, so callers never do the arithmetic.
 */
void xbox_FreeThreadStack(uint32_t stack_top)
{
    if (!stack_top)
        return;
    xbox_HeapFree(stack_top + 16 - XBOX_THREAD_STACK_SIZE);
    if (g_thread_stacks_used > 0)
        g_thread_stacks_used--;
}

/* Allocator over the contiguous window mapped at XBOX_CONTIG_BASE.
 *
 * MmAllocateContiguousMemory hands back physical memory, and on Xbox physical
 * page P is visible at 0x80000000 + P. Drivers rely on that being an exact
 * round trip: Xbox D3D writes its pushbuffer position to the NV2A as
 * `VA & 0x0FFFFFFF` and reads the GPU's position back as `GET | 0x80000000`,
 * then compares the two. That holds for any address in this window and for
 * nothing in the general heap, whose position depends on what the title
 * reserved first -- Half-Life 2 reserves 128 MB and then 200 MB before D3D
 * allocates its pushbuffer, which put the buffer at 0x15782000 and left the
 * engine comparing 0x857844C0 against it forever.
 *
 * Grows up from the base; XBOX_GPU_INSTANCE_DEFAULT is carved off the top by
 * the GPU-instance bridge, so the two do not meet until the window is full.
 *
 * MmFreeContiguousMemory gives a block back and later requests reuse it
 * first-fit (kmem_arena_alloc). The bump pointer stays the high-water mark and
 * is never lowered by a free, because the GP DSP and pushbuffer translations
 * (apu_guest_dma_ptr, dma_resolve) treat a physical offset below it as window
 * memory: every block, live or reused, lies below it. Under RECOMP_KMEM_LEGACY
 * this is the original bump allocator that never frees. */
#define XBOX_CONTIG_MAX_BLOCKS 16384
static struct kmem_block g_contig_blocks[XBOX_CONTIG_MAX_BLOCKS];
static struct kmem_arena g_contig = {
    g_contig_blocks, 0, XBOX_CONTIG_MAX_BLOCKS, XBOX_CONTIG_BASE,
    /* Leave the top of the window for GPU instance memory. */
    (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE - XBOX_GPU_INSTANCE_DEFAULT,
    0, 0, 0, 0
};
static SRWLOCK g_contig_lock = SRWLOCK_INIT;

uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t result;

    AcquireSRWLockExclusive(&g_contig_lock);
    result = kmem_arena_alloc(&g_contig, size, alignment, xbox_KmemLegacy());
    ReleaseSRWLockExclusive(&g_contig_lock);

    if (!result) {
        fprintf(stderr, "  [CONTIG] arena exhausted (%u requested, %u of %u used)\n",
                size, g_contig.next - XBOX_CONTIG_BASE,
                (unsigned)XBOX_CONTIG_SIZE);
        fflush(stderr);
        return 0;
    }

    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
    return result;
}

/* MmFreeContiguousMemory. Returns 0, and frees nothing, for an address that is
 * not the start of a live block: a pinned MmAllocateContiguousMemoryEx range,
 * a heap pointer, or a double free. The first few are logged. */
int xbox_ContiguousFree(uint32_t addr)
{
    int ok;

    AcquireSRWLockExclusive(&g_contig_lock);
    ok = kmem_arena_free(&g_contig, addr);
    ReleaseSRWLockExclusive(&g_contig_lock);

    if (!ok) {
        static int logged;
        if (logged < 8) {
            logged++;
            fprintf(stderr, "  [KMEM] reject kind=contig_free_unknown base=0x%08X\n",
                    addr);
            fflush(stderr);
        }
    }
    return ok;
}

/* Bytes from va to the end of the live contiguous block holding it, or 0. */
uint32_t xbox_ContiguousBlockSize(uint32_t va)
{
    uint32_t n;

    AcquireSRWLockShared(&g_contig_lock);
    n = kmem_arena_block_size(&g_contig, va);
    ReleaseSRWLockShared(&g_contig_lock);
    return n;
}

void xbox_KmemAllocCounters(struct kmem_alloc_counters *out)
{
    *out = g_kmem_alloc;
    AcquireSRWLockShared(&g_contig_lock);
    out->contig_free_ok       = g_contig.frees_ok;
    out->contig_free_unknown  = g_contig.frees_unknown;
    out->contig_untracked     = g_contig.untracked;
    out->contig_split_skipped = g_contig.split_skipped;
    ReleaseSRWLockShared(&g_contig_lock);
}

/* How much of the window has been handed out.
 *
 * Lets a caller holding a physical address decide whether it names contiguous
 * memory this runtime allocated. The pushbuffer executor needs exactly that:
 * a surface offset is physical, and only the window makes it addressable. */
uint32_t xbox_ContiguousAllocatedBytes(void)
{
    return g_contig.next - XBOX_CONTIG_BASE;
}


static uint32_t heap_alloc_locked(uint32_t size, uint32_t alignment)
{
    uint32_t result;

    if (alignment < 4) alignment = 4;

    /* Enforce minimum allocation size.
     * The Xbox D3D8 code sometimes computes resource sizes from GPU
     * capabilities that return 0 (since we don't have real NV2A hardware),
     * resulting in zero-size allocations. With a bump allocator, these all
     * return the same address, causing overlapping structures. Enforce a
     * minimum of 4096 bytes so each allocation gets its own memory. */
    if (size < 16) size = 16;

    /* Reuse a freed block first. Without this the heap only ever grows: Halo's
     * debug build allocates and releases heavily through init, exhausted all
     * 48 MB in 4,726 allocations, and its second D3D CreateDevice then failed
     * with E_OUTOFMEMORY -- which the title reports by clearing
     * global_d3d_device, so the rasterizer asserts and startup stops.
     *
     * A block larger than the request is split and the rest stays free, so a
     * small request cannot pin memory that a later large one needs. */
    {
        int split_result;

        result = kmem_heap_reuse(g_heap_blocks, &g_heap_block_count,
                                 XBOX_HEAP_MAX_BLOCKS, size, alignment,
                                 !xbox_KmemLegacy(), &split_result);
        if (split_result > 0)
            g_kmem_alloc.heap_split++;
        else if (split_result == KMEM_TABLE_FULL)
            g_kmem_alloc.heap_split_full++;
        if (result) {
            memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
            return result;
        }
    }

    /* Align the next pointer */
    result = (g_heap_next + alignment - 1) & ~(alignment - 1);

    if (result + size > XBOX_HEAP_TOP) {
        fprintf(stderr, "xbox_HeapAlloc: out of memory (requested %u, used %u/%u)\n",
                size, g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE));
        /* Who ate the heap? Group live blocks by size -- an exhausted heap is
         * nearly always one request size repeated, and the count names it. */
        {
            static int dumped = 0;
            static struct { uint32_t size; int n; } hist[256];
            if (!dumped) {
                int used = 0;
                dumped = 1;
                for (int i = 0; i < g_heap_block_count; i++) {
                    int j = 0;
                    if (g_heap_blocks[i].free || !g_heap_blocks[i].size) continue;
                    while (j < used && hist[j].size != g_heap_blocks[i].size) j++;
                    if (j == used) {
                        if (used == 256) continue;   /* ponytail: 256 distinct sizes is plenty */
                        hist[used].size = g_heap_blocks[i].size;
                        hist[used++].n = 0;
                    }
                    hist[j].n++;
                }
                for (int j = 0; j < used; j++) {
                    if ((uint64_t)hist[j].n * hist[j].size < 1024 * 1024) continue;
                    fprintf(stderr, "  [HEAP] %d live blocks of %u bytes (%u KB)\n",
                            hist[j].n, hist[j].size,
                            (unsigned)((uint64_t)hist[j].n * hist[j].size / 1024));
                }
                fflush(stderr);
            }
        }
        return 0;
    }

    g_heap_next = result + size;

    /* Zero-fill the allocated block (Xbox memory is always zeroed) */
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);

    if (g_heap_block_count < XBOX_HEAP_MAX_BLOCKS) {
        g_heap_blocks[g_heap_block_count].addr = result;
        g_heap_blocks[g_heap_block_count].size = size;
        g_heap_blocks[g_heap_block_count].free = 0;
        g_heap_block_count++;
    }

    g_heap_alloc_count++;
    /* Rate-limited: a debug title makes thousands of these and the log is a
     * diagnostic, not a transaction record. */
    if (g_heap_alloc_count <= 32 || (g_heap_alloc_count % 512) == 0) {
        fprintf(stderr, "  [HEAP] #%d: size=%u align=%u → 0x%08X..0x%08X (used %u/%u)\n",
                g_heap_alloc_count, size, alignment, result, result + size,
                g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE));
        fflush(stderr);
    }

    return result;
}

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t r;

    AcquireSRWLockExclusive(&g_heap_lock);
    r = heap_alloc_locked(size, alignment);
    ReleaseSRWLockExclusive(&g_heap_lock);
    return r;
}

/* Reserve exactly [base, base+size) in the heap, for a MEM_RESERVE that names
 * its address: the caller compares what comes back with what it asked for, so
 * this answers with that range or nothing. Free means untouched tail, one free
 * block, or a free last block running into the tail (kmem_heap_carve). Returns
 * 1 with the range zeroed and recorded as a live block, 0 when some of it is
 * not free, KMEM_TABLE_FULL when the block table cannot record it. */
static int heap_reserve_at_locked(uint32_t base, uint32_t size)
{
    int r = kmem_heap_carve(g_heap_blocks, &g_heap_block_count,
                            XBOX_HEAP_MAX_BLOCKS, &g_heap_next,
                            XBOX_HEAP_BASE, XBOX_HEAP_TOP, base, size);

    if (r > 0) {
        g_kmem_alloc.heap_carve_ok++;
        memset((void *)((uintptr_t)base + g_memory_offset), 0, size);
    } else if (r == 0) {
        g_kmem_alloc.heap_carve_busy++;
    } else {
        g_kmem_alloc.heap_carve_full++;
    }
    return r;
}

int xbox_HeapReserveAt(uint32_t base, uint32_t size)
{
    int r;

    AcquireSRWLockExclusive(&g_heap_lock);
    r = heap_reserve_at_locked(base, size);
    ReleaseSRWLockExclusive(&g_heap_lock);
    return r;
}

/* How big is the block at this guest address?
 *
 * MmQueryAllocationSize and ExQueryPoolBlockSize both ask this, and both used
 * to answer 0 -- ExQueryPoolBlockSize by returning a literal, and
 * MmQueryAllocationSize by having no bridge at all. The host cannot answer it:
 * VirtualQuery on the translated address reports the size of the whole 64 MB
 * guest mapping, which is a worse answer than none. The block table already
 * has the real one, and it is the same table xbox_HeapFree matches against.
 *
 * Interior addresses count: a title that asks about a pointer it has walked
 * forward is asking about the block that contains it. Returns 0 for an address
 * this heap never handed out, which is what "not one of mine" has to look like.
 */
static uint32_t heap_block_size_locked(uint32_t xbox_va)
{
    int i;

    if (!xbox_va)
        return 0;
    for (i = 0; i < g_heap_block_count; i++) {
        if (g_heap_blocks[i].free)
            continue;
        if (xbox_va >= g_heap_blocks[i].addr &&
            xbox_va <  g_heap_blocks[i].addr + g_heap_blocks[i].size)
            return g_heap_blocks[i].size - (xbox_va - g_heap_blocks[i].addr);
    }
    return 0;
}

uint32_t xbox_HeapBlockSize(uint32_t xbox_va)
{
    uint32_t r;

    AcquireSRWLockShared(&g_heap_lock);
    r = heap_block_size_locked(xbox_va);
    ReleaseSRWLockShared(&g_heap_lock);
    return r;
}

static void heap_free_locked(uint32_t xbox_va)
{
    static int frees = 0, matched = 0;

    if (!xbox_va) {
        return;
    }
    frees++;
    if (frees <= 8) {
        fprintf(stderr, "  [HEAP] free #%d va=0x%08X blocks=%d\n",
                frees, xbox_va, g_heap_block_count);
        fflush(stderr);
    }
    for (int i = 0; i < g_heap_block_count; i++) {
        if (g_heap_blocks[i].addr != xbox_va || g_heap_blocks[i].free) {
            continue;
        }
        g_heap_blocks[i].free = 1;
        if (++matched % 512 == 0) {
            fprintf(stderr, "  [HEAP] frees=%d matched=%d blocks=%d\n",
                    frees, matched, g_heap_block_count);
            fflush(stderr);
        }

        /* Coalesce with neighbours. Blocks are recorded in bump order, so
         * index order is address order and adjacency is a simple end==start
         * test. Keeps large contiguous requests satisfiable after a lot of
         * small churn. */
        if (i + 1 < g_heap_block_count && g_heap_blocks[i + 1].free &&
            g_heap_blocks[i].addr + g_heap_blocks[i].size == g_heap_blocks[i + 1].addr) {
            g_heap_blocks[i].size += g_heap_blocks[i + 1].size;
            g_heap_blocks[i + 1].size = 0;
            g_heap_blocks[i + 1].addr = 0;
        }
        if (i > 0 && g_heap_blocks[i - 1].free &&
            g_heap_blocks[i - 1].addr + g_heap_blocks[i - 1].size == g_heap_blocks[i].addr) {
            g_heap_blocks[i - 1].size += g_heap_blocks[i].size;
            g_heap_blocks[i].size = 0;
            g_heap_blocks[i].addr = 0;
        }
        return;
    }
}

void xbox_HeapFree(uint32_t xbox_va)
{
    AcquireSRWLockExclusive(&g_heap_lock);
    heap_free_locked(xbox_va);
    ReleaseSRWLockExclusive(&g_heap_lock);
}

HANDLE xbox_GetMappingHandle(void)
{
    return g_mapping_handle;
}

/* Ordering boundary for guest WBINVD over coherent emulated memory.
 * This is not a GPU-completion signal or a replacement for DMA/fence handling.
 * No host cache invalidation is needed because no guest CPU cache is modeled. */
void recomp_guest_cache_flush(void)
{
#ifdef _WIN32
    MemoryBarrier();
#else
    __sync_synchronize();
#endif
}
