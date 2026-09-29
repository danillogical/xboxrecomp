"""
Every place the runtime hands a host thread lifted guest code is metered.

Run: py -3 tools/kernel_audit/test_guest_meter_sites.py

The guest concurrency meter (src/kernel/guest_meter.c) counts threads inside
lifted code by bracketing each hand-over: xbox_GuestMeterEnter before the call
into the recompiled function, xbox_GuestMeterRestore after it. A call site
added without the bracket does not fail anything; the meter just under-counts,
which is the one thing a concurrency meter must not do quietly.

The runtime calls a looked-up guest function as `fn();` in these files, so
each such line must sit inside a bracket.
"""

import os
import re

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
SITES = [
    os.path.join(ROOT, "src", "kernel", "kernel_bridge.c"),
    os.path.join(ROOT, "src", "usb", "ohci.c"),
]


def _lines(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read().splitlines()


def test_every_guest_call_is_bracketed():
    bad, seen = [], 0
    for path in SITES:
        lines = _lines(path)
        for i, line in enumerate(lines):
            if line.strip() != "fn();":
                continue
            seen += 1
            before = " ".join(lines[max(0, i - 3):i])
            after = lines[i + 1] if i + 1 < len(lines) else ""
            if "xbox_GuestMeterEnter(" not in before or \
               "xbox_GuestMeterRestore(" not in after:
                bad.append(f"{os.path.relpath(path, ROOT)}:{i + 1}")
    assert seen >= 10, f"found only {seen} guest call sites; did the pattern change?"
    assert not bad, "unmetered guest calls:\n  " + "\n  ".join(bad)
    print(f"ok  every_guest_call_is_bracketed ({seen} sites)")


def test_kernel_dispatch_leaves_and_restores():
    src = "\n".join(_lines(SITES[0]))
    m = re.search(r"static void kernel_thunk_dispatch\(void\)\n\{.*?\n\}", src, re.S)
    assert m, "could not locate kernel_thunk_dispatch"
    body = m.group(0)
    returns = body.count("return;")
    restores = body.count("xbox_GuestMeterRestore(gm, XBOX_GM_KERNEL);")
    assert "xbox_GuestMeterLeave()" in body, "kernel dispatch does not leave guest code"
    assert restores == returns + 1, (
        f"kernel dispatch has {returns} early return(s) but {restores} restore(s)")
    print("ok  kernel_dispatch_leaves_and_restores")


if __name__ == "__main__":
    test_every_guest_call_is_bracketed()
    test_kernel_dispatch_leaves_and_restores()
    print("all passed")
