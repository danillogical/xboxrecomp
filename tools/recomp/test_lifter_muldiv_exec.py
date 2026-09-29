"""Run the lifted 8- and 16-bit mul/imul/div/idiv against x86 answers.

The width forms are checked by text in test_lifter_muldiv.py; this compiles
the lifter's own statements and checks the values they leave in eax/edx,
including that the bits x86 leaves alone (eax's high half, all of edx for the
8-bit forms) survive. The build uses UBSan where the host has it, so a
dividend assembled with a signed left shift fails here rather than working by
luck.
"""

import os
import shutil
import subprocess
import tempfile
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter

PRELUDE = r"""
#include <stdint.h>
#include <stdio.h>
#define LO8(r)  ((uint8_t)((r) & 0xFF))
#define HI8(r)  ((uint8_t)(((r) >> 8) & 0xFF))
#define LO16(r) ((uint16_t)((r) & 0xFFFF))
#define SET_LO8(r, v)  ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(uint8_t)(v)))
#define SET_HI8(r, v)  ((r) = ((r) & 0xFFFF00FFu) | (((uint32_t)(uint8_t)(v)) << 8))
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(uint16_t)(v)))
static uint8_t g_ram[64];
#define MEM8(a)  (*(volatile uint8_t *)(g_ram + (a)))
#define MEM16(a) (*(volatile uint16_t *)(g_ram + (a)))
static int fails;
static void check(const char *name, uint32_t eax, uint32_t edx,
                  uint32_t want_eax, uint32_t want_edx) {
    if (eax != want_eax || edx != want_edx) {
        printf("FAIL %s eax=0x%08X edx=0x%08X want eax=0x%08X edx=0x%08X\n",
               name, eax, edx, want_eax, want_edx);
        fails++;
    }
}
"""

# name, mnemonic, operand, eax, edx, ecx/ebx seed, byte at [esi], want eax, want edx
CASES = [
    ("div byte [esi]", "div", Operand(type="mem", mem_base="esi", mem_size=1),
     0xAABB0107, 0xDEADBEEF, 0, 2, 0xAABB0183, 0xDEADBEEF),
    ("idiv cl", "idiv", Operand(type="reg", reg="cl"),
     0x1234FFF9, 0xDEADBEEF, 0x55555502, 0, 0x1234FFFD, 0xDEADBEEF),
    ("mul bl", "mul", Operand(type="reg", reg="bl"),
     0x123456FF, 0xDEADBEEF, 0x000000FF, 0, 0x1234FE01, 0xDEADBEEF),
    ("imul cl", "imul", Operand(type="reg", reg="cl"),
     0x123456FE, 0xDEADBEEF, 0x00000003, 0, 0x1234FFFA, 0xDEADBEEF),
    ("mul cx", "mul", Operand(type="reg", reg="cx"),
     0x1234FFFF, 0xABCD0000, 0x9999FFFF, 0, 0x12340001, 0xABCDFFFE),
    ("div cx", "div", Operand(type="reg", reg="cx"),
     0x12340000, 0xABCD0001, 0x99990003, 0, 0x12345555, 0xABCD0001),
    # dx:ax = -100000; -100000 / 7 = -14285 remainder -5.
    ("idiv cx", "idiv", Operand(type="reg", reg="cx"),
     0x12347960, 0xABCDFFFE, 0x99990007, 0, 0x1234C833, 0xABCDFFFB),
    # -300 * 300 = -90000 = 0xFFFEA070.
    ("imul cx", "imul", Operand(type="reg", reg="cx"),
     0x1234FED4, 0xABCD0000, 0x9999012C, 0, 0x1234A070, 0xABCDFFFE),
]


def _lift(mnemonic, operand):
    insn = Instruction(0, 2, mnemonic, "", "00")
    insn.operands = [operand]
    return "\n".join(Lifter().lift_instruction(insn))


def _find_cc():
    return shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


class MulDivExecTest(unittest.TestCase):
    def setUp(self):
        if not _find_cc():
            self.skipTest("no C compiler on PATH")

    def test_narrow_forms_match_x86(self):
        body = []
        for name, m, op, eax, edx, reg, mem, want_eax, want_edx in CASES:
            body.append(
                "{ uint32_t eax = 0x%08Xu, edx = 0x%08Xu, ecx = 0x%08Xu,"
                " ebx = ecx, esi = 0; (void)esi; (void)ebx; MEM8(0) = 0x%02X;\n"
                "%s\n  check(\"%s\", eax, edx, 0x%08Xu, 0x%08Xu); }"
                % (eax, edx, reg, mem, _lift(m, op), name, want_eax, want_edx))
        src = (PRELUDE + "int main(void) {\n" + "\n".join(body)
               + "\nif (!fails) printf(\"OK\\n\"); return fails != 0; }\n")
        cc = _find_cc()
        with tempfile.TemporaryDirectory() as tmp:
            c = os.path.join(tmp, "t.c")
            with open(c, "w") as f:
                f.write(src)
            exe = os.path.join(tmp, "t.exe")
            ubsan = ["-fsanitize=undefined", "-fno-sanitize-recover=undefined"]
            r = subprocess.run([cc, "-w", "-O1", *ubsan, c, "-o", exe],
                               capture_output=True, text=True)
            if r.returncode != 0:
                r = subprocess.run([cc, "-w", "-O1", c, "-o", exe],
                                   capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-1500:] + "\n" + src)
            r = subprocess.run([exe], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr + "\n" + src)
        self.assertEqual(r.stdout.strip(), "OK", r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
