"""A conditional branch into another function's interior needs an alias.

_pass_cond_branch_orphans is the pass that rescues a jcc whose target is not
claimed by anyone. It handles two shapes: the target sits in a gap (the pass
creates the alias itself), and the target sits inside *another* function (the
target is a shared epilogue block, so it must become an alias of the function
that owns those bytes -- it must not become a function start, because a start
there would clamp the owner's body in half).

That second shape used to be skipped with a comment saying "handled above",
which was false: _pass_data_ptr_targets only creates aliases for targets it
found in a *data table*, and a branch target is not a table entry. Nothing
created the alias, so the branch was lifted as `goto loc_X` against a label
the owning function emits in a *different* function. That is invalid C and the
label validator deleted it -- silently, leaving the branch falling through.

The pass also cannot live inside _pass_tail_jump_targets, which runs in a loop
that rebuilds `functions` every round: a source body discovered by a later
round is never seen. The pass therefore runs once, after every body-creating
pass, and its body list includes **aliases**, not just `self.functions` -- an
alias owns real bytes, and JSRF's source function (sub_0002DBE0) is itself a
tail_jump_alias.

JSRF: 309 deleted jumps were this case, 322 jcc and 34 jmp, 247 crossing a
function boundary. The worked example is sub_0002DBE0 (ends `ret` at 0x2DF75)
branching to 0x2E00C, a shared epilogue block inside sub_0002DF76.
"""
import bisect
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import FunctionDetector  # noqa: E402


class _Insn:
    def __init__(self, addr, size=1, is_cond_jump=False, target=None):
        self.address = addr
        self.size = size
        self.end_address = addr + size
        self.is_ret = False
        self.is_cond_jump = is_cond_jump
        self.jump_target = target


class _Section:
    executable = True


class _Func:
    def __init__(self, start, end):
        self.start = start
        self.end = end


class _Image:
    def get_section_at_va(self, addr):
        return _Section()


class _Engine:
    def __init__(self, insns):
        self.instructions = {i.address: i for i in insns}

    def probes_as_returning_body(self, addr, max_insns=256):
        return True

    def probes_as_vcall_thunk(self, addr):
        return False

    def block_tail_jump(self, addr):
        return None


def _detector(insns, functions, aliases=None):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns)
    det.image = _Image()
    det.functions = {f.start: f for f in functions}
    det._candidates = {}
    det._alias_entries = dict(aliases or {})
    det.added = []
    det._add_candidate = lambda addr, conf, why: det.added.append((addr, why))
    return det


def _bodies(det):
    """The body list _pass_cond_branch_orphans_after builds."""
    return sorted(set(
        [(f.start, f.end) for f in det.functions.values()]
        + [(a, e) for a, e in det._alias_entries.items() if e > a]))


class CondBranchOrphanTest(unittest.TestCase):
    def test_a_branch_into_another_function_creates_an_alias(self):
        """The JSRF shape: source ends in ret, target is the owner's epilogue."""
        # sub_0002DBE0 owns 0x2DBE0..0x2DF75 and ends in ret; the branch is
        # 0x2DDF2 `je 0x2E00C`, inside it. sub_0002DF76 owns 0x2DF76..0x2E0C0
        # and 0x2E00C is its shared epilogue block.
        funcs = [_Func(0x0002DBE0, 0x0002DF75), _Func(0x0002DF76, 0x0002E0C0)]
        insns = [_Insn(0x0002DDF2, size=2, is_cond_jump=True, target=0x0002E00C),
                 _Insn(0x0002E00C, size=1)]
        det = _detector(insns, funcs)
        bodies = _bodies(det)
        self.assertTrue(det._pass_cond_branch_orphans(bodies, [b[0] for b in bodies]))
        self.assertEqual(det._alias_entries.get(0x0002E00C), 0x0002E0C0)

    def test_the_target_does_not_become_a_function_start(self):
        """A start at 0x2E00C would clamp sub_0002DF76's body in half."""
        funcs = [_Func(0x0002DBE0, 0x0002DF75), _Func(0x0002DF76, 0x0002E0C0)]
        insns = [_Insn(0x0002DDF2, size=2, is_cond_jump=True, target=0x0002E00C),
                 _Insn(0x0002E00C, size=1)]
        det = _detector(insns, funcs)
        bodies = _bodies(det)
        det._pass_cond_branch_orphans(bodies, [b[0] for b in bodies])
        self.assertEqual(det.added, [])
        self.assertNotIn(0x0002E00C, det.functions)
        self.assertEqual(det.functions[0x0002DF76].end, 0x0002E0C0)

    def test_a_source_that_is_an_alias_is_still_a_source(self):
        """sub_0002DBE0 is a tail_jump_alias; self.functions omits it.

        Without aliases in the body list the bisect lands on the function
        *below* the source (0x2DA70, ending 0x2DBD2), concludes the source is
        outside any function, and skips every branch.
        """
        funcs = [_Func(0x0002DA70, 0x0002DBD2)]          # the function below
        aliases = {0x0002DBE0: 0x0002DF75}               # the source itself
        insns = [_Insn(0x0002DDF2, size=2, is_cond_jump=True, target=0x0002E00C),
                 _Insn(0x0002E00C, size=1)]
        det = _detector(insns, funcs, aliases=aliases)
        funcs_span = [_Func(0x0002DF76, 0x0002E0C0)]
        det.functions.update({f.start: f for f in funcs_span})

        # With aliases in the body list the source resolves.
        bodies = _bodies(det)
        starts = [b[0] for b in bodies]
        i = bisect.bisect_right(starts, 0x0002DDF2) - 1
        self.assertEqual(bodies[i], (0x0002DBE0, 0x0002DF75))
        self.assertLess(0x0002DDF2, bodies[i][1])

        self.assertTrue(det._pass_cond_branch_orphans(bodies, starts))
        self.assertEqual(det._alias_entries.get(0x0002E00C), 0x0002E0C0)

    def test_without_aliases_in_the_body_list_the_branch_is_missed(self):
        """The regression this pass fixed: the source looks out of range."""
        funcs = [_Func(0x0002DA70, 0x0002DBD2), _Func(0x0002DF76, 0x0002E0C0)]
        aliases = {0x0002DBE0: 0x0002DF75}
        insns = [_Insn(0x0002DDF2, size=2, is_cond_jump=True, target=0x0002E00C),
                 _Insn(0x0002E00C, size=1)]
        det = _detector(insns, funcs, aliases=aliases)

        functions_only = sorted((f.start, f.end) for f in det.functions.values())
        starts = [b[0] for b in functions_only]
        i = bisect.bisect_right(starts, 0x0002DDF2) - 1
        self.assertEqual(functions_only[i], (0x0002DA70, 0x0002DBD2))
        self.assertGreaterEqual(0x0002DDF2, functions_only[i][1])  # outside

        det._alias_entries.clear()
        self.assertFalse(det._pass_cond_branch_orphans(functions_only, starts))
        self.assertEqual(det._alias_entries, {})

    def test_an_ordinary_intra_function_branch_is_left_alone(self):
        funcs = [_Func(0x00011000, 0x00011100)]
        insns = [_Insn(0x00011010, size=2, is_cond_jump=True, target=0x00011080)]
        det = _detector(insns, funcs)
        bodies = _bodies(det)
        self.assertFalse(det._pass_cond_branch_orphans(bodies, [b[0] for b in bodies]))
        self.assertEqual(det._alias_entries, {})

    def test_a_target_that_is_not_an_instruction_start_is_rejected(self):
        """A branch into mid-instruction bytes is not code."""
        funcs = [_Func(0x0002DF76, 0x0002E0C0)]
        insns = [_Insn(0x0002DDF2, size=2, is_cond_jump=True, target=0x0002E00D)]
        det = _detector(insns, funcs)
        bodies = _bodies(det)
        self.assertFalse(det._pass_cond_branch_orphans(bodies, [b[0] for b in bodies]))
        self.assertEqual(det._alias_entries, {})

    def test_an_already_known_alias_is_not_re_created(self):
        funcs = [_Func(0x0002DF76, 0x0002E0C0)]
        aliases = {0x0002E00C: 0x0002E0C0}
        insns = [_Insn(0x0002DDF2, size=2, is_cond_jump=True, target=0x0002E00C)]
        det = _detector(insns, funcs, aliases=aliases)
        bodies = _bodies(det)
        self.assertFalse(det._pass_cond_branch_orphans(bodies, [b[0] for b in bodies]))


if __name__ == "__main__":
    unittest.main()
