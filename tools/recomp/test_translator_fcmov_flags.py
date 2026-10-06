"""
A function with an fcmovcc and no flag setter must declare `_flags`.

The lifter falls back to `if (_flags ...)` for an fcmovcc whose flags no
instruction in the function set (the condition came from the caller). The
translator declared `int _flags` only for a jcc, set* or cmov*, so such a
function referenced an undeclared variable and did not compile.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000


def _translate(image):
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="fcmov-flags-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def test_fcmov_without_flag_setter_declares_flags():
    image = (b"\xD9\x44\x24\x04"   # fld dword ptr [esp+4]
             b"\xD9\x44\x24\x08"   # fld dword ptr [esp+8]
             b"\xDA\xC9"           # fcmove st(0), st(1)
             b"\xDD\xD8"           # fstp st(0)
             b"\xC3")              # ret
    code = _translate(image)
    assert "if (_flags /* fcmove" in code, code
    assert "int _flags" in code, code


if __name__ == "__main__":
    test_fcmov_without_flag_setter_declares_flags()
    print("ok  fcmov_without_flag_setter_declares_flags")
