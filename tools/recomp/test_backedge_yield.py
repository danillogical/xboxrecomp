"""--backedge-yield, the fork's spelling of --back-edge-yield, marks loop back-edges.

Serial guest mode (RECOMP_GUEST_SERIAL=1) lets one host thread run lifted code
at a time, released across kernel calls. A guest loop that spins without
calling the kernel -- polling a flag another thread sets -- would hold the lock
until a waiter's bounded wait ran out, then run concurrently anyway. The
translator's backedge_yield is upstream's back_edge_yield under the fork's
name: every loop back edge becomes `{ RECOMP_BACK_EDGE(va); goto ...; }`, and
the runtime's RECOMP_BACK_EDGE also runs RECOMP_BACKEDGE(), which offers the
lock through xbox_GuestSerialYield when serial mode is on and reads one flag
when it is off.

The flag is opt-in because targeted relifts write bodies into existing chunks
whose recomp_types.h may predate the macro.
"""

import os
import shutil
import subprocess
import tempfile

from tools.recomp import config
from tools.recomp.translator import FunctionTranslator

BASE = 0x00010000
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HEADER = os.path.join(ROOT, "templates", "runtime", "recomp_types.h")

# config._install replaces module-level layout globals. Put them back after
# each test, so a test that runs later and expects the unconfigured fallback
# (test_config.py) still sees it.
_CONFIG_NAMES = ("_SECTIONS", "SECTIONS", "_configured_from", "TEXT_VA_START",
                 "TEXT_VA_END", "RDATA_VA_START", "RDATA_VA_END", "DATA_VA_START",
                 "DATA_VA_END", "KERNEL_THUNK_ADDR", "ENTRY_POINT")
_CONFIG_SAVED = {name: getattr(config, name) for name in _CONFIG_NAMES}


def teardown_function(_function):
    for name, value in _CONFIG_SAVED.items():
        setattr(config, name, value)

# A counted loop:  L: inc edx; dec ecx; jnz L; mov eax, edx; ret
COUNTED = (b"\x42"              # +0 L: inc edx
           b"\x49"              # +1    dec ecx
           b"\x75\xFC"          # +2    jnz L
           b"\x89\xD0"          # +4    mov eax, edx
           b"\xC3")             # +6    ret

# An unconditional back-edge and a forward jcc out of the loop:
#   +0  sub eax, ecx; +2 L: jz +4 (forward, to ret); +4 inc edx; +5 dec eax;
#   +6  jmp L; +8 ret
JMP_BACK = (b"\x29\xC8" b"\x74\x04" b"\x42" b"\x48" b"\xEB\xFA" b"\xC3")

# Forward jumps only: test ecx, ecx; jz +5; mov eax, 1; ret
FORWARD = b"\x85\xC9" b"\x74\x06" b"\xB8\x01\x00\x00\x00" b"\xC3" b"\x31\xC0" b"\xC3"


def _translate(image, **kw):
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="backedge-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    return FunctionTranslator(image, db, **kw).translate_function(BASE, db[BASE])


def _hook_lines(code):
    return [line for line in code.splitlines() if "RECOMP_BACK_EDGE(" in line]


def test_off_by_default():
    assert "RECOMP_BACK" not in _translate(COUNTED)
    assert "RECOMP_BACK" not in _translate(JMP_BACK)


def test_the_fork_spelling_is_upstreams_switch():
    assert _translate(COUNTED, backedge_yield=True) == \
        _translate_upstream(COUNTED)


def test_a_conditional_back_edge_is_marked():
    hooks = _hook_lines(_translate(COUNTED, backedge_yield=True))
    assert len(hooks) == 1, hooks
    assert "{ RECOMP_BACK_EDGE(0x00010002u); goto loc_00010000; }" in hooks[0], hooks


def test_an_unconditional_back_edge_is_marked_and_a_forward_exit_is_not():
    hooks = _hook_lines(_translate(JMP_BACK, backedge_yield=True))
    assert len(hooks) == 1, hooks
    assert "goto loc_00010002; }" in hooks[0], hooks
    assert not any("goto loc_00010008" in line for line in hooks), hooks


def test_forward_jumps_are_not_marked():
    assert "RECOMP_BACK" not in _translate(FORWARD, backedge_yield=True)


def _translate_upstream(image):
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="backedge-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    t = FunctionTranslator(image, db)
    t.back_edge_yield = True
    return t.translate_function(BASE, db[BASE])


def _backedge_macro():
    """The runtime header's two hooks, verbatim: the serial-mode RECOMP_BACKEDGE
    block and the RECOMP_BACK_EDGE that lifted code calls."""
    text = open(HEADER, encoding="utf-8").read()
    start = text.index("extern volatile int g_xbox_guest_serial_on;")
    end = text.index("} while (0)", start) + len("} while (0)")
    serial = text[start:end]
    start = text.index("extern int32_t g_xbox_guest_cpu_preempt;")
    end = text.index("} while (0)", text.index("#define RECOMP_BACK_EDGE(va)"))
    return serial + "\n" + text[start:end + len("} while (0)")]


PRELUDE = r"""
#include <stdint.h>
#include <stdio.h>
uint32_t eax, ebx, ecx, edx, esi, edi, esp = 0x1000u, g_ebp, g_seh_ebp;
#define LO8(r)  ((uint8_t)((r) & 0xFF))
#define HI8(r)  ((uint8_t)(((r) >> 8) & 0xFF))
#define LO16(r) ((uint16_t)((r) & 0xFFFF))
static inline int recomp_parity8(uint32_t x){x&=0xFFu;x^=x>>4;x^=x>>2;x^=x>>1;return (int)(~x&1u);}
#define RECOMP_PARITY8(x) recomp_parity8((uint32_t)(x))
volatile int g_xbox_guest_serial_on;
static unsigned yields;
void xbox_GuestSerialYield(void) { yields++; }
#define RECOMP_LOAD_RELAXED32(p) __atomic_load_n((p), __ATOMIC_RELAXED)
int32_t g_xbox_guest_cpu_preempt;
static unsigned preempts;
void xbox_GuestCpuPreempt(uint32_t va) { (void)va; preempts++; }
"""


def _flag_macros():
    text = open(HEADER, encoding="utf-8").read()
    start = text.index("#define CMP_EQ(")
    end = text.index("\n\n", text.index("#define TEST_S("))
    return text[start:end]


def test_the_hook_yields_once_per_taken_back_edge_only_in_serial_mode():
    # Five iterations take the back edge four times; the last jnz falls out.
    cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
    if not cc:
        import pytest
        pytest.skip("no C compiler on PATH")
    code = _translate(COUNTED, backedge_yield=True)
    src = PRELUDE + _flag_macros() + "\n" + _backedge_macro() + "\n" + code + r"""
int main(void) {
    eax = 0; ecx = 5; edx = 0;
    g_xbox_guest_serial_on = 0; yields = 0;
    sub_00010000();
    if (eax != 5 || yields != 0) { printf("FAIL off: eax=%u yields=%u\n", eax, yields); return 1; }
    eax = 0; ecx = 5; edx = 0;
    g_xbox_guest_serial_on = 1; yields = 0;
    sub_00010000();
    if (eax != 5 || yields != 4 || preempts) { printf("FAIL on: eax=%u yields=%u\n", eax, yields); return 1; }
    printf("OK\n");
    return 0;
}
"""
    with tempfile.TemporaryDirectory() as tmp:
        c = os.path.join(tmp, "t.c")
        with open(c, "w") as f:
            f.write(src)
        exe = os.path.join(tmp, "t.exe")
        r = subprocess.run([cc, "-w", "-O1", c, "-o", exe], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr[-2000:] + "\n" + code
        r = subprocess.run([exe], capture_output=True, text=True)
    assert r.returncode == 0 and r.stdout.startswith("OK"), r.stdout + r.stderr + "\n" + code
