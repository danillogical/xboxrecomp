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
/* Give back a block xbox_ContiguousAlloc returned: 1 if freed, 0 if addr is not
 * the start of a live block. Bytes from va to the end of its live block, or 0. */
int xbox_ContiguousFree(uint32_t addr);
uint32_t xbox_ContiguousBlockSize(uint32_t va);

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
void xbox_WatchInit(void);

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
/* The remaining DATA exports, sized to the kernel's own layouts (nxdk
 * xboxkrnl.h), above the IDE channel's 512-byte region. */
#define KDATA_KD_DEBUGGER_ENABLED     0x700  /* KdDebuggerEnabled (BOOLEAN) */
#define KDATA_KD_DEBUGGER_NOT_PRESENT 0x710  /* KdDebuggerNotPresent (BOOLEAN) */
#define KDATA_MMGLOBAL          0x720  /* MMGLOBALDATA (8 pointers, 32 bytes) */
#define KDATA_INTERRUPT_TIME    0x740  /* KeInterruptTime (KSYSTEM_TIME, 12 bytes) */
#define KDATA_SYSTEM_TIME       0x750  /* KeSystemTime (KSYSTEM_TIME, 12 bytes) */
#define KDATA_BUGCHECK_DATA     0x760  /* KiBugCheckData (ULONG[5], 20 bytes) */
#define KDATA_OBJ_DIR_TYPE      0x780  /* ObDirectoryObjectType (OBJECT_TYPE, 28 bytes) */
#define KDATA_OBJ_HANDLE_TABLE  0x7A0  /* ObpObjectHandleTable (OBJECT_HANDLE_TABLE, 48 bytes) */
#define KDATA_OBJ_SYM_LINK_TYPE 0x7D0  /* ObSymbolicLinkObjectType (OBJECT_TYPE, 28 bytes) */
#define KDATA_EEPROM_KEY        0x7F0  /* XboxEEPROMKey (16 bytes) */

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
/* ⚠ CAPACITY, AND WHY IT IS NOT THE POINT. MEASURED: ON trial 1 exhausted a 256-record buffer with
 * 129 page writes, because EVERY write consumed TWO records (the fault and its step), and the
 * fail-closed propagation then ended the run. The repair is not "a bigger buffer": the packet's own
 * design already says non-slot page writes are TRAFFIC, and traffic is COUNTED, not recorded. With
 * that implemented, a page-write stream of any length consumes ZERO records here.
 *
 * The array is nonetheless enlarged, because "zero headroom" was itself part of the defect: 1024
 * records is 512 slot writes plus their step records, and the observed run had 128 steps in a
 * partial boot. A reader must never have to reason about the buffer filling on ordinary traffic,
 * because ordinary traffic cannot reach it. */
#define XBOX_A2H_SLOTW_EVENTS_MAX  1024
/* The FIRST-TOUCH CENSUS: one bounded record per DISTINCT faulting address on the watched page,
 * written once. This is the second and last class of detailed record the packet's design admits.
 * 512 distinct dwords is one eighth of the 4 KiB page and is far above any observed touch count. */
#define XBOX_A2H_SLOTW_FIRST_TOUCH_MAX 512
#define XBOX_A2H_SLOTW_THREADS_MAX 64
#define XBOX_A2H_SLOTW_VERSION     4u

/* ── THE RANGE CLASSIFIER: NATIVE-DOMAIN, REPLACING THE VOID GUEST-BYTE PARADIGM ────────────────
 *
 * ⚠⚠ THE PARADIGM THIS REPLACES WAS VOID, AND IT IS RECORDED HERE SO IT IS NEVER REBUILT.
 *
 * `a2h_slotw_classify_store` used to read the NATIVE instruction bytes at the faulting RIP and test
 * them against GUEST ENCODINGS -- `p[0] == 0x89`, `(modrm & 0xC0) == 0x80`, `disp32 == 0x242C` or
 * `0x3EC`. In recompiled code the native instruction at a faulting RIP is GENERATED C, compiled by
 * the host toolchain; it is not the guest's `mov [ecx+0x242c],eax` and it never will be. Those
 * expectations could not match, so every encoding classification this facility ever emitted from a
 * fault RIP was UNSOUND -- including the `enc=3` on ON-3 and the `enc` fields on ON-1/ON-2.
 *
 * ⚠ THE CLASSIFICATION IS NOW BY ADDRESS RANGE, IN THE NATIVE DOMAIN, AND GUEST-BYTE
 * EXPECTATIONS ARE FORBIDDEN FOR NATIVE RIPs. There is no encoding test left anywhere in this
 * facility, and none may be reintroduced:
 *
 *     RIP inside a RECOMPILED function's body       -> GAME_MODULE
 *     else RIP inside the loaded image's OWN bounds  -> TOOLKIT_HOST
 *     else                                           -> UNKNOWN
 *
 * Both bounds are READ FROM THE REAL ARTIFACT at ARM, never hardcoded:
 *   * the recompiled bound is derived from the REAL recompiled function addresses the embedder
 *     resolves through the generated dispatch (`recomp_lookup`), reported by the game through
 *     xbox_A2hSlotWatchSetRecompBounds();
 *   * the image bound is read from this process's OWN PE headers (`__ImageBase` + the optional
 *     header's SizeOfImage), which is the module the RIP actually lives in.
 *
 * ⚠⚠ AND THE RECOMPILED TEST IS A SET OF FUNCTION STARTS, NOT ONE INTERVAL -- WITH A STATED LIMIT.
 *
 * The obvious implementation is `lo <= RIP < hi` over one contiguous span. Both it and the set test
 * were implemented, and the fixture's discriminating arm then showed they have EQUIVALENT COVERAGE:
 * a host run lying strictly between two published starts falls inside the interval attributed to the
 * RECOMPILED function below it under EITHER test. Distinguishing them would need function ENDS, and
 * the generated dispatch answers only function ENTRIES -- so this is a limit of what the embedder can
 * publish, not a defect in the test. MEASURED on the real image, that residual is one 0x1690-byte run
 * of the game's own probe objects (`harness_probes`/`video_probes`/`gpu_probes`) linked into the
 * middle of the recompiled extent; it contains `probe_worker_fault` and `jsrf_probe_gpu`, which run
 * during a probe run and touch memory. It is ASSERTED in the fixture and stated here rather than
 * claimed away.
 *
 * The set is published rather than a bare interval because it is the REAL data the dispatch can
 * answer -- so the archive carries the actual function starts and a reader can re-classify the
 * recorded RIPs by hand -- and because it makes the extent's definition explicit (first start to last
 * start) instead of a min/max over a strided probe that could silently narrow it. */
#define XBOX_A2H_SLOTW_RANGE_UNKNOWN      0u  /* outside every range this facility knows */
#define XBOX_A2H_SLOTW_RANGE_GAME_MODULE  1u  /* inside a RECOMPILED function's body */
#define XBOX_A2H_SLOTW_RANGE_TOOLKIT_HOST 2u  /* inside the loaded image, but NOT recompiled code */
#define XBOX_A2H_SLOTW_RANGE_COUNT        3u

/* HOW MANY RECOMPILED FUNCTION STARTS THE EMBEDDER MAY PUBLISH.
 *
 * The real title has 8 658 of them (measured from the dispatch against the map). The cap is set
 * above that with headroom, and it is a HARD FAIL-CLOSED limit rather than a truncation: a
 * publication that exceeds it is REFUSED WHOLE and `range_unavailable` moves, because a partially
 * recorded set would classify the functions it happened to keep and silently misplace the rest.
 * Overflowing this is an INFRA FAILURE, never a smaller-but-working classifier. */
#define XBOX_A2H_SLOTW_RECOMP_MAX  16384
/* THE LEDGER'S COPY OF THE SET IS SMALLER THAN THE PUBLICATION CAP, AND THE DIFFERENCE IS DELIBERATE.
 * The classifier needs the whole set; the ARCHIVE does not need 8 658 x 8 bytes of it. The ledger
 * carries the first N starts so a reader can check the classification of the RIPs the run actually
 * recorded, and `recomp_start_count` tells the reader how many there really were -- so a truncated
 * archive copy is visible as a number rather than read as the whole set. The recorded RIPs are the
 * only ones a reader has to re-classify, and they are few. */
#define XBOX_A2H_SLOTW_RECOMP_ARCHIVE 512

/* ── BOUNDED SAMPLING OF RIPs THAT CLASSIFIED `UNKNOWN` ─────────────────────────────────────────
 *
 * ⚠ WHY THIS EXISTS, AND WHY IT IS A COUNT PLUS A FEW SAMPLES RATHER THAN A LOG. `UNKNOWN` is INFRA
 * FAILURE by the packet's rule, so a run with `range_unknown > 0` cannot be promoted. That rule was
 * written on the assumption that every write to the watched page comes from the guest; MEASURED on a
 * live run, 264 of 10 592 relevant AVs classified UNKNOWN. Before that can be called an instrument
 * defect the RIPs must be IDENTIFIED, and a count alone cannot do it: "264 unknown" is equally
 * consistent with a classifier bug inside the recompiled extent and with writes from outside the
 * image entirely (a system DLL or the D3D driver), and those two demand OPPOSITE responses -- fix
 * the classifier, or accept that some writers are genuinely unplaceable.
 *
 * So the first N distinct unplaceable RIPs are recorded verbatim, with the range bounds in force when
 * they were seen, and `range_unknown` stays the uncapped count. `range_unknown_sampled` says how many
 * distinct RIPs the sample actually holds, so a reader never mistakes a bounded sample for the whole
 * population.
 *
 * ⚠ AND EACH SAMPLE CARRIES THE RIP'S OWN `VirtualQuery` ALLOCATION BASE, because the FIRST question
 * about an unplaceable RIP is which mapped region it lives in, and that question has to be answerable
 * FROM THE ARCHIVE. `unknown_same_image[]` says whether that base is THIS process's own image -- so
 * "the classifier failed inside our own code" (an instrument bug) and "the writer is in another
 * loaded module" (a genuinely unplaceable writer) are DISTINGUISHED by the record rather than by a
 * re-run. The base is a fact the OS reports; nothing here changes the classification. */
#define XBOX_A2H_SLOTW_UNKNOWN_SAMPLE_MAX 16

/* OPTIONAL NATIVE-DISASSEMBLY CORROBORATION, AND IT IS CORROBORATION ONLY.
 *
 * The range class is the classification; this is a SECOND, INDEPENDENT observation about the same
 * RIP that a reader may use to sanity-check it. It decodes the NATIVE bytes at the RIP by NATIVE
 * x86-64 semantics -- is this instruction a store to memory? -- which is a statement about the
 * host's own instruction set and carries no guest-byte expectation at all.
 *
 * ⚠ IT NEVER GATES ANYTHING. A RIP that decodes to a non-store, or does not decode, still has its
 * range class; the control and the records key on the RANGE CLASS alone. That separation is
 * deliberate: a hand-written decoder must never be able to fail a run that the range classifier
 * placed correctly. */
#define XBOX_A2H_SLOTW_FORM_UNKNOWN   0u  /* not attempted: no recompiled bound, or unreadable RIP */
#define XBOX_A2H_SLOTW_FORM_STORE     1u  /* decoded: a store to memory */
#define XBOX_A2H_SLOTW_FORM_NOT_STORE 2u  /* decoded: not a store to memory */
#define XBOX_A2H_SLOTW_FORM_UNDECODED 3u  /* a decode was attempted and did not complete */

/* WHAT A READER KEYS ON, IN THE NATIVE DOMAIN. `range_class == GAME_MODULE` with `slot_hit == 1`
 * IS the control: a store landed on the derived slot from inside the recompiled module. The VALUE
 * is a comparison made offline against this constant, and it is deliberately NOT part of the test:
 * requiring the pre-value to be zero was a defect, because it made the control depend on nothing
 * having touched the slot earlier, so a run in which the control DID fire could be reported as
 * INFRA FAILURE. */
#define XBOX_A2H_SLOTW_INSTALL_VALUE 0x0015F9D0u   /* what the installer writes (offline check) */

/* ── THE TERMINAL-VALUE COHERENCE GATE ──────────────────────────────────────────────────────────
 *
 * REQUIRED by the packet and by the Advisor's ruling, and it is what makes the accepted shape
 * sound: `VirtualProtect` is PAGE-GRANULAR, so opening the page for a NON-SLOT write also makes the
 * slot writable, and a racing write inside that window would be invisible. The gate does not close
 * that window -- nothing in scope can -- it makes the window FAIL CLOSED:
 *
 *     LAST-RECORDED slot write value  vs  TERMINAL slot read
 *     MISMATCH  =>  UNKNOWN.  Never a claim.
 *
 * "Windows threaten only unrecorded writes; recorded positives stand; coherence converts the
 * residual same-value race to fail-closed."
 *
 * ⚠ THE WORKED EXAMPLE IS ON-3'S OWN NUMBERS: last-recorded `0x0015F9D0` against terminal
 * `0x001D5078` MISMATCHES, so that run is `UNKNOWN`. The mismatch is the GATE WORKING, not a defect
 * in the instrument.
 *
 * ⚠ AND IT IS ONE-DIRECTIONAL. A MATCH cannot prove the absence of an unrecorded same-value racing
 * write, so a coherent pair never yields an absence or exclusivity row -- it only removes the
 * mismatch objection from a RECORDED positive. */
#define XBOX_A2H_SLOTW_COH_NOT_COMPARABLE 0u  /* no recorded slot write, or no terminal read */
#define XBOX_A2H_SLOTW_COH_COHERENT       1u  /* the two values MATCH (still not an absence proof) */
#define XBOX_A2H_SLOTW_COH_MISMATCH       2u  /* THEY DIFFER => UNKNOWN, fail closed */
#define XBOX_A2H_SLOTW_COH_NO_TERMINAL    3u  /* terminal never reached: nothing to compare */

/* One bounded, full record per SLOT-BYTE WRITE, plus the first-touch census below. Nothing here is
 * sampled and nothing is first-N: a full record is written for every fault that can change the slot
 * until the capacity is reached, and the overflow latch is set when it is exceeded. Overflow
 * invalidates absence/order rows; it does not invalidate a positive record, which stands on its own
 * evidence.
 *
 * ⚠ WHAT DOES **NOT** GET A RECORD, AND WHY THAT IS THE PACKET'S DESIGN RATHER THAN A SHORTCUT.
 * A write to the watched page whose effective address is NOT the slot cannot change the slot. The
 * packet calls those TRAFFIC and requires them COUNTED; retaining them as records is what exhausted
 * the buffer on ON trial 1 (129 writes x 2 records > 256) and made the fail-closed path fire on
 * ordinary volume. Traffic now moves `loss.nonslot_writes` (uncapped), `loss.nonslot_distinct`, and
 * at most ONE first-touch census record per distinct address -- so a page-write stream of any length
 * consumes no records at all after the page is first walked. */
typedef struct {
    uint32_t seq;          /* ordered event id, monotonic from ARM; THE ordering key */
    uint32_t kind;         /* 1 write fault, 2 step re-arm, 3 fourth-read sample, 4 installer control */
    uint32_t alias_index;  /* 0 = canonical view, m+1 = mirror view m */
    uint32_t slot_hit;     /* 1 = the effective address IS the re-derived slot VA for this alias */
    uint32_t fault_va;     /* low 32 bits of the host fault address */
    uint32_t pre_value;    /* the SLOT's value BEFORE the store executed */
    uint32_t post_value;   /* the SLOT's value AFTER the store executed (step events only) */
    uint32_t tid;          /* native thread id that took the fault */
    uint32_t range_class;  /* XBOX_A2H_SLOTW_RANGE_* classified from the RIP's RANGE, not its bytes */
    uint32_t form;         /* XBOX_A2H_SLOTW_FORM_* native-disasm corroboration (never gates) */
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
    uint64_t installer_control_hits; /* the required positive control fired (range-classed) */
    uint64_t nonslot_distinct;       /* distinct page offsets touched (the first-touch census) */
    uint64_t first_touch_overflow;   /* the first-touch census array is full (a LATCH) */
    uint64_t first_touch_dropped;    /* first touches beyond the census capacity */
    uint64_t cross_checks;           /* fault-record / step-read pairs compared */
    uint64_t cross_mismatch;         /* of those, how many DISAGREED (0 required) */
    uint64_t cross_skipped;          /* a cross-check was requested with nothing to compare */
    uint64_t overflow;               /* 1 = any bounded array overflowed (a LATCH, not a count) */
    uint64_t base_changed;           /* 1 = MEM32(0x19DCE0) moved between ARM and TERMINAL */
    /* ── THE RANGE CLASSIFIER'S OWN ACCOUNTING ──────────────────────────────────────────────────
     *
     * Every classified RIP lands in exactly one of these three buckets, and the three are printed
     * together so "0 unknown" is never confused with "the classifier never ran". `range_unknown`
     * being non-zero is INFRA FAILURE for the packet -- a RIP this facility could not place must
     * stop the run rather than be attributed. `range_host` is the expected class for a fault taken
     * inside this watch's own handler or anywhere else in the host image; it is NOT a failure. */
    uint64_t range_game;             /* classified GAME_MODULE (the recompiled bound) */
    uint64_t range_host;             /* classified TOOLKIT_HOST (in-image, not recompiled code) */
    uint64_t range_unknown;          /* classified UNKNOWN: outside every range -> INFRA FAILURE */
    uint64_t range_unavailable;      /* no recompiled bound was published: classification refused */
    uint64_t form_store;             /* native-disasm corroboration: decoded a store */
    uint64_t form_not_store;         /* native-disasm corroboration: decoded, not a store */
    uint64_t form_undecoded;         /* native-disasm corroboration: attempted, did not complete */
    /* ── THE TERMINAL-VALUE COHERENCE GATE (REQUIRED) ────────────────────────────────────────────
     *
     * `coherence_verdict` is XBOX_A2H_SLOTW_COH_*. It compares `coherence_last_write_value` --
     * the value of the LAST RECORDED slot write -- against `coherence_terminal_value` -- the
     * TERMINAL slot read. A MISMATCH is `UNKNOWN`: it is the gate working, never a claim. */
    uint64_t coherence_mismatch;     /* 1 = the gate found a MISMATCH (=> UNKNOWN, fail closed) */
} XboxA2hSlotwLoss;

/* ── THE FIRST-TOUCH CENSUS ────────────────────────────────────────────────────────────────────
 *
 * One bounded record per DISTINCT faulting address on the watched page, claimed once. This is what
 * replaces the per-write records for non-slot traffic: the packet asks for the page's touch SET and
 * the count of touches, not a transcript of every one of them. A repeat touch of an address already
 * in the census moves only the counter, so a linear fill of a million dwords costs a million
 * counter increments and ZERO new records.
 *
 * `pre_value` is the value at THAT address before the first touch, read through the faulting alias.
 * `slot_value_at_touch` is the SLOT's value at the same instant -- kept separate because the two are
 * different fields with different meanings, and conflating them is the naming ambiguity the Q3(a)
 * record had to resolve. */
typedef struct {
    uint32_t valid;          /* 1 = this census slot is claimed; written LAST */
    uint32_t offset;         /* page offset of the touched dword (fault_va & 0xFFF) */
    uint32_t alias_index;    /* which alias the first touch arrived through */
    uint32_t tid;            /* native thread id that made the first touch */
    uint32_t pre_value;      /* the value at THAT offset before the store */
    uint32_t slot_value_at_touch; /* the SLOT's value at the same instant */
    uint32_t range_class;    /* XBOX_A2H_SLOTW_RANGE_* classified from the RIP's RANGE */
    uint32_t form;           /* XBOX_A2H_SLOTW_FORM_* native-disasm corroboration (never gates) */
    uint64_t rip;            /* native instruction pointer of the first touch, verbatim */
    uint64_t ticks;
} XboxA2hSlotwFirstTouch;

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
    /* ── THE RECOMPILED-MODULE BOUND, AS PUBLISHED BY THE EMBEDDER AT ARM ────────────────────────
     *
     * ⚠ THESE ARE THE RANGE CLASSIFIER'S ENTIRE BASIS AND THEY ARE READ FROM THE REAL ARTIFACT.
     *
     * The toolkit cannot enumerate the recompiled module's own function symbols -- those are the
     * game's generated translation units, and a static library cannot see them. The EMBEDDER can:
     * `recomp_lookup` is the generated dispatch, and the game resolves the REAL addresses of the
     * REAL recompiled functions through it and hands back the observed SET of function starts here.
     *
     * ⚠⚠ THE SET IS THE TEST AND THE INTERVAL IS ONLY THE SUMMARY. An earlier version of this fix
     * tested a single `[recomp_lo, recomp_hi)` interval, which MEASUREMENT against the real linker
     * map showed to be wrong: the recompiled translation units are not contiguous, host and toolkit
     * objects are linked between them, and 11 505 of the 50 099 symbols inside the span are NOT
     * recompiled code. A single interval therefore classifies roughly a quarter of the host code in
     * the gaps as GAME_MODULE -- which would let a toolkit or runtime store satisfy the installer
     * control, the exact misattribution this fix exists to prevent. The classifier tests membership
     * of `recomp_starts`; `recomp_lo`/`recomp_hi` are reported for the reader and are not the test.
     *
     * `recomp_bound_valid == 1` means the set was derived from addresses `recomp_lookup` actually
     * returned, never from an arithmetic guess. When it is 0 the range classifier REFUSES to
     * classify (every RIP becomes UNKNOWN and `range_unavailable` moves), because an invented bound
     * is exactly the kind of arithmetic this packet exists to remove. */
    uint32_t recomp_bound_valid;   /* 1 = the set below came from recomp_lookup's real answers */
    uint32_t recomp_bound_probes;  /* how many guest VAs were probed to derive it */
    uint32_t recomp_start_count;   /* how many recompiled function STARTS were published */
    uint32_t recomp_start_overflow;/* 1 = the publication exceeded RECOMP_MAX and was REFUSED whole */
    uint64_t recomp_lo;            /* lowest recompiled function start (INCLUSIVE): EXTENT summary */
    uint64_t recomp_hi;            /* highest recompiled start + 1 (EXCLUSIVE): EXTENT summary */
    uint64_t image_lo;             /* this process's own module base, from its PE headers */
    uint64_t image_hi;             /* image_lo + SizeOfImage (EXCLUSIVE) */
    /* THE SORTED SET THE CLASSIFIER ACTUALLY TESTS. Ascending native addresses of every recompiled
     * function START the embedder could resolve. A RIP is GAME_MODULE when it lies at or after some
     * published start with NO OTHER PUBLISHED START between -- i.e. within one recompiled function's
     * body. That is what makes a host function inside the extent classify TOOLKIT_HOST while the
     * recompiled bodies around it classify GAME_MODULE. */
    uint64_t recomp_starts[XBOX_A2H_SLOTW_RECOMP_ARCHIVE];
    /* ── THE TERMINAL-VALUE COHERENCE GATE (REQUIRED) ────────────────────────────────────────────
     *
     * PUBLISHED AS THE TWO OPERANDS AND THE VERDICT, so a reader sees the comparison rather than
     * being told its result. `coherence_last_write_seq == 0` means NO slot write was ever recorded,
     * which is NOT a coherent pair: the gate then reports NOT_COMPARABLE and nothing may be
     * attributed from the terminal alone. */
    uint32_t coherence_verdict;       /* XBOX_A2H_SLOTW_COH_* */
    uint32_t coherence_last_write_value; /* the LAST RECORDED slot write's post value */
    uint32_t coherence_last_write_seq;   /* its event id; 0 = no recorded slot write at all */
    uint32_t coherence_terminal_value;   /* the TERMINAL slot read, through the guest translation */
    /* ⚠ THE SHAPE: RE-ARM AFTER EVERY WRITE, AND THE WINDOW IS A STATED LIMIT RATHER THAN HIDDEN.
     *
     * `VirtualProtect` is PAGE-GRANULAR and the slot sits at offset 0x62C of page 0x0019D000, so
     * opening the page for a NON-SLOT write ALSO makes the slot writable. The narrowing that would
     * "leave RW after a non-slot write" is therefore NOT implemented and MUST NOT BE: after the
     * first traffic write it would blind the instrument completely (ON-3 had 9077 of them).
     *
     * Every write -- slot or not -- is single-stepped and the page is re-armed RO immediately after,
     * so each open window is exactly one instruction wide. The windows are COUNTED
     * (`loss.unprotected_intervals`) and TICK-LOGGED here, so a reader can see how much unprotected
     * time the run contained instead of having to assume it was zero. */
    uint64_t window_open_ticks_last;  /* tick at which the page was last opened RW */
    uint64_t window_close_ticks_last; /* tick at which it was last re-armed RO */
    uint64_t window_open_count;       /* how many RW windows this arm has opened */
    uint32_t window_open;             /* 1 = a window is open RIGHT NOW (never silently assumed 0) */
    /* ── THE UNPLACEABLE-RIP SAMPLE ──────────────────────────────────────────────────────────────
     *
     * See XBOX_A2H_SLOTW_UNKNOWN_SAMPLE_MAX for why a count is not enough. `unknown_rips[]` holds the
     * first N DISTINCT unplaceable RIPs, and `unknown_rip_count` is how many the sample holds -- so a
     * reader sees the bounded sample AS a sample. Each entry is written once and never rewritten. */
    uint32_t unknown_rip_count;
    uint32_t reserved0;
    uint64_t unknown_rips[XBOX_A2H_SLOTW_UNKNOWN_SAMPLE_MAX];
    /* The allocation base of each sampled RIP, as `VirtualQuery` reports it, and whether that base is
     * THIS image. Together they answer "which module is this RIP in?" from the archive. */
    uint64_t unknown_rip_bases[XBOX_A2H_SLOTW_UNKNOWN_SAMPLE_MAX];
    uint32_t unknown_same_image[XBOX_A2H_SLOTW_UNKNOWN_SAMPLE_MAX];
    uint32_t unknown_reserved[XBOX_A2H_SLOTW_UNKNOWN_SAMPLE_MAX];
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
    uint32_t last_write_range;  /* its RANGE class (native domain); no encoding is ever recorded */
    uint32_t last_write_alias;  /* its alias index */
    uint32_t last_write_value;  /* its post-value */
    uint64_t last_write_rip;
    uint64_t last_write_ticks;
    /* THE STEP-READ / FAULT-RECORD CROSS-CHECK (Q3(c)). The step handler reads the slot back
     * through the same alias the store used and reports that value in the STEP record's post_value;
     * the fault handler read the slot's pre_value at the same instant through the same alias. For a
     * store that lands ON the slot those two readings cover the SAME ADDRESS, so they must AGREE --
     * this is the overlap the Advisor requires to be cross-validated, and it is the check that
     * caught the byte-order trap on the older line.
     *
     * `cross_mismatch != 0` means the instrument's own two reads of one address disagreed, which is
     * an INSTRUMENT BUG and fails the packet closed. `cross_checks` is the denominator, so "0
     * mismatches" is never confused with "the check never ran". */
    uint32_t cross_checks;      /* slot-hit fault/step pairs actually compared */
    uint32_t cross_mismatch;    /* of those, how many DISAGREED (must be 0) */
    uint32_t first_touch_count; /* first-touch census records written */
    uint32_t first_touch_overflow;
    /* THE LAST SLOT VALUE THIS INSTRUMENT READ, AND THE EVENT ID IT WAS READ AT. Published so an
     * INDEPENDENT reader -- the game-side hook, which reaches the same guest address through the
     * guest's own translation rather than through this facility's raw alias base -- can cross-check
     * it. `last_slot_read_seq` is what makes the comparison sound: a hook that reads the pair,
     * reads the slot, and finds the seq UNCHANGED knows no new slot read was published in its
     * window, so the two readers cover the SAME address at the SAME time and must agree. Without
     * the seq the comparison would race a concurrent write and report the race as a disagreement. */
    uint32_t last_slot_read_value;
    uint32_t last_slot_read_seq;
    /* THE GUARD THAT MAKES THE CROSS-VALIDATION SOUND. The slot-hit count at the moment
     * `last_slot_read_value` was published. A later independent reader that finds this count
     * UNCHANGED knows no slot write landed in between, so the two readings cover the same value and
     * must agree; if it moved, the pair is a race and is counted as SKIPPED rather than as
     * agreement. Without this, a concurrent write would be reported as a disagreement and the
     * check would fail closed on correct behaviour. */
    uint32_t last_slot_read_hits;
    /* ⚠ WHICH ALIAS THE INSTRUMENT READ THROUGH, AND WHY THE HOOK MUST KNOW. A slot write through a
     * MIRROR view is read back through that mirror by design, and this host's mirror views are NOT
     * coherent with the canonical view -- the fixture measured that a store through mirror view 1 did
     * not become visible canonically. The guest's own read is canonical, so comparing a MIRROR read
     * against a canonical one would report a host mapping property as an instrument disagreement.
     * The hook therefore cross-validates ONLY canonical-alias reads (alias 1) and counts every other
     * case as SKIPPED, which keeps "not comparable" distinct from "agreement". */
    uint32_t last_slot_read_alias;
    /* 1 once a slot store was observed to CHANGE the slot (post != pre). This is the positive half
     * of value fidelity: a facility that reported every store as a no-op would show 0 here, and a
     * reader would see that the value fields were never exercised rather than trusting them. */
    uint32_t last_slot_change_seen;
    /* ── THE TERMINAL CROSS-VALIDATION (Q3(c), HALF TWO) ─────────────────────────────────────────
     *
     * At the terminal point the slot is read TWICE, back to back, by two independent address
     * computations: through this facility's CACHED alias base (the one every fault record used) and
     * through the guest's OWN translation (`g_memory_base + (slot - XBOX_BASE_ADDRESS)`, which is
     * what `MEM32(slot_va)` computes in generated code). Those two values are published here
     * alongside the verdict, so a reader sees the comparison rather than being told its result.
     *
     * `terminal_cross_ok == 1` means the two AGREED on a quiet window. `== 0` means either they
     * DISAGREED (an instrument bug, also latched in `loss.cross_mismatch` and `loss.overflow`) or
     * the window was not quiet and no comparison was made -- and `loss.cross_skipped` separates the
     * second from the first, so "not compared" is never read as "agreed". */
    uint32_t terminal_alias_value;   /* read through g_a2h_slotw_pages[0] + (slot & 0xFFF) */
    uint32_t terminal_guest_value;   /* read through the guest's own translation of the slot */
    uint32_t terminal_cross_ok;      /* 1 = compared AND agreed; 0 = disagreed or not compared */
    XboxA2hSlotwLoss loss;
    XboxA2hSlotwEvent events[XBOX_A2H_SLOTW_EVENTS_MAX];
    XboxA2hSlotwFirstTouch first_touch[XBOX_A2H_SLOTW_FIRST_TOUCH_MAX];
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
/* ── THE RANGE CLASSIFIER'S INPUT, PUBLISHED BY THE EMBEDDER (NATIVE DOMAIN) ─────────────────────
 *
 * ⚠ CALL THIS BEFORE ARMING. The recompiled module's own code bounds cannot be derived inside the
 * toolkit -- the generated `sub_*` functions are the GAME's translation units and a static library
 * cannot enumerate them. The embedder CAN, through the generated dispatch (`recomp_lookup`), and it
 * must hand the resulting bound here so the classifier works from the REAL artifact.
 *
 * `probes` is how many guest VAs were probed, `lo`/`hi` are the observed native extent (hi
 * EXCLUSIVE), and `valid` must be 1 only when those came from addresses `recomp_lookup` actually
 * returned. A call with `valid == 0` clears the bound and makes the classifier refuse to classify,
 * which is the correct behaviour for a build whose dispatch the embedder could not read: an
 * invented bound is exactly the arithmetic this packet removes. */
void xbox_A2hSlotWatchSetRecompBounds(uint64_t lo, uint64_t hi, uint32_t probes, uint32_t valid);
/* The GAME sets this when its own probe loop hit JSRF_RECOMP_STARTS_MAX and had to refuse. It is a
 * SEPARATE report from the set itself because the refusal happens BEFORE the set reaches the toolkit,
 * so the toolkit would otherwise record a clean refusal with no reason attached -- and a reader must
 * be able to tell "the embedder could not enumerate the module" from "the toolkit rejected what it
 * was given". */
void xbox_A2hSlotWatchNoteRecompOverflow(void);
/* ⚠ THE PUBLICATION THE GAME ACTUALLY USES: EVERY RECOMPILED FUNCTION START, not one interval.
 *
 * `starts` are the native addresses the generated dispatch returned for real guest VAs. They are
 * copied and sorted internally, and the set becomes visible WHOLE (valid is set last), so a
 * classifier running concurrently never sees a partial set. An empty set, a NULL pointer, or a count
 * above XBOX_A2H_SLOTW_RECOMP_MAX is REFUSED WHOLE -- never truncated -- and clears the bound, so
 * the classifier reports UNKNOWN and `range_unavailable` moves. Returns 1 on acceptance. */
uint32_t xbox_A2hSlotWatchSetRecompStarts(const uint64_t *starts, uint32_t count, uint32_t probes);
/* How many recompiled function starts are currently published (0 when the bound is refused). */
uint32_t xbox_A2hSlotWatchRecompStartCount(void);
/* The range class of one native RIP, using the bound above and this process's own image headers.
 * Exposed for the offline proof: Exp0 must classify KNOWN RIPs against the REAL loaded modules, and
 * a proof that could only be run from inside a live fault would not be offline. */
uint32_t xbox_A2hSlotWatchClassifyRip(uint64_t rip);
/* The two ranges the classifier is actually using, so an offline reader can print them rather than
 * infer them. Either bound may be 0 when it is not available; `valid` says whether the recompiled
 * bound was published. */
void xbox_A2hSlotWatchRangeBounds(uint64_t *recomp_lo, uint64_t *recomp_hi, uint32_t *valid,
                                  uint64_t *image_lo, uint64_t *image_hi);
/* The coherence gate's verdict as (verdict, last_write_value, last_write_seq, terminal_value).
 * Returns 1 when a terminal read exists and the gate has run. A MISMATCH verdict is UNKNOWN and is
 * the gate working, not a defect. */
int xbox_A2hSlotWatchCoherence(uint32_t *verdict, uint32_t *last_write_value,
                               uint32_t *last_write_seq, uint32_t *terminal_value);
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
/* THE Q3(c) CROSS-VALIDATION, ASKED FROM OUTSIDE. The instrument's own two reads of the slot are
 * reconciled inside the step handler, but the game-side hook can read the same address at the same
 * moment through the guest's own translation. This entry point reports a fault-record read and an
 * independent hook read of the SAME guest address and asserts they agree; a disagreement is an
 * instrument bug and returns 0, which the caller must treat as fail-closed.
 *
 * Returns 1 when the pair agrees or when there is nothing to compare (the read is then counted as
 * `cross_skipped` so "no comparison" cannot be read as "agreement"). */
int xbox_A2hSlotWatchCrossCheck(uint32_t guest_va, uint32_t fault_record_value,
                                uint32_t hook_value);
/* The slot VA this arm derived, or 0 when unarmed. A cross-checking caller must read the SAME
 * address the instrument reads, so it asks rather than re-deriving it. */
uint32_t xbox_A2hSlotWatchSlotVa(void);
/* The instrument's last published slot read, as a (value, seq, hit_count) triple. A hook reads the
 * triple, performs its own read of the same address, then reads the hit count again via
 * xbox_A2hSlotWatchSlotHits(): an unchanged count proves no slot write landed across its window,
 * which is what makes the comparison a same-address same-time one rather than a race. Returns 1 when
 * a read has been published. */
int xbox_A2hSlotWatchLastSlotRead(uint32_t *out_value, uint32_t *out_seq, uint32_t *out_hits);
/* The alias the instrument last read the slot through: 1 = canonical, m+1 = mirror m, 0 = none yet.
 * The hook cross-validates only canonical reads, because the mirror views are NOT coherent with the
 * canonical view on this host and comparing across them would report a mapping property as an
 * instrument disagreement. See xbox_A2hSlotWatchCrossCheck(). */
uint32_t xbox_A2hSlotWatchLastSlotReadAlias(void);
/* The instrument's slot-hit count right now; the quiet-window test for xbox_A2hSlotWatchCrossCheck. */
uint32_t xbox_A2hSlotWatchSlotHits(void);

/**
 * Free a block from the Xbox heap. Currently a no-op (bump allocator).
 */
void xbox_HeapFree(uint32_t xbox_va);

/* RECOMP_KMEM_LEGACY=1 (read once): the allocator and kernel memory behaviour
 * that preceded the region registry -- whole-block heap reuse, a contiguous
 * arena that never frees, and the old NtAllocate/NtFreeVirtualMemory. */
int xbox_KmemLegacy(void);

/* Snapshot of the allocator counters kept beside the heap and arena. */
struct kmem_alloc_counters;
void xbox_KmemAllocCounters(struct kmem_alloc_counters *out);

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
