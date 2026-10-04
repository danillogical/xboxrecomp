"""Execute MSVC's float min/max tails (fcom; fnstsw; test ah,1; fcmov) as lifted.

JSRF's sub_0014C870 (min) and sub_0014C850 (max) end in fcmove/fcmovne. The
lifter used to drop those as bare comments, so min(x, 1.0) always returned 1.0
and max(x, 0) always returned 0. This compiles the lifted C and runs it, so the
test fails on the old lift for the intended reason: the wrong value comes back.
"""
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _mem(disp):
    return Operand(type="mem", mem_base="esp", mem_disp=disp, mem_size=4)


def _function(cmov, cmov_bytes):
    """sub_0014C870 / sub_0014C850 body, byte for byte."""
    r = lambda n: Operand(type="reg", reg=n)
    i = [
        Instruction(0, 4, "fld", "dword ptr [esp + 4]", ""),
        Instruction(4, 4, "fld", "dword ptr [esp + 8]", ""),
        Instruction(8, 2, "fcom", "st(1)", ""),
        Instruction(10, 2, "xor", "eax, eax", ""),
        Instruction(12, 2, "fnstsw", "ax", ""),
        Instruction(14, 3, "test", "ah, 1", ""),
        Instruction(17, 2, cmov, "st(0), st(1)", cmov_bytes),
        Instruction(19, 2, "fxch", "st(1)", ""),
        Instruction(21, 2, "fstp", "st(0)", ""),
    ]
    i[0].operands = [_mem(4)]
    i[1].operands = [_mem(8)]
    i[2].operands = [r("st(1)")]
    i[3].operands = [r("eax"), r("eax")]
    i[4].operands = [r("ax")]
    i[5].operands = [r("ah"), Operand(type="imm", imm=1)]
    i[6].operands = [r("st(0)"), r("st(1)")]
    i[7].operands = [r("st(0)"), r("st(1)")]
    i[8].operands = [r("st(0)")]
    stmts, _ = lift_basic_block(Lifter(), BasicBlock(start=0, instructions=i))
    return "\n".join(stmts)


def _compiler():
    cc = shutil.which("clang") or shutil.which("gcc")
    if not cc and Path(r"C:\Program Files\LLVM\bin\clang.exe").exists():
        cc = r"C:\Program Files\LLVM\bin\clang.exe"
    if not cc:
        pytest.skip("C compiler unavailable")
    return cc


def test_min_max_clamp_tails_execute_correctly():
    cc = _compiler()
    header = (Path(__file__).resolve().parents[2]
              / "templates/runtime/recomp_types.h").read_text()
    macros = "\n".join(
        line for line in header.splitlines()
        if line.startswith(("#define RECOMP_FCMP(", "#define RECOMP_FCMP_CC(",
                            "#define TEST_Z(", "#define TEST_NZ(",
                            "#define HI8(")))
    # Lifted bodies reference these locals; declare them like a generated chunk.
    src = ("#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n"
           + macros + """
static double g_fp_stack[8]; static unsigned g_fp_top;
static int g_fp_cmp; static uint16_t g_fp_cc;
static uint8_t guest[64];
#define MEMF(a) (*(float *)(guest + (uint32_t)(a)))
#define fp_top() g_fp_stack[g_fp_top]
#define fp_st1() g_fp_stack[(g_fp_top + 1u) & 7u]
#define fp_push(v) do { double _v = (v); g_fp_top = (g_fp_top + 7u) & 7u; \\
    g_fp_stack[g_fp_top] = _v; } while (0)
#define fp_pop() (g_fp_top = (g_fp_top + 1u) & 7u)
""")
    for name, cmov, enc in (("min_", "fcmove", "dac9"),
                            ("max_", "fcmovne", "dbc9")):
        src += (f"static float {name}(float a, float b) {{\n"
                "  uint32_t eax = 0, esp = 0, _fa, _fb; int32_t _fas, _fbs;\n"
                "  (void)_fa;(void)_fb;(void)_fas;(void)_fbs;(void)esp;\n"
                "  g_fp_top = 0; MEMF(4) = a; MEMF(8) = b;\n"
                + _function(cmov, enc) + "\n  return (float)fp_top();\n}\n")
    src += """
int main(void) {
  /* sub_0014C870(x, 1.0) is min; sub_0014C850(x, 0.0) is max. The tail leaves
     the result in st0 after the final pop. */
  float x = 1.0f / 120.0f;
  float lo = min_(x, 1.0f), hi = max_(x, 0.0f);
  float big = min_(2.0f, 1.0f), neg = max_(-1.0f, 0.0f);
  printf("%.9g %.9g %.9g %.9g\\n", lo, hi, big, neg);
  if (lo != x) return 1;      /* min(1/120, 1.0) must be 1/120, not 1.0 */
  if (hi != x) return 2;      /* max(1/120, 0.0) must be 1/120, not 0.0 */
  if (big != 1.0f) return 3;  /* min(2, 1) = 1 */
  if (neg != 0.0f) return 4;  /* max(-1, 0) = 0 */
  return 0;
}
"""
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "t.c"
        path.write_text(src)
        exe = Path(tmp) / "t.exe"
        cmd = [cc, str(path), "-o", str(exe), "-w"]
        if os.name != "nt":
            cmd.append("-lm")
        r = subprocess.run(cmd, capture_output=True, text=True)
        assert r.returncode == 0, r.stderr + src
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        assert r.returncode == 0, f"rc={r.returncode} out={r.stdout}"
