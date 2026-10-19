/*
 * mmio_decode.h -- one x86-64 instruction decoder for trapped MMIO.
 *
 * A device whose registers need semantics cannot be plain memory: the reads
 * have to be answered and the writes have to be seen. The way that works here
 * is to leave the page PAGE_NOACCESS, catch the access in a vectored handler,
 * decode the faulting instruction, service it against the device model, and
 * step over it.
 *
 * That decoder existed twice before this file -- once in nv2a_mmio_hook.c and
 * once in apu_mmio_hook.c -- as the same opcode table written out against two
 * different pairs of accessors. This is the same logic with the accessors
 * passed in, so a third device does not need a third copy. The APU hook has
 * since moved onto it; nv2a_mmio_hook.c still carries its own copy.
 *
 * THE DECODE IS FACTORED FROM THE SERVICE (`mmio_decode` then
 * `mmio_execute`), because a handler has to be able to VALIDATE an access
 * before anything mutates -- and because the fault address is not necessarily
 * the operand start: an access that crosses a page boundary can fault on the
 * second page. `mmio_decode` fills a descriptor (width, direction, operands,
 * the effective start address of the memory operand, the instruction length)
 * and touches neither the context nor the device. `mmio_emulate` is the
 * two-line composition of both, so every existing caller behaves as before.
 *
 * Header-only and static inline: one function, one caller per device, and a
 * library for it would be more build wiring than code.
 *
 * The instructions covered are what the XDK device code actually emits against
 * registers -- moves both ways, the immediate forms, the zero-extending loads,
 * and the ALU, read-modify-write and flag-setting forms a poll loop is built
 * from (DirectSound polls with `test dword [mem], imm32`, and a C compiler
 * folds `x += REG` into an ALU op with a memory operand).
 * Anything outside that set returns 0 rather than guessing: stepping over an
 * instruction that was not understood corrupts the guest silently, which is
 * far worse than a fault naming the opcode. A form with no memory operand
 * (mod == 3) is likewise refused -- it cannot be a trapped device access -- and
 * so are the segment-override and LOCK prefixes, which change the effective
 * address or the atomicity in ways this decoder does not model.
 */
#ifndef MMIO_DECODE_H
#define MMIO_DECODE_H

#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>

/* Service one register access. dev is passed straight back to the callbacks. */
typedef uint64_t (*mmio_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*mmio_write_fn)(void *dev, uint32_t off, uint64_t val, int size);

static inline uint64_t *mmio_ctx_reg(PCONTEXT c, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t *)&c->Rax;   case 1:  return (uint64_t *)&c->Rcx;
    case 2:  return (uint64_t *)&c->Rdx;   case 3:  return (uint64_t *)&c->Rbx;
    case 4:  return (uint64_t *)&c->Rsp;   case 5:  return (uint64_t *)&c->Rbp;
    case 6:  return (uint64_t *)&c->Rsi;   case 7:  return (uint64_t *)&c->Rdi;
    case 8:  return (uint64_t *)&c->R8;    case 9:  return (uint64_t *)&c->R9;
    case 10: return (uint64_t *)&c->R10;   case 11: return (uint64_t *)&c->R11;
    case 12: return (uint64_t *)&c->R12;   case 13: return (uint64_t *)&c->R13;
    case 14: return (uint64_t *)&c->R14;   case 15: return (uint64_t *)&c->R15;
    default: return NULL;
    }
}

/* Length of the ModRM byte and everything it introduces, plus what it
 * introduced: `sib_out` is 1 when a SIB byte follows the ModRM and `disp_out`
 * is the displacement size in bytes (0, 1 or 4).
 *
 * THE SIB CASE THAT USED TO BE MISSED. With mod == 0 and a SIB whose base field
 * is 5, the form has no base register and carries a full disp32 -- so the SIB
 * byte costs two extra bytes, not one. Counting only the SIB advanced Rip four
 * bytes short and re-executed mid-instruction, which is silent corruption.
 * (`mod == 0 && rm == 5` without a SIB is the RIP-relative form: also disp32.) */
static inline int mmio_modrm_len(const uint8_t *ip, int rex_b,
                                 int *sib_out, int *disp_out)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm  = (modrm & 7) | (rex_b ? 8 : 0);
    int len = 1, sib = 0, disp = 0;

    if (mod != 3) {
        if ((rm & 7) == 4) {                        /* SIB                    */
            uint8_t s = ip[1];
            sib = 1;
            len += 1;
            if (mod == 0 && (s & 7) == 5) disp = 4;  /* no base: disp32        */
        }
        if (!sib && mod == 0 && (rm & 7) == 5) disp = 4;  /* RIP-relative     */
        else if (mod == 1) disp = 1;
        else if (mod == 2) disp = 4;
        len += disp;
    }
    if (sib_out)  *sib_out  = sib;
    if (disp_out) *disp_out = disp;
    return len;
}

/* The ALU operations, numbered as the reg field of group 1 (80/81/83) and as
 * bits 5:3 of the classic two-operand opcodes. TEST has no slot of its own. */
enum { MMIO_ALU_ADD = 0, MMIO_ALU_OR = 1, MMIO_ALU_AND = 4, MMIO_ALU_SUB = 5,
       MMIO_ALU_XOR = 6, MMIO_ALU_CMP = 7, MMIO_ALU_TEST = 8 };

static inline uint64_t mmio_size_mask(int size)
{
    return size >= 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
}

/* a OP b at the given width, with the flags a following jcc reads: ZF, SF, CF
 * and OF. PF and AF are left alone; nothing branches on them after a register
 * read. The logical operations clear CF and OF, as the hardware does. */
static inline uint64_t mmio_alu(PCONTEXT ctx, int op, uint64_t a, uint64_t b,
                                int size)
{
    uint64_t m = mmio_size_mask(size), sign = 1ULL << (size * 8 - 1), r;
    int cf = 0, of = 0;

    a &= m; b &= m;
    switch (op) {
    case MMIO_ALU_ADD:
        r  = (a + b) & m;
        cf = r < a;
        of = ((~(a ^ b) & (a ^ r)) & sign) != 0;
        break;
    case MMIO_ALU_SUB: case MMIO_ALU_CMP:
        r  = (a - b) & m;
        cf = a < b;
        of = (((a ^ b) & (a ^ r)) & sign) != 0;
        break;
    case MMIO_ALU_OR:  r = a | b; break;
    case MMIO_ALU_XOR: r = a ^ b; break;
    default:           r = a & b; break;                /* AND, TEST */
    }
    ctx->EFlags &= ~(0x0001u | 0x0040u | 0x0080u | 0x0800u);
    if (cf)       ctx->EFlags |= 0x0001u;               /* CF */
    if (r == 0)   ctx->EFlags |= 0x0040u;               /* ZF */
    if (r & sign) ctx->EFlags |= 0x0080u;               /* SF */
    if (of)       ctx->EFlags |= 0x0800u;               /* OF */
    return r;
}

/* A general register at an operand width. Without a REX prefix, byte
 * registers 4-7 are AH, CH, DH and BH -- bits 15:8 of registers 0-3. */
static inline uint64_t mmio_get_reg(PCONTEXT c, int reg, int size, int has_rex)
{
    if (size == 1 && !has_rex && reg >= 4 && reg < 8)
        return (*mmio_ctx_reg(c, reg - 4) >> 8) & 0xFF;
    return *mmio_ctx_reg(c, reg) & mmio_size_mask(size);
}

/* Write with x86-64 width rules: a 32-bit write zeroes bits 63:32, an 8- or
 * 16-bit write merges into what was there. */
static inline void mmio_set_reg(PCONTEXT c, int reg, uint64_t v, int size,
                                int has_rex)
{
    uint64_t *d;
    if (size == 1 && !has_rex && reg >= 4 && reg < 8) {
        d  = mmio_ctx_reg(c, reg - 4);
        *d = (*d & ~0xFF00ULL) | ((v & 0xFF) << 8);
        return;
    }
    d = mmio_ctx_reg(c, reg);
    if (size == 1)      *d = (*d & ~0xFFULL)   | (v & 0xFF);
    else if (size == 2) *d = (*d & ~0xFFFFULL) | (v & 0xFFFF);
    else if (size == 4) *d = v & 0xFFFFFFFFULL;
    else                *d = v;
}

/* The immediate of an instruction whose operand size is `size`: imm8 for byte
 * forms, imm16 under a 66 prefix, otherwise imm32 sign-extended (REX.W). */
static inline uint64_t mmio_imm(const uint8_t *p, int size, int *len)
{
    if (size == 1) { *len = 1; return p[0]; }
    if (size == 2) { *len = 2; return (uint64_t)p[0] | ((uint64_t)p[1] << 8); }
    *len = 4;
    return (uint64_t)(int64_t)(int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)
                                      | ((uint32_t)p[2] << 16)
                                      | ((uint32_t)p[3] << 24));
}

/* ── One decoded trapped access ──────────────────────────────────────────────
 *
 * `mmio_decode` fills this and mutates nothing. `eff` is the EFFECTIVE START of
 * the memory operand -- not the fault address -- so a handler can compare the
 * real operand against its region and compute a device offset from it. */
enum { MMIO_OP_NONE = 0, MMIO_OP_MOV_W, MMIO_OP_MOV_I, MMIO_OP_MOV_R,
       MMIO_OP_TEST_R, MMIO_OP_TEST_I, MMIO_OP_MOVZX, MMIO_OP_ALU };

typedef struct {
    int      op;         /* MMIO_OP_*                                          */
    int      alu;        /* MMIO_ALU_* when op == MMIO_OP_ALU                  */
    int      size;       /* memory-operand width: 1, 2, 4 or 8                 */
    int      dstsize;    /* MOVZX destination width                            */
    int      is_write;   /* the memory operand is written (RMW forms: both)     */
    int      has_reg;    /* the form has a register operand                    */
    int      reg;        /* that register                                      */
    int      has_imm;    /* the form has an immediate                          */
    uint64_t imm;        /* that immediate                                     */
    int      mem_first;  /* the memory operand is the FIRST source (r/m OP r)  */
    int      has_rex;    /* a REX prefix was present (byte-register naming)    */
    int      rip_delta;  /* instruction length in bytes                        */
    uint64_t eff;        /* effective start address of the memory operand      */
} mmio_insn_t;

/* 1 when the instruction at ctx->Rip is one this decoder covers AND has a
 * memory operand; all of *d is then filled and nothing else has changed. 0
 * otherwise, with the context untouched. */
static inline int mmio_decode(PCONTEXT ctx, mmio_insn_t *d)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    const uint8_t *op, *mo;
    int prefix = 0, has66 = 0, rex = 0, has_rex = 0;
    int rex_w, rex_r, rex_x, rex_b;
    int mo_off, mlen, ilen = 0, sib = 0, disp_len = 0, mod;

    for (;;) {
        uint8_t b = ip[prefix];
        if (b == 0x66) { has66 = 1; prefix++; }
        else if (b == 0xF2 || b == 0xF3) prefix++;
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix++; }
        else break;
    }
    {   /* Segment overrides (address) and LOCK (atomicity) are not modelled. */
        uint8_t b = ip[prefix];
        if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E
                || b == 0x64 || b == 0x65 || b == 0xF0)
            return 0;
    }

    rex_w = has_rex && (rex & 0x08);
    rex_r = has_rex && (rex & 0x04);
    rex_x = has_rex && (rex & 0x02);
    rex_b = has_rex && (rex & 0x01);
    op = ip + prefix;

    d->op = MMIO_OP_NONE; d->alu = 0; d->dstsize = 0; d->is_write = 0;
    d->has_reg = 0; d->reg = -1; d->has_imm = 0; d->imm = 0; d->mem_first = 0;
    d->has_rex = has_rex; d->rip_delta = 0; d->eff = 0;
    d->size = rex_w ? 8 : (has66 ? 2 : 4);

    if (op[0] < 0x40 && (op[0] & 7) < 4) {              /* two-operand ALU    */
        int alu = op[0] >> 3;
        if (alu == 2 || alu == 3) return 0;             /* ADC, SBB           */
        d->op = MMIO_OP_ALU;
        d->alu = alu;
        if (!(op[0] & 1)) d->size = 1;
        d->has_reg = 1;
        /* Bit 1 selects the direction: set is "op r, r/m" (register first), clear is
         * "op r/m, r" (memory first). CMP must keep that order because the flags a following jcc
         * reads are SIGNED and CARRY flags of `a - b`, which are not commutative. */
        d->mem_first = (op[0] & 2) ? 0 : 1;
        d->is_write = (alu == MMIO_ALU_CMP) ? 0 : d->mem_first;
        mo_off = 1;
    } else switch (op[0]) {
    case 0x88: case 0x89:                               /* MOV r/m, r         */
        d->op = MMIO_OP_MOV_W;
        d->is_write = 1;
        d->has_reg = 1;
        if (op[0] == 0x88) d->size = 1;
        mo_off = 1;
        break;
    case 0xC6: case 0xC7:                               /* MOV r/m, imm       */
        d->op = MMIO_OP_MOV_I;
        d->is_write = 1;
        d->has_imm = 1;
        if (op[0] == 0xC6) d->size = 1;
        mo_off = 1;
        break;
    case 0x8A: case 0x8B:                               /* MOV r, r/m         */
        d->op = MMIO_OP_MOV_R;
        d->has_reg = 1;
        if (op[0] == 0x8A) d->size = 1;
        mo_off = 1;
        break;
    case 0x84: case 0x85:                               /* TEST r/m, r        */
        d->op = MMIO_OP_TEST_R;
        d->has_reg = 1;
        if (op[0] == 0x84) d->size = 1;
        mo_off = 1;
        break;
    case 0x80: case 0x81: case 0x83:                    /* group 1 r/m, imm   */
        d->op = MMIO_OP_ALU;
        d->alu = (op[1] >> 3) & 7;
        if (d->alu == 2 || d->alu == 3) return 0;       /* ADC, SBB           */
        d->has_imm = 1;
        d->is_write = (d->alu != MMIO_ALU_CMP);
        if (op[0] == 0x80) d->size = 1;
        mo_off = 1;
        break;
    case 0xF6: case 0xF7:                               /* TEST r/m, imm      */
        if (((op[1] >> 3) & 7) > 1) return 0;           /* /1 aliases /0      */
        d->op = MMIO_OP_TEST_I;
        d->has_imm = 1;
        if (op[0] == 0xF6) d->size = 1;
        mo_off = 1;
        break;
    case 0x0F:
        if (op[1] != 0xB6 && op[1] != 0xB7) return 0;   /* MOVZX r, r/m8|16   */
        d->op = MMIO_OP_MOVZX;
        d->size = (op[1] == 0xB6) ? 1 : 2;
        d->dstsize = rex_w ? 8 : (has66 ? 2 : 4);
        d->has_reg = 1;
        mo_off = 2;
        break;
    default:
        return 0;
    }

    mo = op + mo_off;
    mod = (mo[0] >> 6) & 3;
    if (mod == 3) return 0;                             /* no memory operand  */
    mlen = mmio_modrm_len(mo, rex_b, &sib, &disp_len);
    d->reg = ((mo[0] >> 3) & 7) | (rex_r ? 8 : 0);
    if (d->has_imm) {
        if (op[0] == 0x83) {                            /* imm8, sign-extended */
            d->imm = (uint64_t)(int64_t)(int8_t)mo[mlen];
            ilen = 1;
        } else {
            d->imm = mmio_imm(mo + mlen, d->size, &ilen);
        }
    }
    d->rip_delta = prefix + mo_off + mlen + ilen;

    {
        const uint8_t *dp = mo + 1 + (sib ? 1 : 0);
        int64_t disp = 0;

        if (disp_len == 1) {
            disp = (int8_t)dp[0];
        } else if (disp_len == 4) {
            disp = (int32_t)((uint32_t)dp[0] | ((uint32_t)dp[1] << 8)
                           | ((uint32_t)dp[2] << 16) | ((uint32_t)dp[3] << 24));
        }

        if (sib) {
            uint8_t s = mo[1];
            int ss = (s >> 6) & 3;
            int idx = ((s >> 3) & 7) | (rex_x ? 8 : 0);
            int base = (s & 7) | (rex_b ? 8 : 0);
            uint64_t basev  = (mod == 0 && (s & 7) == 5)
                              ? 0 : mmio_get_reg(ctx, base, 8, 1);
            uint64_t indexv = (idx == 4) ? 0 : mmio_get_reg(ctx, idx, 8, 1);
            d->eff = basev + (indexv << ss) + (uint64_t)disp;
        } else if (mod == 0 && (mo[0] & 7) == 5) {
            /* RIP-relative: the displacement is measured from the END of the
             * instruction, so the length had to be known first. */
            d->eff = (uint64_t)(uintptr_t)ctx->Rip + (uint64_t)d->rip_delta
                     + (uint64_t)disp;
        } else {
            d->eff = mmio_get_reg(ctx, ((mo[0] & 7) | (rex_b ? 8 : 0)), 8, 1)
                     + (uint64_t)disp;
        }
    }
    return 1;
}

/* Service one decoded access through the caller's callbacks and advance Rip
 * past the instruction. `off` is the device-relative offset the caller decided
 * on; this function does not second-guess it. */
static inline int mmio_execute(PCONTEXT ctx, const mmio_insn_t *d, uint32_t off,
                               void *dev, mmio_read_fn rd, mmio_write_fn wr)
{
    uint64_t v, mem;

    if (!rd || !wr) return 0;

    switch (d->op) {
    case MMIO_OP_MOV_W:
        wr(dev, off, mmio_get_reg(ctx, d->reg, d->size, d->has_rex), d->size);
        break;
    case MMIO_OP_MOV_I:
        wr(dev, off, d->imm & mmio_size_mask(d->size), d->size);
        break;
    case MMIO_OP_MOV_R:
        mmio_set_reg(ctx, d->reg, rd(dev, off, d->size), d->size, d->has_rex);
        break;
    case MMIO_OP_TEST_R:
        mmio_alu(ctx, MMIO_ALU_TEST, rd(dev, off, d->size),
                 mmio_get_reg(ctx, d->reg, d->size, d->has_rex), d->size);
        break;
    case MMIO_OP_TEST_I:
        mmio_alu(ctx, MMIO_ALU_TEST, rd(dev, off, d->size), d->imm, d->size);
        break;
    case MMIO_OP_MOVZX:
        mmio_set_reg(ctx, d->reg, rd(dev, off, d->size) & mmio_size_mask(d->size),
                     d->dstsize, 1);
        break;
    case MMIO_OP_ALU:
        if (d->has_imm) {                               /* op r/m, imm        */
            v = mmio_alu(ctx, d->alu, rd(dev, off, d->size), d->imm, d->size);
            if (d->alu != MMIO_ALU_CMP) wr(dev, off, v, d->size);
        } else if (d->mem_first) {                      /* op r/m, r          */
            mem = rd(dev, off, d->size);
            v = mmio_alu(ctx, d->alu, mem,
                         mmio_get_reg(ctx, d->reg, d->size, d->has_rex),
                         d->size);
            if (d->is_write) wr(dev, off, v, d->size);
        } else {                                        /* op r, r/m          */
            v = mmio_alu(ctx, d->alu,
                         mmio_get_reg(ctx, d->reg, d->size, d->has_rex),
                         rd(dev, off, d->size), d->size);
            if (d->alu != MMIO_ALU_CMP)
                mmio_set_reg(ctx, d->reg, v, d->size, d->has_rex);
        }
        break;
    default:
        return 0;
    }
    ctx->Rip += d->rip_delta;
    return 1;
}

/* 1 if the instruction at ctx->Rip was serviced and Rip advanced past it.
 *
 * Covered: MOV both ways (88/89 8A/8B C6/C7), MOVZX (0F B6/B7), the
 * two-operand ALU forms in both directions for ADD/OR/AND/SUB/XOR/CMP
 * (00-03 08-0B 20-23 28-2B 30-33 38-3B), group 1 with an immediate
 * (80/81/83, not ADC/SBB), and TEST against a register or an immediate
 * (84/85, F6/F7 /0 and /1). A register-destination form reads the device
 * once; a memory-destination form reads, then writes (CMP and TEST only
 * read). ADC/SBB, the rest of F6/F7 (NOT/NEG/MUL/DIV), string moves,
 * register-only forms, segment/LOCK prefixes and anything else return 0. */
static inline int mmio_emulate(PCONTEXT ctx, uint32_t off, void *dev,
                               mmio_read_fn rd, mmio_write_fn wr)
{
    mmio_insn_t d;

    if (!mmio_decode(ctx, &d))
        return 0;
    return mmio_execute(ctx, &d, off, dev, rd, wr);
}

#endif /* _WIN32 */
#endif /* MMIO_DECODE_H */
