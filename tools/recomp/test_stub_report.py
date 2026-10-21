"""An unresolved-target stub reports that it ran before it returns.

A stub stands in for code the disassembler never found, so a stub that runs
means the original executed something the recompiled build skipped. Each stub
calls recomp_icall_fail_log(va), which the project implements (a project can
make it fatal), and only then pops what a `ret N` would.

The behavioural half compiles a generated stub against a recording
implementation; it skips itself if no C compiler is on PATH.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.translator import _stub_definition  # noqa: E402


def _cc():
    return shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


class StubDefinition(unittest.TestCase):
    def test_reports_then_pops_the_return_address(self):
        line = _stub_definition("sub_00054750", 0x00054750, 0)
        self.assertIn("recomp_icall_fail_log(0x00054750u);", line)
        self.assertIn("g_esp += 4;", line)
        self.assertLess(line.index("recomp_icall_fail_log"), line.index("g_esp"))
        self.assertIn("not detected", line)

    def test_stdcall_bytes_are_still_popped(self):
        line = _stub_definition("sub_00011000", 0x00011000, 8)
        self.assertIn("g_esp += 12;", line)
        self.assertIn("ret 8", line)


class StubReportRuns(unittest.TestCase):
    PROGRAM = r"""
#include <stdint.h>
#include <stdio.h>
static uint32_t reported;
static uint32_t esp_at_report;
static int calls;
uint32_t g_esp = 0x1000;
void recomp_icall_fail_log(uint32_t va) { reported = va; esp_at_report = g_esp; ++calls; }
%s
int main(void) {
    sub_00054750();
    printf("%%u %%08X %%08X %%08X\n", (unsigned)calls, (unsigned)reported,
           (unsigned)esp_at_report, (unsigned)g_esp);
    return 0;
}
"""

    def test_report_runs_before_the_stub_returns(self):
        cc = _cc()
        if cc is None:
            self.skipTest("no C compiler on PATH")
        source = self.PROGRAM % _stub_definition("sub_00054750", 0x00054750, 8)
        with tempfile.TemporaryDirectory() as tmp:
            c = os.path.join(tmp, "stub.c")
            exe = os.path.join(tmp, "stub.exe")
            with open(c, "w", encoding="utf-8") as f:
                f.write(source)
            subprocess.run([cc, c, "-o", exe], check=True, capture_output=True)
            out = subprocess.run([exe], check=True, capture_output=True,
                                 text=True).stdout.split()
        # One report, of the stub's own address, before esp moved; then ret 8.
        self.assertEqual(out, ["1", "00054750", "00001000", "0000100C"])


if __name__ == "__main__":
    unittest.main()
