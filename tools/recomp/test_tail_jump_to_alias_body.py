"""A jump out of a function to an entry with its own symbol is a tail call.

The disassembler's `tail_jump_alias` label says where a tail jump landed, not
whether a body exists there. A regenerated database can label an address
`tail_jump_alias` that the pass still emits as its own function (nothing earlier
to fold it into), or that the project defines by hand. A jump to such an entry
must be lifted as a tail call. Lifted as `goto loc_X` it names a label in
another function, the label validator deletes it, and the jump never runs: the
adjustor thunk `sub dword ptr [esp+4], 4; jmp 0x168230` returned without the
callee's `ret 8` and left the guest stack 8 bytes low.

What stays as it was: a jump to a folded alias (no body of its own) or to the
interior of another body (no symbol at all) has nowhere to be called, so it is
still the goto the validator deletes. Generation now reports every such
deletion, fails when one had a callable target, and `--strict-jumps` fails on
any.

These tests drive the real CLI over a synthetic image, so they exercise the
batch pass that decides which entries keep a body rather than a copy of it.
"""
import json
import re
import struct
import sys

import pytest

from . import __main__ as recomp_main
from . import config
from .lifter import Lifter

BASE = 0x10000

THUNK_TO_OWN_BODY = BASE + 0x000    # sub [esp+4],4; jmp OWN_BODY_ALIAS
OWN_BODY_ALIAS = BASE + 0x020       # tail_jump_alias with nothing to fold into
PARENT = BASE + 0x040               # real body whose interior the alias marks
FOLDED_ALIAS = BASE + 0x050         # tail_jump_alias folded into PARENT
THUNK_TO_FOLDED = BASE + 0x060
THUNK_TO_INTERIOR = BASE + 0x080    # jumps to FOLDED_ALIAS, which has no entry
THUNK_TO_REAL = BASE + 0x0A0
REAL_ENTRY = BASE + 0x0C0
THUNK_TO_MANUAL = BASE + 0x0E0
MANUAL_ALIAS = BASE + 0x100         # tail_jump_alias, hand-written
PARENT_END = BASE + 0x052
SECTION_SIZE = 0x400


def _thunk(at, target):
    """sub dword ptr [esp+4], 4 ; jmp target ; int3 padding to 16 bytes."""
    jmp_at = at + 5
    rel = target - (jmp_at + 5)
    code = bytes.fromhex("836c240404") + b"\xe9" + struct.pack("<i", rel)
    return code + b"\xcc" * (16 - len(code))


def _image():
    img = bytearray(b"\xcc" * SECTION_SIZE)

    def put(va, code):
        img[va - BASE:va - BASE + len(code)] = code

    put(THUNK_TO_OWN_BODY, _thunk(THUNK_TO_OWN_BODY, OWN_BODY_ALIAS))
    put(OWN_BODY_ALIAS, bytes.fromhex("c20800"))          # ret 8
    # test eax,eax; jz FOLDED_ALIAS; nop x12; inc eax; ret
    # The jz gives FOLDED_ALIAS a label inside PARENT.
    put(PARENT, bytes.fromhex("85c0740c") + b"\x90" * 12 + bytes.fromhex("40c3"))
    put(THUNK_TO_FOLDED, _thunk(THUNK_TO_FOLDED, FOLDED_ALIAS))
    put(THUNK_TO_INTERIOR, _thunk(THUNK_TO_INTERIOR, FOLDED_ALIAS))
    put(THUNK_TO_REAL, _thunk(THUNK_TO_REAL, REAL_ENTRY))
    put(REAL_ENTRY, bytes.fromhex("c20800"))
    put(THUNK_TO_MANUAL, _thunk(THUNK_TO_MANUAL, MANUAL_ALIAS))
    put(MANUAL_ALIAS, bytes.fromhex("c3"))
    return bytes(img)


def _entry(start, end, method="call_target"):
    return {"start": f"0x{start:08X}", "end": f"0x{end:08X}",
            "size": end - start, "section": ".text",
            "detection_method": method}


ENTRIES = {
    "thunk_own": _entry(THUNK_TO_OWN_BODY, THUNK_TO_OWN_BODY + 16),
    "own_alias": _entry(OWN_BODY_ALIAS, OWN_BODY_ALIAS + 3, "tail_jump_alias"),
    "parent": _entry(PARENT, PARENT_END, "prologue"),
    "folded": _entry(FOLDED_ALIAS, PARENT_END, "tail_jump_alias"),
    "thunk_folded": _entry(THUNK_TO_FOLDED, THUNK_TO_FOLDED + 16),
    "thunk_interior": _entry(THUNK_TO_INTERIOR, THUNK_TO_INTERIOR + 16),
    "thunk_real": _entry(THUNK_TO_REAL, THUNK_TO_REAL + 16),
    "real": _entry(REAL_ENTRY, REAL_ENTRY + 3),
    "thunk_manual": _entry(THUNK_TO_MANUAL, THUNK_TO_MANUAL + 16),
    "manual_alias": _entry(MANUAL_ALIAS, MANUAL_ALIAS + 1, "tail_jump_alias"),
}


@pytest.fixture(autouse=True)
def layout(monkeypatch):
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", BASE, SECTION_SIZE, 0, SECTION_SIZE, True)])
    # The synthetic image is raw bytes, not an XBE header.
    monkeypatch.setattr(config, "configure_from_xbe", lambda path: None)


def _generate(tmp_path, capsys, keys, manual=(), extra_args=()):
    """Run the CLI's split pass over the named entries.

    Returns (exit code or 0, stderr, gen dir).
    """
    (tmp_path / "default.xbe").write_bytes(_image())
    funcs = tmp_path / "functions.json"
    funcs.write_text(json.dumps([dict(ENTRIES[k]) for k in keys]))
    argv = ["recomp", str(tmp_path / "default.xbe"),
            "--functions", str(funcs), "--skip-binary-check",
            "--icall-sites", str(tmp_path / "none.json"),
            "--all", "--split", "1000",
            "--gen-dir", str(tmp_path / "gen"),
            "-o", str(tmp_path / "out")]
    if manual:
        mf = tmp_path / "manual.json"
        mf.write_text(json.dumps([f"0x{a:08X}" for a in manual]))
        argv += ["--manual-functions", str(mf)]
    argv += list(extra_args)
    old = sys.argv
    sys.argv = argv
    code = 0
    try:
        recomp_main.main()
    except SystemExit as exc:
        code = exc.code or 0
    finally:
        sys.argv = old
    return code, capsys.readouterr().err, tmp_path / "gen"


def _function(gen_dir, start):
    """The emitted C of sub_<start>, or None when no body was emitted."""
    text = "".join(p.read_text(encoding="utf-8")
                   for p in sorted(gen_dir.glob("recomp_*.c")))
    m = re.search(rf"\nvoid sub_{start:08X}\(void\)\n\{{.*?\n\}}\n", text, re.S)
    return m.group(0) if m else None


def test_jump_to_an_alias_that_kept_a_body_is_a_tail_call(tmp_path, capsys):
    code, err, gen = _generate(tmp_path, capsys, ["thunk_own", "own_alias"])
    thunk = _function(gen, THUNK_TO_OWN_BODY)
    # Premise: the alias kept a body, so there is a symbol to call.
    assert _function(gen, OWN_BODY_ALIAS) is not None, err
    assert f"sub_{OWN_BODY_ALIAS:08X}(); return;" in thunk, thunk
    assert "(void)0" not in thunk, thunk
    assert "dead code" not in thunk, thunk
    # The decrement before the jump is still emitted, then control leaves.
    assert thunk.index("MEM32(esp + 4)") < thunk.index(
        f"sub_{OWN_BODY_ALIAS:08X}();")
    assert code == 0, err
    assert "JUMPS:" not in err, err


def test_jump_to_a_hand_written_alias_is_a_manual_tail_call(tmp_path, capsys):
    code, err, gen = _generate(tmp_path, capsys,
                               ["thunk_manual", "manual_alias"],
                               manual=[MANUAL_ALIAS])
    thunk = _function(gen, THUNK_TO_MANUAL)
    assert f"RECOMP_ITAIL(0x{MANUAL_ALIAS:08X}u); return;" in thunk, thunk
    assert "(void)0" not in thunk, thunk
    assert code == 0, err


def test_jump_to_an_ordinary_entry_is_still_a_tail_call(tmp_path, capsys):
    """Control: the common case worked before and must keep working."""
    code, err, gen = _generate(tmp_path, capsys, ["thunk_real", "real"])
    thunk = _function(gen, THUNK_TO_REAL)
    assert f"sub_{REAL_ENTRY:08X}(); return;" in thunk, thunk
    assert code == 0, err


@pytest.mark.parametrize("keys", [
    pytest.param(["parent", "folded", "thunk_folded"], id="folded-alias"),
    pytest.param(["parent", "thunk_interior"], id="fragment-interior"),
])
def test_jump_into_another_body_keeps_the_old_handling(tmp_path, capsys, keys):
    """No symbol at the target: the goto is deleted as before, and reported."""
    thunk_at = THUNK_TO_FOLDED if "thunk_folded" in keys else THUNK_TO_INTERIOR
    code, err, gen = _generate(tmp_path, capsys, keys)
    thunk = _function(gen, thunk_at)
    assert "(void)0; /* goto loc_%08X - dead code, label not in function */" \
        % FOLDED_ALIAS in thunk, thunk
    assert f"sub_{FOLDED_ALIAS:08X}();" not in thunk, thunk
    # Premise: the folded alias has no body; the label lives in the parent.
    assert _function(gen, FOLDED_ALIAS) is None
    assert f"loc_{FOLDED_ALIAS:08X}:" in _function(gen, PARENT)
    # Reported, but not an error: nothing callable was lost.
    assert "JUMPS: 1 jump(s) deleted as dead code" in err, err
    assert f"sub_{thunk_at:08X} -> 0x{FOLDED_ALIAS:08X}  no symbol at target" \
        in err, err
    assert "ERROR callable target" not in err, err
    assert code == 0, err


@pytest.mark.parametrize("keys", [
    pytest.param(["parent", "folded", "thunk_folded"], id="folded-alias"),
    pytest.param(["parent", "thunk_interior"], id="fragment-interior"),
])
def test_strict_jumps_fails_on_a_remaining_deleted_jump(tmp_path, capsys, keys):
    code, err, _ = _generate(tmp_path, capsys, keys,
                             extra_args=["--strict-jumps"])
    assert code == 1, (code, err)
    assert "--strict-jumps: 1 jump(s) deleted as dead code" in err, err


def test_strict_jumps_passes_when_nothing_was_deleted(tmp_path, capsys):
    code, err, _ = _generate(tmp_path, capsys, ["thunk_own", "own_alias"],
                             extra_args=["--strict-jumps"])
    assert code == 0, (code, err)
    assert "JUMPS:" not in err, err


def test_a_deleted_jump_with_a_callable_target_fails_generation(
        tmp_path, capsys, monkeypatch):
    """The backstop: should a lifter regress, the deletion cannot go quiet.

    The jump is forced back to a goto, as the pre-fix lifter emitted it. The
    validator deletes it and the target has a body, which must be an error
    even without --strict-jumps.
    """
    monkeypatch.setattr(Lifter, "_is_external_target", lambda self, addr: False)
    code, err, _ = _generate(tmp_path, capsys, ["thunk_own", "own_alias"])
    assert code == 1, (code, err)
    assert f"sub_{THUNK_TO_OWN_BODY:08X} -> 0x{OWN_BODY_ALIAS:08X}  " \
        "ERROR callable target" in err, err
    assert "1 jump(s) to a function with its own symbol were deleted" in err, err


def test_emitted_entries_reads_back_every_body_but_no_folded_alias(
        tmp_path, capsys):
    from .translator import emitted_entries
    _, err, gen = _generate(tmp_path, capsys,
                            ["thunk_own", "own_alias", "parent", "folded"])
    assert emitted_entries(str(gen)) == {
        THUNK_TO_OWN_BODY, OWN_BODY_ALIAS, PARENT}, err
