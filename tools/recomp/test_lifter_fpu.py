import unittest
from pathlib import Path

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter
from .translator import FunctionTranslator


class FpuLifterTest(unittest.TestCase):
    def test_stack_register_load_duplicates_the_old_top(self):
        instruction = Instruction(0, 2, "fld", "st(0)", "d9c0")
        instruction.operands = [Operand(type="reg", reg="st(0)")]

        self.assertEqual(
            Lifter().lift_instruction(instruction),
            ["{ double _t = fp_top(); fp_push(_t); } /* fld st(0) */"],
        )

    def test_fst_does_not_pop_and_fstp_does(self):
        operand = Operand(type="mem", mem_base="esp", mem_disp=0x18,
                          mem_size=4)
        store = Instruction(0, 4, "fst", "dword ptr [esp + 0x18]", "")
        store.operands = [operand]
        store_pop = Instruction(
            0, 4, "fstp", "dword ptr [esp + 0x18]", "")
        store_pop.operands = [operand]

        self.assertEqual(
            Lifter().lift_instruction(store),
            ["MEMF(esp + 0x18) = (float)fp_top(); /* fst */"],
        )
        self.assertEqual(
            Lifter().lift_instruction(store_pop),
            ["MEMF(esp + 0x18) = (float)fp_top(); fp_pop(); /* fstp */"],
        )

    def test_qword_integer_conversion_uses_signed_64_bit_storage(self):
        operand = Operand(type="mem", mem_base="esp", mem_disp=0x10,
                          mem_size=8)
        store = Instruction(
            0, 4, "fistp", "qword ptr [esp + 0x10]", "")
        store.operands = [operand]
        load = Instruction(0, 4, "fild", "qword ptr [esp + 0x10]", "")
        load.operands = [operand]

        self.assertEqual(
            Lifter().lift_instruction(store),
            ["SMEM64(esp + 0x10) = (int64_t)recomp_fist(fp_top(), g_fp_control_word, 64); "
             "fp_pop(); /* fistp */"],
        )
        self.assertEqual(
            Lifter().lift_instruction(load),
            ["fp_push((double)SMEM64(esp + 0x10)); /* fild */"],
        )

    def test_memory_arithmetic_updates_st0_without_popping(self):
        instruction = Instruction(
            0, 6, "fdiv", "dword ptr [0x00123456]", "")
        instruction.operands = [
            Operand(type="mem", mem_disp=0x00123456, mem_size=4),
        ]

        self.assertEqual(
            Lifter().lift_instruction(instruction),
            ["fp_top() = fp_top() / MEMF(0x123456); "
             "/* fdiv dword ptr [0x00123456] */"],
        )

    def test_register_arithmetic_updates_st0_without_popping(self):
        instruction = Instruction(0, 2, "fmul", "st(1)", "d8c9")
        instruction.operands = [Operand(type="reg", reg="st(1)")]

        self.assertEqual(
            Lifter().lift_instruction(instruction),
            ["fp_top() = fp_top() * fp_st1(); /* fmul st(1) */"],
        )

    def test_translated_functions_share_runtime_fpu_state(self):
        start = 0x00010000
        instructions = [
            Instruction(start, 2, "fld", "st(0)", "d9c0"),
            Instruction(start + 2, 1, "ret", "", "c3"),
        ]
        instructions[0].operands = [Operand(type="reg", reg="st(0)")]
        block = BasicBlock(start=start, instructions=instructions)
        translator = FunctionTranslator(
            b"\0", {start: {"_addr": start, "end": start + 3}})
        translator._read_func_bytes = lambda _start, _end: b"\xd9\xc0\xc3"
        translator.disasm.disassemble_function = (
            lambda _raw, _start, _end: instructions)
        translator.disasm.build_basic_blocks = (
            lambda _instructions, _start, _end, extra_leaders=None,
            stop_mnemonics=(): [block])

        generated = translator.translate_function(
            start, {"_addr": start, "end": start + 3})

        self.assertIn("g_fp_stack", generated)
        self.assertIn("g_fp_top", generated)
        self.assertNotIn("double _fp_stack[8]", generated)
        self.assertNotIn("int _fp_top = 0", generated)

    def test_runtime_template_exports_shared_fpu_state(self):
        root = Path(__file__).resolve().parents[2]
        runtime_types = (
            root / "templates" / "runtime" / "recomp_types.h"
        ).read_text()
        main = (
            root / "templates" / "new-game" / "src" / "main.c"
        ).read_text()

        self.assertIn("extern RECOMP_TLS double g_fp_stack[8];", runtime_types)
        self.assertIn("extern RECOMP_TLS int g_fp_top;", runtime_types)
        self.assertIn("#define SMEM64(addr)", runtime_types)
        self.assertIn("extern RECOMP_TLS double g_fp_stack[8];", main)
        self.assertIn("extern RECOMP_TLS int g_fp_top;", main)
        self.assertIn("extern RECOMP_TLS uint16_t g_fp_control_word;", runtime_types)
        self.assertIn("#define RECOMP_PARITY8(x)", runtime_types)
        self.assertIn("extern RECOMP_TLS uint16_t g_fp_control_word;", main)

    def test_control_word_store_and_load_use_shared_state(self):
        operand = Operand(type="mem", mem_base="ebp", mem_disp=0xFFFFFFFC,
                          mem_size=2)
        store = Instruction(0, 3, "fnstcw", "word ptr [ebp - 4]", "")
        store.operands = [operand]
        load = Instruction(0, 3, "fldcw", "word ptr [ebp - 4]", "")
        load.operands = [operand]

        self.assertEqual(
            Lifter().lift_instruction(store),
            ["MEM16(ebp + 0xFFFFFFFCu) = g_fp_control_word;"
             " /* fnstcw word ptr [ebp - 4] */"],
        )
        self.assertEqual(
            Lifter().lift_instruction(load),
            ["g_fp_control_word = MEM16(ebp + 0xFFFFFFFCu);"
             " /* fldcw word ptr [ebp - 4] */"],
        )

    def test_status_word_store_reports_the_compare_result(self):
        instruction = Instruction(0, 2, "fnstsw", "ax", "dfe0")
        instruction.operands = [Operand(type="reg", reg="ax")]

        lifted = Lifter().lift_instruction(instruction)

        self.assertEqual(len(lifted), 1)
        # The compare can live in a different lifted body than the FNSTSW that
        # reads it, so the result has to be shared state, not a function local.
        self.assertIn("g_fp_cc", lifted[0])
        self.assertNotIn("_fpu_cmp", lifted[0])
        # C3 (equal) and C0 (less) at their status-word positions, plus TOP
        self.assertIn("(g_fp_top & 7u) << 11", lifted[0])
        self.assertNotIn("/* fnstsw ax - store FPU status word */", lifted[0])

    def test_compare_writes_the_shared_result(self):
        instruction = Instruction(0, 4, "fcomp", "qword ptr [ebp + 8]", "dc5d08")
        instruction.operands = [
            Operand(type="mem", mem_base="ebp", mem_disp=8, mem_size=8)
        ]

        lifted = Lifter().lift_instruction(instruction)

        self.assertEqual(len(lifted), 1)
        self.assertTrue(lifted[0].startswith("g_fp_cmp ="))
        self.assertNotIn("_fpu_cmp", lifted[0])

    def test_fxch_swaps_with_the_explicit_register(self):
        """Capstone reports fxch with both operands -- (st(0), st(i)) -- and it
        is the only x87 form that does. Reading operand 0 picked up the
        implicit st(0), so every `fxch st(i)` emitted a swap of st0 with
        itself and silently did nothing."""
        instruction = Instruction(0, 2, "fxch", "st(1)", "d9c9")
        instruction.operands = [Operand(type="reg", reg="st(0)"),
                                Operand(type="reg", reg="st(1)")]

        lifted = Lifter().lift_instruction(instruction)[0]

        self.assertIn("fp_st1()", lifted)
        self.assertNotIn("fp_top() = fp_top()", lifted)

    def _clamp_block(self, cmov):
        """The MSVC clamp tail: fcom st(1); xor eax,eax; fnstsw ax;
        test ah,1; fcmov<cc> st(0),st(1); fxch st(1); fstp st(0)."""
        insns = [
            Instruction(0x100, 2, "fcom", "st(1)", "d8d1"),
            Instruction(0x102, 2, "xor", "eax, eax", "31c0"),
            Instruction(0x104, 2, "fnstsw", "ax", "dfe0"),
            Instruction(0x106, 3, "test", "ah, 1", "f6c401"),
            Instruction(0x109, 2, cmov, "st(0), st(1)", "dac9"),
        ]
        insns[0].operands = [Operand(type="reg", reg="st(1)")]
        insns[1].operands = [Operand(type="reg", reg="eax"),
                             Operand(type="reg", reg="eax")]
        insns[2].operands = [Operand(type="reg", reg="ax")]
        insns[3].operands = [Operand(type="reg", reg="ah"),
                             Operand(type="imm", imm=1)]
        insns[4].operands = [Operand(type="reg", reg="st(0)"),
                             Operand(type="reg", reg="st(1)")]
        from .lifter import lift_basic_block
        bb = BasicBlock(start=0x100, instructions=insns)
        stmts, _ = lift_basic_block(Lifter(), bb)
        return "\n".join(stmts)

    def test_fcmove_and_fcmovne_are_translated_not_dropped(self):
        """Regression: both were emitted as a bare `/* FPU: ... */` comment,
        so the [0,1] clamp in JSRF's sub_0014C870/sub_0014C850 never took
        its conditional move."""
        for cmov in ("fcmove", "fcmovne"):
            out = self._clamp_block(cmov)
            self.assertNotIn("/* FPU:", out, cmov)
            self.assertNotIn("RECOMP_UNIMPL", out, cmov)
            self.assertRegex(out, r"if \(.*\) fp_top\(\) = fp_st1\(\);", cmov)
            self.assertIn(f"/* {cmov} */", out)

    def test_fcmov_without_tracked_flags_uses_materialised_flags(self):
        insn = Instruction(0, 2, "fcmovb", "st(0), st(1)", "dac1")
        insn.operands = [Operand(type="reg", reg="st(0)"),
                         Operand(type="reg", reg="st(1)")]
        out = " ".join(Lifter().lift_instruction(insn))
        self.assertIn("if (_flags", out)
        self.assertIn("fp_top() = fp_st1()", out)

    def test_unhandled_fpu_instruction_reports_itself(self):
        lifter = Lifter()
        insn = Instruction(0x200, 2, "f2xm1_bogus", "", "")
        insn.operands = []
        out = " ".join(lifter.lift_instruction(insn))
        self.assertIn('RECOMP_UNIMPL("f2xm1_bogus", 0x00000200u);', out)
        self.assertIn("f2xm1_bogus", lifter.unimplemented)

    def test_fldenv_is_reported_fisttp_translated_fnclex_decided(self):
        lifter = Lifter()
        mem = Operand(type="mem", mem_base="edi", mem_size=8)
        fldenv = Instruction(0x300, 3, "fldenv", "[edi]", "")
        fldenv.operands = [Operand(type="mem", mem_base="edi", mem_size=0)]
        self.assertIn("RECOMP_UNIMPL", " ".join(lifter.lift_instruction(fldenv)))
        fisttp = Instruction(0x304, 2, "fisttp", "qword ptr [edi]", "")
        fisttp.operands = [mem]
        out = " ".join(lifter.lift_instruction(fisttp))
        self.assertIn("0x0C00", out)
        self.assertIn("fp_pop()", out)
        self.assertNotIn("RECOMP_UNIMPL", out)
        fnclex = Instruction(0x308, 2, "fnclex", "", "")
        fnclex.operands = []
        out = " ".join(lifter.lift_instruction(fnclex))
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertNotIn("fnclex", lifter.unimplemented)


if __name__ == "__main__":
    unittest.main()
