"""SSE compares with an unordered (NaN) operand must branch as x86 does.

comiss/ucomiss/comisd/ucomisd set ZF=PF=CF=1 when either operand is NaN, so
after such a compare je, jb, jbe, jp and cmovb are taken, while jne, ja, jae
and jnp are not. C relational operators are all false on NaN, so a bare
`a == b` or `a < b` gets the unordered case wrong.

Each case is lifted from real instruction bytes through FunctionTranslator,
compiled with the host C compiler, and run on ordered and NaN inputs. The
expectation comes from the x86 flag table below, not from the lifted text.
"""
import math
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from . import config
from .translator import FunctionTranslator

BASE = 0x10000

# Operand pairs (xmm0, xmm7) and the (ZF, PF, CF) a compare of them leaves.
NAN = float("nan")
CASES = {
    "less": (1.0, 2.0, (0, 0, 1)),
    "equal": (2.0, 2.0, (1, 0, 0)),
    "greater": (3.0, 2.0, (0, 0, 0)),
    "nan_left": (NAN, 2.0, (1, 1, 1)),
    "nan_right": (2.0, NAN, (1, 1, 1)),
    "nan_both": (NAN, NAN, (1, 1, 1)),
}

# mnemonic -> (opcode byte, predicate over ZF, PF, CF)
JCC = {
    "je": (0x74, lambda z, p, c: z),
    "jne": (0x75, lambda z, p, c: not z),
    "jb": (0x72, lambda z, p, c: c),
    "jbe": (0x76, lambda z, p, c: c or z),
    "ja": (0x77, lambda z, p, c: not c and not z),
    "jae": (0x73, lambda z, p, c: not c),
    "jp": (0x7A, lambda z, p, c: p),
    "jnp": (0x7B, lambda z, p, c: not p),
}

# Compare encodings, `<op> xmm0, xmm7`.
COMPARES = {
    "comiss": "0f2fc7",
    "ucomiss": "0f2ec7",
    "comisd": "660f2fc7",
    "ucomisd": "660f2ec7",
}


def _translate(image):
    config._install(
        [config.Section(".text", BASE, len(image), 0, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="sse-unordered-test")
    db = {BASE: {"start": hex(BASE), "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def _jcc_image(compare, jcc):
    # <compare>; jcc taken; mov eax,0; ret; taken: mov eax,1; ret
    return bytes.fromhex(COMPARES[compare]) + bytes([JCC[jcc][0], 6]) + \
        bytes.fromhex("b800000000c3" "b801000000c3")


def _cmovb_image(compare):
    # <compare>; mov eax,0; mov ecx,1; cmovb eax,ecx; ret
    return bytes.fromhex(COMPARES[compare] + "b800000000" "b901000000"
                         "0f42c1" "c3")


def _expected(taken_fn, case):
    _, _, flags = CASES[case]
    return 1 if taken_fn(*[bool(f) for f in flags]) else 0


def _compiler():
    cc = shutil.which("clang") or shutil.which("gcc")
    if not cc and Path(r"C:\Program Files\LLVM\bin\clang.exe").exists():
        cc = r"C:\Program Files\LLVM\bin\clang.exe"
    return cc


PRELUDE = """\
#include <math.h>
#include <stdint.h>
#include <stdio.h>
typedef union { float f[4]; double d[2]; uint32_t u[4]; } Xmm;
static Xmm xmm0, xmm7;
static uint32_t eax, ecx, esp;
#define MEMF(a) (*(float *)0)
#define MEMD(a) (*(double *)0)
"""


class SseUnorderedLifterTest(unittest.TestCase):
    # name -> (image, taken predicate, compare mnemonic)
    variants = {}

    @classmethod
    def setUpClass(cls):
        for compare in COMPARES:
            for jcc in JCC:
                if compare != "comiss" and jcc != "je":
                    continue
                cls.variants[f"{compare}_{jcc}"] = (
                    _jcc_image(compare, jcc), JCC[jcc][1], compare)
        cls.variants["comiss_cmovb"] = (
            _cmovb_image("comiss"), JCC["jb"][1], "comiss")
        cls.code = {name: _translate(image)
                    for name, (image, _, _) in cls.variants.items()}

    # -- emitted text: the unordered term is present where x86 takes it --

    def test_condition_text_names_the_unordered_term(self):
        term = "(_fca != _fca || _fcb != _fcb)"
        needs = {"je", "jb", "jbe", "jp", "jne", "jnp"}
        for name, code in self.code.items():
            jcc = name.split("_", 1)[1]
            if jcc == "cmovb":
                jcc = "jb"
            with self.subTest(name):
                if jcc in needs:
                    self.assertIn(term, code)
                else:  # ja / jae: C relational operators are already right
                    self.assertNotIn(term, code)

    # -- execution against the x86 table --

    def _run_all(self, opt):
        cc = _compiler()
        if not cc:
            self.skipTest("C compiler unavailable")
        src = PRELUDE
        for name, code in self.code.items():
            src += code.replace("sub_00010000", f"fn_{name}")
        src += "int main(void) {\n"
        for name in self.code:
            double = self.variants[name][2].endswith("sd")
            for case, (a, b, _) in CASES.items():
                if double:
                    setter = f"xmm0.d[0] = {self._c(a)}; xmm7.d[0] = {self._c(b)};"
                else:
                    setter = (f"xmm0.f[0] = (float){self._c(a)}; "
                              f"xmm7.f[0] = (float){self._c(b)};")
                src += (f"  eax = 7; ecx = 0; {setter} fn_{name}();"
                        f' printf("{name} {case} %u\\n", (unsigned)eax);\n')
        src += "  return 0;\n}\n"
        with tempfile.TemporaryDirectory() as tmp:
            c_file, exe = Path(tmp) / "t.c", Path(tmp) / "t.exe"
            c_file.write_text(src)
            cmd = [cc, str(c_file), "-o", str(exe), "-w", opt]
            if os.name != "nt":
                cmd.append("-lm")
            r = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr + src)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout)
        got = {}
        for line in r.stdout.splitlines():
            name, case, value = line.rsplit(" ", 2)
            got[(name, case)] = int(value)
        return got

    @staticmethod
    def _c(v):
        return "NAN" if math.isnan(v) else repr(v)

    def _check(self, opt, only=None):
        got = self._run_all(opt)
        for name, (_, taken, _) in self.variants.items():
            if only and not only(name):
                continue
            for case in CASES:
                with self.subTest(opt=opt, name=name, case=case):
                    self.assertEqual(
                        got[(name, case)], _expected(taken, case),
                        f"{name} on {case} (1 = branch/move taken)")

    @staticmethod
    def _is_double(name):
        return name.startswith(("comisd", "ucomisd"))

    def test_single_precision_branches_match_x86_o0(self):
        self._check("-O0", lambda n: not self._is_double(n))

    def test_single_precision_branches_match_x86_o2(self):
        self._check("-O2", lambda n: not self._is_double(n))

    # comisd/ucomisd compare the full double in xmm.d[0], as x86 does.
    def test_double_precision_branches_match_x86(self):
        self._check("-O0", lambda n: self._is_double(n))


if __name__ == "__main__":
    unittest.main()
