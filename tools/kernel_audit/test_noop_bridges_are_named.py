"""
Every bridge that does nothing says so the first time it runs.
Run: py -3 tools/kernel_audit/test_noop_bridges_are_named.py

A bridge whose body only discards its arguments and returns 0 keeps an import
from faulting, but a title can wait forever for the work it never does, and
nothing in the log would show the call. BRIDGE_NOOP (kernel_bridge.c) prints
one line the first time each such bridge runs. This keeps a new do-nothing
bridge from being added without it.
"""
import os
import re

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
BRIDGE = os.path.join(ROOT, "src", "kernel", "kernel_bridge.c")

FUNCTION = re.compile(r"^static void (bridge_(\w+))\(void\)\n\{\n(.*?)\n\}", re.M | re.S)
NOOP_STATEMENT = re.compile(r"\(void\)STACK_ARG\(\d+\);|g_eax = 0;(\s*/\*.*\*/)?")
# Unused fallback with no ordinal of its own; nothing routes to it.
EXEMPT = {"generic_stub"}


def _statements(body):
    lines = [line.strip() for line in body.split("\n")]
    return [line for line in lines if line and not line.startswith(("/*", "*", "//"))]


def test_every_noop_bridge_is_named():
    with open(BRIDGE, encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    unnamed, named = [], 0
    for match in FUNCTION.finditer(src):
        name, body = match.group(2), match.group(3)
        stmts = _statements(body)
        marker = f'BRIDGE_NOOP("{name}");'
        if stmts and stmts[0] == marker:
            named += 1
            rest = stmts[1:]
            assert rest and all(NOOP_STATEMENT.fullmatch(s) for s in rest), (
                f"bridge_{name} is marked BRIDGE_NOOP but does work; drop the marker")
            continue
        if name in EXEMPT:
            continue
        if stmts and all(NOOP_STATEMENT.fullmatch(s) for s in stmts) \
                and any(s.startswith("g_eax") for s in stmts):
            unnamed.append(f"bridge_{name}")
    assert named >= 60, f"found only {named} marked bridges; did the pattern change?"
    assert not unnamed, "do-nothing bridges without BRIDGE_NOOP:\n  " + "\n  ".join(unnamed)
    print(f"ok  every_noop_bridge_is_named ({named} bridges)")


if __name__ == "__main__":
    test_every_noop_bridge_is_named()
    print("all passed")
