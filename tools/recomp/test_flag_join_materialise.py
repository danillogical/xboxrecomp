"""A join whose predecessors set the flags differently gets real conditions.

_merge_flag_states refuses to merge `test ah, 1` with `cmp [ebx+0x10], 7`,
or a 32-bit `cmp` with an 8-bit one: no single expression over the shared
_fa/_fb snapshot answers for both. The consumer used to fall back to
`_flags`, which nothing assigned, so the branch was never taken. JSRF has
this at loc_000153A9 (sub_00015130: six `jmp` predecessors doing `test ah,1`
or `cmp [ebx+0x10],N` plus a fall-through doing `cmp [ebx+0x10],7`) and at
five joins in sub_00130FD0 (a 32-bit and an 8-bit `cmp`).

Now each predecessor computes the condition from its own flags into
_fc_<key> before its edge, including the fall-through edge. These tests lift
synthetic instruction streams, compile the translated function and run it on
both paths; with the fallback, one path of each goes the wrong way. The last
group checks the guarantee: whatever still reads an unassigned `_flags` is
recorded by the lifter and listed by flag_gaps().
"""

import os
import re
import shutil
import subprocess
import tempfile

from tools.recomp import config
from tools.recomp.translator import FunctionTranslator, flag_gaps

BASE = 0x00010000
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HEADER = os.path.join(ROOT, "templates", "runtime", "recomp_types.h")


def _translate(image):
    """Translate `image` as one function; returns (C text, translator)."""
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="flag-join-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    translator = FunctionTranslator(image, db)
    return translator.translate_function(BASE, db[BASE]), translator


def _flag_macros():
    """The runtime header's CMP_*/TEST_* block, verbatim."""
    text = open(HEADER, encoding="utf-8").read()
    start = text.index("#define CMP_EQ(")
    end = text.index("#define TEST_S(")
    end = text.index("\n\n", end)
    return text[start:end]


PRELUDE = r"""
#include <stdint.h>
#include <stdio.h>
uint32_t eax, ebx, ecx, edx, esi, edi, esp = 0x1000u, g_ebp, g_seh_ebp;
#define LO8(r)  ((uint8_t)((r) & 0xFF))
#define HI8(r)  ((uint8_t)(((r) >> 8) & 0xFF))
#define LO16(r) ((uint16_t)((r) & 0xFFFF))
static inline int recomp_parity8(uint32_t x){x&=0xFFu;x^=x>>4;x^=x>>2;x^=x>>1;return (int)(~x&1u);}
#define RECOMP_PARITY8(x) recomp_parity8((uint32_t)(x))
"""


def _run(code, cases):
    """Run the function once per (eax, ecx, edx, expected eax) case."""
    cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
    if not cc:
        import pytest
        pytest.skip("no C compiler on PATH")
    rows = ",\n".join("    {0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu}" % case
                      for case in cases)
    src = (PRELUDE + _flag_macros() + "\n" + code + r"""
static const uint32_t cases[][4] = {
""" + rows + r"""
};
int main(void) {
    int bad = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        eax = cases[i][0]; ecx = cases[i][1]; edx = cases[i][2]; esp = 0x1000u;
        sub_00010000();
        if (eax != cases[i][3]) {
            printf("FAIL case %u: eax=%X want %X\n", i, eax, cases[i][3]);
            bad = 1;
        }
    }
    if (!bad) printf("OK\n");
    return bad;
}
""")
    with tempfile.TemporaryDirectory() as tmp:
        c = os.path.join(tmp, "t.c")
        with open(c, "w") as f:
            f.write(src)
        exe = os.path.join(tmp, "t.exe")
        r = subprocess.run([cc, "-w", "-O1", c, "-o", exe],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr[-2000:] + "\n" + code
        r = subprocess.run([exe], capture_output=True, text=True)
    assert r.returncode == 0 and r.stdout.startswith("OK"), \
        r.stdout + r.stderr + "\n" + code


# The shared tail: J+0 jcc X; mov eax, 1; ret; X: mov eax, 2; ret.
def _tail(jcc_opcode):
    return (bytes([jcc_opcode, 0x06])          # jcc +6 -> X
            + b"\xB8\x01\x00\x00\x00\xC3"      # mov eax, 1; ret
            + b"\xB8\x02\x00\x00\x00\xC3")     # X: mov eax, 2; ret


def test_jmp_predecessors_each_write_the_condition():
    #   +0  test ecx, ecx
    #   +2  jz   B (+9)
    #   +4  test ah, 1          ; 8-bit test
    #   +7  jmp  J (+0x0E)
    #   +9 B: cmp edx, 7        ; 32-bit cmp
    #   +C  jmp  J
    #   +E J: jne X
    image = (b"\x85\xC9" b"\x74\x05"
             b"\xF6\xC4\x01" b"\xEB\x05"
             b"\x83\xFA\x07" b"\xEB\x00"
             + _tail(0x75))
    code, _ = _translate(image)
    assert "_flags /*" not in code, code
    assert code.count("_fc_e = (") == 2, code
    # Written before the jmp, or the taken edge would skip it.
    for match in re.finditer(r"_fc_e = \(.*\n(.*)\n", code):
        assert "goto loc_0001000E" in match.group(1), code
    _run(code, [
        (0x00000100, 1, 0, 2),     # ah bit 0 set: ZF=0, jne taken
        (0x00000200, 1, 0, 1),     # ah bit 0 clear: ZF=1
        (0, 0, 7, 1),              # edx == 7: ZF=1
        (0, 0, 8, 2),              # edx != 7: ZF=0, jne taken
    ])


def test_fall_through_predecessor_writes_the_condition():
    # loc_000153A9's shape: `test ah, 1; jmp J` and a `cmp edx, 7` that falls
    # through into J.
    #   +0  test ecx, ecx
    #   +2  jz   B (+9)
    #   +4  test ah, 1
    #   +7  jmp  J (+0x0C)
    #   +9 B: cmp edx, 7
    #   +C J: jne X
    image = (b"\x85\xC9" b"\x74\x05"
             b"\xF6\xC4\x01" b"\xEB\x03"
             b"\x83\xFA\x07"
             + _tail(0x75))
    code, _ = _translate(image)
    assert "_flags /*" not in code, code
    assert code.count("_fc_e = (") == 2, code
    assert "if (!_fc_e) goto loc_00010014;" in code, code
    _run(code, [
        (0x00000100, 1, 0, 2),
        (0x00000200, 1, 0, 1),
        (0, 0, 7, 1),
        (0, 0, 8, 2),
    ])


def test_mixed_widths_answer_at_each_predecessors_width():
    # sub_00130FD0's shape: a 32-bit cmp on one edge and an 8-bit cmp on the
    # other. The signed condition must be taken at each one's own width.
    #   +0  test ecx, ecx
    #   +2  jz   B (+8)
    #   +4  cmp eax, edx        ; 32-bit
    #   +6  jmp  J (+0x0A)
    #   +8 B: cmp al, dl        ; 8-bit
    #   +A J: jl X
    image = (b"\x85\xC9" b"\x74\x04"
             b"\x39\xD0" b"\xEB\x02"
             b"\x38\xD0"
             + _tail(0x7C))
    code, _ = _translate(image)
    assert "_flags /*" not in code, code
    assert code.count("_fc_l = (") == 2, code
    _run(code, [
        (0x00000080, 1, 0x00000001, 1),   # 32-bit: 0x80 > 1, not less
        (0xFFFFFFFF, 1, 0x00000001, 2),   # 32-bit: -1 < 1
        (0x00000080, 0, 0x00000001, 2),   # 8-bit: al=-128 < 1
        (0x00000101, 0, 0x00000080, 1),   # 8-bit: 1 > -128
    ])


def test_mixed_widths_for_the_zero_flag():
    #   +0..+8 as above, J: je X. 0x100 and 0 differ at 32 bits, not at 8.
    image = (b"\x85\xC9" b"\x74\x04"
             b"\x39\xD0" b"\xEB\x02"
             b"\x38\xD0"
             + _tail(0x74))
    code, _ = _translate(image)
    assert "_flags /*" not in code, code
    _run(code, [
        (0x00000100, 1, 0, 1),    # 32-bit: not equal
        (0x00000100, 0, 0, 2),    # 8-bit: equal
    ])


def test_a_join_that_sets_its_own_flags_needs_nothing():
    #   +0  test ecx, ecx; jz B; test ah, 1; jmp J; B: cmp edx, 7
    #   J:  cmp eax, 3; jne X   -- the join's jcc reads its own cmp
    image = (b"\x85\xC9" b"\x74\x05"
             b"\xF6\xC4\x01" b"\xEB\x03"
             b"\x83\xFA\x07"
             b"\x83\xF8\x03"
             + _tail(0x75))
    code, _ = _translate(image)
    assert "_fc_" not in code, code
    assert "_flags /*" not in code, code


def test_an_unanswerable_predecessor_keeps_the_fallback_and_is_recorded():
    # One edge arrives after `mul`, whose flags the lifter does not model.
    # Nothing may be guessed for it: the jcc keeps the fallback, and the
    # lifter records it.
    #   +0  test ecx, ecx
    #   +2  jz   B (+8)
    #   +4  mul ecx
    #   +6  jmp  J (+0x0B)
    #   +8 B: cmp edx, 7
    #   +B J: jne X
    image = (b"\x85\xC9" b"\x74\x04"
             b"\xF7\xE1" b"\xEB\x03"
             b"\x83\xFA\x07"
             + _tail(0x75))
    code, translator = _translate(image)
    assert "if (_flags /* jne" in code, code
    assert "_fc_" not in code, code
    gaps = flag_gaps(translator.lifter)
    assert gaps == [(BASE, BASE + 0x0B, "jne", None)], gaps


def test_every_fallback_form_is_recorded():
    # Flags live into the function: nothing in it can answer them.
    #   +0  jne +0
    #   +2  sete al
    #   +5  cmovne eax, ecx
    #   +8  loopne +0
    #   +A  ret
    image = (b"\x75\x00" b"\x0F\x94\xC0" b"\x0F\x45\xC1" b"\xE0\x00" b"\xC3")
    code, translator = _translate(image)
    forms = [gap[2] for gap in flag_gaps(translator.lifter)]
    assert forms == ["jne", "sete", "cmovne", "loopne"], (forms, code)
    assert all(gap[3] is None for gap in flag_gaps(translator.lifter))


def test_a_function_with_no_gaps_records_none():
    image = (b"\x85\xC9" b"\x74\x05"
             b"\xF6\xC4\x01" b"\xEB\x03"
             b"\x83\xFA\x07"
             + _tail(0x75))
    _, translator = _translate(image)
    assert flag_gaps(translator.lifter) == []


def test_a_predecessor_ending_in_its_own_branch_writes_before_it():
    # The fall-through predecessor ends `cmp edx, 7; jb Y`, fused. The join's
    # condition must be written before that branch, on both of its edges.
    #   +0  test ecx, ecx
    #   +2  jz   B (+9)
    #   +4  test ah, 1
    #   +7  jmp  J (+0x0E)
    #   +9 B: cmp edx, 7
    #   +C  jb   Y (+0x10)
    #   +E J: jne X
    image = (b"\x85\xC9" b"\x74\x05"
             b"\xF6\xC4\x01" b"\xEB\x05"
             b"\x83\xFA\x07" b"\x72\x02"
             + _tail(0x75))
    code, _ = _translate(image)
    assert "_flags /*" not in code, code
    block = code[code.index("loc_00010009:"):code.index("loc_0001000E:")]
    assert block.index("_fc_e = (") < block.index("goto loc_00010010"), block
    _run(code, [
        (0x00000100, 1, 0, 2),
        (0x00000200, 1, 0, 1),
        (0, 0, 3, 1),              # jb taken
        (0, 0, 7, 1),
        (0, 0, 8, 2),
    ])


def test_the_command_line_reports_gaps_loudly(capsys):
    from tools.recomp.__main__ import _report_flag_gaps
    gaps = [(BASE, BASE + 0x0B, "jne", None),
            (BASE, BASE + 0x20, "join jl", "adc"),
            (BASE + 0x100, BASE + 0x104, "setg", "adc")]
    assert _report_flag_gaps({"flag_gaps": gaps}) == 3
    err = capsys.readouterr().err
    assert "FLAGS: 3 conditional(s) in 2 function(s)" in err, err
    assert "no single flag state" in err and "join edge" in err, err
    assert "adc cannot answer" in err, err
    assert "0x0001000B" in err, err
    assert _report_flag_gaps({}) == 0
    assert capsys.readouterr().err == ""
