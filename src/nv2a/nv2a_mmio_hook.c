/*
 * NV2A MMIO Hook - VEH instruction decoder for GPU register access
 *
 * Decodes x86-64 MOV instructions that access NV2A MMIO registers,
 * routes them through the NV2A register handlers, and advances RIP.
 */

#include "nv2a_mmio_hook.h"
#include "nv2a_state.h"
#include <stdio.h>
#include <stdlib.h>   /* getenv: without this the declaration is implicit on x64 */

/* NV2A MMIO base in Xbox VA space */
#define NV2A_MMIO_BASE  0xFD000000u
#define NV2A_MMIO_SIZE  0x01000000u  /* 16MB */
#define NV2A_VRAM_BASE  0xF0000000u
#define NV2A_VRAM_SIZE  (64 * 1024 * 1024)  /* 64MB Xbox VRAM */
#define NV2A_RAMIN_SIZE (1 * 1024 * 1024)   /* 1MB RAMIN */

static ptrdiff_t g_mem_offset = 0;
static uint8_t *g_nv2a_vram = NULL;

/* Statistics */
static int g_mmio_read_count = 0;
static int g_mmio_write_count = 0;
static int g_mmio_decode_fail = 0;
static uint8_t *g_mmio_aperture = NULL;
static SRWLOCK g_mmio_owner_lock = SRWLOCK_INIT;
static volatile LONG g_mmio_owner_active = 0;
static HANDLE g_ptimer_wake_event = NULL;
static HANDLE g_ptimer_thread = NULL;
static volatile LONG g_ptimer_stopping = 0;
/* The external collector reads only this published model snapshot. An even
 * generation is stable; odd means the process was frozen during publication. */
__declspec(dllexport) uint8_t g_nv2a_mmio_snapshot[NV2A_MMIO_SIZE];
__declspec(dllexport) volatile LONG g_nv2a_mmio_snapshot_generation = 0;
__declspec(dllexport) volatile LONG g_nv2a_mmio_snapshot_available = 0;
static uint8_t g_nv2a_owner_storage[NV2A_MMIO_SIZE - 0x00800000u];
extern volatile LONG g_nv2a_ack_enabled;
extern void xbox_Nv2aClaimRegisterOwner(void);

/* Global APU state pointer is declared in apu.h and referenced from main.c
 * (regardless of whether the MMIO hook is active). Keep its definition
 * outside the Win32 guard. */

#if defined(_WIN32)

/* ============================================================
 * x86-64 register access helpers
 * ============================================================ */

/* Map ModRM reg field (+ REX.R) to CONTEXT register pointer */
static uint64_t *ctx_reg64(PCONTEXT ctx, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t*)&ctx->Rax;
    case 1:  return (uint64_t*)&ctx->Rcx;
    case 2:  return (uint64_t*)&ctx->Rdx;
    case 3:  return (uint64_t*)&ctx->Rbx;
    case 4:  return (uint64_t*)&ctx->Rsp;
    case 5:  return (uint64_t*)&ctx->Rbp;
    case 6:  return (uint64_t*)&ctx->Rsi;
    case 7:  return (uint64_t*)&ctx->Rdi;
    case 8:  return (uint64_t*)&ctx->R8;
    case 9:  return (uint64_t*)&ctx->R9;
    case 10: return (uint64_t*)&ctx->R10;
    case 11: return (uint64_t*)&ctx->R11;
    case 12: return (uint64_t*)&ctx->R12;
    case 13: return (uint64_t*)&ctx->R13;
    case 14: return (uint64_t*)&ctx->R14;
    case 15: return (uint64_t*)&ctx->R15;
    default: return NULL;
    }
}

static uint64_t ctx_reg_read(PCONTEXT ctx, int reg, unsigned size, int has_rex)
{
    if (size == 1 && !has_rex && reg >= 4 && reg <= 7)
        return (*ctx_reg64(ctx, reg - 4) >> 8) & 0xFF;
    return *ctx_reg64(ctx, reg);
}

static void ctx_reg_write(PCONTEXT ctx, int reg, unsigned size,
                          int has_rex, uint64_t value)
{
    if (size == 1 && !has_rex && reg >= 4 && reg <= 7) {
        uint64_t *dest = ctx_reg64(ctx, reg - 4);
        *dest = (*dest & ~0xFF00ULL) | ((value & 0xFF) << 8);
        return;
    }
    uint64_t *dest = ctx_reg64(ctx, reg);
    if (size == 1) *dest = (*dest & ~0xFFULL) | (value & 0xFF);
    else if (size == 2) *dest = (*dest & ~0xFFFFULL) | (value & 0xFFFF);
    else *dest = value & 0xFFFFFFFFULL;
}

static bool access_valid(uint32_t offset, unsigned size);
static void set_logic_flags(PCONTEXT ctx, uint64_t result, unsigned size);
static void set_sub_flags(PCONTEXT ctx, uint64_t lhs, uint64_t rhs,
                          uint64_t result, unsigned size);
static uint64_t owner_read(NV2AState *nv2a, uint32_t offset, unsigned size);
static void owner_write(NV2AState *nv2a, uint32_t offset,
                        uint64_t value, unsigned size);
static void publish_diagnostic_state(NV2AState *nv2a, bool extra_valid,
                                     uint32_t extra_offset, uint32_t extra_value);
static DWORD WINAPI ptimer_service_thread(void *opaque);

/* ============================================================
 * x86-64 instruction decoder (focused on MOV patterns)
 *
 * We only need to handle the patterns MSVC generates for
 * volatile memory access (MEM8/MEM16/MEM32 macros):
 *
 * Writes:
 *   89 /r      MOV r/m32, r32       (32-bit reg → memory)
 *   88 /r      MOV r/m8, r8         (8-bit reg → memory)
 *   66 89 /r   MOV r/m16, r16       (16-bit reg → memory)
 *   C7 /0 id   MOV r/m32, imm32     (32-bit immediate → memory)
 *   C6 /0 ib   MOV r/m8, imm8       (8-bit immediate → memory)
 *
 * Reads:
 *   8B /r      MOV r32, r/m32       (memory → 32-bit reg)
 *   8A /r      MOV r8, r/m8         (memory → 8-bit reg)
 *   66 8B /r   MOV r16, r/m16       (memory → 16-bit reg)
 *   0F B6 /r   MOVZX r32, r/m8      (zero-extend byte → 32-bit)
 *   0F B7 /r   MOVZX r32, r/m16     (zero-extend word → 32-bit)
 *
 * With REX prefixes for 64-bit register extension.
 * ============================================================ */

/* Decode ModRM + optional SIB + displacement, return instruction length */
static int decode_modrm_len(const uint8_t *ip, int has_rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm_low = modrm & 7;
    int len = 1; /* modrm byte */

    (void)has_rex_b;

    if (mod == 3) {
        return -1;
    }

    /* Check for SIB byte */
    if (rm_low == 4) {
        uint8_t sib = ip[len];
        len++; /* SIB byte */
        if (mod == 0 && (sib & 7) == 5)
            len += 4;
    }

    /* Displacement */
    if (mod == 0) {
        if (rm_low == 5) len += 4; /* disp32 (RIP-relative or [disp32]) */
    } else if (mod == 1) {
        len += 1; /* disp8 */
    } else if (mod == 2) {
        len += 4; /* disp32 */
    }

    return len;
}

/*
 * Try to decode and handle the faulting instruction.
 * Returns true if successfully handled, false if unrecognized.
 */
static bool decode_and_handle(PCONTEXT ctx, uint32_t mmio_offset, int is_write)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    NV2AState *nv2a = nv2a_get_state();
    if (!nv2a) return false;

    int prefix_len = 0;
    int has_66 = 0;     /* operand size override */
    int rex = 0;        /* REX prefix byte */
    int has_rex = 0;
    int has_lock = 0;

    /* Parse prefixes */
    while (1) {
        uint8_t b = ip[prefix_len];
        if (b == 0x66) {
            has_66 = 1;
            prefix_len++;
        } else if (b == 0xF0) {
            /* LOCK is accepted only for the supported read-modify-write forms. */
            has_lock = 1;
            prefix_len++;
        } else if (b >= 0x40 && b <= 0x4F) {
            /* REX prefix */
            rex = b;
            has_rex = 1;
            prefix_len++;
        } else {
            break;
        }
    }

    int rex_w = has_rex && (rex & 0x08); /* 64-bit operand */
    int rex_r = has_rex && (rex & 0x04); /* extends ModRM reg */
    int rex_b = has_rex && (rex & 0x01); /* extends ModRM r/m */

    const uint8_t *opcode = ip + prefix_len;
    int access_size = 4; /* default 32-bit */
    if (has_66) access_size = 2;
    if (rex_w) access_size = 8;
    if (has_lock && opcode[0] != 0x08 && opcode[0] != 0x09 &&
        opcode[0] != 0x20 && opcode[0] != 0x21) {
        fprintf(stderr, "[NV2A] unsupported LOCK MMIO opcode %02X\n", opcode[0]);
        return false;
    }

    /* ── MOV r/m, r (write: 88/89) ── */
    if (opcode[0] == 0x89 || opcode[0] == 0x88) {
        if (opcode[0] == 0x88) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        if (modrm_len < 0 || !access_valid(mmio_offset, access_size)) return false;
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = ctx_reg_read(ctx, reg, access_size, has_rex);

        /* Mask to access size */
        if (access_size == 1) val &= 0xFF;
        else if (access_size == 2) val &= 0xFFFF;
        else if (access_size == 4) val &= 0xFFFFFFFF;

        owner_write(nv2a, mmio_offset, val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_write_count++;
        return true;
    }

    /* ── MOV r, r/m (read: 8A/8B) ── */
    if (opcode[0] == 0x8B || opcode[0] == 0x8A) {
        if (opcode[0] == 0x8A) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        if (modrm_len < 0 || !access_valid(mmio_offset, access_size)) return false;
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = owner_read(nv2a, mmio_offset, access_size);

        ctx_reg_write(ctx, reg, access_size, has_rex, val);

        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── MOV r/m32, imm32 (C7 /0) ── */
    if (opcode[0] == 0xC7) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        if (modrm_len < 0 || ((opcode[1] >> 3) & 7) != 0 ||
            !access_valid(mmio_offset, access_size)) return false;
        /* immediate follows modrm+sib+disp */
        const uint8_t *imm_ptr = opcode + 1 + modrm_len;
        uint32_t imm = access_size == 2 ? *(const uint16_t *)imm_ptr
                                        : *(const uint32_t *)imm_ptr;
        int imm_len = access_size == 2 ? 2 : 4;

        owner_write(nv2a, mmio_offset, imm, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len + imm_len;
        g_mmio_write_count++;
        return true;
    }

    /* ── MOV r/m8, imm8 (C6 /0) ── */
    if (opcode[0] == 0xC6) {
        access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        if (modrm_len < 0 || ((opcode[1] >> 3) & 7) != 0 ||
            !access_valid(mmio_offset, 1)) return false;
        uint8_t imm = *(opcode + 1 + modrm_len);

        owner_write(nv2a, mmio_offset, imm, 1);
        ctx->Rip += prefix_len + 1 + modrm_len + 1;
        g_mmio_write_count++;
        return true;
    }

    /* ── MOVZX r32, r/m8 (0F B6) ── */
    if (opcode[0] == 0x0F && opcode[1] == 0xB6) {
        if (has_66) return false; /* 16-bit destination is outside the contract. */
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        if (modrm_len < 0 || !access_valid(mmio_offset, 1)) return false;
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = owner_read(nv2a, mmio_offset, 1) & 0xFF;

        uint64_t *dest = ctx_reg64(ctx, reg);
        *dest = val; /* zero-extend to 64-bit */

        ctx->Rip += prefix_len + 2 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── MOVZX r32, r/m16 (0F B7) ── */
    if (opcode[0] == 0x0F && opcode[1] == 0xB7) {
        if (has_66) return false; /* 16-bit destination is outside the contract. */
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        if (modrm_len < 0 || !access_valid(mmio_offset, 2)) return false;
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = owner_read(nv2a, mmio_offset, 2) & 0xFFFF;

        uint64_t *dest = ctx_reg64(ctx, reg);
        *dest = val;

        ctx->Rip += prefix_len + 2 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── TEST r/m, r (84/85) - reads memory for flag comparison ── */
    if (opcode[0] == 0x85 || opcode[0] == 0x84) {
        if (opcode[0] == 0x84) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        if (modrm_len < 0 || !access_valid(mmio_offset, access_size)) return false;
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = owner_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = ctx_reg_read(ctx, reg, access_size, has_rex);

        if (access_size == 1) { mem_val &= 0xFF; reg_val &= 0xFF; }
        else if (access_size == 2) { mem_val &= 0xFFFF; reg_val &= 0xFFFF; }
        else if (access_size == 4) { mem_val &= 0xFFFFFFFF; reg_val &= 0xFFFFFFFF; }

        uint64_t result = mem_val & reg_val;

        set_logic_flags(ctx, result, access_size);

        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── CMP r/m, r (38/39) or CMP r, r/m (3A/3B) ── */
    if (opcode[0] == 0x39 || opcode[0] == 0x38) {
        if (opcode[0] == 0x38) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        if (modrm_len < 0 || !access_valid(mmio_offset, access_size)) return false;
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = owner_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = ctx_reg_read(ctx, reg, access_size, has_rex);

        if (access_size <= 4) {
            mem_val &= (1ULL << (access_size * 8)) - 1;
            reg_val &= (1ULL << (access_size * 8)) - 1;
        }

        /* CMP r/m, r: compute r/m - r */
        uint64_t result = mem_val - reg_val;
        set_sub_flags(ctx, mem_val, reg_val, result, access_size);

        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    if (opcode[0] == 0x3B || opcode[0] == 0x3A) {
        if (opcode[0] == 0x3A) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        if (modrm_len < 0 || !access_valid(mmio_offset, access_size)) return false;
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = owner_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = ctx_reg_read(ctx, reg, access_size, has_rex);

        if (access_size <= 4) {
            mem_val &= (1ULL << (access_size * 8)) - 1;
            reg_val &= (1ULL << (access_size * 8)) - 1;
        }

        /* CMP r, r/m: compute r - r/m */
        uint64_t result = reg_val - mem_val;
        set_sub_flags(ctx, reg_val, mem_val, result, access_size);

        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── OR r/m, r (08/09) - read-modify-write ── */
    if (opcode[0] == 0x09 || opcode[0] == 0x08) {
        if (opcode[0] == 0x08) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        if (modrm_len < 0 || !access_valid(mmio_offset, access_size)) return false;
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = owner_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = ctx_reg_read(ctx, reg, access_size, has_rex);
        uint64_t result = mem_val | reg_val;

        owner_write(nv2a, mmio_offset, result, access_size);
        set_logic_flags(ctx, result, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_write_count++;
        return true;
    }

    /* ── AND r/m, r (20/21) ── */
    if (opcode[0] == 0x21 || opcode[0] == 0x20) {
        if (opcode[0] == 0x20) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        if (modrm_len < 0 || !access_valid(mmio_offset, access_size)) return false;
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = owner_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = ctx_reg_read(ctx, reg, access_size, has_rex);
        uint64_t result = mem_val & reg_val;

        owner_write(nv2a, mmio_offset, result, access_size);
        set_logic_flags(ctx, result, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_write_count++;
        return true;
    }

    /* Unrecognized instruction */
    g_mmio_decode_fail++;
    if (g_mmio_decode_fail <= 20) {
        fprintf(stderr, "[NV2A] MMIO decode fail at RIP=%p: %02X %02X %02X %02X %02X %02X\n",
                (void*)ctx->Rip, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return false;
}

/* ============================================================
 * Public API
 * ============================================================ */

void nv2a_hook_init(ptrdiff_t xbox_mem_offset)
{
    g_mem_offset = xbox_mem_offset;

    /* Allocate NV2A VRAM (64MB) */
    g_nv2a_vram = (uint8_t *)VirtualAlloc(NULL, NV2A_VRAM_SIZE + NV2A_RAMIN_SIZE,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_nv2a_vram) {
        fprintf(stderr, "[NV2A] Failed to allocate %u MB for VRAM!\n",
                (NV2A_VRAM_SIZE + NV2A_RAMIN_SIZE) / (1024*1024));
        return;
    }

    uint8_t *ramin_ptr = g_nv2a_vram + NV2A_VRAM_SIZE;

    /* State publication and a concurrent instance claim are one owner-locked
     * transaction.  The initializer itself does not acquire this lock. */
    AcquireSRWLockExclusive(&g_mmio_owner_lock);
    NV2AState *nv2a = nv2a_init_standalone(g_nv2a_vram, NV2A_VRAM_SIZE,
                                            ramin_ptr, NV2A_RAMIN_SIZE);
    ReleaseSRWLockExclusive(&g_mmio_owner_lock);
    if (!nv2a) return;

    /* USER DMA pointers are physical addresses in the separately mapped
     * contiguous window. Keep the exact mapping; never alias them into RAM. */
    if (!nv2a_set_pushbuffer_window(nv2a,
            (uint8_t *)((uintptr_t)0x80000000ULL + (intptr_t)xbox_mem_offset),
            0x00000000u, 0x04000000u)) {
        fprintf(stderr, "[NV2A] contiguous pushbuffer mapping unavailable\n");
    }

    g_ptimer_wake_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (g_ptimer_wake_event) {
        InterlockedExchange(&g_ptimer_stopping, 0);
        g_ptimer_thread = CreateThread(NULL, 0, ptimer_service_thread,
                                       nv2a, 0, NULL);
        if (!g_ptimer_thread) {
            CloseHandle(g_ptimer_wake_event);
            g_ptimer_wake_event = NULL;
        }
    }

    fprintf(stderr, "[NV2A] MMIO hook initialized: VRAM=%p RAMIN=%p\n",
            (void*)g_nv2a_vram, (void*)ramin_ptr);
}

static uint64_t owner_read(NV2AState *nv2a, uint32_t offset, unsigned size)
{
    uint64_t value = 0;
    if (offset == 0x00800040u || offset == 0x00800044u)
        return nv2a_mmio_read(nv2a, offset, size);
    if (offset >= 0x00800000u) {
        memcpy(&value, g_nv2a_owner_storage + offset - 0x00800000u, size);
        return value;
    }
    return nv2a_mmio_read(nv2a, offset, size);
}

static void owner_write(NV2AState *nv2a, uint32_t offset,
                        uint64_t value, unsigned size)
{
    if (offset == 0x00800040u || offset == 0x00800044u) {
        nv2a_mmio_write(nv2a, offset, value, size);
        return;
    }
    if (offset >= 0x00800000u) {
        memcpy(g_nv2a_owner_storage + offset - 0x00800000u, &value, size);
        return;
    }
    nv2a_mmio_write(nv2a, offset, value, size);
    if (g_ptimer_wake_event &&
        ((offset >= 0x009000u && offset < 0x00a000u) ||
         offset == 0x680000u + NV_PRAMDAC_NVPLL_COEFF))
        SetEvent(g_ptimer_wake_event);
}

bool nv2a_claim_instance_memory_threadsafe(uint32_t guest_base,
                                           uint8_t *host_ptr, uint32_t size)
{
    bool accepted;
    AcquireSRWLockExclusive(&g_mmio_owner_lock);
    accepted = nv2a_bind_instance_memory(guest_base, host_ptr, size);
    ReleaseSRWLockExclusive(&g_mmio_owner_lock);
    return accepted;
}

/* Publish every register consumed by the frozen GPU report from one locked
 * owner-state point. This includes coupled state that a write may change. */
static void publish_diagnostic_state(NV2AState *nv2a, bool extra_valid,
                                     uint32_t extra_offset, uint32_t extra_value)
{
    static const uint32_t offsets[] = {
        0x000000, 0x000100, 0x000140,
        0x001800, 0x001804, 0x001808,
        0x002100, 0x003240, 0x003244,
        0x009100, 0x009140, 0x009200, 0x009210, 0x009400, 0x009410,
        0x10020C, 0x100410, 0x400100, 0x600100, 0x600140, 0x600800,
        0x800040, 0x800044
    };
    enum { DIAG_COUNT = sizeof(offsets) / sizeof(offsets[0]) };
    uint32_t values[DIAG_COUNT];
    for (size_t i = 0; i < DIAG_COUNT; ++i)
        values[i] = (uint32_t)owner_read(nv2a, offsets[i], 4);

    /* All state is staged before the generation becomes odd. The collector's
     * unavailable window is therefore only this bounded commit. */
    InterlockedIncrement(&g_nv2a_mmio_snapshot_generation);
    if (extra_valid)
        memcpy(g_nv2a_mmio_snapshot + extra_offset,
               &extra_value, sizeof(extra_value));
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        memcpy(g_nv2a_mmio_snapshot + offsets[i], &values[i], sizeof(values[i]));
    }
    MemoryBarrier();
    InterlockedIncrement(&g_nv2a_mmio_snapshot_generation);
    /* USER_DMA_PUT/GET are storage-only; the handled access supplies the
     * aligned owner-storage value as the extra published dword. */
}

static DWORD ptimer_wait_ms(uint64_t delay_ns)
{
    if (delay_ns == UINT64_MAX) return INFINITE;
    if (delay_ns == 0) return 0;
    uint64_t delay_ms = delay_ns / 1000000 + (delay_ns % 1000000 != 0);
    return delay_ms >= INFINITE ? INFINITE - 1 : (DWORD)delay_ms;
}

static DWORD WINAPI ptimer_service_thread(void *opaque)
{
    NV2AState *nv2a = (NV2AState *)opaque;
    HANDLE wake = g_ptimer_wake_event;
    uint64_t next_vblank_ns = 0;
    uint64_t frame_ns = 0;
    ULONGLONG last_retry_ms = 0;
    for (;;) {
        uint64_t delay_ns, now_ns;
        uint32_t pending_before, pmc_before;
        bool retried = false, stalled;
        AcquireSRWLockExclusive(&g_mmio_owner_lock);
        if (InterlockedCompareExchange(&g_ptimer_stopping, 0, 0)) {
            ReleaseSRWLockExclusive(&g_mmio_owner_lock);
            break;
        }
        pending_before = nv2a->ptimer.pending_interrupts;
        pmc_before = nv2a->pmc.pending_interrupts;

        /* The display clock. Every other interrupt source in this model is
         * driven by a guest register write; a vertical blank is the one that
         * has to come from the card, so it lives on this service loop. */
        now_ns = nv2a->ptimer.clock_ns ?
            nv2a->ptimer.clock_ns(nv2a->ptimer.clock_opaque) :
            (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        frame_ns = nv2a_display_frame_ns(nv2a);
        /* The scheduling decision is a pure function so it can be tested
         * without a live service thread -- see nv2a_vblank_advance. */
        {
            int pulse = 0;
            next_vblank_ns = nv2a_vblank_advance(next_vblank_ns, now_ns, frame_ns,
                                                 &pulse);
            if (pulse)
                nv2a_vblank_pulse(nv2a);
        }

        nv2a_ptimer_service(nv2a);
        delay_ns = nv2a_ptimer_next_alarm_ns(nv2a);
        {
            uint64_t until_vblank = next_vblank_ns > now_ns
                                  ? next_vblank_ns - now_ns : 0;
            if (until_vblank < delay_ns) delay_ns = until_vblank;
        }
        /* Never sleep for more than a few frames on the vblank account: a
         * deadline computed from a clock that is about to step is not a reason
         * to stop servicing the display. Without this a bad `until_vblank`
         * turns into a multi-day sleep, which is how the clock wrap presented
         * (the guest simply never received another vertical blank). */
        if (delay_ns > frame_ns * 4) delay_ns = frame_ns * 4;

        /* A rejected walk is sticky: GET stays put and D3D's ring-space wait
         * only polls, it never kicks again. Re-walk at most every 100 ms so a
         * rejection that has since cleared is consumed. The owner lock held
         * here serializes this walk with the guest's PUT writes. */
        stalled = InterlockedCompareExchange(&g_mmio_owner_active, 0, 0) &&
                  g_nv2a_submit_state.consecutive_rejections > 0;
        if (stalled) {
            ULONGLONG now_ms = GetTickCount64();
            if (now_ms - last_retry_ms >= 100) {
                last_retry_ms = now_ms;
                retried = nv2a_retry_stalled_walk(nv2a);
            }
            if (delay_ns > 100000000ull) delay_ns = 100000000ull;
        }

        /* Never a zero-length wait: `ptimer_wait_ms(0)` returns immediately and
         * this loop would spin instead of idling between frames. */
        if (delay_ns < 1000000ull) delay_ns = 1000000ull;
        if (InterlockedCompareExchange(&g_mmio_owner_active, 0, 0) &&
            (retried ||
             pending_before != nv2a->ptimer.pending_interrupts ||
             pmc_before != nv2a->pmc.pending_interrupts)) {
            publish_diagnostic_state(nv2a, false, 0, 0);
        }
        ReleaseSRWLockExclusive(&g_mmio_owner_lock);
        WaitForSingleObject(wake, ptimer_wait_ms(delay_ns));
    }
    return 0;
}

static bool access_valid(uint32_t offset, unsigned size)
{
    if ((size != 1 && size != 2 && size != 4) ||
        offset > NV2A_MMIO_SIZE - size ||
        (offset & 0xFFFu) > 0x1000u - size) {
        fprintf(stderr, "[NV2A] unsupported MMIO width/page edge offset=%08X size=%u\n",
                offset, size);
        return false;
    }
    if (offset < 0x00800048u && offset + size > 0x00800040u &&
        !((offset == 0x00800040u || offset == 0x00800044u) && size == 4)) {
        fprintf(stderr, "[NV2A] USER DMA pointers require aligned dword access offset=%08X size=%u\n",
                offset, size);
        return false;
    }
    return true;
}

#define X86_CF 0x0001u
#define X86_PF 0x0004u
#define X86_AF 0x0010u
#define X86_ZF 0x0040u
#define X86_SF 0x0080u
#define X86_OF 0x0800u

static unsigned parity_even8(uint8_t value)
{
    value ^= value >> 4;
    value &= 0xFu;
    return (0x9669u >> value) & 1u;
}

static void set_logic_flags(PCONTEXT ctx, uint64_t result, unsigned size)
{
    uint64_t sign = 1ULL << (size * 8 - 1);
    uint64_t mask = size == 4 ? 0xFFFFFFFFULL : (1ULL << (size * 8)) - 1;
    result &= mask;
    ctx->EFlags &= ~(X86_CF | X86_PF | X86_ZF | X86_SF | X86_OF);
    if (parity_even8((uint8_t)result)) ctx->EFlags |= X86_PF;
    if (!result) ctx->EFlags |= X86_ZF;
    if (result & sign) ctx->EFlags |= X86_SF;
}

static void set_sub_flags(PCONTEXT ctx, uint64_t lhs, uint64_t rhs,
                          uint64_t result, unsigned size)
{
    uint64_t sign = 1ULL << (size * 8 - 1);
    uint64_t mask = size == 4 ? 0xFFFFFFFFULL : (1ULL << (size * 8)) - 1;
    lhs &= mask; rhs &= mask; result &= mask;
    ctx->EFlags &= ~(X86_CF | X86_PF | X86_AF | X86_ZF | X86_SF | X86_OF);
    if (lhs < rhs) ctx->EFlags |= X86_CF;
    if (parity_even8((uint8_t)result)) ctx->EFlags |= X86_PF;
    if ((lhs ^ rhs ^ result) & 0x10) ctx->EFlags |= X86_AF;
    if (!result) ctx->EFlags |= X86_ZF;
    if (result & sign) ctx->EFlags |= X86_SF;
    if (((lhs ^ rhs) & (lhs ^ result) & sign) != 0) ctx->EFlags |= X86_OF;
}

bool nv2a_hook_install_aperture(void *aperture, size_t size)
{
    if (!aperture || size < NV2A_MMIO_SIZE || !g_ptimer_thread ||
        ((uintptr_t)aperture & 0xFFFu) != 0)
        return false;

    g_mmio_aperture = (uint8_t *)aperture;
    memset(g_nv2a_mmio_snapshot, 0, sizeof(g_nv2a_mmio_snapshot));
    memset(g_nv2a_owner_storage, 0, sizeof(g_nv2a_owner_storage));
    InterlockedExchange(&g_nv2a_mmio_snapshot_generation, 0);
    InterlockedExchange(&g_nv2a_mmio_snapshot_available, 0);
    /* Atomically retire the legacy register mutator before interception can
     * become active. Kernel/APU clock work remains in the worker. */
    xbox_Nv2aClaimRegisterOwner();
    DWORD old_protect = 0;
    if (!VirtualProtect(g_mmio_aperture, NV2A_MMIO_SIZE,
                        PAGE_NOACCESS, &old_protect)) {
        g_mmio_aperture = NULL;
        InterlockedExchange(&g_nv2a_mmio_snapshot_available, 0);
        return false;
    }
    publish_diagnostic_state(nv2a_get_state(), false, 0, 0);
    InterlockedExchange(&g_nv2a_mmio_snapshot_available, 1);
    InterlockedExchange(&g_mmio_owner_active, 1);
    fprintf(stderr, "[NV2A] MMIO state owner installed at %p (%u MB, serialized PAGE_NOACCESS)\n",
            aperture, NV2A_MMIO_SIZE / (1024 * 1024));
    return true;
}

/* Read-only, lock-free owner-state query: reads the same interlocked flag the
 * install/disable paths write, so it never takes the owner lock and cannot
 * deadlock a caller that already holds one. */
bool nv2a_hook_owner_active(void)
{
    return InterlockedCompareExchange(&g_mmio_owner_active, 0, 0) != 0;
}

void nv2a_hook_disable_aperture(void)
{
    AcquireSRWLockExclusive(&g_mmio_owner_lock);
    InterlockedExchange(&g_mmio_owner_active, 0);
    InterlockedExchange(&g_nv2a_mmio_snapshot_available, 0);
    ReleaseSRWLockExclusive(&g_mmio_owner_lock);
}

void nv2a_hook_shutdown(void)
{
    uint8_t *aperture;
    HANDLE thread;
    HANDLE wake;
    AcquireSRWLockExclusive(&g_mmio_owner_lock);
    thread = g_ptimer_thread;
    wake = g_ptimer_wake_event;
    g_ptimer_thread = NULL;
    g_ptimer_wake_event = NULL;
    if (thread) {
        InterlockedExchange(&g_ptimer_stopping, 1);
        SetEvent(wake);
    }
    ReleaseSRWLockExclusive(&g_mmio_owner_lock);
    if (thread) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
        CloseHandle(wake);
        fprintf(stderr, "[NV2A] PTIMER service stopped\n");
    }
    AcquireSRWLockExclusive(&g_mmio_owner_lock);
    InterlockedExchange(&g_mmio_owner_active, 0);
    InterlockedExchange(&g_nv2a_mmio_snapshot_available, 0);
    aperture = g_mmio_aperture;
    g_mmio_aperture = NULL;
    if (aperture) {
        MEMORY_BASIC_INFORMATION mbi;
        DWORD old_protect;
        if (VirtualQuery(aperture, &mbi, sizeof(mbi)) == sizeof(mbi) &&
            mbi.State == MEM_COMMIT)
            VirtualProtect(aperture, NV2A_MMIO_SIZE, PAGE_READWRITE, &old_protect);
    }
    ReleaseSRWLockExclusive(&g_mmio_owner_lock);
}

bool nv2a_hook_set_ptimer_clock(uint64_t (*clock_ns)(void *), void *opaque)
{
    NV2AState *nv2a = nv2a_get_state();
    bool ok = false;
    if (!nv2a) return false;
    AcquireSRWLockExclusive(&g_mmio_owner_lock);
    if (g_ptimer_thread && g_ptimer_wake_event &&
        !InterlockedCompareExchange(&g_ptimer_stopping, 0, 0)) {
        nv2a_ptimer_set_clock(nv2a, clock_ns, opaque);
        SetEvent(g_ptimer_wake_event);
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_mmio_owner_lock);
    return ok;
}

void nv2a_hook_notify_ptimer_clock_changed(void)
{
    AcquireSRWLockExclusive(&g_mmio_owner_lock);
    if (g_ptimer_wake_event &&
        !InterlockedCompareExchange(&g_ptimer_stopping, 0, 0))
        SetEvent(g_ptimer_wake_event);
    ReleaseSRWLockExclusive(&g_mmio_owner_lock);
}

bool nv2a_hook_run_decoder_tests(void)
{
    static const uint8_t put8[]  = {0xC6,0x05,0,0,0,0,0x80};
    static const uint8_t put16[] = {0x66,0xC7,0x05,0,0,0,0,0x34,0x12};
    static const uint8_t put32[] = {0xC7,0x05,0,0,0,0,0x78,0x56,0x34,0x12};
    static const uint8_t get8[]  = {0x8A,0x05,0,0,0,0};
    static const uint8_t get16[] = {0x66,0x8B,0x05,0,0,0,0};
    static const uint8_t get32[] = {0x8B,0x05,0,0,0,0};
    static const uint8_t test8[] = {0x84,0x05,0,0,0,0};
    static const uint8_t cmp8[]  = {0x38,0x05,0,0,0,0};
    static const uint8_t cmp16[] = {0x66,0x39,0x05,0,0,0,0};
    static const uint8_t cmp32[] = {0x39,0x05,0,0,0,0};
    static const uint8_t or8[]   = {0x08,0x05,0,0,0,0};
    static const uint8_t and32[] = {0x21,0x05,0,0,0,0};
    static const uint8_t edge16[] = {0x66,0xC7,0x05,0,0,0,0,1,0};
    static const uint8_t unsupported[] = {0x90};
    const uint32_t base = 0x00800100u;
    CONTEXT c;
    bool ok = true;

#define RUN(code, off) do { memset(&c, 0, sizeof(c)); c.Rip=(DWORD64)(uintptr_t)(code); \
    if (!decode_and_handle(&c, (off), 0)) ok=false; } while (0)
    AcquireSRWLockExclusive(&g_mmio_owner_lock);
    RUN(put8, base);
    RUN(get8, base);
    if ((uint8_t)c.Rax != 0x80) ok = false;
    RUN(put16, base + 4);
    RUN(get16, base + 4);
    if ((uint16_t)c.Rax != 0x1234) ok = false;
    RUN(put32, base + 8);
    RUN(get32, base + 8);
    if ((uint32_t)c.Rax != 0x12345678u) ok = false;

    memset(&c, 0, sizeof(c)); c.Rip=(DWORD64)(uintptr_t)test8;
    c.Rax=0xFF; c.EFlags=X86_CF|X86_AF|X86_OF;
    if (!decode_and_handle(&c, base, 0) ||
        (c.EFlags & (X86_CF|X86_PF|X86_ZF|X86_SF|X86_OF)) != X86_SF)
        ok=false;
    memset(&c, 0, sizeof(c)); c.Rip=(DWORD64)(uintptr_t)cmp8;
    c.Rax=1;
    if (!decode_and_handle(&c, base, 0) ||
        (c.EFlags & (X86_CF|X86_PF|X86_AF|X86_ZF|X86_SF|X86_OF)) !=
            (X86_AF|X86_OF))
        ok=false;
    memset(&c, 0, sizeof(c)); c.Rip=(DWORD64)(uintptr_t)cmp16;
    c.Rax=0x1234;
    if (!decode_and_handle(&c, base + 4, 0) ||
        (c.EFlags & (X86_CF|X86_PF|X86_AF|X86_ZF|X86_SF|X86_OF)) !=
            (X86_PF|X86_ZF))
        ok=false;
    memset(&c, 0, sizeof(c)); c.Rip=(DWORD64)(uintptr_t)cmp32;
    c.Rax=0x12345679;
    if (!decode_and_handle(&c, base + 8, 0) ||
        (c.EFlags & (X86_CF|X86_PF|X86_AF|X86_ZF|X86_SF|X86_OF)) !=
            (X86_CF|X86_PF|X86_AF|X86_SF))
        ok=false;
    memset(&c, 0, sizeof(c)); c.Rip=(DWORD64)(uintptr_t)or8;
    c.Rax=1; c.EFlags=X86_CF|X86_OF;
    if (!decode_and_handle(&c, base, 1) ||
        (c.EFlags & (X86_CF|X86_PF|X86_ZF|X86_SF|X86_OF)) !=
            (X86_PF|X86_SF))
        ok=false;
    memset(&c, 0, sizeof(c)); c.Rip=(DWORD64)(uintptr_t)and32;
    c.Rax=0; c.EFlags=X86_CF|X86_OF;
    if (!decode_and_handle(&c, base + 8, 1) ||
        (c.EFlags & (X86_CF|X86_PF|X86_ZF|X86_SF|X86_OF)) !=
            (X86_PF|X86_ZF))
        ok=false;

    memset(&c, 0, sizeof(c)); c.Rip=(DWORD64)(uintptr_t)edge16;
    if (decode_and_handle(&c, 0x00800FFFu, 1)) ok=false;
    memset(&c, 0, sizeof(c)); c.Rip=(DWORD64)(uintptr_t)unsupported;
    if (decode_and_handle(&c, base, 0)) ok=false;
    ReleaseSRWLockExclusive(&g_mmio_owner_lock);
#undef RUN
    fprintf(stderr, "[NV2A] decoder regression: %s (8/16/32, flags, edge, unsupported)\n",
            ok ? "PASS" : "FAIL");
    return ok;
}

/* ── MMIO access trace (RECOMP_MMIO_TRACE=1) ───────────────────────────────
 * A handled MMIO fault is invisible otherwise: the hook returns
 * CONTINUE_EXECUTION, so the game log never sees the address, the collector's
 * exception lines carry no address, and a deadline run writes no
 * ExceptionStream to the minidump. That makes "which register is this guest
 * loop waiting on, and how far does it sweep" unanswerable from any artifact a
 * run produces.
 *
 * Hashed by faulting RIP, not linear-scanned and not keyed by offset. Both
 * alternatives were tried and both mislead: an offset-keyed table cannot tell a
 * poll from a sweep (it fills with the sweep's own addresses and reports a
 * meaningless "hottest offset count=8"), and a 256-entry linear table fills
 * immediately on this title -- 3.19M of 3.2M accesses landed in "untracked" and
 * the hot site was invisible. Per RIP the trace keeps the count and the
 * minimum/maximum offset seen, which is what separates a poll (min == max) from
 * a bounded sweep from a runaway one. Off unless RECOMP_MMIO_TRACE is set. */
#define MMIO_TRACE_SLOTS 4096            /* power of two */
static struct {
    uint64_t rip;
    unsigned long count;
    uint32_t min_off, max_off;
    unsigned long writes;
} g_mmio_sites[MMIO_TRACE_SLOTS];
static unsigned long g_mmio_site_used = 0;
static int g_mmio_trace_enabled = -1;
static unsigned long g_mmio_trace_total = 0;
static unsigned long g_mmio_trace_untracked = 0;

static void mmio_trace_dump(void)
{
    int picked[12];
    int npicked = 0, pass, i, j, skip;
    fprintf(stderr, "  [NV2A-TRACE] total=%lu sites=%lu untracked=%lu top:\n",
            g_mmio_trace_total, g_mmio_site_used, g_mmio_trace_untracked);
    for (pass = 0; pass < 12; pass++) {
        unsigned long best = 0;
        int bi = -1;
        for (i = 0; i < MMIO_TRACE_SLOTS; i++) {
            if (!g_mmio_sites[i].count) continue;
            skip = 0;
            for (j = 0; j < npicked; j++) if (picked[j] == i) { skip = 1; break; }
            if (skip) continue;
            if (g_mmio_sites[i].count > best) { best = g_mmio_sites[i].count; bi = i; }
        }
        if (bi < 0) break;
        picked[npicked++] = bi;
        fprintf(stderr, "    rip=0x%llX count=%lu off=0x%06X..0x%06X span=0x%X writes=%lu\n",
                (unsigned long long)g_mmio_sites[bi].rip, g_mmio_sites[bi].count,
                g_mmio_sites[bi].min_off, g_mmio_sites[bi].max_off,
                g_mmio_sites[bi].max_off - g_mmio_sites[bi].min_off,
                g_mmio_sites[bi].writes);
    }
    fflush(stderr);
}

static void mmio_trace_record(uint32_t offset, uint64_t rip, int is_write)
{
    uint32_t h, s, probe;
    if (g_mmio_trace_enabled < 0)
        g_mmio_trace_enabled = getenv("RECOMP_MMIO_TRACE") ? 1 : 0;
    if (!g_mmio_trace_enabled) return;
    g_mmio_trace_total++;
    h = (uint32_t)((rip * 2654435761ull) >> 19) & (MMIO_TRACE_SLOTS - 1);
    for (probe = 0; probe < 64; probe++) {
        s = (h + probe) & (MMIO_TRACE_SLOTS - 1);
        if (g_mmio_sites[s].count == 0) {
            g_mmio_sites[s].rip = rip;
            g_mmio_sites[s].min_off = offset;
            g_mmio_sites[s].max_off = offset;
            g_mmio_sites[s].writes = 0;
            g_mmio_site_used++;
            break;
        }
        if (g_mmio_sites[s].rip == rip) break;
    }
    if (probe == 64) {
        /* Keep counting and keep dumping. Returning here is how the previous
         * revision went silent once its table filled. */
        g_mmio_trace_untracked++;
    } else {
        g_mmio_sites[s].count++;
        if (offset < g_mmio_sites[s].min_off) g_mmio_sites[s].min_off = offset;
        if (offset > g_mmio_sites[s].max_off) g_mmio_sites[s].max_off = offset;
        if (is_write) g_mmio_sites[s].writes++;
    }
    if (g_mmio_trace_total % 200000u == 0u) mmio_trace_dump();
}

bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write)
{
    /* Compute MMIO offset within NV2A register space */
    uint32_t mmio_offset = fault_xbox_va - NV2A_MMIO_BASE;
    NV2AState *nv2a = nv2a_get_state();
    if (!nv2a || !InterlockedCompareExchange(&g_mmio_owner_active, 0, 0) ||
        !g_mmio_aperture || fault_addr < (uintptr_t)g_mmio_aperture ||
        fault_addr >= (uintptr_t)g_mmio_aperture + NV2A_MMIO_SIZE)
        return false;

    mmio_trace_record(mmio_offset, ctx->Rip, is_write);

    AcquireSRWLockExclusive(&g_mmio_owner_lock);
    if (!InterlockedCompareExchange(&g_mmio_owner_active, 0, 0)) {
        ReleaseSRWLockExclusive(&g_mmio_owner_lock);
        return false;
    }
    uint32_t ptimer_pending_before = nv2a->ptimer.pending_interrupts;
    uint32_t pmc_pending_before = nv2a->pmc.pending_interrupts;
    bool handled = decode_and_handle(ctx, mmio_offset, is_write);
    if (handled && (is_write ||
        ptimer_pending_before != nv2a->ptimer.pending_interrupts ||
        pmc_pending_before != nv2a->pmc.pending_interrupts)) {
        uint32_t aligned = mmio_offset & ~3u;
        uint32_t value = (uint32_t)owner_read(nv2a, aligned, 4);
        publish_diagnostic_state(nv2a, true, aligned, value);
    }
    ReleaseSRWLockExclusive(&g_mmio_owner_lock);
    return handled;
}

bool nv2a_hook_set_irq_sink(void (*sink)(void *opaque, int asserted),
                            void *opaque)
{
    NV2AState *nv2a = nv2a_get_state();
    if (!nv2a) return false;
    AcquireSRWLockExclusive(&g_mmio_owner_lock);
    nv2a_set_irq_sink(nv2a, sink, opaque);
    ReleaseSRWLockExclusive(&g_mmio_owner_lock);
    return true;
}

bool nv2a_hook_pci_config_read(uint32_t offset, void *buffer, uint32_t length)
{
    NV2AState *nv2a = nv2a_get_state();
    if (!nv2a || !buffer) return false;
    AcquireSRWLockExclusive(&g_mmio_owner_lock);
    bool ok = nv2a_pci_config_read(nv2a, offset, buffer, length);
    ReleaseSRWLockExclusive(&g_mmio_owner_lock);
    return ok;
}

bool nv2a_hook_get_submission_snapshot(NV2AHookSubmissionSnapshot *snapshot)
{
    NV2AState *nv2a = nv2a_get_state();
    if (!nv2a || !snapshot) return false;
    AcquireSRWLockExclusive(&g_mmio_owner_lock);
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->get = nv2a->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
    snapshot->put = nv2a->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];
    snapshot->diagnostic_get = nv2a->pfifo.submit_diag_get;
    snapshot->diagnostic_subchannel = nv2a->pfifo.submit_diag_subchannel;
    snapshot->diagnostic_method = nv2a->pfifo.submit_diag_method;
    snapshot->diagnostic_param = nv2a->pfifo.submit_diag_param;
    snapshot->sink_count = nv2a->pfifo.sink_count;
    snapshot->successes = nv2a->pfifo.submit_successes;
    snapshot->last_method = nv2a->pfifo.submit_last_method;
    snapshot->last_param = nv2a->pfifo.submit_last_param;
    strncpy_s(snapshot->diagnostic, sizeof(snapshot->diagnostic),
              nv2a_submit_diagnostic(nv2a->pfifo.submit_diag), _TRUNCATE);
    ReleaseSRWLockExclusive(&g_mmio_owner_lock);
    return true;
}

bool nv2a_hook_pci_config_write(uint32_t offset, const void *buffer,
                                uint32_t length)
{
    NV2AState *nv2a = nv2a_get_state();
    if (!nv2a || !buffer) return false;
    AcquireSRWLockExclusive(&g_mmio_owner_lock);
    bool ok = nv2a_pci_config_write(nv2a, offset, buffer, length);
    if (ok && InterlockedCompareExchange(&g_mmio_owner_active, 0, 0)) {
        publish_diagnostic_state(nv2a, false, 0, 0);
    }
    ReleaseSRWLockExclusive(&g_mmio_owner_lock);
    return ok;
}

bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va)
{
    /* For VRAM range (0xF0000000-0xF3FFFFFF), allocate pages as before.
     * In the future, we can map these to NV2A VRAM for push buffer DMA.
     * For now, just allocate writable pages. */
    uintptr_t alloc_base = fault_addr & ~(uintptr_t)0xFFFF;
    LPVOID result = VirtualAlloc((LPVOID)alloc_base, 0x10000,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!result) {
        result = VirtualAlloc((LPVOID)alloc_base, 0x10000,
                              MEM_COMMIT, PAGE_READWRITE);
    }
    if (result) {
        memset(result, 0, 0x10000);
        return true;
    }
    return false;
}

#else /* !_WIN32 -- SIGSEGV-based MMIO trapping deferred to main.c port */

bool nv2a_claim_instance_memory_threadsafe(uint32_t guest_base,
                                           uint8_t *host_ptr, uint32_t size)
{ return nv2a_bind_instance_memory(guest_base, host_ptr, size); }

void nv2a_hook_init(ptrdiff_t xbox_mem_offset)
{ (void)xbox_mem_offset; }
bool nv2a_hook_install_aperture(void *aperture, size_t size)
{ (void)aperture; (void)size; return false; }
void nv2a_hook_disable_aperture(void) {}
void nv2a_hook_shutdown(void) {}
bool nv2a_hook_run_decoder_tests(void) { return false; }
bool nv2a_hook_set_irq_sink(void (*sink)(void *opaque, int asserted), void *opaque)
{ (void)sink; (void)opaque; return false; }
bool nv2a_hook_set_ptimer_clock(uint64_t (*clock_ns)(void *), void *opaque)
{ (void)clock_ns; (void)opaque; return false; }
void nv2a_hook_notify_ptimer_clock_changed(void) {}
bool nv2a_hook_get_submission_snapshot(NV2AHookSubmissionSnapshot *snapshot)
{ (void)snapshot; return false; }
bool nv2a_hook_pci_config_read(uint32_t offset, void *buffer, uint32_t length)
{ (void)offset; (void)buffer; (void)length; return false; }
bool nv2a_hook_pci_config_write(uint32_t offset, const void *buffer,
                                uint32_t length)
{ (void)offset; (void)buffer; (void)length; return false; }

bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write)
{ (void)ctx; (void)fault_addr; (void)fault_xbox_va; (void)is_write; return false; }

bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va)
{ (void)fault_addr; (void)fault_xbox_va; return false; }

#endif /* _WIN32 */
