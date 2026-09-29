/*
 * DSP56300 emulator
 *
 * Copyright (c) 2015 espes
 * Copyright (c) 2020-2025 Matt Borgerson
 *
 * Adapted from Hatari DSP M56001 emulation
 * (C) 2003-2008 ARAnyM developer team
 * Adaption to Hatari (C) 2008 by Thomas Huth
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "dsp_cpu.h"
#include "debug.h"
#include "trace.h"

/* A4b1 LOCAL MODIFICATION (new include after dsp_cpu.c:30): the toolkit's GP
 * input accounting, for the MIXBUF read hook below. */
#include "apu_watch.h"

/* A4b2-NR-followup: DSPState, for the is_gp field reached through the core's
 * opaque back-pointer. Needed because the interpreter core's own is_gp is never
 * populated (dsp_c_sync_from_vm is registered but not called), so reading it
 * would silently filter out every GP instruction. */
#include "dsp.h"

#define BITMASK(x)  ((1<<(x))-1)

#define TRACE_DSP_DISASM 0
#define TRACE_DSP_DISASM_REG 0
#define TRACE_DSP_DISASM_MEM 0

// #define DSP_COUNT_IPS     /* Count instruction per seconds */


/**********************************
 *  Defines
 **********************************/

#define SIGN_PLUS  0
#define SIGN_MINUS 1

/**********************************
 *  Functions
 **********************************/

static void dsp_postexecute_update_pc(dsp_core_t* dsp);
static void dsp_postexecute_interrupts(dsp_core_t* dsp);

static uint32_t read_memory_p(dsp_core_t* dsp, uint32_t address);
static uint32_t read_memory_disasm(dsp_core_t* dsp, int space, uint32_t address);

static void write_memory_raw(dsp_core_t* dsp, int space, uint32_t address, uint32_t value);
static void write_memory_disasm(dsp_core_t* dsp, int space, uint32_t address, uint32_t value);

static void dsp_write_reg(dsp_core_t* dsp, uint32_t numreg, uint32_t value);

static void dsp_stack_push(dsp_core_t* dsp, uint32_t curpc, uint32_t cursr, uint16_t sshOnly);
static void dsp_stack_pop(dsp_core_t* dsp, uint32_t *curpc, uint32_t *cursr);
static void dsp_compute_ssh_ssl(dsp_core_t* dsp);

/* 56bits arithmetic */
static uint16_t dsp_abs56(uint32_t *dest);
static uint16_t dsp_asl56(uint32_t *dest, int n);
static uint16_t dsp_asr56(uint32_t *dest, int n);
static uint16_t dsp_add56(uint32_t *source, uint32_t *dest);
static uint16_t dsp_sub56(uint32_t *source, uint32_t *dest);
static void dsp_mul56(uint32_t source1, uint32_t source2, uint32_t *dest, uint8_t signe);
static void dsp_rnd56(dsp_core_t* dsp, uint32_t *dest);
static uint32_t dsp_signextend(int bits, uint32_t v);

/* Vector addresses per DSP56300FM Table 2-2, indexed by DSP_INTER_* */
static const dsp_interrupt_t dsp_interrupt[4] = {
    { DSP_INTER_RESET, 0x00, 0, "Reset" },
    { DSP_INTER_ILLEGAL, 0x04, 0, "Illegal" },
    { DSP_INTER_STACK_ERROR, 0x02, 0, "Stack Error" },
    { DSP_INTER_TRAP, 0x08, 0, "Trap" },
};

static const int registers_tcc[16][2] = {
    {DSP_REG_B,DSP_REG_A},
    {DSP_REG_A,DSP_REG_B},
    {DSP_REG_NULL,DSP_REG_NULL},
    {DSP_REG_NULL,DSP_REG_NULL},

    {DSP_REG_NULL,DSP_REG_NULL},
    {DSP_REG_NULL,DSP_REG_NULL},
    {DSP_REG_NULL,DSP_REG_NULL},
    {DSP_REG_NULL,DSP_REG_NULL},

    {DSP_REG_X0,DSP_REG_A},
    {DSP_REG_X0,DSP_REG_B},
    {DSP_REG_Y0,DSP_REG_A},
    {DSP_REG_Y0,DSP_REG_B},

    {DSP_REG_X1,DSP_REG_A},
    {DSP_REG_X1,DSP_REG_B},
    {DSP_REG_Y1,DSP_REG_A},
    {DSP_REG_Y1,DSP_REG_B}
};

static const int registers_mask[64] = {
    0, 0, 0, 0,
    24, 24, 24, 24,
    24, 24, 8, 8,
    24, 24, 24, 24,

    16, 16, 16, 16,
    16, 16, 16, 16,
    16, 16, 16, 16,
    16, 16, 16, 16,

    16, 16, 16, 16,
    16, 16, 16, 16,
    0, 0, 0, 0,
    0, 0, 0, 0,

    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 16, 8, 6,
    16, 16, 16, 16
};

#include "dsp_emu.c.inc"
#include "dsp_dis.c.inc"

typedef bool (*match_func_t)(uint32_t op);

typedef struct OpcodeEntry {
    const char* template;
    const char* name;
    dis_func_t dis_func;
    emu_func_t emu_func;
    match_func_t match_func;
} OpcodeEntry;

static bool match_MMMRRR(uint32_t op)
{
    uint32_t RRR = (op >> 8) & BITMASK(3);
    uint32_t MMM = (op >> 11) & BITMASK(3);
    if (MMM == 0x6) {
        return RRR == 0x0 || RRR == 0x4;
    }
    return true;
}

static const OpcodeEntry nonparallel_opcodes[] = {
    { "0000000101iiiiii1000d000", "add #xx, D", dis_add_imm, emu_add_imm },
    { "00000001010000001100d000", "add #xxxx, D", dis_add_long, emu_add_long },
    { "0000000101iiiiii1000d110", "and #xx, D", dis_and_imm, emu_and_imm },
    { "00000001010000001100d110", "and #xxxx, D", dis_and_long, emu_and_long },
    { "00000000iiiiiiii101110EE", "andi #xx, D", dis_andi, emu_andi },
    { "0000110000011101SiiiiiiD", "asl #ii, S2, D", dis_asl_imm, emu_asl_imm },
    { "0000110000011110010SsssD", "asl S1, S2, D", NULL, NULL },
    { "0000110000011100SiiiiiiD", "asr #ii, S2, D", dis_asr_imm, emu_asr_imm },
    { "0000110000011110011SsssD", "asr S1, S2, D", NULL, NULL },
    { "00001101000100000100CCCC", "bcc xxxx", dis_bcc_long, emu_bcc_long }, //??
    { "00000101CCCC01aaaa0aaaaa", "bcc xxx", dis_bcc_imm, emu_bcc_imm },
    { "0000110100011RRR0100CCCC", "bcc Rn", NULL, NULL },
    { "0000101101MMMRRR0S00bbbb", "bchg #n, [X or Y]:ea", dis_bchg_ea, emu_bchg_ea, match_MMMRRR },
    { "0000101100aaaaaa0S00bbbb", "bchg #n, [X or Y]:aa", dis_bchg_aa, emu_bchg_aa },
    { "0000101110pppppp0S00bbbb", "bchg #n, [X or Y]:pp", dis_bchg_pp, emu_bchg_pp },
    { "0000000101qqqqqq0S0bbbbb", "bchg #n, [X or Y]:qq", NULL, NULL },
    { "0000101111DDDDDD010bbbbb", "bchg, #n, D", dis_bchg_reg, emu_bchg_reg },
    { "0000101001MMMRRR0S00bbbb", "bclr #n, [X or Y]:ea", dis_bclr_ea, emu_bclr_ea, match_MMMRRR },
    { "0000101000aaaaaa0S00bbbb", "bclr #n, [X or Y]:aa", dis_bclr_aa, emu_bclr_aa },
    { "0000101010pppppp0S00bbbb", "bclr #n, [X or Y]:pp", dis_bclr_pp, emu_bclr_pp },
    { "0000000100qqqqqq0S00bbbb", "bclr #n, [X or Y]:qq", NULL, NULL },
    { "0000101011DDDDDD010bbbbb", "bclr #n, D", dis_bclr_reg, emu_bclr_reg },
    { "000011010001000011000000", "bra xxxx", dis_bra_long, emu_bra_long },
    { "00000101000011aaaa0aaaaa", "bra xxx", dis_bra_imm, emu_bra_imm },
    { "0000110100011RRR11000000", "bra Rn", NULL, NULL },
    { "0000110010MMMRRR0S0bbbbb", "brclr #n, [X or Y]:ea, xxxx", NULL, NULL, match_MMMRRR },
    { "0000110010aaaaaa1S0bbbbb", "brclr #n, [X or Y]:aa, xxxx", NULL, NULL },
    { "0000110011pppppp0S0bbbbb", "brclr #n, [X or Y]:pp, xxxx", dis_brclr_pp, emu_brclr_pp },
    { "0000010010qqqqqq0S0bbbbb", "brclr #n, [X or Y]:qq, xxxx", NULL, NULL },
    { "0000110011DDDDDD100bbbbb", "brclr #n, S, xxxx", dis_brclr_reg, emu_brclr_reg },
    { "00000000000000100001CCCC", "brkcc", NULL, NULL },
    { "0000110010MMMRRR0S1bbbbb", "brset #n, [X or Y]:ea, xxxx", NULL, NULL, match_MMMRRR },
    { "0000110010aaaaaa1S1bbbbb", "brset #n, [X or Y]:aa, xxxx", NULL, NULL },
    { "0000110011pppppp0S1bbbbb", "brset #n, [X or Y]:pp, xxxx", dis_brset_pp, emu_brset_pp },
    { "0000010010qqqqqq0S1bbbbb", "brset #n, [X or Y]:qq, xxxx", NULL, NULL },
    { "0000110011DDDDDD101bbbbb", "brset #n, S, xxxx", dis_brset_reg, emu_brset_reg },
    { "00001101000100000000CCCC", "bscc xxxx", NULL, NULL },
    { "00000101CCCC00aaaa0aaaaa", "bscc xxx", NULL, NULL },
    { "0000110100011RRR0000CCCC", "bscc Rn", NULL, NULL },
    { "0000110110MMMRRR0S0bbbbb", "bsclr #n, [X or Y]:ea, xxxx", NULL, NULL, match_MMMRRR },
    { "0000110110aaaaaa1S0bbbbb", "bsclr #n, [X or Y]:aa, xxxx", NULL, NULL },
    { "0000010010qqqqqq1S0bbbbb", "bsclr #n, [X or Y]:qq, xxxx", NULL, NULL },
    { "0000110111pppppp0S0bbbbb", "bsclr #n, [X or Y]:pp, xxxx", NULL, NULL },
    { "0000110111DDDDDD100bbbbb", "bsclr, #n, S, xxxx", NULL, NULL },
    { "0000101001MMMRRR0S1bbbbb", "bset #n, [X or Y]:ea", dis_bset_ea, emu_bset_ea, match_MMMRRR },
    { "0000101000aaaaaa0S1bbbbb", "bset #n, [X or Y]:aa", dis_bset_aa, emu_bset_aa },
    { "0000101010pppppp0S1bbbbb", "bset #n, [X or Y]:pp", dis_bset_pp, emu_bset_pp },
    { "0000000100qqqqqq0S1bbbbb", "bset #n, [X or Y]:qq", NULL, NULL },
    { "0000101011DDDDDD011bbbbb", "bset, #n, D", dis_bset_reg, emu_bset_reg },
    { "000011010001000010000000", "bsr xxxx", dis_bsr_long, emu_bsr_long },
    { "00000101000010aaaa0aaaaa", "bsr xxx", dis_bsr_imm, emu_bsr_imm },
    { "0000110100011RRR10000000", "bsr Rn", NULL, NULL },
    { "0000110110MMMRRR0S1bbbbb", "bsset #n, [X or Y]:ea, xxxx", NULL, NULL, match_MMMRRR },
    { "0000110110aaaaaa1S1bbbbb", "bsset #n, [X or Y]:aa, xxxx", NULL, NULL },
    { "0000110111pppppp0S1bbbbb", "bsset #n, [X or Y]:pp, xxxx", NULL, NULL },
    { "0000010010qqqqqq1S1bbbbb", "bsset #n, [X or Y]:qq, xxxx", NULL, NULL },
    { "0000110111DDDDDD101bbbbb", "bsset #n, S, xxxx", NULL, NULL },
    { "0000101101MMMRRR0S10bbbb", "btst #n, [X or Y]:ea", dis_btst_ea, emu_btst_ea, match_MMMRRR },
    { "0000101100aaaaaa0S10bbbb", "btst #n, [X or Y]:aa", dis_btst_aa, emu_btst_aa },
    { "0000101110pppppp0S10bbbb", "btst #n, [X or Y]:pp", dis_btst_pp, emu_btst_pp },
    { "0000000101qqqqqq0S10bbbb", "btst #n, [X or Y]:qq", NULL, NULL },
    { "0000101111DDDDDD0110bbbb", "btst #n, D", dis_btst_reg, emu_btst_reg },
    { "0000110000011110000000SD", "clb S, D", NULL, NULL },
    { "0000000101iiiiii1000d101", "cmp #xx, S2", dis_cmp_imm, emu_cmp_imm },
    { "00000001010000001100d101", "cmp #xxxx, S2", dis_cmp_long, emu_cmp_long },
    { "00001100000111111111gggd", "cmpu S1, S2", dis_cmpu, emu_cmpu },
    { "000000000000001000000000", "debug", NULL, NULL },
    { "00000000000000110000CCCC", "debugcc", NULL, NULL },
    { "00000000000000000000101d", "dec D", NULL /*dis_dec*/, emu_dec },
    { "000000011000000001JJd000", "div S, D", dis_div, emu_div },
    { "000000010010010s1sdkQQQQ", "dmac S1, S2, D", NULL, NULL },
    { "0000011001MMMRRR0S000000", "do [X or Y]:ea, expr", dis_do_ea, emu_do_ea, match_MMMRRR },
    { "0000011000aaaaaa0S000000", "do [X or Y]:aa, expr", dis_do_aa, emu_do_aa },
    { "00000110iiiiiiii1000hhhh", "do #xxx, expr", dis_do_imm, emu_do_imm },
    { "0000011011DDDDDD00000000", "do S, expr", dis_do_reg, emu_do_reg },
    { "000000000000001000000011", "do_f", NULL, NULL },
    { "0000011001MMMRRR0S010000", "dor [X or Y]:ea, label", NULL, NULL, match_MMMRRR },
    { "0000011000aaaaaa0S010000", "dor [X or Y]:aa, label", NULL, NULL },
    { "00000110iiiiiiii1001hhhh", "dor #xxx, label", dis_dor_imm, emu_dor_imm },
    { "0000011011DDDDDD00010000", "dor S, label", dis_dor_reg, emu_dor_reg },
    { "000000000000001000000010", "dor_f", NULL, NULL },
    { "000000000000000010001100", "enddo", NULL, emu_enddo },
    { "0000000101iiiiii1000d011", "eor #xx, D", NULL, NULL },
    { "00000001010000001100d011", "eor #xxxx, D", NULL, NULL },
    { "0000110000011010000sSSSD", "extract S1, S2, D", NULL, NULL },
    { "0000110000011000000s000D", "extract #CO, S2, D", NULL, NULL },
    { "0000110000011010100sSSSD", "extractu S1, S2, D", NULL, NULL },
    { "0000110000011000100s000D", "extractu #CO, S2, D", NULL, NULL },
    { "000000000000000000000101", "ill", NULL, emu_illegal },
    { "00000000000000000000100d", "inc D", NULL, emu_inc },
    { "00001100000110110qqqSSSD", "insert S1, S2, D", NULL, NULL },
    { "00001100000110010qqq000D", "insert #CO, S2, D", NULL, NULL },
    { "00001110CCCCaaaaaaaaaaaa", "jcc xxx", dis_jcc_imm, emu_jcc_imm },
    { "0000101011MMMRRR1010CCCC", "jcc ea", dis_jcc_ea, emu_jcc_ea, match_MMMRRR },
    { "0000101001MMMRRR1S00bbbb", "jclr #n, [X or Y]:ea, xxxx", dis_jclr_ea, emu_jclr_ea, match_MMMRRR },
    { "0000101000aaaaaa1S00bbbb", "jclr #n, [X or Y]:aa, xxxx", dis_jclr_aa, emu_jclr_aa },
    { "0000101010pppppp1S00bbbb", "jclr #n, [X or Y]:pp, xxxx", dis_jclr_pp, emu_jclr_pp },
    { "0000000110qqqqqq1S00bbbb", "jclr #n, [X or Y]:qq, xxxx", NULL, NULL },
    { "0000101011DDDDDD0000bbbb", "jclr #n, S, xxxx", dis_jclr_reg, emu_jclr_reg },
    { "0000101011MMMRRR10000000", "jmp ea", dis_jmp_ea, emu_jmp_ea, match_MMMRRR },
    { "000011000000aaaaaaaaaaaa", "jmp xxx", dis_jmp_imm, emu_jmp_imm },
    { "00001111CCCCaaaaaaaaaaaa", "jscc xxx", dis_jscc_imm, emu_jscc_imm },
    { "0000101111MMMRRR1010CCCC", "jscc ea", dis_jscc_ea, emu_jscc_ea, match_MMMRRR },
    { "0000101101MMMRRR1S00bbbb", "jsclr #n, [X or Y]:ea, xxxx", dis_jsclr_ea, emu_jsclr_ea, match_MMMRRR },
    { "0000101100MMMRRR1S00bbbb", "jsclr #n, [X or Y]:aa, xxxx", dis_jsclr_aa, emu_jsclr_aa, match_MMMRRR },
    { "0000101110pppppp1S0bbbbb", "jsclr #n, [X or Y]:pp, xxxx", dis_jsclr_pp, emu_jsclr_pp },
    { "0000000111qqqqqq1S0bbbbb", "jsclr #n, [X or Y]:qq, xxxx", NULL, NULL },
    { "0000101111DDDDDD000bbbbb", "jsclr #n, S, xxxx", dis_jsclr_reg, emu_jsclr_reg },
    { "0000101001MMMRRR1S10bbbb", "jset #n, [X or Y]:ea, xxxx", dis_jset_ea, emu_jset_ea, match_MMMRRR },
    { "0000101000MMMRRR1S10bbbb", "jset #n, [X or Y]:aa, xxxx", dis_jset_aa, emu_jset_aa, match_MMMRRR },
    { "0000101010pppppp1S10bbbb", "jset #n, [X or Y]:pp, xxxx", dis_jset_pp, emu_jset_pp },
    { "0000000110qqqqqq1S10bbbb", "jset #n, [X or Y]:qq, xxxx", NULL, NULL },
    { "0000101011DDDDDD0010bbbb", "jset #n, S, xxxx", dis_jset_reg, emu_jset_reg },
    { "0000101111MMMRRR10000000", "jsr ea", dis_jsr_ea, emu_jsr_ea, match_MMMRRR },
    { "000011010000aaaaaaaaaaaa", "jsr xxx", dis_jsr_imm, emu_jsr_imm },
    { "0000101101MMMRRR1S10bbbb", "jsset #n, [X or Y]:ea, xxxx", dis_jsset_ea, emu_jsset_ea, match_MMMRRR },
    { "0000101100aaaaaa1S10bbbb", "jsset #n, [X or Y]:aa, xxxx", dis_jsset_aa, emu_jsset_aa },
    { "0000101110pppppp1S1bbbbb", "jsset #n, [X or Y]:pp, xxxx", dis_jsset_pp, emu_jsset_pp },
    { "0000000111qqqqqq1S1bbbbb", "jsset #n, [X or Y]:qq, xxxx", NULL, NULL },
    { "0000101111DDDDDD001bbbbb", "jsset #n, S, xxxx", dis_jsset_reg, emu_jsset_reg },
    { "0000010011000RRR000ddddd", "lra Rn, D", NULL, NULL },
    { "0000010001000000010ddddd", "lra xxxx, D", NULL, NULL },
    { "000011000001111010iiiiiD", "lsl #ii, D", dis_lsl_imm, emu_lsl_imm },
    { "00001100000111100001sssD", "lsl S, D", NULL, NULL },
    { "000011000001111011iiiiiD", "lsr #ii, D", NULL, NULL },
    { "00001100000111100011sssD", "lsr S, D", NULL, NULL },
    { "00000100010MMRRR000ddddd", "lua ea, D", dis_lua, emu_lua },
    { "0000010000aaaRRRaaaadddd", "lua (Rn + aa), D", dis_lua_rel, emu_lua_rel },
    { "00000001000sssss11QQdk10", "mac S, #n, D", NULL, NULL },
    { "000000010100000111qqdk10", "maci #xxxx, S, D", NULL, NULL },
    { "00000001001001101sdkQQQQ", "mac_s_u S1, S2, D", NULL, NULL },
    { "00000001000sssss11QQdk11", "macr S1, S2, D", NULL, NULL },
    { "000000010100000111qqdk11", "macri #xxxx, S, D", NULL, NULL },
    { "00001100000110111000sssD", "merge S, D", NULL, NULL },
    { "0000101001110RRR1WDDDDDD", "move X:(Rn + xxxx) <-> R", dis_move_x_long, emu_move_x_long },
    { "0000101101110RRR1WDDDDDD", "move Y:(Rn + xxxx) <-> R", NULL, NULL },
    { "0000001aaaaaaRRR1a0WDDDD", "move X:(Rn + xxx) <-> R", dis_move_x_imm, emu_move_x_imm },
    { "0000001aaaaaaRRR1a1WDDDD", "move Y:(Rn + xxx) <-> R", dis_move_y_imm, emu_move_y_imm },
    { "00000101W1MMMRRR0s1ddddd", "movec [X or Y]:ea <-> R", dis_movec_ea, emu_movec_ea, match_MMMRRR },
    { "00000101W0aaaaaa0s1ddddd", "movec [X or Y]:aa <-> R", dis_movec_aa, emu_movec_aa, match_MMMRRR },
    { "00000100W1eeeeee101ddddd", "movec R1, R2", dis_movec_reg, emu_movec_reg },
    { "00000101iiiiiiii101ddddd", "movec #xx, D1", dis_movec_imm, emu_movec_imm },
    { "00000111W1MMMRRR10dddddd", "movem P:ea <-> R", dis_movem_ea, emu_movem_ea, match_MMMRRR },
    { "00000111W0aaaaaa00dddddd", "movem P:ea <-> R", dis_movem_aa, emu_movem_aa, match_MMMRRR },
    { "0000100sW1MMMRRR1Spppppp", "movep [X or Y]:ea <-> [X or Y]:pp", dis_movep_23, emu_movep_23, match_MMMRRR },
    { "00000111W1MMMRRR0Sqqqqqq", "movep [X or Y]:ea <-> X:qq", dis_movep_x_qq, emu_movep_x_qq, match_MMMRRR },
    { "00000111W0MMMRRR1Sqqqqqq", "movep [X or Y]:ea <-> Y:qq", NULL, NULL, match_MMMRRR },
    { "0000100sW1MMMRRR01pppppp", "movep [X or Y]:pp <-> P:ea", dis_movep_1, emu_movep_1, match_MMMRRR },
    { "000000001WMMMRRR0sqqqqqq", "movep [X or Y]:qq <-> P:ea", NULL, NULL, match_MMMRRR },
    { "0000100sW1dddddd00pppppp", "movep [X or Y]:pp <-> R", dis_movep_0, emu_movep_0 },
    { "00000100W1dddddd1q0qqqqq", "movep X:qq <-> R", NULL, NULL },
    { "00000100W1dddddd0q1qqqqq", "movep Y:qq <-> R", NULL, NULL },
    { "00000001000sssss11QQdk00", "mpy S, #n, D", NULL, NULL },
    { "00000001001001111sdkQQQQ", "mpy_s_u S1, S2, D", NULL, NULL },
    { "000000010100000111qqdk00", "mpyi #xxxx, S, D", dis_mpyi, emu_mpyi },
    { "00000001000sssss11QQdk01", "mpyr S, #n, D", NULL, NULL },
    { "000000010100000111qqdk01", "mpyri #xxxx, S, D", NULL, NULL },
    { "000000000000000000000000", "nop", NULL, emu_nop},
    { "0000000111011RRR0001d101", "norm Rn, D", dis_norm, emu_norm },
    { "00001100000111100010sssD", "normf S, D", NULL, NULL },
    { "0000000101iiiiii1000d010", "or #xx, D", NULL, NULL },
    { "00000001010000001100d010", "or #xxxx, D", dis_or_long, emu_or_long },
    { "00000000iiiiiiii111110EE", "ori #xx, D", dis_ori, emu_ori },
    { "000000000000000000000011", "pflush", NULL, NULL },
    { "000000000000000000000001", "pflushun", NULL, NULL },
    { "000000000000000000000010", "pfree", NULL, NULL },
    { "0000101111MMMRRR10000001", "plock ea", NULL, NULL, match_MMMRRR },
    { "000000000000000000001111", "plockr xxxx", NULL, NULL },
    { "0000101011MMMRRR10000001", "punlock ea", NULL, NULL, match_MMMRRR },
    { "000000000000000000001110", "punlockr xxxx", NULL, NULL },
    { "0000011001MMMRRR0S100000", "rep [X or Y]:ea", dis_rep_ea, emu_rep_ea, match_MMMRRR },
    { "0000011000aaaaaa0S100000", "rep [X or Y]:aa", dis_rep_aa, emu_rep_aa },
    { "00000110iiiiiiii1010hhhh", "rep #xxx", dis_rep_imm, emu_rep_imm },
    { "0000011011dddddd00100000", "rep S", dis_rep_reg, emu_rep_reg },
    { "000000000000000010000100", "reset", NULL, emu_reset },
    { "000000000000000000000100", "rti", NULL, emu_rti },
    { "000000000000000000001100", "rts", NULL, emu_rts },
    { "000000000000000010000111", "stop", NULL, emu_stop },
    { "0000000101iiiiii1000d100", "sub #xx, D", dis_sub_imm, emu_sub_imm },
    { "00000001010000001100d100", "sub #xxxx, D", dis_sub_long, emu_sub_long },
    { "00000010CCCC00000JJJd000", "tcc S1, D1", dis_tcc, emu_tcc },
    { "00000011CCCC0ttt0JJJdTTT", "tcc S1,D2 S2,D2", dis_tcc, emu_tcc },
    { "00000010CCCC1ttt00000TTT", "tcc S2, D2", dis_tcc, emu_tcc },
    { "000000000000000000000110", "trap", NULL, NULL },
    { "00000000000000000001CCCC", "trapcc", NULL, NULL },
    { "0000101S11MMMRRR110i0000", "vsl", NULL, NULL, match_MMMRRR },
    { "000000000000000010000110", "wait", NULL, emu_wait },
};

static bool matches_initialised;
static uint32_t nonparallel_matches[ARRAY_SIZE(nonparallel_opcodes)][2];

/**********************************
 *  Emulator kernel
 **********************************/

void dsp56k_reset_cpu(dsp_core_t* dsp)
{
    int i;
    if (!matches_initialised) {
        matches_initialised = true;
        for (i=0; i<ARRAY_SIZE(nonparallel_opcodes); i++) {
            const OpcodeEntry t = nonparallel_opcodes[i];
            assert(strlen(t.template) == 24);

            uint32_t mask = 0;
            uint32_t match = 0;
            int j;
            for (j=0; j<24; j++) {
                if (t.template[j] == '0' || t.template[j] == '1') {
                    mask |= 1 << (24-j-1);
                    match |= (t.template[j] - '0') << (24-j-1);
                }
            }

            nonparallel_matches[i][0] = mask;
            nonparallel_matches[i][1] = match;
        }
    }

    /* Memory */
    memset(dsp->periph, 0, sizeof(dsp->periph));
    memset(dsp->stack, 0, sizeof(dsp->stack));
    memset(dsp->registers, 0, sizeof(dsp->registers));

    /* Registers */
    dsp->pc = 0x0000;
    dsp->registers[DSP_REG_OMR]=0x02;
    for (i=0;i<8;i++) {
        dsp->registers[DSP_REG_M0+i]=0x00ffff;
    }

    /* Interruptions */
    memset(dsp->interrupt_is_pending, 0, sizeof(dsp->interrupt_is_pending));
    dsp->interrupt_state = DSP_INTERRUPT_NONE;
    dsp->interrupt_instr_fetch = -1;
    dsp->interrupt_save_pc = -1;
    dsp->interrupt_counter = 0;
    dsp->interrupt_pipeline_count = 0;
    for (i=0;i<4;i++) {
        dsp->interrupt_ipl[i] = 3;
    }

    /* Misc */
    dsp->loop_rep = 0;


    /* runtime shit */

    // start_time = SDL_GetTicks();
    dsp->num_inst = 0;

    dsp->exception_debugging = true;
    dsp->disasm_prev_inst_pc = 0xFFFFFFFF;
}

static const OpcodeEntry *lookup_opcode_slow(uint32_t op) {
    for (int i = 0; i < ARRAY_SIZE(nonparallel_opcodes); i++) {
        if ((op & nonparallel_matches[i][0]) == nonparallel_matches[i][1]) {
            if (nonparallel_opcodes[i].match_func
                && !nonparallel_opcodes[i].match_func(op)) continue;
            return &nonparallel_opcodes[i];
        }
    }

    fprintf(stderr, "op = %08x\n", op);
    assert(!"Invalid op code in dsp_cpu");
    return NULL;
}

static const OpcodeEntry *lookup_opcode(uint32_t op) {
    static struct opcache_entry {
        uint32_t op;
        const OpcodeEntry *entry;
    } opcache[256];

    uint8_t tag =
        ((op >> 24) & 0xff) ^
        ((op >> 16) & 0xff) ^
        ((op >>  8) & 0xff) ^
        ((op >>  0) & 0xff);
    if (opcache[tag].op != op || opcache[tag].entry == NULL) {
        opcache[tag].op = op;
        opcache[tag].entry = lookup_opcode_slow(op);
    }

    return opcache[tag].entry;
}

/* ============================================================
 * A4b2-NR diagnostic P-memory decode (discovery instrumentation)
 * ============================================================
 *
 * The pinned DSP56300 decoder below (disasm_instruction) already exists, but its
 * only output path is DPRINTF, gated by DEBUG_DSP == 0 (debug.h:25), so nothing
 * reaches a log. A4b2-NR Leg 1 needs the loaded program image as text, so this
 * exposes it behind a read-once environment gate.
 *
 * It is read-only with respect to the DSP: it saves and restores pc and the
 * decoder scratch state, and it changes no memory, register or control flow.
 * Strict no-op unless RECOMP_APU_GP_DECODE is set, so a production run is
 * unaffected.
 *
 * Undecodable words are printed as explicit gaps, never as NOPs: a decoder that
 * silently substituted NOPs would fabricate instructions in the very image the
 * static leg reasons about.
 */
static int gp_decode_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_GP_DECODE");
        on = (e && *e) ? 1 : 0;
    }
    return on;
}

/* Defined below; the diagnostic decoder needs it. */
static uint16_t disasm_instruction(dsp_core_t* dsp, dsp_trace_disasm_t mode);

/* A4b2-NR-followup: the P 00B9 effective-address trace, defined below and used
 * by dsp56k_execute_instruction() and dsp56k_read_memory(). */
static int b9_core_is_gp(dsp_core_t *dsp);
void dsp56k_b9_note_exec(dsp_core_t *dsp);
void dsp56k_b9_note_read(dsp_core_t *dsp, uint32_t address, uint32_t value);
void dsp56k_b9_terminal(const char *why);
void dsp56k_b9_epoch_begin(void);

/* A4b2-NR-next-edge-followup: the P-memory write watch, defined below and used
 * by write_memory_raw(). */
void dsp56k_pwrite_watch(dsp_core_t *dsp, uint32_t address, uint32_t value);
void dsp56k_pwrite_terminal(const char *why);
/* The bulk-load notes call this before its definition; without the declaration
 * C99 has no prototype in scope and MSVC only warns (C4013, then C4211). */
static int pwrite_watch_enabled(void);

/* A4b2-NR: set by dsp56k_request_decode() and serviced on the next executed
 * instruction. The request takes no argument because dsp_core_t is opaque
 * outside this translation unit, and gp_ep.c -- the packet's in-scope call
 * site -- cannot name it. */
static int gp_decode_pending = 0;
static int gp_decode2_pending = 0;

/* ============================================================
 * A4b2-NR-next-edge-followup: the P-memory write watch
 * ============================================================
 *
 * The Advisor mandated this before any slice may be built: the previous packet
 * found the GP loads a SECOND program into P-memory after the bootstrap, so a
 * slice is only valid over bytes PROVEN STABLE across its claimed window.
 *
 * This records every write to P-memory -- address, value, and the PC that made
 * it -- so quiescence over a range can be established, or every modification
 * mapped. It sits at write_memory_raw(), which is the single choke point for all
 * P writes regardless of which instruction or DMA path performed them.
 *
 * It is also the instrument that closes the doorbell-path transient-modification
 * gap: a watch over 0x000-0x172 showing no writes from bootstrap to exchange is
 * what upgrades "the doorbell instructions are byte-identical in two samples" to
 * "the doorbell instructions were not modified in between".
 *
 * Read-only with respect to the DSP (it observes, then the caller stores), and a
 * strict no-op unless RECOMP_APU_PWRITE_WATCH is set.
 *
 * Completeness: one event per write, streamed synchronously, uncapped, with a
 * monotonic ordinal and a terminal record carrying the count and per-range
 * tallies, so a truncated watch is detectable rather than silently read as
 * "no writes happened".
 */
#define PWRITE_IMAGE_I_LO   0x0000u
#define PWRITE_IMAGE_I_HI   0x0172u

static struct {
    int inited;
    int on;
    FILE *fp;
    int invalid;
    uint64_t events;
    uint64_t ord;
    uint64_t in_image_i;         /* writes landing in the boot image */
    uint64_t above_image_i;
    uint32_t first_in_image_i;   /* first such address, or ~1 */
    uint64_t first_in_image_ord;
    uint32_t first_in_image_pc;
    uint64_t ngp_skipped;        /* non-GP writes seen and excluded (EP) */
    /* Coverage audit: PRAM has mutation paths that do NOT go through
     * write_memory_raw (dsp_c.c). Each is reported so the residual is visible
     * in the record instead of being an unstated assumption. */
    uint64_t bootstrap_bulk;     /* dsp_c_bootstrap scratch_rw into core->pram */
    uint64_t sync_from_vm_bulk;  /* dsp_c_sync_from_vm memcpy core->pram */
} pw;

/* Called from dsp_c_bootstrap's bulk scratch_rw, so a bootstrap-time PRAM load is
 * RECORDED rather than silently bypassing the watch. */
void dsp56k_pwrite_note_bootstrap_bulk(size_t words)
{
    if (!pwrite_watch_enabled()) {
        return;
    }
    pw.bootstrap_bulk++;
    if (pw.fp) {
        fprintf(pw.fp, "# BOOTSTRAP_BULK_LOAD words=%zu (bypasses write_memory_raw)\n",
                words);
        fflush(pw.fp);
    }
}

/* Called from dsp_c_sync_from_vm's memcpy, which currently has zero callers.
 * If that ever changes, the record shows it. */
void dsp56k_pwrite_note_sync_bulk(size_t words)
{
    if (!pwrite_watch_enabled()) {
        return;
    }
    pw.sync_from_vm_bulk++;
    if (pw.fp) {
        fprintf(pw.fp, "# SYNC_FROM_VM_BULK words=%zu (bypasses write_memory_raw)\n",
                words);
        fflush(pw.fp);
    }
}

static void pw_fail(const char *why)
{
    if (!pw.invalid) {
        pw.invalid = 1;
        fprintf(stderr, "[GPWRITE] TRACE INVALID: %s (events=%llu)\n", why,
                (unsigned long long)pw.events);
        fflush(stderr);
    }
}

static int pwrite_watch_enabled(void)
{
    if (!pw.inited) {
        const char *e = getenv("RECOMP_APU_PWRITE_WATCH");
        pw.inited = 1;
        pw.on = (e && *e) ? 1 : 0;
        pw.first_in_image_i = 0xFFFFFFFFu;
        if (pw.on) {
            const char *p = getenv("RECOMP_APU_PWRITE_WATCH_FILE");
            pw.fp = fopen((p && *p) ? p : "gpwrite_watch.txt", "wb");
            if (!pw.fp) {
                pw_fail("cannot open watch artifact");
            } else {
                fprintf(pw.fp, "# A4b2-NR-next-edge-followup P-memory write watch\n");
                fprintf(pw.fp, "# fields: ord pc addr oldvalue newvalue in_image_i is_gp\n");
                fflush(pw.fp);
                fprintf(stderr, "[GPWRITE] watch enabled -> %s\n",
                        (p && *p) ? p : "gpwrite_watch.txt");
                fflush(stderr);
            }
        }
    }
    return pw.on;
}

void dsp56k_pwrite_watch(dsp_core_t *dsp, uint32_t address, uint32_t value)
{
    uint32_t old;
    int in_i;
    int is_gp;

    if (!pwrite_watch_enabled() || !dsp) {
        return;
    }
    /* GP-only. The GP and EP are separate cores over separate P-memories, so an
     * EP write is not an event in the GP program's epoch structure at all; mixing
     * the two would corrupt the quiescence argument. The flag is read through the
     * opaque back-pointer (DSPState.is_gp), NOT core->is_gp, which is never
     * populated -- see the guardrail comment on that field. Measured on the first
     * watch run: 3512 events, all is_gp=1, zero non-GP. The filter is here so that
     * the instrument is correct by construction rather than by the EP happening
     * not to run.
     *
     * Non-GP writes are COUNTED, not silently dropped: a nonzero count is
     * reported in the terminal so "the EP was quiet" is visible rather than
     * assumed. */
    is_gp = b9_core_is_gp(dsp);
    if (!is_gp) {
        pw.ngp_skipped++;
        return;
    }
    old = dsp->pram[address] & 0xFFFFFFu;
    in_i = (address >= PWRITE_IMAGE_I_LO && address <= PWRITE_IMAGE_I_HI);

    if (in_i) {
        pw.in_image_i++;
        if (pw.first_in_image_i == 0xFFFFFFFFu) {
            pw.first_in_image_i = address;
            pw.first_in_image_ord = pw.ord;
            pw.first_in_image_pc = dsp->pc;
        }
    } else {
        pw.above_image_i++;
    }

    if (!pw.fp || pw.invalid) {
        return;
    }
    if (fprintf(pw.fp, "%llu %04X %04X %06X %06X %d\n",
                (unsigned long long)pw.ord, dsp->pc, address, old,
                value & 0xFFFFFFu, in_i) < 0) {
        pw_fail("write failed");
        return;
    }
    pw.ord++;
    pw.events++;
}

void dsp56k_pwrite_terminal(const char *why)
{
    if (!pwrite_watch_enabled() || !pw.fp) {
        return;
    }
    if (fflush(pw.fp) != 0) {
        pw_fail("flush failed at terminal");
    }
    fprintf(stderr,
            "[GPWRITE] terminal reason=%s events=%llu in_image_i=%llu "
            "above_image_i=%llu ngp_skipped=%llu first_in_image=%s invalid=%d\n",
            why, (unsigned long long)pw.events,
            (unsigned long long)pw.in_image_i,
            (unsigned long long)pw.above_image_i,
            (unsigned long long)pw.ngp_skipped,
            (pw.first_in_image_i == 0xFFFFFFFFu) ? "none" : "PRESENT",
            pw.invalid);
    fflush(stderr);
    fprintf(pw.fp, "# terminal reason=%s events=%llu in_image_i=%llu "
                   "above_image_i=%llu ngp_skipped=%llu invalid=%d\n",
            why, (unsigned long long)pw.events,
            (unsigned long long)pw.in_image_i,
            (unsigned long long)pw.above_image_i,
            (unsigned long long)pw.ngp_skipped, pw.invalid);
    if (pw.first_in_image_i != 0xFFFFFFFFu) {
        fprintf(pw.fp, "# FIRST_IMAGE_I_WRITE addr=%04X ord=%llu pc=%04X\n",
                pw.first_in_image_i, (unsigned long long)pw.first_in_image_ord,
                pw.first_in_image_pc);
    }
    fflush(pw.fp);
}

/* ============================================================
 * A4b2-NR-followup: the P 00B9 effective-address trace
 * ============================================================
 *
 * The predecessor packet left one gap open: at P 00B9 (`move x:(r1),b`) the
 * address is `r1 = 0x80 + x:$0000`, and `x:$0000` is a guest-written mailbox, so
 * a static decode cannot bound it and cannot exclude the mix-buffer range.
 *
 * This records the ACTUAL effective X address of every such read, at the
 * dsp56k_read_memory call, so the question is answered by measurement rather
 * than inference. It is a complete record of the bounded observed exchange, not
 * a sample:
 *
 *   * one event per actual read, streamed synchronously, uncapped -- no first-N
 *     limit, no ring, no sampling, no best-effort drop;
 *   * an independent execution counter (gp_b9_exec_count) incremented at the
 *     instruction-entry choke point, so a missing read event shows up as a
 *     counter disagreement instead of being read as "no mix read happened";
 *   * a monotonic ordinal on every event, and a terminal record carrying both
 *     counters, so contiguity and completeness are checkable after the fact;
 *   * every failure (open/write/flush/ordinal) latches TRACE INVALID and is
 *     reported, because an incomplete trace must never be read as a negative.
 *
 * It is read-only with respect to the DSP: it observes the address and the
 * returned value and changes neither. Strict no-op unless RECOMP_APU_GP_B9_TRACE
 * is set, so a production run is unaffected.
 *
 * Stated limit, carried from the packet: this does NOT prove an address bound
 * for all possible future mailbox values. It answers what THIS run reads.
 */
#define GP_B9_PC 0x00B9u
#define GP_MIXBUF_LO  0x1400u
#define GP_MIXBUF_HI  0x17FFu
#define GP_MIXALIAS_LO 0x0C00u
#define GP_MIXALIAS_HI 0x0FFFu

static struct {
    int inited;
    int on;
    FILE *fp;
    int invalid;                 /* latched: an incomplete trace is never a negative */
    uint64_t events;             /* read events written */
    uint64_t execs;              /* independent P 00B9 executions entered */
    uint64_t epoch;              /* bootstrap epoch, for provenance */
    uint64_t ord_seq;            /* expected next ordinal */
    uint64_t in_mixbuf, in_alias;
    uint32_t first_bad;          /* first in-range address seen, or ~0 */
    uint64_t first_bad_ord;
    /* A4b2-NR-followup diagnostic: a PC histogram over the FULL P-memory
     * (DSP_PRAM_SIZE = 4096 words), so "P 00B9 never executed" can be
     * distinguished from "the trace is misattributing", and so the executed
     * program's true extent is visible. This was originally bounded at 0x200
     * (the decoded window) and that was WRONG for diagnosis: it silently dropped
     * every execution above 0x1FF, while the range tracker still recorded them.
     * A fixed key universe, so nothing is dropped. */
    uint64_t pc_hist[DSP_PRAM_SIZE];
    uint64_t gp_pc_hist[DSP_PRAM_SIZE];
    uint64_t exec_total;
    uint64_t gp_exec_total;
    uint64_t ngp_exec_total;
    uint32_t first_pc_seen;
    uint32_t gp_pc_min, gp_pc_max;
    uint32_t ngp_pc_min, ngp_pc_max;
    uint32_t gp_pc_first_high;   /* first GP PC >= 0x200 seen, or ~0 */
    int is_gp_seen;
    int gp_pc_seen;
    int ngp_pc_seen;
    int hist_dumped;
} b9;

static void b9_fail(const char *why)
{
    if (!b9.invalid) {
        b9.invalid = 1;
        fprintf(stderr, "[GPB9] TRACE INVALID: %s (events=%llu execs=%llu)\n", why,
                (unsigned long long)b9.events, (unsigned long long)b9.execs);
        fflush(stderr);
    }
}

/* Is this core the GP?
 *
 * NOT dsp->is_gp on the interpreter core: that field is only ever written by
 * dsp_c_sync_from_vm(), which is registered in the ops table but never called on
 * this path, so it is permanently 0 and would silently filter out every GP
 * instruction. (Measured: with it, the trace reported gp_exec_total=0 while the
 * PC histogram showed 288 distinct PCs of the GP image executing, P 00B9 among
 * them exactly 6 times -- the dor #$0006 count.)
 *
 * The reliable source is core->opaque, which dsp_c_init() points back at the
 * DSPState (dsp_c.c:282) and whose is_gp IS set correctly at dsp_init()
 * (dsp.c:143). dsp.h is included below for that one field. */
static int b9_core_is_gp(dsp_core_t *dsp)
{
    return (dsp && dsp->opaque) ? (((DSPState *)dsp->opaque)->is_gp ? 1 : 0) : 0;
}

/* A4b2-NR-next-edge: log the per-core identity ONCE per distinct opaque pointer,
 * so the trace's GP/EP attribution can be checked rather than trusted. A histogram
 * that classified every execution as GP while the EP also runs would silently mix
 * two programs; this makes that visible. */
static void b9_note_core_identity(dsp_core_t *dsp)
{
    static void *seen_ptr[8];
    static int seen_gp[8];
    static int n = 0;
    int i;

    if (!dsp) {
        return;
    }
    for (i = 0; i < n; i++) {
        if (seen_ptr[i] == dsp->opaque) {
            return;
        }
    }
    if (n < 8) {
        seen_ptr[n] = dsp->opaque;
        seen_gp[n] = b9_core_is_gp(dsp);
        fprintf(stderr,
                "[GPB9] core #%d: opaque=%p is_gp=%d (opaque resolves to a "
                "DSPState)\n", n, dsp->opaque, seen_gp[n]);
        fflush(stderr);
        n++;
    }
}

static int gp_b9_enabled(void)
{
    if (!b9.inited) {
        const char *e = getenv("RECOMP_APU_GP_B9_TRACE");
        b9.inited = 1;
        b9.on = (e && *e) ? 1 : 0;
        b9.first_bad = 0xFFFFFFFFu;
        if (b9.on) {
            const char *p = getenv("RECOMP_APU_GP_B9_TRACE_FILE");
            b9.fp = fopen((p && *p) ? p : "gpb9_trace.txt", "wb");
            if (!b9.fp) {
                b9_fail("cannot open trace artifact");
            } else {
                fprintf(b9.fp, "# A4b2-NR-followup P 00B9 effective-address trace\n");
                fprintf(b9.fp, "# pc=00B9 every execution; uncapped; ordinal is contiguous\n");
                fprintf(b9.fp, "# fields: ord epoch r1 x0000 derived effaddr value mixbuf alias\n");
                fflush(b9.fp);
                fprintf(stderr, "[GPB9] trace enabled -> %s\n",
                        (p && *p) ? p : "gpb9_trace.txt");
                fflush(stderr);
            }
        }
    }
    return b9.on;
}

/* Called at the instruction-entry choke point, before execution. */
void dsp56k_b9_note_exec(dsp_core_t *dsp)
{
    if (!gp_b9_enabled() || !dsp) {
        return;
    }
    /* Histogram every core, tagged by is_gp, so the trace can show which core is
     * running which PC rather than assuming the GP is the one at 00B9. */
    if (!b9.is_gp_seen) {
        b9.is_gp_seen = 1;
        b9.first_pc_seen = dsp->pc;
        fprintf(stderr, "[GPB9] first instruction: is_gp=%d pc=%04X\n",
                b9_core_is_gp(dsp), dsp->pc);
        fflush(stderr);
    }
    b9_note_core_identity(dsp);
    b9.exec_total++;
    if (dsp->pc < DSP_PRAM_SIZE) {
        b9.pc_hist[dsp->pc]++;
    }
    if (b9_core_is_gp(dsp)) {
        b9.gp_exec_total++;
        if (dsp->pc < DSP_PRAM_SIZE) {
            b9.gp_pc_hist[dsp->pc]++;
        }
        if (!b9.gp_pc_seen) {
            b9.gp_pc_seen = 1;
            b9.gp_pc_min = b9.gp_pc_max = dsp->pc;
        } else {
            if (dsp->pc < b9.gp_pc_min) b9.gp_pc_min = dsp->pc;
            if (dsp->pc > b9.gp_pc_max) b9.gp_pc_max = dsp->pc;
        }
        if (dsp->pc >= 0x200u && b9.gp_pc_first_high == 0) {
            b9.gp_pc_first_high = dsp->pc ? dsp->pc : 1u;
        }
        if (dsp->pc == GP_B9_PC) {
            b9.execs++;
        }
    } else {
        b9.ngp_exec_total++;
        if (!b9.ngp_pc_seen) {
            b9.ngp_pc_seen = 1;
            b9.ngp_pc_min = b9.ngp_pc_max = dsp->pc;
        } else {
            if (dsp->pc < b9.ngp_pc_min) b9.ngp_pc_min = dsp->pc;
            if (dsp->pc > b9.ngp_pc_max) b9.ngp_pc_max = dsp->pc;
        }
    }
}

/* Called at the read, after the address is computed and the value is known. */
void dsp56k_b9_note_read(dsp_core_t *dsp, uint32_t address, uint32_t value)
{
    uint32_t r1, mb0;
    int in_mix, in_alias;

    if (!gp_b9_enabled() || !dsp || !b9_core_is_gp(dsp)) {
        return;
    }
    /* Only reads attributed to the executing P 00B9 are events. `pc` still
     * holds the executing instruction at this point, which is what makes the
     * attribution exact rather than heuristic. */
    if (dsp->pc != GP_B9_PC) {
        return;
    }

    r1  = dsp->registers[DSP_REG_R1] & 0xFFFFFFu;
    mb0 = dsp->xram[0] & 0xFFFFFFu;   /* x:$0000, the guest mailbox word */
    in_mix   = (address >= GP_MIXBUF_LO   && address <= GP_MIXBUF_HI);
    in_alias = (address >= GP_MIXALIAS_LO && address <= GP_MIXALIAS_HI);

    if (in_mix)   b9.in_mixbuf++;
    if (in_alias) b9.in_alias++;
    if ((in_mix || in_alias) && b9.first_bad == 0xFFFFFFFFu) {
        b9.first_bad = address;
        b9.first_bad_ord = b9.ord_seq;
    }

    /* The packet requires the recorded address to be the effective address and
     * to agree with r1; a disagreement means the trace is not measuring what it
     * claims, so it is an evidence failure rather than a silent discrepancy. */
    if (address != r1) {
        b9_fail("effective address != r1 at P 00B9");
    }

    if (!b9.fp || b9.invalid) {
        return;
    }
    if (fprintf(b9.fp, "%llu %llu %06X %06X %06X %06X %06X %d %d\n",
                (unsigned long long)b9.ord_seq, (unsigned long long)b9.epoch,
                r1, mb0, (0x80u + mb0) & 0xFFFFFFu, address, value & 0xFFFFFFu,
                in_mix, in_alias) < 0) {
        b9_fail("write failed");
        return;
    }
    b9.ord_seq++;
    b9.events++;
}

/* Called at the first-exchange latch and at run end. Emits the terminal record
 * with both counters so completeness is checkable. */
void dsp56k_b9_terminal(const char *why)
{
    if (!gp_b9_enabled() || !b9.fp) {
        return;
    }
    if (fflush(b9.fp) != 0) {
        b9_fail("flush failed at terminal");
    }
    fprintf(stderr,
            "[GPB9] terminal reason=%s events=%llu execs=%llu in_mixbuf=%llu "
            "in_alias=%llu first_bad=%s invalid=%d\n",
            why, (unsigned long long)b9.events, (unsigned long long)b9.execs,
            (unsigned long long)b9.in_mixbuf, (unsigned long long)b9.in_alias,
            (b9.first_bad == 0xFFFFFFFFu) ? "none" : "PRESENT",
            b9.invalid);
    fflush(stderr);
    fprintf(b9.fp, "# terminal reason=%s events=%llu execs=%llu in_mixbuf=%llu "
                   "in_alias=%llu invalid=%d\n",
            why, (unsigned long long)b9.events, (unsigned long long)b9.execs,
            (unsigned long long)b9.in_mixbuf, (unsigned long long)b9.in_alias,
            b9.invalid);
    if (b9.first_bad != 0xFFFFFFFFu) {
        fprintf(b9.fp, "# FIRST_IN_RANGE addr=%06X ord=%llu\n", b9.first_bad,
                (unsigned long long)b9.first_bad_ord);
    }
    /* Dump the PC histogram on every terminal, so the record shows the running
     * state rather than a single early sample. The output is bounded (at most
     * 0x200 lines per terminal) and the histogram is a fixed key universe, so
     * nothing is dropped. Dumping once was wrong: the first terminal arrives at
     * an early counts emission, before the GP has executed anything. */
    {
        uint32_t pc;
        fprintf(b9.fp, "# exec_total=%llu gp_exec_total=%llu ngp_exec_total=%llu "
                       "first_pc=%04X is_gp_seen=%d gp_pc_range=%04X..%04X "
                       "ngp_pc_range=%04X..%04X gp_first_high=%04X\n",
                (unsigned long long)b9.exec_total,
                (unsigned long long)b9.gp_exec_total,
                (unsigned long long)b9.ngp_exec_total, b9.first_pc_seen,
                b9.is_gp_seen, b9.gp_pc_min, b9.gp_pc_max,
                b9.ngp_pc_min, b9.ngp_pc_max, b9.gp_pc_first_high);
        fprintf(b9.fp, "# pc_hist (pc count) for PCs with nonzero count:\n");
        for (pc = 0; pc < DSP_PRAM_SIZE; pc++) {
            if (b9.pc_hist[pc]) {
                fprintf(b9.fp, "#H %04X %llu\n", pc,
                        (unsigned long long)b9.pc_hist[pc]);
            }
        }
        fprintf(b9.fp, "# gp_pc_hist (pc count) for GP PCs with nonzero count:\n");
        for (pc = 0; pc < DSP_PRAM_SIZE; pc++) {
            if (b9.gp_pc_hist[pc]) {
                fprintf(b9.fp, "#G %04X %llu\n", pc,
                        (unsigned long long)b9.gp_pc_hist[pc]);
            }
        }
        fflush(b9.fp);
    }
    fflush(b9.fp);
}

/* Called when a GP bootstrap begins, so every epoch is distinguishable and no
 * earlier epoch can be silently omitted. */
void dsp56k_b9_epoch_begin(void)
{
    if (!gp_b9_enabled()) {
        return;
    }
    b9.epoch++;
    if (b9.fp) {
        fprintf(b9.fp, "# epoch %llu begin\n", (unsigned long long)b9.epoch);
        fflush(b9.fp);
    }
}

/* A4b2-NR: request a one-shot decode of the loaded program image. Called from
 * the in-scope gp_ep.c immediately after the bootstrap that loaded it. Takes no
 * argument because dsp_core_t is opaque outside this file; the pending flag is
 * serviced by the next executed instruction, which has the core in hand. */
void dsp56k_request_decode(void)
{
    if (gp_decode_enabled()) {
        gp_decode_pending = 1;
    }
}

/* A4b2-NR-next-edge: request the second, at-exchange decode. Same gate. */
void dsp56k_request_decode2(void)
{
    if (gp_decode_enabled()) {
        gp_decode2_pending = 1;
    }
}

void dsp56k_decode_p_range(dsp_core_t *dsp, uint32_t pc_lo, uint32_t pc_hi)
{
    uint32_t pc;
    uint32_t saved_pc;

    if (!dsp || !gp_decode_enabled() || pc_lo > pc_hi) {
        return;
    }
    saved_pc = dsp->pc;
    fprintf(stderr, "[GPDECODE] begin P[%04X..%04X]\n", pc_lo, pc_hi);
    for (pc = pc_lo; pc <= pc_hi; ) {
        uint32_t inst = dsp56k_read_memory(dsp, DSP_SPACE_P, pc);
        uint16_t len;

        dsp->pc = pc;
        dsp->disasm_prev_inst_pc = 0xFFFFFFFFu;  /* defeat the loop suppression */
        dsp->disasm_is_looping = false;
        dsp->disasm_str_instr[0] = 0;
        dsp->disasm_parallelmove_name[0] = 0;
        /* Guard the decode. lookup_opcode_slow() ends in
         * `assert(!"Invalid op code in dsp_cpu"); return NULL;` -- and in a
         * Release build assert() is a no-op, so an unrecognised word makes
         * lookup_opcode() return NULL. disasm_instruction() then dereferences
         * it and the run dies with an access violation.
         *
         * So test the pointer, not a field of it. The packet requires
         * undecodable words to be reported as gaps, and a decoder that faults on
         * the very image under study would destroy the evidence it exists to
         * produce. */
        if (inst < 0x100000u && lookup_opcode(inst) == NULL) {
            fprintf(stderr, "[GPDECODE] P %04X %06X  <UNDECODED>\n", pc,
                    inst & 0xFFFFFFu);
            pc += 1;
            continue;
        }
        len = disasm_instruction(dsp, DSP_DISASM_MODE);
        if (len == 0) {
            len = 1;
        }
        if (dsp->disasm_str_instr[0]) {
            fprintf(stderr, "[GPDECODE] P %04X %06X  %s\n", pc, inst & 0xFFFFFFu,
                    dsp->disasm_str_instr);
        } else {
            /* A gap, stated as a gap. Never a fabricated NOP. */
            fprintf(stderr, "[GPDECODE] P %04X %06X  <UNDECODED>\n", pc,
                    inst & 0xFFFFFFu);
        }
        pc += len;
    }
    fprintf(stderr, "[GPDECODE] end\n");
    fflush(stderr);
    dsp->pc = saved_pc;
}

static uint16_t disasm_instruction(dsp_core_t* dsp, dsp_trace_disasm_t mode)
{
    dsp->disasm_mode = mode;
    if (mode == DSP_TRACE_MODE) {
        if (dsp->disasm_prev_inst_pc == dsp->pc) {
            if (!dsp->disasm_is_looping) {
                DPRINTF("Looping on DSP instruction at PC = $%04x\n", dsp->disasm_prev_inst_pc);
                dsp->disasm_is_looping = true;
            }
            return 0;
        }
    }

    dsp->disasm_prev_inst_pc = dsp->pc;
    dsp->disasm_is_looping = false;

    dsp->disasm_cur_inst = dsp56k_read_memory(dsp, DSP_SPACE_P, dsp->pc);
    dsp->disasm_cur_inst_len = 1;

    dsp->disasm_parallelmove_name[0] = 0;

    if (dsp->disasm_cur_inst < 0x100000) {
        const OpcodeEntry *op = lookup_opcode(dsp->disasm_cur_inst);
        if (op->template) {
            if (op->dis_func) {
                op->dis_func(dsp);
            } else {
                sprintf(dsp->disasm_str_instr, "%s", op->name);
            }
        } else {
            dis_undefined(dsp);
        }
    } else {
        dis_pm(dsp);
        sprintf(dsp->disasm_str_instr, "%s %s",
            disasm_opcodes_alu[dsp->disasm_cur_inst & BITMASK(8)], dsp->disasm_parallelmove_name);
    }
    return dsp->disasm_cur_inst_len;
}

static void disasm_reg_save(dsp_core_t* dsp)
{
    memcpy(dsp->disasm_registers_save, dsp->registers , sizeof(dsp->disasm_registers_save));
#ifdef DSP_DISASM_REG_PC
    dsp->pc_save = dsp->pc;
#endif
}

static void disasm_reg_compare(dsp_core_t* dsp)
{
    int i;
    bool bRegA = false;
    bool bRegB = false;

    for (i=4; i<64; i++) {
        if (dsp->disasm_registers_save[i] == dsp->registers[i]) {
            continue;
        }

        switch(i) {
            case DSP_REG_X0:
            case DSP_REG_X1:
            case DSP_REG_Y0:
            case DSP_REG_Y1:
                DPRINTF("\tReg: %s  $%06x -> $%06x\n",
                    registers_name[i], dsp->disasm_registers_save[i], dsp->registers[i]);
                break;
            case DSP_REG_R0:
            case DSP_REG_R1:
            case DSP_REG_R2:
            case DSP_REG_R3:
            case DSP_REG_R4:
            case DSP_REG_R5:
            case DSP_REG_R6:
            case DSP_REG_R7:
            case DSP_REG_M0:
            case DSP_REG_M1:
            case DSP_REG_M2:
            case DSP_REG_M3:
            case DSP_REG_M4:
            case DSP_REG_M5:
            case DSP_REG_M6:
            case DSP_REG_M7:
            case DSP_REG_N0:
            case DSP_REG_N1:
            case DSP_REG_N2:
            case DSP_REG_N3:
            case DSP_REG_N4:
            case DSP_REG_N5:
            case DSP_REG_N6:
            case DSP_REG_N7:
            case DSP_REG_SR:
            case DSP_REG_LA:
            case DSP_REG_LC:
                DPRINTF("\tReg: %s  $%04x -> $%04x\n",
                    registers_name[i], dsp->disasm_registers_save[i], dsp->registers[i]);
                break;
            case DSP_REG_OMR:
            case DSP_REG_SP:
            case DSP_REG_SSH:
            case DSP_REG_SSL:
                DPRINTF("\tReg: %s  $%02x -> $%02x\n",
                    registers_name[i], dsp->disasm_registers_save[i], dsp->registers[i]);
                break;
            case DSP_REG_A0:
            case DSP_REG_A1:
            case DSP_REG_A2:
                if (bRegA == false) {
                    DPRINTF("\tReg: a   $%02x:%06x:%06x -> $%02x:%06x:%06x\n",
                        dsp->disasm_registers_save[DSP_REG_A2], dsp->disasm_registers_save[DSP_REG_A1], dsp->disasm_registers_save[DSP_REG_A0],
                        dsp->registers[DSP_REG_A2], dsp->registers[DSP_REG_A1], dsp->registers[DSP_REG_A0]
                    );
                    bRegA = true;
                }
                break;
            case DSP_REG_B0:
            case DSP_REG_B1:
            case DSP_REG_B2:
                if (bRegB == false) {
                    DPRINTF("\tReg: b   $%02x:%06x:%06x -> $%02x:%06x:%06x\n",
                        dsp->disasm_registers_save[DSP_REG_B2], dsp->disasm_registers_save[DSP_REG_B1], dsp->disasm_registers_save[DSP_REG_B0],
                        dsp->registers[DSP_REG_B2], dsp->registers[DSP_REG_B1], dsp->registers[DSP_REG_B0]
                    );
                    bRegB = true;
                }
                break;
        }
    }

#ifdef DSP_DISASM_REG_PC
    if (pc_save != dsp->pc) {
        DPRINTF("\tReg: pc  $%04x -> $%04x\n", pc_save, dsp->pc);
    }
#endif
}

static const char* disasm_get_instruction_text(dsp_core_t* dsp)
{
    if (dsp->disasm_is_looping) {
        dsp->disasm_str_instr2[0] = 0;
    }
    if (dsp->disasm_cur_inst_len == 1) {
        snprintf(dsp->disasm_str_instr2, sizeof(dsp->disasm_str_instr2), "p:%04x  %06x         (%02d cyc)  %s", dsp->disasm_prev_inst_pc, dsp->disasm_cur_inst, dsp->instr_cycle, dsp->disasm_str_instr);
    } else {
        snprintf(dsp->disasm_str_instr2, sizeof(dsp->disasm_str_instr2), "p:%04x  %06x %06x  (%02d cyc)  %s", dsp->disasm_prev_inst_pc, dsp->disasm_cur_inst, read_memory_p(dsp, dsp->disasm_prev_inst_pc + 1), dsp->instr_cycle, dsp->disasm_str_instr);
    }
    return dsp->disasm_str_instr2;
}

void dsp56k_execute_instruction(dsp_core_t* dsp)
{
    trace_dsp56k_execute_instruction(dsp->is_gp, dsp->pc);

    /* A4b2-NR: service a pending diagnostic decode request. Placed here because
     * this is the first point after the in-scope caller that has the core. It
     * runs before execution, so it decodes exactly the image the bootstrap
     * loaded. Cleared unconditionally so a failed decode cannot retry forever. */
    if (gp_decode_pending) {
        gp_decode_pending = 0;
        /* A4b2-NR-followup: decode the FULL P-memory, not just the 0x200-word
         * window the predecessor used. The B9 trace showed the GP executing PCs
         * up to 0x0F28, so a slice built over 0x0000-0x01FF would silently omit
         * most of the program -- exactly the kind of partial enumeration
         * AGENTS.md warns about. DSP_PRAM_SIZE is the true bound. */
        dsp56k_decode_p_range(dsp, 0, DSP_PRAM_SIZE - 1);
    }

    /* A4b2-NR-next-edge: a SECOND decode at the first exchange, tagged so the two
     * can be compared. The bootstrap-time decode is only a snapshot: if the GP
     * loads or patches P-memory afterwards, the snapshot does not describe what
     * the GP actually executed. Comparing the two is how that is established
     * rather than assumed. */
    if (gp_decode2_pending) {
        gp_decode2_pending = 0;
        fprintf(stderr, "[GPDECODE] second-decode-at-exchange begin\n");
        dsp56k_decode_p_range(dsp, 0, DSP_PRAM_SIZE - 1);
        fprintf(stderr, "[GPDECODE] second-decode-at-exchange end\n");
        fflush(stderr);
    }

    uint32_t disasm_return = 0;
    dsp->disasm_memory_ptr = 0;

    /* Decode and execute current instruction */
    dsp->cur_inst = read_memory_p(dsp, dsp->pc);

    /* A4b2-NR-followup: count this P 00B9 execution at the instruction-entry
     * choke point, BEFORE it executes. Independent of the read-event counter, so
     * an executed-but-unrecorded read shows up as a counter disagreement rather
     * than being read as "no mix read happened". */
    dsp56k_b9_note_exec(dsp);

    /* Initialize instruction size and cycle counter */
    dsp->cur_inst_len = 1;
    dsp->instr_cycle = 2;

    bool tracing = TRACE_DSP_DISASM || trace_event_get_state(TRACE_DSP56K_EXECUTE_INSTRUCTION_DISASM);

    /* Disasm current instruction ? (trace mode only) */
    if (tracing) {
        disasm_return = disasm_instruction(dsp, DSP_TRACE_MODE);
        if (disasm_return) {
            const char *text = disasm_get_instruction_text(dsp);
            trace_dsp56k_execute_instruction_disasm(text);
            if (TRACE_DSP_DISASM) {
                DPRINTF("%s\n", text);
            }
            if (TRACE_DSP_DISASM_REG) {
                disasm_reg_save(dsp);
            }
        }
    }

    if (dsp->cur_inst < 0x100000) {
        const OpcodeEntry *op = dsp->pram_opcache[dsp->pc];
        if (op == NULL) {
            op = lookup_opcode(dsp->cur_inst);
            dsp->pram_opcache[dsp->pc] = op;
        }
        if (op->emu_func) {
            op->emu_func(dsp);
        } else {
            DPRINTF("%x - %s\n", dsp->cur_inst, op->name);
            emu_undefined(dsp);
        }
    } else {
        /* Do parallel move read */
        opcodes_parmove[(dsp->cur_inst>>20) & BITMASK(4)](dsp);
    }

    /* Disasm current instruction ? (trace mode only) */
    if (tracing && disasm_return) {
        if (TRACE_DSP_DISASM_REG) {
            disasm_reg_compare(dsp);
        }
        if (TRACE_DSP_DISASM_MEM) {
            /* 1 memory change to display ? */
            if (dsp->disasm_memory_ptr == 1)
                DPRINTF("\t%s\n", dsp->str_disasm_memory[0]);
            /* 2 memory changes to display ? */
            else if (dsp->disasm_memory_ptr == 2) {
                DPRINTF("\t%s\n", dsp->str_disasm_memory[0]);
                DPRINTF("\t%s\n", dsp->str_disasm_memory[1]);
            }
        }
    }

    /* Process the PC */
    dsp_postexecute_update_pc(dsp);

    /* Process Interrupts */
    dsp_postexecute_interrupts(dsp);


    dsp->num_inst += dsp->instr_cycle;

#ifdef DSP_COUNT_IPS
    ++dsp->num_inst;
    if ((dsp->num_inst & 63) == 0) {
        /* Evaluate time after <N> instructions have been executed to avoid asking too frequently */
        uint32_t cur_time = SDL_GetTicks();
        if (cur_time-start_time>1000) {
            DPRINTF("Dsp: %d i/s\n", (dsp->num_inst*1000)/(cur_time-start_time));
            start_time=cur_time;
            dsp->num_inst=0;
        }
    }
#endif
}

/**********************************
 *  Update the PC
**********************************/

static void dsp_postexecute_update_pc(dsp_core_t* dsp)
{
    /* When running a REP, PC must stay on the current instruction */
    if (dsp->loop_rep) {
        /* Is PC on the instruction to repeat ? */
        if (dsp->pc_on_rep==0) {
            --dsp->registers[DSP_REG_LC];
            dsp->registers[DSP_REG_LC] &= BITMASK(16);

            if (dsp->registers[DSP_REG_LC] > 0) {
                dsp->cur_inst_len = 0;   /* Stay on this instruction */
            } else {
                dsp->loop_rep = 0;
                dsp->registers[DSP_REG_LC] = dsp->registers[DSP_REG_LCSAVE];
            }
        } else {
            /* Init LC at right value */
            if (dsp->registers[DSP_REG_LC] == 0) {
                dsp->registers[DSP_REG_LC] = 0x010000;
            }
            dsp->pc_on_rep = 0;
        }
    }

    /* Normal execution, go to next instruction */
    dsp->pc += dsp->cur_inst_len;

    /* When running a DO loop, we test the end of loop with the */
    /* updated PC, pointing to last instruction of the loop */
    if (dsp->registers[DSP_REG_SR] & (1<<DSP_SR_LF)) {

        /* Did we execute the last instruction in loop ? */
        if (dsp->pc == dsp->registers[DSP_REG_LA] + 1) {
            --dsp->registers[DSP_REG_LC];
            dsp->registers[DSP_REG_LC] &= BITMASK(16);

            if (dsp->registers[DSP_REG_LC] == 0) {
                /* end of loop */
                uint32_t saved_pc, saved_sr;

                dsp_stack_pop(dsp, &saved_pc, &saved_sr);
                dsp->registers[DSP_REG_SR] &= 0x7f;
                dsp->registers[DSP_REG_SR] |= saved_sr & (1<<DSP_SR_LF);
                dsp_stack_pop(dsp, &dsp->registers[DSP_REG_LA], &dsp->registers[DSP_REG_LC]);
            } else {
                /* Loop one more time */
                dsp->pc = dsp->registers[DSP_REG_SSH];
            }
        }
    }
}

/**********************************
 *  Interrupts
**********************************/

/* Post a new interrupt to the interrupt table */
void dsp56k_add_interrupt(dsp_core_t* dsp, uint16_t inter)
{
    /* detect if this interrupt is used or not */
    if (dsp->interrupt_ipl[inter] == -1)
        return;

    /* add this interrupt to the pending interrupts table */
    if (dsp->interrupt_is_pending[inter] == 0) {
        dsp->interrupt_is_pending[inter] = 1;
        dsp->interrupt_counter ++;
    }
}

static void dsp_postexecute_interrupts(dsp_core_t* dsp)
{
    uint32_t index, instr, i;
    int32_t ipl_to_raise, ipl_sr;

    /* REP is not interruptible */
    if (dsp->loop_rep) {
        return;
    }

    /* A fast interrupt can not be interrupted. */
    if (dsp->interrupt_state == DSP_INTERRUPT_DISABLED) {

        switch (dsp->interrupt_pipeline_count) {
            case 5:
                dsp->interrupt_pipeline_count --;
                return;
            case 4:
                /* Prefetch interrupt instruction 1 */
                dsp->interrupt_save_pc = dsp->pc;
                dsp->pc = dsp->interrupt_instr_fetch;

                /* is it a LONG interrupt ? */
                instr = read_memory_p(dsp, dsp->interrupt_instr_fetch);
                if ( ((instr & 0xfff000) == 0x0d0000) || ((instr & 0xffc0ff) == 0x0bc080) ) {
                    dsp->interrupt_state = DSP_INTERRUPT_LONG;
                    dsp_stack_push(dsp, dsp->interrupt_save_pc, dsp->registers[DSP_REG_SR], 0);
                    dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_LF)|(1<<DSP_SR_FV)  |
                                            (1<<DSP_SR_S1)|(1<<DSP_SR_S0) |
                                            (1<<DSP_SR_I0)|(1<<DSP_SR_I1));
                    dsp->registers[DSP_REG_SR] |= dsp->interrupt_ipl_to_raise<<DSP_SR_I0;
                }
                dsp->interrupt_pipeline_count --;
                return;
            case 3:
                /* Prefetch interrupt instruction 2 */
                if (dsp->pc == dsp->interrupt_instr_fetch+1) {
                    instr = read_memory_p(dsp, dsp->pc);
                    if ( ((instr & 0xfff000) == 0x0d0000) || ((instr & 0xffc0ff) == 0x0bc080) ) {
                        dsp->interrupt_state = DSP_INTERRUPT_LONG;
                        dsp_stack_push(dsp, dsp->interrupt_save_pc, dsp->registers[DSP_REG_SR], 0);
                        dsp->registers[DSP_REG_SR] &= BITMASK(16)-((1<<DSP_SR_LF)|(1<<DSP_SR_FV)  |
                                                (1<<DSP_SR_S1)|(1<<DSP_SR_S0) |
                                                (1<<DSP_SR_I0)|(1<<DSP_SR_I1));
                        dsp->registers[DSP_REG_SR] |= dsp->interrupt_ipl_to_raise<<DSP_SR_I0;
                    }
                }
                dsp->interrupt_pipeline_count --;
                return;
            case 2:
                /* 1 instruction executed after interrupt */
                /* before re enable interrupts */
                /* Was it a FAST interrupt ? */
                if (dsp->pc == dsp->interrupt_instr_fetch+2) {
                    dsp->pc = dsp->interrupt_save_pc;
                }
                dsp->interrupt_pipeline_count --;
                return;
            case 1:
                /* Last instruction executed after interrupt */
                /* before re enable interrupts */
                dsp->interrupt_pipeline_count --;
                return;
            case 0:
                /* Re enable interrupts */
                /* All 6 instruction are done, Interrupts can be enabled again */
                dsp->interrupt_save_pc = -1;
                dsp->interrupt_instr_fetch = -1;
                dsp->interrupt_state = DSP_INTERRUPT_NONE;
                break;
        }
    }

    /* No interrupt to execute */
    if (dsp->interrupt_counter == 0) {
        return;
    }

    /* search for an interrupt */
    ipl_sr = (dsp->registers[DSP_REG_SR]>>DSP_SR_I0) & BITMASK(2);
    index = 0xffff;
    ipl_to_raise = -1;

    /* Arbitrate between all pending interrupts */
    for (i=0; i<4; i++) {
        if (dsp->interrupt_is_pending[i] == 1) {

            /* level 3 interrupt ? */
            if (dsp->interrupt_ipl[i] == 3) {
                index = i;
                break;
            }

            /* level 0, 1 ,2 interrupt ? */
            /* if interrupt is masked in SR, don't process it */
            if (dsp->interrupt_ipl[i] < ipl_sr)
                continue;

            /* if interrupt is lower or equal than current arbitrated interrupt */
            if (dsp->interrupt_ipl[i] <= ipl_to_raise)
                continue;

            /* save current arbitrated interrupt */
            index = i;
            ipl_to_raise = dsp->interrupt_ipl[i];
        }
    }

    /* If there's no interrupt to process, return */
    if (index == 0xffff) {
        return;
    }

    /* remove this interrupt from the pending interrupts table */
    dsp->interrupt_is_pending[index] = 0;
    dsp->interrupt_counter --;

    /* process arbritrated interrupt */
    ipl_to_raise = dsp->interrupt_ipl[index] + 1;
    if (ipl_to_raise > 3) {
        ipl_to_raise = 3;
    }

    dsp->interrupt_instr_fetch = dsp_interrupt[index].vectorAddr;
    dsp->interrupt_pipeline_count = 5;
    dsp->interrupt_state = DSP_INTERRUPT_DISABLED;
    dsp->interrupt_ipl_to_raise = ipl_to_raise;

    DPRINTF("Dsp interrupt: %s\n", dsp_interrupt[index].name);
}

/**********************************
 *  Read/Write memory functions
 **********************************/

static uint32_t read_memory_p(dsp_core_t* dsp, uint32_t address)
{
    assert((address & 0xFF000000) == 0);
    assert(address < DSP_PRAM_SIZE);
    uint32_t r = ldl_le_p(&dsp->pram[address]);
    assert((r & 0xFF000000) == 0);
    return r;
}

uint32_t dsp56k_read_memory(dsp_core_t* dsp, int space, uint32_t address)
{
    assert((address & 0xFF000000) == 0);

    /* A4b2-NR-followup: record the effective X address of every P 00B9 read at
     * the read itself, so the mix-buffer question is answered by measurement
     * rather than by inferring r1. Strict no-op unless the gate is set; the
     * attribution uses dsp->pc, which still holds the executing instruction. */
    if (space == DSP_SPACE_X) {
        uint32_t v;
        if (address >= DSP_PERIPH_BASE) {
            v = dsp->read_peripheral(dsp, address);
            dsp56k_b9_note_read(dsp, address, v);
            return v;
        } else if (address >= DSP_MIXBUFFER_BASE && address < DSP_MIXBUFFER_BASE+DSP_MIXBUFFER_SIZE) {
            v = apu_watch_perturb_mixbuf(dsp->mixbuffer[address-DSP_MIXBUFFER_BASE]);
            apu_gpin_mixbuf_read(address - DSP_MIXBUFFER_BASE, v);
            dsp56k_b9_note_read(dsp, address, v);
            return v;
        } else if (address >= 0xc00 && address < 0xc00+DSP_MIXBUFFER_SIZE) {
            v = apu_watch_perturb_mixbuf(dsp->mixbuffer[address-0xc00]);
            apu_gpin_mixbuf_read(address - 0xc00, v);
            dsp56k_b9_note_read(dsp, address, v);
            return v;
        } else if (address < DSP_XRAM_SIZE) {
            v = dsp->xram[address];
            dsp56k_b9_note_read(dsp, address, v);
            return v;
        } else {
            fprintf(stderr, "Out of bounds read at %x!\n", address);
            v = 0x00FFFFFF; // FIXME: What does the DSP actually do in this case?
            dsp56k_b9_note_read(dsp, address, v);
            return v;
        }
    } else if (space == DSP_SPACE_Y) {
        assert(address < DSP_YRAM_SIZE);
        return dsp->yram[address];
    } else if (space == DSP_SPACE_P) {
        return read_memory_p(dsp, address);
    } else {
        assert(!"Invalid dsp space in read memory");
        return 0;
    }
}

void dsp56k_write_memory(dsp_core_t* dsp, int space, uint32_t address, uint32_t value)
{
    if (TRACE_DSP_DISASM_MEM)
        write_memory_disasm(dsp, space, address, value);
    else
        write_memory_raw(dsp, space, address, value);
}

static void write_memory_raw(dsp_core_t* dsp, int space, uint32_t address, uint32_t value)
{
    assert((value & 0xFF000000) == 0);
    assert((address & 0xFF000000) == 0);

    if (space == DSP_SPACE_X) {
        if (address >= DSP_PERIPH_BASE) {
            assert(dsp->write_peripheral);
            dsp->write_peripheral(dsp, address, value);
            return;
        } else if (address >= DSP_MIXBUFFER_BASE && address < DSP_MIXBUFFER_BASE+DSP_MIXBUFFER_SIZE) {
            dsp->mixbuffer[address-DSP_MIXBUFFER_BASE] = value;
        } else if (address >= 0xc00 && address < 0xc00+DSP_MIXBUFFER_SIZE) {
            dsp->mixbuffer[address-0xc00] = value;
        } else {
            assert(address < DSP_XRAM_SIZE);
            dsp->xram[address] = value;
        }
    } else if (space == DSP_SPACE_Y) {
        assert(address < DSP_YRAM_SIZE);
        dsp->yram[address] = value;
    } else if (space == DSP_SPACE_P) {
        assert(address < DSP_PRAM_SIZE);
        dsp56k_pwrite_watch(dsp, address, value);
        stl_le_p(&dsp->pram[address], value);
        dsp->pram_opcache[address] = NULL;
    } else {
        assert(!"Invalid dsp space in write raw memory");
    }
}

static uint32_t read_memory_disasm(dsp_core_t* dsp, int space, uint32_t address)
{
    return dsp56k_read_memory(dsp, space, address);
}

static void write_memory_disasm(dsp_core_t* dsp, int space, uint32_t address, uint32_t value)
{
    uint32_t oldvalue, curvalue;
    char space_c;

    oldvalue = read_memory_disasm(dsp, space, address);

    write_memory_raw(dsp, space, address, value);

    switch(space) {
        case DSP_SPACE_X:
            space_c = 'x';
            break;
        case DSP_SPACE_Y:
            space_c = 'y';
            break;
        case DSP_SPACE_P:
            space_c = 'p';
            break;
        default:
            assert(!"Invalid dsp space in write memory disasm");
    }

    curvalue = read_memory_disasm(dsp, space, address);
    if (dsp->disasm_memory_ptr < ARRAY_SIZE(dsp->str_disasm_memory)) {
        sprintf(dsp->str_disasm_memory[dsp->disasm_memory_ptr], "Mem: %c:0x%04x  0x%06x -> 0x%06x", space_c, address, oldvalue, curvalue);
        dsp->disasm_memory_ptr ++;
    }
}

static void dsp_write_reg(dsp_core_t* dsp, uint32_t numreg, uint32_t value)
{
    uint32_t stack_error;

    switch (numreg) {
        case DSP_REG_A:
            dsp->registers[DSP_REG_A0] = 0;
            dsp->registers[DSP_REG_A1] = value;
            dsp->registers[DSP_REG_A2] = value & (1<<23) ? 0xff : 0x0;
            break;
        case DSP_REG_B:
            dsp->registers[DSP_REG_B0] = 0;
            dsp->registers[DSP_REG_B1] = value;
            dsp->registers[DSP_REG_B2] = value & (1<<23) ? 0xff : 0x0;
            break;
        case DSP_REG_OMR:
            dsp->registers[DSP_REG_OMR] = value & 0xc7;
            break;
        case DSP_REG_SR:
            dsp->registers[DSP_REG_SR] = value & 0xaf7f;
            break;
        case DSP_REG_SP:
            stack_error = dsp->registers[DSP_REG_SP] & (3<<DSP_SP_SE);
            if ((stack_error==0) && (value & (3<<DSP_SP_SE))) {
                /* Stack underflow or overflow detected, raise interrupt */
                dsp56k_add_interrupt(dsp, DSP_INTER_STACK_ERROR);
                dsp->registers[DSP_REG_SP] = value & (3<<DSP_SP_SE);
                DPRINTF("Dsp: Stack Overflow or Underflow\n");
                if (dsp->exception_debugging) {
                    assert(!"Dsp stack overflow or underflow detected");
                }
            } else {
                dsp->registers[DSP_REG_SP] = value & BITMASK(6);
            }
            dsp_compute_ssh_ssl(dsp);
            break;
        case DSP_REG_SSH:
            dsp_stack_push(dsp, value, 0, 1);
            break;
        case DSP_REG_SSL:
            numreg = dsp->registers[DSP_REG_SP] & BITMASK(4);
            if (numreg == 0) {
                value = 0;
            }
            dsp->stack[1][numreg] = value & BITMASK(16);
            dsp->registers[DSP_REG_SSL] = value & BITMASK(16);
            break;
        default:
            dsp->registers[numreg] = value;
            dsp->registers[numreg] &= BITMASK(registers_mask[numreg]);
            break;
    }
}

/**********************************
 *  Stack push/pop
 **********************************/

static void dsp_stack_push(dsp_core_t* dsp, uint32_t curpc, uint32_t cursr, uint16_t sshOnly)
{
    uint32_t stack_error, underflow, stack;

    stack_error = dsp->registers[DSP_REG_SP] & (1<<DSP_SP_SE);
    underflow = dsp->registers[DSP_REG_SP] & (1<<DSP_SP_UF);
    stack = (dsp->registers[DSP_REG_SP] & BITMASK(4)) + 1;


    if ((stack_error==0) && (stack & (1<<DSP_SP_SE))) {
        /* Stack full, raise interrupt */
        dsp56k_add_interrupt(dsp, DSP_INTER_STACK_ERROR);
        DPRINTF("Dsp: Stack Overflow\n");
        if (dsp->exception_debugging)
            assert(!"dsp stack overflow");
    }

    dsp->registers[DSP_REG_SP] = (underflow | stack_error | stack) & BITMASK(6);
    stack &= BITMASK(4);

    if (stack) {
        /* SSH part */
        dsp->stack[0][stack] = curpc & BITMASK(16);
        /* SSL part, if instruction is not like "MOVEC xx, SSH"  */
        if (sshOnly == 0) {
            dsp->stack[1][stack] = cursr & BITMASK(16);
        }
    } else {
        dsp->stack[0][0] = 0;
        dsp->stack[1][0] = 0;
    }

    /* Update SSH and SSL registers */
    dsp->registers[DSP_REG_SSH] = dsp->stack[0][stack];
    dsp->registers[DSP_REG_SSL] = dsp->stack[1][stack];
}

static void dsp_stack_pop(dsp_core_t* dsp, uint32_t *newpc, uint32_t *newsr)
{
    uint32_t stack_error, underflow, stack;

    stack_error = dsp->registers[DSP_REG_SP] & (1<<DSP_SP_SE);
    underflow = dsp->registers[DSP_REG_SP] & (1<<DSP_SP_UF);
    stack = (dsp->registers[DSP_REG_SP] & BITMASK(4)) - 1;

    if ((stack_error==0) && (stack & (1<<DSP_SP_SE))) {
        /* Stack empty*/
        dsp56k_add_interrupt(dsp, DSP_INTER_STACK_ERROR);
        DPRINTF("Dsp: Stack underflow\n");
        if (dsp->exception_debugging)
            assert(!"Dsp stack underflow");
    }

    dsp->registers[DSP_REG_SP] = (underflow | stack_error | stack) & BITMASK(6);
    stack &= BITMASK(4);
    *newpc = dsp->registers[DSP_REG_SSH];
    *newsr = dsp->registers[DSP_REG_SSL];

    dsp->registers[DSP_REG_SSH] = dsp->stack[0][stack];
    dsp->registers[DSP_REG_SSL] = dsp->stack[1][stack];
}

static void dsp_compute_ssh_ssl(dsp_core_t* dsp)
{
    uint32_t stack;

    stack = dsp->registers[DSP_REG_SP];
    stack &= BITMASK(4);
    dsp->registers[DSP_REG_SSH] = dsp->stack[0][stack];
    dsp->registers[DSP_REG_SSL] = dsp->stack[1][stack];
}



/**********************************
 *  56bit arithmetic
 **********************************/

/* source,dest[0] is 55:48 */
/* source,dest[1] is 47:24 */
/* source,dest[2] is 23:00 */

static uint16_t dsp_abs56(uint32_t *dest)
{
    uint32_t zerodest[3];
    uint16_t newsr;

    /* D=|D| */

    if (dest[0] & (1<<7)) {
        zerodest[0] = zerodest[1] = zerodest[2] = 0;

        newsr = dsp_sub56(dest, zerodest);

        dest[0] = zerodest[0];
        dest[1] = zerodest[1];
        dest[2] = zerodest[2];
    } else {
        newsr = 0;
    }

    return newsr;
}

static uint16_t dsp_asl56(uint32_t *dest, int n)
{
    /* Shift left dest n bits: D<<=n */

    uint64_t dest_v = dest[2] | ((uint64_t)dest[1] << 24) | ((uint64_t)dest[0] << 48);

    uint32_t carry = (dest_v >> (56-n)) & 1;

    uint64_t dest_s = dest_v << n;
    dest[2] = dest_s & BITMASK(24);
    dest[1] = (dest_s >> 24) & BITMASK(24);
    dest[0] = (dest_s >> 48) & BITMASK(8);

    uint32_t overflow = (dest_v >> (56-n)) != 0;
    uint32_t v = ((dest_v >> 55) & 1) != ((dest_s >> 55) & 1);

    return (overflow<<DSP_SR_L)|(v<<DSP_SR_V)|(carry<<DSP_SR_C);
}

static uint16_t dsp_asr56(uint32_t *dest, int n)
{
    /* Shift right dest n bits: D>>=n */

    uint64_t dest_v = dest[2] | ((uint64_t)dest[1] << 24) | ((uint64_t)dest[0] << 48);

    uint16_t carry = (dest_v >> (n-1)) & 1;

    dest_v >>= n;
    dest[2] = dest_v & BITMASK(24);
    dest[1] = (dest_v >> 24) & BITMASK(24);
    dest[0] = (dest_v >> 48) & BITMASK(8);

    return (carry<<DSP_SR_C);
}

static uint16_t dsp_add56(uint32_t *source, uint32_t *dest)
{
    uint16_t overflow, carry, flg_s, flg_d, flg_r;

    flg_s = (source[0]>>7) & 1;
    flg_d = (dest[0]>>7) & 1;

    /* Add source to dest: D = D+S */
    dest[2] += source[2];
    dest[1] += source[1]+((dest[2]>>24) & 1);
    dest[0] += source[0]+((dest[1]>>24) & 1);

    carry = (dest[0]>>8) & 1;

    dest[2] &= BITMASK(24);
    dest[1] &= BITMASK(24);
    dest[0] &= BITMASK(8);

    flg_r = (dest[0]>>7) & 1;

    /*set overflow*/
    overflow = (flg_s ^ flg_r) & (flg_d ^ flg_r);

    return (overflow<<DSP_SR_L)|(overflow<<DSP_SR_V)|(carry<<DSP_SR_C);
}

static uint16_t dsp_sub56(uint32_t *source, uint32_t *dest)
{
    uint16_t overflow, carry, flg_s, flg_d, flg_r, dest_save;

    dest_save = dest[0];

    /* Subtract source from dest: D = D-S */
    dest[2] -= source[2];
    dest[1] -= source[1]+((dest[2]>>24) & 1);
    dest[0] -= source[0]+((dest[1]>>24) & 1);

    carry = (dest[0]>>8) & 1;

    dest[2] &= BITMASK(24);
    dest[1] &= BITMASK(24);
    dest[0] &= BITMASK(8);

    flg_s = (source[0]>>7) & 1;
    flg_d = (dest_save>>7) & 1;
    flg_r = (dest[0]>>7) & 1;

    /* set overflow */
    overflow = (flg_s ^ flg_d) & (flg_r ^ flg_d);

    return (overflow<<DSP_SR_L)|(overflow<<DSP_SR_V)|(carry<<DSP_SR_C);
}

static void dsp_mul56(uint32_t source1, uint32_t source2, uint32_t *dest, uint8_t signe)
{
    uint32_t part[4], zerodest[3], value;

    /* Multiply: D = S1*S2 */
    if (source1 & (1<<23)) {
        signe ^= 1;
        source1 = (1<<24) - source1;
    }
    if (source2 & (1<<23)) {
        signe ^= 1;
        source2 = (1<<24) - source2;
    }

    /* bits 0-11 * bits 0-11 */
    part[0]=(source1 & BITMASK(12))*(source2 & BITMASK(12));
    /* bits 12-23 * bits 0-11 */
    part[1]=((source1>>12) & BITMASK(12))*(source2 & BITMASK(12));
    /* bits 0-11 * bits 12-23 */
    part[2]=(source1 & BITMASK(12))*((source2>>12)  & BITMASK(12));
    /* bits 12-23 * bits 12-23 */
    part[3]=((source1>>12) & BITMASK(12))*((source2>>12) & BITMASK(12));

    /* Calc dest 2 */
    dest[2] = part[0];
    dest[2] += (part[1] & BITMASK(12)) << 12;
    dest[2] += (part[2] & BITMASK(12)) << 12;

    /* Calc dest 1 */
    dest[1] = (part[1]>>12) & BITMASK(12);
    dest[1] += (part[2]>>12) & BITMASK(12);
    dest[1] += part[3];

    /* Calc dest 0 */
    dest[0] = 0;

    /* Add carries */
    value = (dest[2]>>24) & BITMASK(8);
    if (value) {
        dest[1] += value;
        dest[2] &= BITMASK(24);
    }
    value = (dest[1]>>24) & BITMASK(8);
    if (value) {
        dest[0] += value;
        dest[1] &= BITMASK(24);
    }

    /* Get rid of extra sign bit */
    dsp_asl56(dest, 1);

    if (signe) {
        zerodest[0] = zerodest[1] = zerodest[2] = 0;

        dsp_sub56(dest, zerodest);

        dest[0] = zerodest[0];
        dest[1] = zerodest[1];
        dest[2] = zerodest[2];
    }
}

static void dsp_rnd56(dsp_core_t* dsp, uint32_t *dest)
{
    uint32_t rnd_const[3];

    rnd_const[0] = 0;

    /* Scaling mode S0 */
    if (dsp->registers[DSP_REG_SR] & (1<<DSP_SR_S0)) {
        rnd_const[1] = 1;
        rnd_const[2] = 0;
        dsp_add56(rnd_const, dest);

        if ((dest[2]==0) && ((dest[1] & 1) == 0)) {
            dest[1] &= (0xffffff - 0x3);
        }
        dest[1] &= 0xfffffe;
        dest[2]=0;
    }
    /* Scaling mode S1 */
    else if (dsp->registers[DSP_REG_SR] & (1<<DSP_SR_S1)) {
        rnd_const[1] = 0;
        rnd_const[2] = (1<<22);
        dsp_add56(rnd_const, dest);

        if ((dest[2] & 0x7fffff) == 0){
            dest[2] = 0;
        }
        dest[2] &= 0x800000;
    }
    /* No Scaling */
    else {
        rnd_const[1] = 0;
        rnd_const[2] = (1<<23);
        dsp_add56(rnd_const, dest);

        if (dest[2] == 0) {
            dest[1] &= 0xfffffe;
        }
        dest[2]=0;
    }
}

static uint32_t dsp_signextend(int bits, uint32_t v) {
    const int shift = sizeof(int)*8 - bits;
    assert(shift > 0);
    return (uint32_t)(((int32_t)v << shift) >> shift);
}
