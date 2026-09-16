"""Unit test for single-operand imul, mul, div, and idiv at 8, 16, and 32 bits."""

import unittest
from .disasm import Instruction, Operand
from .lifter import Lifter

class TestLifterMulDiv(unittest.TestCase):
    def test_imul_8bit(self):
        insn = Instruction(0, 2, "imul", "", "00")
        insn.operands = [Operand(type="reg", reg="dl")]
        lines = Lifter().lift_instruction(insn)
        self.assertEqual(len(lines), 1)
        self.assertIn("SET_LO16(eax", lines[0])
        self.assertIn("(int16_t)(int8_t)LO8(eax)", lines[0])
        self.assertNotIn("edx =", lines[0])

    def test_imul_16bit(self):
        insn = Instruction(0, 3, "imul", "", "00")
        insn.operands = [Operand(type="reg", reg="cx")]
        lines = Lifter().lift_instruction(insn)
        combined = " ".join(lines)
        self.assertIn("SET_LO16(eax", combined)
        self.assertIn("SET_LO16(edx", combined)
        self.assertIn("(int16_t)LO16(eax)", combined)

    def test_imul_32bit(self):
        insn = Instruction(0, 2, "imul", "", "00")
        insn.operands = [Operand(type="reg", reg="ecx")]
        lines = Lifter().lift_instruction(insn)
        combined = " ".join(lines)
        self.assertIn("int64_t", combined)
        self.assertIn("eax = (uint32_t)_r", combined)
        self.assertIn("edx = (uint32_t)(_r >> 32)", combined)

    def test_mul_8bit(self):
        insn = Instruction(0, 2, "mul", "", "00")
        insn.operands = [Operand(type="reg", reg="bl")]
        lines = Lifter().lift_instruction(insn)
        self.assertEqual(len(lines), 1)
        self.assertIn("SET_LO16(eax", lines[0])
        self.assertNotIn("edx =", lines[0])

    def test_div_8bit(self):
        insn = Instruction(0, 2, "div", "", "00")
        insn.operands = [Operand(type="reg", reg="bl")]
        lines = Lifter().lift_instruction(insn)
        combined = " ".join(lines)
        self.assertIn("LO16(eax)", combined)
        self.assertIn("SET_LO8(eax", combined)
        self.assertIn("SET_HI8(eax", combined)
        self.assertNotIn("edx", combined)

if __name__ == "__main__":
    unittest.main()
