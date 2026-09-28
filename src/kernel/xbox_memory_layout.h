/**
 * Xbox Memory Layout Compatibility
 *
 * The Xbox has 64MB of unified memory shared between CPU and GPU.
 * Memory is identity-mapped (physical == virtual for most of it).
 * Game code and data are linked to specific address ranges which vary
 * per game. Section addresses are parsed dynamically from the XBE header
 * at runtime, so this module works with ANY Xbox game.
 *
 * On Windows, we:
 * 1. Create a 64MB file mapping (CreateFileMapping)
 * 2. Map the base view + 28 mirror views at 64MB intervals
 * 3. Parse the XBE section table and copy sections to their Xbox VAs
 * 4. Set up simulated stack, heap, TIB, and kernel data area
 *
 * The mirror views ensure Xbox RAM wrapping works correctly: the Xbox
 * memory controller uses a 26-bit address bus, so ALL addresses wrap
 * modulo 64MB. File mapping views backed by the same section give us
 * true aliases where writes at one address are visible at all mirrors.
 */

#ifndef XBOX_MEMORY_LAYOUT_H
#define XBOX_MEMORY_LAYOUT_H

#include "platform/xbox_winnt.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Xbox memory map constants
 * ================================================================ */

/* Base address of all XBE files in Xbox memory */
#define XBOX_BASE_ADDRESS       0x00010000

/* Start of mapped region - includes low memory (KPCR at 0x0) because
 * game code reads from addresses like 0x20 and 0x28 (Xbox kernel structures). */
#define XBOX_MAP_START          0x00000000

/* Xbox physical memory. 64 MB is the retail default; debug/beta builds ship for
 * 128 MB devkits and allocate accordingly (Halo's cachebeta pre-allocates ~57 MB
 * plus its debug arrays, which only fits on a devkit). Runtime-overridable via
 * xbox_SetTotalRam() before xbox_MemoryLayoutInit(); see g_xbox_total_ram. */
#define XBOX_TOTAL_RAM          (64 * 1024 * 1024)  /* 64 MB (default) */
#define XBOX_DEVKIT_RAM         (128 * 1024 * 1024) /* 128 MB (debug kit) */
#define XBOX_GPU_RESERVED       (4 * 1024 * 1024)   /* ~4 MB for GPU */

/* Actual mapped RAM for this run. Defaults to XBOX_TOTAL_RAM; a title with a
 * devkit build calls xbox_SetTotalRam(XBOX_DEVKIT_RAM) before init. Heap top and
 * mirror stride derive from this, not from the compile-time constant. */
extern size_t g_xbox_total_ram;

/* How much guest address space to map, when that must exceed RAM.
 *
 * These are not the same quantity and conflating them is a bug. RAM is what
 * the console has and what the heap is carved out of; the mapped range is how
 * much guest address space is backed by distinct host pages. The runtime
 * mirrors RAM at intervals of the mapped size, because a real Xbox wraps
 * addresses on a 26-bit bus -- so anything a title allocates above the mapped
 * range silently shares storage with low memory.
 *
 * Half-Life 2 needs this: its allocator sub-allocates past the top of RAM, and
 * at 64 MB its first commit past the boundary (0x04F80000) aliases the base of
 * the live heap (0x00F80000). A CUtlRBTree element array landed at 0x0CB80000,
 * aliasing 0x00B80000, and its links were overwritten between one insert and
 * the next search.
 *
 * Raising g_xbox_total_ram instead does not work: the heap top and anything
 * the guest is told about memory derive from that, so the title sizes itself
 * differently and faults during CRT init. Growing only the mapping leaves both
 * alone.
 *
 * Zero means "same as RAM", which is the existing behaviour for every title
 * that does not ask. Set before xbox_MemoryLayoutInit(). */
extern size_t g_xbox_map_size;
void xbox_SetMapSize(size_t bytes);

/* Carve a pure address-space reservation from the mapped range above RAM.
 * Returns 0 if the mapping is no larger than RAM, or if it is exhausted.
 * See the implementation for why reservations must not come from the heap. */
uint32_t xbox_ReserveAlloc(uint32_t size, uint32_t align);

/* Bounds of the guest's executable sections, derived from the XBE at load.
 *
 * recomp_types.h declares these too, for RECOMP_ICALL_IS_CODE. They are
 * repeated here so hand-written host code -- a fault handler wanting to tell a
 * guest return address on the stack from ordinary data, say -- can use them
 * without including the generated-code header, which redefines `eax` and
 * friends as macros.
 */
/* Full extent of the loaded XBE image -- every section, not just the
 * executable ones. Anything writing guest memory on the title's behalf must
 * stay out of this range. */
extern uint32_t g_xbox_image_lo;
extern uint32_t g_xbox_image_hi;

extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;
void xbox_SetTotalRam(size_t bytes);

/* NOTE: Section addresses (.text, .rdata, .data, etc.) are NOT hardcoded.
 * They are parsed from the XBE header at runtime in xbox_MemoryLayoutInit().
 * This allows the toolkit to work with ANY Xbox game without modification. */

/* ================================================================
 * Memory initialization
 * ================================================================ */

/**
 * Initialize the Xbox memory layout.
 *
 * Reserves the virtual address range 0x00010000 through 0x0076F000
 * and maps the XBE sections to their expected addresses:
 * - .rdata: copied from XBE, read-only
 * - .data: initialized portion copied from XBE, BSS zeroed
 *
 * Note: .text is NOT mapped here - the recompiled code is native
 * Windows code and doesn't need to be at the original address.
 * The data sections DO need to be at their original addresses
 * because the recompiled code references globals by absolute address.
 *
 * @param xbe_data  Pointer to the loaded XBE file contents.
 * @param xbe_size  Size of the XBE file.
 * @return TRUE on success, FALSE on failure.
 */
BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size);

/**
 * Release the reserved Xbox memory layout.
 */
/**
 * Mirror a GPU completion fence the title spins on.
 *
 * The NV2A tables in xbox_memory_layout.c acknowledge handshakes that live at
 * fixed aperture offsets. Some titles instead wait on a semaphore the GPU
 * writes into contiguous memory: D3D seeds it, submits work, then spins until
 * it reaches the submitted count. Wreckless does this at guest 0x000FE920,
 * waiting on the 96-byte MmAllocateContiguousMemoryEx block its device struct
 * points at.
 *
 * Nothing executes the push buffer -- the D3D11 layer draws -- so everything
 * submitted is complete, and advancing the fence is the same honest
 * acknowledgement the register tables make.
 *
 * The fence has no fixed address; it is reached through the title's device
 * struct, so it is registered as the chain of indirections to follow:
 *
 *     device = MEM32(device_ptr_va)
 *     fence  = MEM32(device + ptr_off)
 *     MEM32(fence) = MEM32(device + src_off)
 *
 * Every step is bounds-checked each poll, so registering a chain that is not
 * yet initialised (or never becomes valid) is harmless.
 *
 * src_off names the device field holding the fence VALUE the title compares
 * against, and is not necessarily a push buffer position. JSRF waits on the
 * word at `*(device + 0x34)` and compares it against the counter at
 * `device + 0x30`; publishing the push buffer position there instead makes
 * the wait's `limit - value` subtraction wrap and the comparison invert, so
 * the wait never ends. What the wait needs is the counter, which is what the
 * synchronous model has completed.
 *
 * Returns 0 on success, -1 if the table is full.
 */
/* Advance a frame/swap counter inside the D3D device at ~60 Hz.
 *
 * A title that waits a frame reads the device's swap count and spins until it
 * moves. Nothing here presents, so without this the count never changes and
 * the wait never ends. Followed through the device pointer, like the fence,
 * because the device is allocated at runtime.
 *
 * Returns 0 on success, -1 if the table is full. */
/* Mirror one field of the device struct onto another, for the counter pair a
 * swap throttle waits on: the title bumps "frames submitted" itself and waits
 * for "frames completed", which only the GPU moves on hardware. Unlike
 * xbox_Nv2aFrameCounter this cannot run ahead of the title, so the unsigned
 * submitted - completed it gets compared against cannot underflow. */
int xbox_Nv2aMirrorCounter(uint32_t device_ptr_va,
                           uint32_t src_off, uint32_t dst_off);

int xbox_Nv2aFrameCounter(uint32_t device_ptr_va, uint32_t counter_off);
/* Advance those counters now, because a swap really completed. Called by the
 * pushbuffer executor on FLIP_STALL; while these arrive the 60 Hz fallback
 * stands down, so the count follows what was actually drawn. */
void xbox_Nv2aFrameCounterFlip(void);

/* Tell the runtime where the display framebuffer is (from AvSetDisplayMode). */
void xbox_SetDisplayFramebuffer(uint32_t fb_va, uint32_t pitch);
/* Read it back: 0 until the title sets a mode. Pitch is optional. */
uint32_t xbox_GetDisplayFramebuffer(uint32_t *pitch);

/* Allocate from the contiguous (physical-mirror) arena. Returns a guest VA
 * below 256 MB, or 0 when the arena is exhausted. */
uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment);
uint32_t xbox_ContiguousAllocatedBytes(void);

int xbox_Nv2aMirrorFence(uint32_t device_ptr_va,
                         uint32_t src_off, uint32_t ptr_off);

void xbox_MemoryLayoutShutdown(void);

/**
 * Check if an address falls within the Xbox memory map.
 */
BOOL xbox_IsXboxAddress(uintptr_t address);

/**
 * Get the base pointer for direct memory access.
 * Returns NULL if memory layout is not initialized.
 */
void *xbox_GetMemoryBase(void);

/**
 * Get the offset from Xbox VA to actual mapped address.
 * actual_address = xbox_va + offset
 * Returns 0 if memory is mapped at original Xbox addresses (ideal case).
 */
ptrdiff_t xbox_GetMemoryOffset(void);
/* Bytes of guest address space mapped, for bounds-checking a guest pointer
 * that came out of guest memory rather than from this side. */
size_t xbox_GetMappedSize(void);
void xbox_ProtectMirrorsForDebug(void);

/* Dump the guest call stack and abort if the title has not exited within
 * RECOMP_WATCHDOG_SECS seconds. Call from the thread that runs guest code;
 * does nothing unless that variable is set. */
void xbox_WatchdogStart(void);

/* Print the globals named by RECOMP_PEEK, tagged with `label`. No-op when
 * RECOMP_PEEK is unset. Called at a hang and at an early exit. */
void xbox_PeekSample(const char *label);

/* ================================================================
 * Xbox stack for recompiled code
 * ================================================================ */

/* ================================================================
 * Kernel data export area
 * ================================================================ */

/** Base VA for kernel data exports (XboxHardwareInfo, XboxKrnlVersion, etc.)
 *  These are kernel exports that are DATA, not functions. The game reads
 *  their thunk entries and dereferences them to access the data. */
#define XBOX_KERNEL_DATA_BASE   0x00740000
#define XBOX_KERNEL_DATA_SIZE   4096   /* 4 KB - plenty for all data exports */

/* Offsets within the kernel data area */
#define KDATA_HARDWARE_INFO     0x000  /* XBOX_HARDWARE_INFO (8 bytes) */
#define KDATA_KRNL_VERSION      0x010  /* XBOX_KRNL_VERSION (8 bytes) */
#define KDATA_TICK_COUNT        0x020  /* KeTickCount (4 bytes) */
#define KDATA_LAUNCH_DATA_PAGE  0x030  /* LaunchDataPage (4 bytes, pointer) */
#define KDATA_THREAD_OBJ_TYPE   0x040  /* PsThreadObjectType (4 bytes) */
#define KDATA_EVENT_OBJ_TYPE    0x050  /* ExEventObjectType (4 bytes) */
#define KDATA_XE_IMAGE_FILENAME 0x060  /* XeImageFileName (ANSI_STRING) */
#define KDATA_IO_COMPLETION_TYPE 0x070 /* IoCompletionObjectType (4 bytes) */
#define KDATA_IO_DEVICE_TYPE    0x080  /* IoDeviceObjectType (4 bytes) */
/* Object-type exports a title may compare against each other, so each needs a
 * distinct non-zero value rather than a shared placeholder. */
#define KDATA_MUTANT_OBJ_TYPE   0x090  /* ExMutantObjectType (4 bytes) */
#define KDATA_SEMAPHORE_OBJ_TYPE 0x0A0 /* ExSemaphoreObjectType (4 bytes) */
#define KDATA_TIMER_OBJ_TYPE    0x0B0  /* ExTimerObjectType (4 bytes) */
#define KDATA_FILE_OBJ_TYPE     0x0C0  /* IoFileObjectType (4 bytes) */
#define KDATA_TIME_INCREMENT    0x0D0  /* KeTimeIncrement (4 bytes) */
#define KDATA_BOOT_SMC_VIDEO    0x0E0  /* HalBootSMCVideoMode (4 bytes) */
#define KDATA_IDEX_CHANNEL      0x500  /* IDE_CHANNEL_OBJECT (512-byte reserved region) */
#define KDATA_HD_KEY            0x100  /* XboxHDKey (16 bytes) */
#define KDATA_SIGNATURE_KEY     0x110  /* XboxSignatureKey (16 bytes) */
#define KDATA_LAN_KEY           0x120  /* XboxLANKey (16 bytes) */
#define KDATA_ALT_SIGNATURE_KEYS 0x130 /* XboxAlternateSignatureKeys (256 bytes) */
#define KDATA_XE_PUBLIC_KEY     0x300  /* XePublicKeyData (284 bytes) */
/* HAL disk identity strings (ordinals 41/42). Each is an XBOX_ANSI_STRING
 * (Length, MaximumLength, Buffer VA) followed by the string bytes it points to,
 * because HalRandGather dereferences Buffer to read the text as entropy. */
#define KDATA_DISK_MODEL_STR    0x420  /* XBOX_ANSI_STRING (8 bytes) */
#define KDATA_DISK_MODEL_BUF    0x430  /* model text (up to 48 bytes) */
#define KDATA_DISK_SERIAL_STR   0x460  /* XBOX_ANSI_STRING (8 bytes) */
#define KDATA_DISK_SERIAL_BUF   0x470  /* serial text (up to 32 bytes) */
#define KDATA_DISK_CACHE_PARTS  0x4A0  /* HalDiskCachePartitionCount (4 bytes) */
/* XeImageFileName's text. The exported symbol at KDATA_XE_IMAGE_FILENAME is
 * an XBOX_ANSI_STRING, and a title dereferences its Buffer -- the CRT reads
 * it to work out the running image's path. The struct was declared without
 * anything to point at, so Buffer held whatever was in that page. */
#define KDATA_XE_IMAGE_BUF      0x4B0  /* image path text (up to 64 bytes) */

/** Size of the simulated Xbox stack (8 MB).
 *  Increased from 1 MB because failed RECOMP_ICALL indirect calls
 *  can leak stdcall args onto the stack each frame. An 8 MB stack
 *  provides enough headroom for extended gameplay sessions. */

/* Thread-local storage class for the recompiled register set. Must match
 * templates/runtime/recomp_types.h -- a mismatch is a link-time surprise. */
#if defined(_MSC_VER)
#  define RECOMP_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define RECOMP_TLS __thread
#else
#  define RECOMP_TLS _Thread_local
#endif

/* SSE register storage, shared with the generated code. Defined in both this
 * header and templates/runtime/recomp_types.h -- a translation unit can end up
 * including both, so the guard keeps that from being a redefinition. Keep the
 * two identical: the generated code and the runtime have to agree on the
 * layout, and nothing else checks. */
#ifndef RECOMP_MMX_DEFINED
#define RECOMP_MMX_DEFINED
typedef union RecompMmx {
    int8_t   b[8];
    uint8_t  ub[8];
    int16_t  w[4];
    uint16_t uw[4];
    int32_t  d[2];
    uint32_t ud[2];
    uint64_t q;
} RecompMmx;
#endif

#ifndef RECOMP_XMM_DEFINED
#define RECOMP_XMM_DEFINED
typedef union RecompXmm {
    float    f[4];
    double   d[2];
    uint32_t u[4];
    int32_t  i[4];
    uint64_t q[2];
} RecompXmm;
#endif

#define XBOX_STACK_SIZE     (8 * 1024 * 1024)

/** Base VA of the stack area (above last XBE section). */
/* Where the fake TIB lives -- the linear address fs: is based at.
 *
 * Deliberately not 0. The TIB used to sit on page zero, because the lifter
 * dropped the fs prefix and fs:[N] became linear [N]. That made a null
 * dereference read or write the TIB instead of faulting: a null check of the
 * form `cmp byte [ecx], 0` saw the exception-chain head's 0xFF and passed, and
 * a store through a null pointer quietly overwrote that head. Both then
 * surfaced somewhere else entirely. With the TIB up here, page zero is left
 * unmapped and either mistake faults where it happens.
 *
 * Sits below every XBE's image base (0x00010000), so it displaces nothing.
 *
 * 0x4000 rather than 0x1000 because protection is applied at *host* page
 * granularity. Apple Silicon pages are 16 KB, so RECOMP_TRAP_NULL asking to
 * protect guest page zero actually covers guest 0..0x3FFF -- which reached a
 * TIB at 0x1000 and killed the run, so the guard disabled itself on every
 * such host and the diagnostic quietly did nothing. At 0x4000 the largest
 * page any supported host uses fits below the TIB and the guard installs.
 * Nothing else lives in the low 64 KB, and no guest code names the address:
 * fs: resolves through g_fs_base.
 *
 * Per-thread, because a TIB is. It used to be one constant address for the
 * whole process, which meant every guest thread shared one SEH chain head
 * and -- through fs:[4] -- one CRT per-thread data block. Half-Life 2
 * deadlocked on that: two threads in _lock() each holding the CRT lock the
 * other wanted, because the bookkeeping that decides who owns what was
 * shared between them.
 *
 * XBOX_TIB_MAIN is where the first thread's TIB is built; every spawned
 * thread gets its own from xbox_AllocThreadTib() and points g_fs_base at
 * it. */
#define XBOX_TIB_MAIN       0x00004000
extern RECOMP_TLS uint32_t g_fs_base;
#define XBOX_FS_BASE        g_fs_base

#define XBOX_STACK_BASE     0x00780000

/** Initial ESP value (top of stack, 16-byte aligned). */
#define XBOX_STACK_TOP      (XBOX_STACK_BASE + XBOX_STACK_SIZE - 16)

/* ================================================================
 * Worker stack slices (host-tick-driven titles)
 * ================================================================
 *
 * A second way to drive a recompiled title, ported from the Burnout 3 fork as
 * the runtimes reunite (see docs/technical/burnout3-reunification.md).
 *
 * The default model (Halo, Crimson Skies) runs the game's entry routine inline
 * and it drives its own main loop. Some titles instead return from their entry
 * after spawning an init thread, and expect the *host* to drive the per-frame
 * tick -- Burnout 3 is tick-driven, not main-loop-driven. To call recompiled
 * code from the host's own message-loop thread, that thread needs a guest stack
 * (its g_esp starts at 0), which is what a worker slice provides.
 *
 * These slices carve the low end of the same 8 MB stack region xbox_AllocThreadStack
 * uses, and a title uses one model or the other -- never both -- so they do not
 * coexist at runtime. For a title that never calls xbox_worker_stack_alloc
 * (every default-model title), this is unused address space and dead code, so
 * adding it changes nothing for them.
 */
#define XBOX_WORKER_STACK_SIZE   (256 * 1024)
#define XBOX_WORKER_STACK_BASE   XBOX_STACK_BASE             /* 0x00780000 */
#define XBOX_WORKER_STACK_COUNT  16                          /* 4 MB total */
#define XBOX_WORKER_STACK_END    (XBOX_WORKER_STACK_BASE + \
                                  XBOX_WORKER_STACK_SIZE * XBOX_WORKER_STACK_COUNT)

/** Top (initial esp) of worker stack slice n, 16-byte aligned, growing down. */
#define XBOX_WORKER_STACK_TOP(n) (XBOX_WORKER_STACK_BASE + \
                                  XBOX_WORKER_STACK_SIZE * ((n) + 1) - 16)

/* ================================================================
 * Xbox dynamic heap (for MmAllocateContiguousMemory, etc.)
 * ================================================================ */

/** Base VA of the dynamic heap area (above stack). */
#define XBOX_HEAP_BASE      (XBOX_STACK_BASE + XBOX_STACK_SIZE)  /* 0x00F80000 */

/** Exclusive top of the dynamic heap: the end of RAM for this run. Runtime,
 *  not a macro, because RAM size is now configurable (retail 64 MB vs devkit
 *  128 MB). The total mapped region (data + stack + heap) equals RAM so the
 *  engine's memory probing stops at the correct boundary. */
/* The heap runs to the end of the *mapped* range, not the end of RAM.
 *
 * When a title maps more address space than it has RAM, a large
 * MEM_RESERVE has to come from somewhere. Carving it out of a separate
 * arena above the heap looked tidy and was wrong: the guest CRT's own
 * bookkeeping never learns about that region, so realloc's block lookup
 * fails for a pointer in it and the copy is skipped -- a grown buffer
 * comes back empty with the old one still intact. Half-Life 2 loses a
 * 129-node CUtlRBTree that way, and the tree then self-cycles.
 *
 * Letting the ordinary heap serve the whole mapped range keeps every
 * allocation inside one allocator the guest already understands.
 */
#define XBOX_HEAP_TOP       ((uint32_t)(g_xbox_map_size ? g_xbox_map_size \
                                                        : g_xbox_total_ram))

/** No static mirror/guard region. RAM mirror is handled via file mapping
 *  views that alias the same physical pages as the base 64 MB region. */
#define XBOX_MIRROR_SIZE    0
#define XBOX_GUARD_SIZE     0

/** Number of 64 MB mirror views to pre-map (covers 1.75 GB of address space). */
#define XBOX_NUM_MIRRORS    28

/* Tiled / write-combined aperture. The NV2A shows physical RAM again here, and
 * titles render through it: physical page P is at XBOX_TILED_BASE + P. Aliases
 * the RAM mapping rather than getting its own storage, because a title writes a
 * surface through the tiled address and reads it back through the normal one. */
#define XBOX_TILED_BASE     0xF0000000u

/**
 * Allocate from the Xbox heap. Returns an Xbox VA, or 0 on failure.
 * Alignment must be a power of 2 (minimum 4).
 * Thread-safe: no (single-threaded recompiled code).
 */
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

/* A2h alias first-touch census (gated by JSRF_TRACE_A2H_DR, observation only).
 *
 * Read-only-protects the 4 KiB page holding guest VA 0x001C4064 in every MAPPED mirror view and
 * records the first write fault to each. Returns 1 only when every mapped view was protected --
 * a mapped view that could not be protected is a coverage failure, not a warning. Disarm restores
 * the original protections. Both are no-ops with the gate unset. */
uint32_t xbox_A2hAliasCensusArm(void);
void xbox_A2hAliasCensusDisarm(void);

/* ── A2h LIVE slot-write watch: PAGE-PROTECTION ONLY, PAGE-GRANULAR ─────────────────────────────
 *
 * Gate: JSRF_TRACE_A2H_SLOTW. OFF by default; with it unset no page is protected, no handler is
 * registered, no counter moves and nothing is printed.
 *
 * MECHANISM. The watched object is not an address: the slot is `MEM32(0x19DCE0) + 0x242C`, a
 * runtime-allocated software-device field. The slot's 4 KiB page is made PAGE_READONLY in the
 * canonical view AND in every mapped RAM mirror view of the same physical page, so a store is
 * caught whichever alias it arrives through. Reads are untouched: an RO page faults on WRITE and
 * not on READ, so the poll loop's read volume is free and no read filtering is needed.
 *
 * PAGE GRANULARITY IS LOAD-BEARING AND DELIBERATE. Protection is page-granular, so the watch sees
 * EVERY write to the page, not only writes to the slot. A fault is a slot hit only when its
 * effective address equals the re-derived slot VA for that alias; every other fault is TRAFFIC.
 *
 * NO DEBUG REGISTERS. There is no DR0/DR6/DR7 anywhere in this facility, and none may be added:
 * the DR channel is EXCLUDED, not demoted.
 *
 * OBSERVATION ONLY. The one thing the handler changes is the faulting thread's own single-step
 * (TF) state, so the faulting store can execute and be read back; the store itself is executed by
 * the guest, unmodified, and no guest register, memory, allocation or device state is altered. */
#define XBOX_A2H_SLOTW_GATE        "JSRF_TRACE_A2H_SLOTW"
#define XBOX_A2H_SLOTW_DEVICE_PTR  0x0019DCE0u   /* software_device pointer global (a POINTER) */
#define XBOX_A2H_SLOTW_SLOT_OFFSET 0x242Cu       /* context+0x1C4 == software_device+0x242C */
#define XBOX_A2H_SLOTW_PAGE_SIZE   4096u
#define XBOX_A2H_SLOTW_PAGES_MAX   (1 + XBOX_NUM_MIRRORS)
#define XBOX_A2H_SLOTW_EVENTS_MAX  256
#define XBOX_A2H_SLOTW_THREADS_MAX 64
#define XBOX_A2H_SLOTW_VERSION     2u

/* The required positive control, as an ENCODING signature rather than a guessed native address.
 *
 * `0x0018CE3A  mov [ecx+0x242c], eax` assembles to `89 81 2C 24 00 00` -- a ModRM disp32 store
 * carrying the literal displacement 0x242C. The candidate `0x00199F45  mov [esi+ebp*4+0x3ec], eax`
 * assembles to `89 84 AE EC 03 00 00` -- a SIB store whose displacement is 0x3EC and which contains
 * no 0x242C anywhere. A native RIP is NOT a guest VA and cannot be compared to one; the bytes AT
 * the recorded RIP can be read, and they name the instruction exactly. Both forms are therefore
 * classified from the faulting instruction itself, and the installer's value 0x0015F9D0 is
 * recorded alongside.
 *
 * ⚠ THE CONTROL IS THE ENCODING, NOT THE VALUE. Requiring the pre-value to be zero as well was a
 * defect: it made the control depend on nothing having touched the slot earlier, so a run in which
 * the control DID fire could still be reported as INFRA FAILURE. The value is carried in the step
 * record's post_value and compared offline. */
#define XBOX_A2H_SLOTW_ENC_UNKNOWN  0u
#define XBOX_A2H_SLOTW_ENC_MODRM    1u   /* disp32 == 0x242C: the installer's own encoding */
#define XBOX_A2H_SLOTW_ENC_SIB      2u   /* disp32 == 0x3EC with SIB: the candidate's encoding */
#define XBOX_A2H_SLOTW_ENC_OTHER    3u   /* a store to the page with neither displacement */

/* WHAT A READER KEYS ON. `enc == MODRM` with `slot_hit == 1` IS the control; the value is a
 * comparison made offline against these two constants, and the two encodings are distinct so the
 * control and the target cannot be confused. */
#define XBOX_A2H_SLOTW_INSTALL_VALUE 0x0015F9D0u   /* what the installer writes */
#define XBOX_A2H_SLOTW_CANDIDATE_SIB 0x000003ECu   /* the candidate's SIB displacement */

/* One bounded, full record per observed page write. Nothing here is sampled and nothing is
 * first-N: a full record is written for every fault until the capacity is reached, and the
 * overflow latch is set when it is exceeded. Overflow invalidates absence/order rows; it does not
 * invalidate a positive record, which stands on its own evidence. */
typedef struct {
    uint32_t seq;          /* ordered event id, monotonic from ARM; THE ordering key */
    uint32_t kind;         /* 1 write fault, 2 step re-arm, 3 fourth-read sample, 4 installer control */
    uint32_t alias_index;  /* 0 = canonical view, m+1 = mirror view m */
    uint32_t slot_hit;     /* 1 = the effective address IS the re-derived slot VA for this alias */
    uint32_t fault_va;     /* low 32 bits of the host fault address */
    uint32_t pre_value;    /* canonical slot value BEFORE the store executed */
    uint32_t post_value;   /* canonical slot value AFTER the store executed (step events only) */
    uint32_t tid;          /* native thread id that took the fault */
    uint32_t enc;          /* XBOX_A2H_SLOTW_ENC_* classified from the bytes at rip */
    uint32_t reserved;
    uint64_t rip;          /* native instruction pointer of the faulting store, verbatim */
    uint64_t ticks;        /* QPC at the fault, for cross-thread ordering */
} XboxA2hSlotwEvent;

/* Loss accounting. Every quantity is an UNCAPPED counter; none is derived from a bounded array. */
typedef struct {
    uint64_t relevant_av;            /* write AVs on an owned protected page */
    uint64_t slot_hits;              /* of those, effective address == the slot */
    uint64_t nonslot_writes;         /* of those, a write elsewhere on the page (TRAFFIC) */
    uint64_t steps;                  /* single-steps claimed for our own faulting store */
    uint64_t rearm_ok;               /* successful RO re-protect after a step */
    uint64_t rearm_failed;           /* FAILED re-protect: the page is open, a coverage hole */
    uint64_t protected_intervals;    /* times a page went RO (arm + every re-arm) */
    uint64_t unprotected_intervals;  /* times a page went RW (open for a step) */
    uint64_t concurrent_overlap;     /* a write to an owned page while another thread was stepping */
    uint64_t threads_new;            /* threads first seen by this watch */
    uint64_t threads_gone;           /* threads seen at arm that later exited */
    uint64_t publish_failed;         /* record could not be published: page left CLOSED */
    uint64_t protect_failed;         /* VirtualProtect RO refused at arm or re-arm */
    uint64_t dropped_events;         /* bounded event array full: record lost */
    uint64_t unexpected_exception;   /* an exception this handler saw and did not own */
    uint64_t db_unowned;             /* a #DB with no pending owner: never consumed */
    uint64_t db_own_serviced;        /* own-TF owners serviced exactly once */
    uint64_t db_ac97_serviced;       /* AC97-TF owners serviced exactly once */
    uint64_t db_dual_serviced;       /* #DB with BOTH owners pending: both serviced once */
    uint64_t read_samples;           /* instrumented fourth-read-path samples */
    uint64_t installer_control_hits; /* the required positive control fired */
    uint64_t overflow;               /* 1 = any bounded array overflowed (a LATCH, not a count) */
    uint64_t base_changed;           /* 1 = MEM32(0x19DCE0) moved between ARM and TERMINAL */
} XboxA2hSlotwLoss;

typedef struct {
    uint32_t magic;       /* 'A2SW' */
    uint32_t version;     /* XBOX_A2H_SLOTW_VERSION */
    uint32_t size;        /* sizeof(XboxA2hSlotwLedger): a reader checks this before reading */
    uint32_t armed;
    uint32_t arm_base;    /* MEM32(0x19DCE0) read at ARM */
    uint32_t term_base;   /* MEM32(0x19DCE0) re-read at TERMINAL */
    uint32_t arm_slot;    /* checked 32-bit addition arm_base + 0x242C, at ARM */
    uint32_t term_slot;   /* re-derived at TERMINAL; a difference is a RE-SCOPE, never a compare */
    uint32_t slot_stable; /* 1 = arm_slot == term_slot */
    uint32_t page_offset; /* arm_slot & 0xFFF: the slot's offset within its protected page */
    uint32_t mapped_mask; /* bit m = alias m present (bit 0 = canonical view) */
    uint32_t protect_mask;/* bit m = alias m successfully PAGE_READONLY */
    uint32_t alias_count;
    uint32_t protected_count;
    uint32_t event_count; /* full records written */
    uint32_t event_overflow;
    uint32_t thread_count;
    uint32_t thread_overflow;
    uint32_t terminal_seen;
    uint32_t terminal_target;   /* the raw value the fourth read produced, re-read at TERMINAL */
    uint32_t arm_reason;        /* why ARM refused, when armed == 0 */
    /* ⚠ THE TERMINAL BASE CAN FAIL TO BE A POINTER AT ALL, AND THAT IS NOT THE SAME FINDING AS A
     * MOVED ONE. MEASURED on an archived run: at the terminal point MEM32(0x19DCE0) held
     * 0x30766A64 -- ASCII "djv0", string data from the 0x001D5078 region -- rather than a device
     * pointer. 0x19DCE0 and the slot 0x19D62C are on the SAME PAGE (0x0019D000), so the global is
     * itself inside the watched page and is written by the same data traffic the watch exists to
     * sort from the slot. `term_base_ok` separates "the base is still a plausible guest pointer but
     * different (RE-SCOPE)" from "the base is no longer a pointer (the global was overwritten)",
     * because a reader must not treat the second as a device move. */
    uint32_t term_base_ok;
    uint32_t reserved;
    uint64_t arm_ticks;
    uint64_t terminal_ticks;
    /* The FOURTH-read latch, tied to the ledger by ORDERED EVENT IDs and not by log chronology. */
    uint32_t read_count;        /* 0x00193E62 reads instrumented since ARM */
    uint32_t fourth_reached;    /* 1 once read #4 was instrumented */
    uint32_t fourth_value;      /* the value that read produced */
    uint32_t fourth_seq;        /* event seq of that read sample */
    uint32_t last_write_seq;    /* seq of the LAST slot-hit write at or before that read */
    uint32_t last_write_enc;    /* its encoding class */
    uint32_t last_write_alias;  /* its alias index */
    uint32_t last_write_value;  /* its post-value */
    uint64_t last_write_rip;
    uint64_t last_write_ticks;
    XboxA2hSlotwLoss loss;
    XboxA2hSlotwEvent events[XBOX_A2H_SLOTW_EVENTS_MAX];
} XboxA2hSlotwLedger;

/* The live ledger. NON-STATIC AND STABLE-NAMED on purpose: tools/harness/collect.c is a separate
 * process and resolves it BY SYMBOL from the target's PDB, the same way it resolves g_jsrf_debug,
 * so the archive carries the ledger bytes rather than a re-print of them. */
extern XboxA2hSlotwLedger g_xbox_a2h_slotw;

/* Arm/disarm. Arm reads MEM32(0x19DCE0), derives the slot by CHECKED 32-bit addition, maps and
 * RO-protects the canonical page and every mapped mirror alias, and returns 1 only when every
 * mapped alias was protected. Returns 0 -- and protects nothing -- when the gate is unset, the
 * device pointer is not yet a plausible guest RAM address, or any mapped alias refuses protection.
 *
 * ARM IS DEFERRED IN PRACTICE, AND THAT IS CORRECT RATHER THAN A WORKAROUND. `MEM32(0x19DCE0)` is a
 * POINTER to an object the title allocates at runtime, so before the title's D3D device exists the
 * slot has no address at all. Arming at process start would read a zero and protect nothing.
 * xbox_A2hSlotWatchStart() is therefore called from the point where the title's device is known to
 * be live, and it arms on the first poll at which the pointer is plausible -- so the ARM base is a
 * READ of the live device, never a preselected address. The arm_reason field records a refusal. */
#define XBOX_A2H_SLOTW_ARM_NOT_YET  0u   /* pointer not plausible yet: keep waiting */
#define XBOX_A2H_SLOTW_ARM_OK       1u
#define XBOX_A2H_SLOTW_ARM_NO_VEH   2u
#define XBOX_A2H_SLOTW_ARM_COVERAGE 3u   /* a mapped alias could not be protected */
uint32_t xbox_A2hSlotWatchArm(void);
void xbox_A2hSlotWatchDisarm(void);
/* Start the deferred ARM and the all-thread census. Returns immediately; the poll thread arms as
 * soon as MEM32(0x19DCE0) names a plausible device. No-op with the gate unset. */
void xbox_A2hSlotWatchStart(void);
/* Stop the census, disarm and restore every protected page. */
void xbox_A2hSlotWatchStop(void);
/* Re-derive the slot from MEM32(0x19DCE0) at the terminal point and record whether it moved. */
void xbox_A2hSlotWatchTerminal(uint32_t target);
/* Instrumented `0x00193E62 mov eax,[esi+0x1C4]` -> `0x00193EB5 call eax`. `value` is what the read
 * produced and `read_index` is which read of that site this is, recovered from the guest's own
 * counter rather than from faulting reads (an RO page must let the poll loop's read volume pass
 * silently, so reads cannot be trapped). The latch ties the value to the ledger by ORDERED EVENT
 * IDs, not by sampled log chronology. */
void xbox_A2hSlotWatchNoteFourthRead(uint32_t value, uint32_t read_index);
/* 1 when the gate is set. The game checks this instead of reading the environment itself, so the
 * gate is read exactly once and OFF is provably inert. */
int xbox_A2hSlotWatchEnabled(void);

/**
 * Free a block from the Xbox heap. Currently a no-op (bump allocator).
 */
void xbox_HeapFree(uint32_t xbox_va);

/**
 * Bytes remaining in the heap block containing this guest address, or 0 if the
 * heap never handed it out. Backs MmQueryAllocationSize and
 * ExQueryPoolBlockSize -- the host cannot answer either, since VirtualQuery on
 * the translated address describes the whole guest mapping.
 */
uint32_t xbox_HeapBlockSize(uint32_t xbox_va);

/**
 * Get the file mapping handle for the Xbox memory region.
 * Used by the VEH handler to map additional mirror views on demand.
 * Returns NULL if file mapping is not available.
 */
HANDLE xbox_GetMappingHandle(void);

/* Atomically retire the legacy NV2A register mutator and wait for any
 * in-flight iteration before a serialized register owner protects the BAR. */
void xbox_Nv2aClaimRegisterOwner(void);

#ifdef __cplusplus
}
#endif


/* Carve a simulated stack for a spawned thread. Returns the Xbox VA of the
 * stack top, or 0 when the pool is exhausted. */
uint32_t xbox_AllocThreadStack(void);

/* A TIB and TLS block for a newly spawned guest thread, copied from the
 * template the loader built. Returns the new TIB's Xbox VA, or 0. */
uint32_t xbox_AllocThreadTib(void);

/**
 * Return a worker's stack when the worker ends. Takes the value
 * xbox_AllocThreadStack returned. Without this the pool counts threads ever
 * created rather than threads alive, and a title that cycles workers exhausts
 * it -- after which PsCreateSystemThreadEx runs them inline, which deadlocks
 * any caller that then waits for the worker it thought it had spawned.
 */
void xbox_FreeThreadStack(uint32_t stack_top);

/* Worker stack slices for host-tick-driven titles (see XBOX_WORKER_STACK_* and
 * docs/technical/burnout3-reunification.md). Additive; unused by default-model
 * titles. */
int  xbox_worker_stack_alloc(void);   /* slice index, or -1 if none free */
void xbox_worker_stack_free(int slot);
void   xbox_set_game_thread(void *h);  /* HANDLE, recorded for the host watchdog */
void  *xbox_thread_debug_handle(void); /* the game thread, or NULL under inline model */

/* PsCreateSystemThreadEx behaviour. Default INLINE runs the first call as the
 * game (Halo, Crimson Skies). SPAWN makes every call a real thread so a
 * host-tick-driven title's entry can return and let the host drive -- call
 * xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN) before the game starts. */
#define XBOX_THREAD_MODE_INLINE 0
#define XBOX_THREAD_MODE_SPAWN  1
void xbox_SetThreadMode(int mode);

#endif /* XBOX_MEMORY_LAYOUT_H */
