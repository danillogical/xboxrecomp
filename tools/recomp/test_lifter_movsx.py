"""movsx sign-extends every 16-bit register source, bp and sp included."""

import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _movsx(dst, src):
    insn = Instruction(0, 3, "movsx", "", "00")
    insn.operands = [Operand(type="reg", reg=dst), Operand(type="reg", reg=src)]
    return " ".join(Lifter().lift_instruction(insn))


class MovsxTest(unittest.TestCase):
    def test_every_16bit_register_is_sign_extended(self):
        for reg in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp"):
            with self.subTest(src=reg):
                self.assertIn("SX16(LO16(e%s))" % reg, _movsx("ecx", reg))

    def test_bp_source_is_not_zero_extended(self):
        # A bare LO16(ebp) turns bp = 0xFFFF into 0x0000FFFF instead of -1.
        self.assertEqual(_movsx("ecx", "bp"), "ecx = SX16(LO16(ebp));")


if __name__ == "__main__":
    unittest.main()
