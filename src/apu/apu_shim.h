/*
 * QEMU/SDL/libsamplerate Shim Layer for xemu MCPX APU extraction
 *
 * Replaces QEMU types, SDL audio, and libsamplerate functions with
 * minimal Win32 equivalents so that xemu's APU code can compile standalone.
 *
 * Copyright (c) 2026 Burnout 3 Static Recompilation Project
 * Based on xemu code: Copyright (c) 2012 espes, 2018-2025 Matt Borgerson et al.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

/* ============================================================
 * Pull in base QEMU type shims from NV2A extraction
 * ============================================================ */
#include "../nv2a/qemu_shim.h"

#include <math.h>

/* ============================================================
 * The pinned xemu DSP56300 sources (A4b1 step 2)
 *
 * src/apu/dsp/ is vendored byte-for-byte from xemu at
 * 67cc79e663038d1f55448c0f566b37dde016adf6 and needs a handful of symbols the
 * NV2A shim does not carry. They are added here, in the toolkit's own shim
 * header, rather than by editing a vendored file.
 *
 * The surface was inventoried, not guessed: see
 * src/apu/dsp/PROVENANCE.md, "QEMU shim surface". Everything
 * below is a NO-OP or a PASS-THROUGH. Nothing here invents device behaviour.
 * ============================================================ */

/* ssize_t is not part of the MSVC CRT vocabulary. dsp_dma.c declares
 * `static ssize_t scratch_buf_size` for its intermediate DMA buffer, so the
 * name has to exist. intptr_t is the same width the type has on the POSIX
 * hosts this toolkit also builds for. */
#if defined(_MSC_VER) && !defined(_SSIZE_T_DEFINED)
#define _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif

/* QEMU's dirty-tracking entry point, called by the pinned gp_ep.c after it
 * writes a scatter-gather page (gp_ep.c:77).
 *
 * A NO-OP, and honestly so: this toolkit has no migration and no dirty bitmap
 * -- guest RAM is a plain host mapping that the guest and the model share, so
 * there is nothing to mark. Adding a bitmap nothing reads would be the
 * dishonest version of this shim. */
#ifndef memory_region_set_dirty
#define memory_region_set_dirty(mr, addr, size) ((void)0)
#endif

/* xemu's audio settings, which the pinned dsp.c and gp_ep.c read.
 *
 * The two fields they use are set to the values Device semantics 2 requires,
 * with NO environment variable:
 *
 *   use_dsp     = true   the GP is enabled unconditionally
 *                        (gp_ep.c:31-41 sets d->gp.realtime/d->ep.realtime)
 *   use_dsp_jit = false  the C interpreter is chosen, never the JIT. The JIT is
 *                        a packet non-goal, and dsp_jit.* is deliberately
 *                        absent from the vendored set.
 *
 * So `if (g_config.audio.use_dsp_jit) dsp_jit_init(dsp); else dsp_c_init(dsp);`
 * at dsp.c:122-126 takes the dsp_c_init arm unconditionally, which is what
 * Device semantics 2 says, and dsp_set_engine() at gp_ep.c:44-48 is a no-op
 * because its preference never changes. */
static struct {
    struct {
        float volume_limit;
        bool hrtf;
        struct {
            int num_workers;
        } vp;
        bool use_dsp;
        bool use_dsp_jit;
    } audio;
} g_config = {
    .audio = {
        .volume_limit = 1.0f,
        .hrtf = false,
        .vp = { .num_workers = 1 },
        .use_dsp = true,
        .use_dsp_jit = false,
    },
};

/* ============================================================
 * Additional atomic ops needed by APU code
 * ============================================================ */

#ifndef qatomic_or
#ifdef _MSC_VER
#define qatomic_or(ptr, val)  _InterlockedOr((long volatile*)(ptr), (long)(val))
#define qatomic_and(ptr, val) _InterlockedAnd((long volatile*)(ptr), (long)(val))
#else
#define qatomic_or(ptr, val)  __sync_fetch_and_or((ptr), (val))
#define qatomic_and(ptr, val) __sync_fetch_and_and((ptr), (val))
#endif
#endif

/* ============================================================
 * QEMU clock helpers (beyond what NV2A shim provides)
 * ============================================================ */

#define QEMU_CLOCK_REALTIME 1

#ifndef qemu_clock_get_us
static inline int64_t qemu_clock_get_us(int type) {
    (void)type;
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (int64_t)(count.QuadPart * 1000000LL / freq.QuadPart);
}
#endif

#ifndef qemu_clock_get_ms
static inline int64_t qemu_clock_get_ms(int type) {
    return qemu_clock_get_us(type) / 1000;
}
#endif

/* ============================================================
 * Threading extensions (timedwait, thread create/join)
 * ============================================================ */

#ifndef qemu_cond_timedwait
static inline void qemu_cond_timedwait(QemuCond *c, QemuMutex *m, int ms) {
    SleepConditionVariableCS(&c->cv, &m->cs, (DWORD)ms);
}
#endif

#define QEMU_THREAD_JOINABLE 0

typedef void *(*QemuThreadFunc)(void *arg);

#ifndef qemu_thread_create
static inline void qemu_thread_create(QemuThread *t, const char *name,
                                       void *(*func)(void *), void *arg,
                                       int mode) {
    (void)name; (void)mode;
    t->thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)func, arg, 0, NULL);
}
#endif

#ifndef qemu_thread_join
static inline void qemu_thread_join(QemuThread *t) {
    WaitForSingleObject(t->thread, INFINITE);
    CloseHandle(t->thread);
}
#endif

/* ============================================================
 * Physical memory access (direct RAM pointer)
 * We access Xbox RAM via a global pointer, same as NV2A.
 * ============================================================ */

extern uint8_t *g_apu_ram_ptr; /* Set at init to point at Xbox 64MB RAM */

/* Little-endian physical memory reads */
static inline uint32_t ldl_le_phys(void *as, hwaddr addr) {
    (void)as;
    return *(uint32_t *)(g_apu_ram_ptr + (addr & 0x03FFFFFF));
}
static inline uint16_t lduw_le_phys(void *as, hwaddr addr) {
    (void)as;
    return *(uint16_t *)(g_apu_ram_ptr + (addr & 0x03FFFFFF));
}
static inline uint8_t ldub_phys(void *as, hwaddr addr) {
    (void)as;
    return *(uint8_t *)(g_apu_ram_ptr + (addr & 0x03FFFFFF));
}

/* Little-endian physical memory writes */
static inline void stl_le_phys(void *as, hwaddr addr, uint32_t val) {
    (void)as;
    *(uint32_t *)(g_apu_ram_ptr + (addr & 0x03FFFFFF)) = val;
}
static inline void stw_le_phys(void *as, hwaddr addr, uint16_t val) {
    (void)as;
    *(uint16_t *)(g_apu_ram_ptr + (addr & 0x03FFFFFF)) = val;
}
static inline void stb_phys(void *as, hwaddr addr, uint8_t val) {
    (void)as;
    *(uint8_t *)(g_apu_ram_ptr + (addr & 0x03FFFFFF)) = val;
}

/* Stub address space - just passed to ldl_le_phys etc. (ignored) */
static int address_space_memory_dummy;
#define address_space_memory (&address_space_memory_dummy)

/* Xbox page size */
#ifndef TARGET_PAGE_SIZE
#define TARGET_PAGE_SIZE 4096
#endif

/* ============================================================
 * PCI / IRQ stubs (APU uses these minimally)
 * ============================================================ */

/* These are already in qemu_shim.h, but ensure they exist */
#ifndef pci_register_bar
#define pci_register_bar(dev, bar, flags, mr) ((void)0)
#endif

#define PCI_INTERRUPT_PIN       0x3D
#define PCI_BASE_ADDRESS_SPACE_MEMORY 0
#define PCI_VENDOR_ID_NVIDIA    0x10DE
#define PCI_DEVICE_ID_NVIDIA_MCPX_APU 0x01B0
#define PCI_CLASS_MULTIMEDIA_AUDIO 0x0401

/* ============================================================
 * Memory region stubs (beyond NV2A shim)
 * ============================================================ */

static inline void memory_region_init_io(MemoryRegion *mr, void *owner,
                                          const MemoryRegionOps *ops,
                                          void *opaque, const char *name,
                                          uint64_t size) {
    (void)owner; (void)ops; (void)name;
    mr->size = size;
    mr->opaque = opaque;
}

static inline void memory_region_add_subregion(MemoryRegion *parent,
                                                hwaddr offset,
                                                MemoryRegion *child) {
    (void)parent; (void)offset; (void)child;
}

static inline uint8_t *memory_region_get_ram_ptr(MemoryRegion *mr) {
    return (uint8_t *)mr->opaque;
}

/* ============================================================
 * libsamplerate stubs (SRC)
 *
 * The VP uses libsamplerate for pitch-shifted voice resampling.
 * We stub it initially; voices will play at native rate.
 * ============================================================ */

typedef void SRC_STATE;

#define SRC_SINC_FASTEST 2

typedef long (*src_callback_t)(void *cb_data, float **data);

static inline SRC_STATE *src_callback_new(src_callback_t func, int type,
                                           int channels, int *error,
                                           void *cb_data) {
    (void)func; (void)type; (void)channels; (void)cb_data;
    if (error) *error = 0;
    /* Return non-NULL so callers think init succeeded */
    static int dummy_src;
    return (SRC_STATE *)&dummy_src;
}

static inline int src_callback_read(SRC_STATE *state, double ratio,
                                     long frames, float *data) {
    (void)state; (void)ratio; (void)frames; (void)data;
    /* Return 0 frames - caller will handle silence */
    return 0;
}

static inline void src_reset(SRC_STATE *state) { (void)state; }
static inline void src_delete(SRC_STATE *state) { (void)state; }
static inline const char *src_strerror(int error) {
    (void)error;
    return "libsamplerate stubbed";
}

/* Float-to-short conversion (from libsamplerate) */
static inline void src_float_to_short_array(const float *in, int16_t *out,
                                             int len) {
    for (int i = 0; i < len; i++) {
        float s = in[i] * 32767.0f;
        if (s > 32767.0f) s = 32767.0f;
        if (s < -32768.0f) s = -32768.0f;
        out[i] = (int16_t)s;
    }
}

/* ============================================================
 * SDL Audio stubs
 *
 * The monitor output uses SDL. We stub it; actual audio output
 * will be connected via WASAPI separately.
 * ============================================================ */

typedef void SDL_AudioStream;
typedef uint32_t SDL_AudioDeviceID;

#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK 0
#define SDL_AUDIO_S16LE 0x8010
#define SDL_AUDIO_BYTESIZE(fmt) (((fmt) & 0xFF) / 8)
#define SDL_INIT_AUDIO 0x00000010

typedef struct SDL_AudioSpec {
    int freq;
    int format;
    int channels;
} SDL_AudioSpec;

static inline int SDL_Init(uint32_t flags) { (void)flags; return 0; }
static inline const char *SDL_GetError(void) { return "SDL stubbed"; }
static inline int SDL_GetAudioStreamQueued(SDL_AudioStream *stream) {
    (void)stream; return -1;
}
static inline int SDL_PutAudioStreamData(SDL_AudioStream *stream,
                                          const void *buf, int len) {
    (void)stream; (void)buf; (void)len; return 0;
}
static inline SDL_AudioStream *SDL_OpenAudioDeviceStream(
    SDL_AudioDeviceID dev, const SDL_AudioSpec *spec, void *cb, void *ud) {
    (void)dev; (void)spec; (void)cb; (void)ud; return NULL;
}
static inline SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *s) {
    (void)s; return 0;
}
static inline int SDL_GetAudioDeviceFormat(SDL_AudioDeviceID dev,
                                            SDL_AudioSpec *spec, int *frames) {
    (void)dev; (void)spec; (void)frames; return 0;
}
static inline void SDL_ResumeAudioDevice(SDL_AudioDeviceID dev) { (void)dev; }
static inline void SDL_DestroyAudioStream(SDL_AudioStream *s) { (void)s; }
static inline void SDL_SetAudioStreamGain(SDL_AudioStream *s, float g) {
    (void)s; (void)g;
}
static inline int SDL_GetNumLogicalCPUCores(void) { return 1; }

/* ============================================================
 * VM state / migration stubs
 * ============================================================ */

typedef int VMStateField;
#define VMSTATE_UINT8(f, s)           0
#define VMSTATE_UINT16(f, s)          0
#define VMSTATE_UINT32(f, s)          0
#define VMSTATE_UINT64(f, s)          0
#define VMSTATE_INT16(f, s)           0
#define VMSTATE_INT32(f, s)           0
#define VMSTATE_BOOL(f, s)            0
#define VMSTATE_UINT8_ARRAY(f, s, n)  0
#define VMSTATE_UINT16_ARRAY(f, s, n) 0
#define VMSTATE_INT16_ARRAY(f, s, n)  0
#define VMSTATE_UINT32_ARRAY(f, s, n) 0
#define VMSTATE_UINT64_ARRAY(f, s, n) 0
#define VMSTATE_UINT32_2DARRAY(f, s, m, n) 0
#define VMSTATE_STRUCT(f, s, v, vmsd, type) 0
#define VMSTATE_STRUCT_POINTER(f, s, vmsd, type) 0
#define VMSTATE_STRUCT_ARRAY(f, s, n, v, vmsd, type) 0
#define VMSTATE_PCI_DEVICE(f, s)      0
#define VMSTATE_UNUSED(n)             0
#define VMSTATE_END_OF_LIST()         0

/* ============================================================
 * QEMU Object model stubs
 * ============================================================ */

typedef void Object;
typedef int ResetType;
typedef void ObjectClass;
typedef void InterfaceInfo;
typedef void DeviceClass;
typedef void ResettableClass;
typedef void PCIDeviceClass;
typedef void TypeInfo;

#define DEVICE_CLASS(k) (k)
#define RESETTABLE_CLASS(k) (k)
#define PCI_DEVICE_CLASS(k) (k)
#define INTERFACE_CONVENTIONAL_PCI_DEVICE NULL
#define TYPE_PCI_DEVICE "pci-device"

static inline void type_register_static(const void *info) { (void)info; }
#define type_init(fn) /* no-op */

/* ============================================================
 * Run state / VM change handler stubs
 * ============================================================ */

typedef int RunState;
typedef void (*VMChangeStateHandler)(void *opaque, bool running, RunState state);

static inline void *qemu_add_vm_change_state_handler(VMChangeStateHandler fn,
                                                       void *opaque) {
    (void)fn; (void)opaque;
    return NULL;
}

/* ============================================================
 * Tracing stubs (APU-specific)
 * ============================================================ */

#define trace_mcpx_apu_reg_read(addr, size, val)    ((void)0)
#define trace_mcpx_apu_reg_write(addr, size, val)   ((void)0)
#define trace_mcpx_apu_method(method, arg)          ((void)0)

/* ============================================================
 * Warn/error reporting
 * ============================================================ */

#ifndef warn_reportf_err
static inline void warn_reportf_err(Error *err, const char *fmt, ...) {
    if (err) {
        fprintf(stderr, "[APU-WARN] %s%s\n", fmt ? fmt : "", err->msg);
        error_free(err);
    }
}
#endif

/* ============================================================
 * g_config stub (xemu settings used by APU)
 *
 * Defined above, together with the two fields the pinned DSP56300 sources read.
 * ============================================================ */

/* ============================================================
 * RCU stubs (used by worker threads)
 * ============================================================ */

static inline void rcu_register_thread(void) {}
static inline void rcu_unregister_thread(void) {}

/* ============================================================
 * g_malloc0_n (QEMU/GLib allocation)
 * ============================================================ */

#ifndef g_malloc0_n
#define g_malloc0_n(n, size) calloc((n), (size))
#endif

/* HWADDR_PRIx format specifier.
 *
 * The pinned gp_ep.c uses `"0x%" HWADDR_PRIx` with a hwaddr (uint64_t) argument
 * in its DPRINTF calls, so this has to expand to a 64-bit length modifier. The
 * NV2A shim does not define PRIx64, so it is defined here rather than assumed. */
#ifndef PRIx64
#define PRIx64 "llx"
#endif
#ifndef HWADDR_PRIx
#define HWADDR_PRIx PRIx64
#endif

/* ============================================================
 * ctz64 (count trailing zeros 64-bit)
 * ============================================================ */

#ifdef _MSC_VER
static inline int ctz64(uint64_t val) {
    unsigned long index;
#ifdef _WIN64
    _BitScanForward64(&index, val);
#else
    if ((uint32_t)val) {
        _BitScanForward(&index, (uint32_t)val);
    } else {
        _BitScanForward(&index, (uint32_t)(val >> 32));
        index += 32;
    }
#endif
    return (int)index;
}
#else
static inline int ctz64(uint64_t val) {
    return __builtin_ctzll(val);
}
#endif

/* ============================================================
 * MCPX APU device macro
 * ============================================================ */

#define MCPX_APU_DEVICE(obj) ((MCPXAPUState *)(obj))

/* ============================================================
 * PCI helper (create simple - stub)
 * ============================================================ */

static inline void *pci_create_simple(void *bus, int devfn, const char *name) {
    (void)bus; (void)devfn; (void)name;
    return NULL;
}
