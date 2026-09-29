"""The dispatch unit counts calls that enter a function through an alias VA.

An alias VA dispatches to its owner's symbol, so a call to it runs the owner
from its start. The generated wrappers must keep that behaviour, count every
such call in g_recomp_alias_icall_count, print the first eight distinct
aliases, and leave lookups and non-alias calls uncounted. This compiles the
generated dispatch file against a harness and checks all of that.
"""

import os
import shutil
import subprocess
import tempfile
import unittest

from .translator import BatchTranslator

OWNER = 0x00011000
ALIASES = [OWNER + 0x10 * i for i in range(1, 11)]  # ten aliases of one owner

HARNESS = r"""
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t);
int recomp_dispatch_init(void);
extern volatile uint64_t g_recomp_alias_icall_count;
extern volatile uint64_t g_recomp_alias_icall_hits[];
extern const uint32_t g_recomp_alias_icall_map[][2];
extern const uint32_t g_recomp_alias_icall_entries;

static int owner_runs, other_runs;
void f_owner(void) { owner_runs++; }
void f_other(void) { other_runs++; }

int main(void) {
    uint32_t va;
    int k;
    /* A lookup alone is not a call. */
    for (va = 0x00011000u; va < 0x00011100u; va += 4) (void)recomp_lookup(va);
    if (g_recomp_alias_icall_count) { printf("FAIL: lookup counted\n"); return 1; }
    recomp_lookup(0x00011000u)();
    recomp_lookup(0x00012000u)();
    if (g_recomp_alias_icall_count) { printf("FAIL: plain call counted\n"); return 1; }
    /* Every alias reaches the owner; the first is called three times. */
    for (k = 0; k < 3; k++) recomp_lookup(0x00011010u)();
    for (va = 0x00011020u; va <= 0x000110A0u; va += 0x10) recomp_lookup(va)();
    if (!recomp_dispatch_init()) { printf("FAIL: init\n"); return 1; }
    recomp_lookup(0x00011010u)();
    if (owner_runs != 1 + 3 + 9 + 1 || other_runs != 1) {
        printf("FAIL: runs owner=%d other=%d\n", owner_runs, other_runs); return 1; }
    if (g_recomp_alias_icall_count != 13) {
        printf("FAIL: count=%llu\n", (unsigned long long)g_recomp_alias_icall_count); return 1; }
    if (g_recomp_alias_icall_entries != 10 || g_recomp_alias_icall_hits[0] != 4
            || g_recomp_alias_icall_map[0][0] != 0x00011010u
            || g_recomp_alias_icall_map[0][1] != 0x00011000u) {
        printf("FAIL: per-alias table\n"); return 1; }
    printf("OK\n");
    return 0;
}
"""


def _find_cc():
    return shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


def _write(tmp, alias_owner):
    translations = sorted(
        [(OWNER, "f_owner", None), (0x00012000, "f_other", None)]
        + [(a, "sub_%08X" % a, None) for a in ALIASES])
    redirect = {a: "f_owner" for a in ALIASES}
    with open(os.path.join(tmp, "recomp_funcs.h"), "w") as f:
        f.write("#include <stdint.h>\n#include <stddef.h>\n"
                "void f_owner(void); void f_other(void);\n")
    disp = os.path.join(tmp, "recomp_dispatch.c")
    BatchTranslator._write_dispatch_table(
        object.__new__(BatchTranslator), translations, disp, "recomp_funcs.h",
        alias_redirect=redirect, alias_owner=alias_owner)
    return disp


class AliasObserverTest(unittest.TestCase):
    def test_alias_calls_are_counted_and_the_first_eight_printed(self):
        cc = _find_cc()
        if not cc:
            self.skipTest("no C compiler on PATH")
        with tempfile.TemporaryDirectory() as tmp:
            disp = _write(tmp, {a: OWNER for a in ALIASES})
            harness = os.path.join(tmp, "harness.c")
            with open(harness, "w") as f:
                f.write(HARNESS)
            exe = os.path.join(tmp, "t.exe")
            r = subprocess.run([cc, "-Wall", "-Werror", "-Wno-unused-function",
                                "-I", tmp, harness, disp, "-o", exe],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-2000:])
            r = subprocess.run([exe], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout.strip(), "OK", r.stdout)
        printed = [l for l in r.stderr.splitlines() if l.startswith("[ALIAS-ICALL]")]
        self.assertEqual(printed, [
            "[ALIAS-ICALL] target=0x%08X owner=0x%08X" % (a, OWNER)
            for a in ALIASES[:8]])

    def test_without_owner_vas_the_entries_name_the_owner_directly(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = open(_write(tmp, None)).read()
        self.assertIn("{ 0x00011010u, (recomp_func_t)f_owner },", src)
        self.assertNotIn("recomp_alias_00011010", src)
        # The counter is always defined, so a debugger can find it by name.
        self.assertIn("volatile uint64_t g_recomp_alias_icall_count = 0;", src)


if __name__ == "__main__":
    unittest.main()
