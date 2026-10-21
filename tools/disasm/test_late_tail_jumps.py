"""A tail jump into a gap from a body made after pass 6 registers an alias.

_pass_tail_jump_targets only sees the bodies that exist when the tail-jump
closure runs. An alias body made later, ending in a jmp to a function nothing
calls, had that target lowered to an unresolved-target stub: the whole
function never ran. The shape: Jet Set Radio Future's state handler 0x0005BB31
(a gap prologue, reached through a table) ends in `jmp 0x00054750`.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import FunctionDetector, Function  # noqa: E402

SEC_LO, SEC_HI = 0x1000, 0x1100


class _Insn:
    def __init__(self, addr, ret=False, jcc=None, jmp=None):
        self.address = addr
        self.size = 4
        self.end_address = addr + 4
        self.is_ret = ret
        self.is_jump = jmp is not None
        self.is_cond_jump = jcc is not None
        self.is_branch = jcc is not None or jmp is not None
        self.is_call = False
        self.is_terminator = ret or jmp is not None
        self.jump_target = jcc if jcc is not None else jmp
        self.jump_table = None
        self.mnemonic = "ret" if ret else ("jmp" if jmp is not None else "nop")


class _Section:
    name = "TEXT"
    virtual_addr = SEC_LO
    virtual_size = SEC_HI - SEC_LO
    raw_size = SEC_HI - SEC_LO
    executable = True


class _Image:
    def get_section_at_va(self, addr):
        return _Section() if SEC_LO <= addr < SEC_HI else None


class _Engine:
    jump_tables = {}

    def __init__(self, insns):
        self.instructions = {i.address: i for i in insns}

    def get_instructions_in_range(self, lo, hi):
        return [i for a, i in sorted(self.instructions.items()) if lo <= a < hi]

    def get_instruction(self, addr):
        return self.instructions.get(addr)

    def jump_table_entries(self, *args, **kwargs):
        return []

    def recursive_descent(self, starts, bounds):
        seen, work = set(), list(starts)
        while work:
            addr = work.pop()
            while addr in self.instructions and addr not in seen:
                seen.add(addr)
                insn = self.instructions[addr]
                if insn.jump_target is not None:
                    work.append(insn.jump_target)
                if insn.is_terminator:
                    break
                addr = insn.end_address
        return seen


def _detector(insns, functions, aliases=None):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns)
    det.image = _Image()
    det.functions = {s: Function(start=s, end=e, name=f"sub_{s:08X}") for s, e in functions}
    det._alias_entries = dict(aliases or {})
    det._candidates = {}
    return det


class LateTailJumps(unittest.TestCase):
    def test_jmp_from_a_body_into_a_gap_becomes_an_alias(self):
        # sub_1000 ends in jmp 0x1040; 0x1040..0x1048 is a gap ending in ret.
        insns = [_Insn(0x1000), _Insn(0x1004, jmp=0x1040),
                 _Insn(0x1040), _Insn(0x1044, ret=True),
                 _Insn(0x1080), _Insn(0x1084, ret=True)]
        det = _detector(insns, functions=[(0x1000, 0x1008), (0x1080, 0x1088)])
        self.assertTrue(det._pass_late_tail_jumps())
        self.assertIn(0x1040, det._alias_entries)
        self.assertEqual(det._alias_entries[0x1040], 0x1080)  # up to the next start

    def test_bytes_after_the_last_jmp_are_not_followed(self):
        # sub_1000 ends in jmp 0x1080; the bytes after it, still inside its
        # measured range, decode as a jmp into the gap at 0x1040.
        insns = [_Insn(0x1000, jmp=0x1080), _Insn(0x1004, jmp=0x1040),
                 _Insn(0x1040), _Insn(0x1044, ret=True),
                 _Insn(0x1080), _Insn(0x1084, ret=True)]
        det = _detector(insns, functions=[(0x1000, 0x1008), (0x1080, 0x1088)])
        self.assertFalse(det._pass_late_tail_jumps())
        self.assertNotIn(0x1040, det._alias_entries)

    def test_jmp_from_a_gap_alias_into_a_further_gap(self):
        # A gap alias at 0x1040 tail-jumps to 0x1060, also in the gap.
        insns = [_Insn(0x1040), _Insn(0x1044, jmp=0x1060),
                 _Insn(0x1060), _Insn(0x1064, ret=True),
                 _Insn(0x1080), _Insn(0x1084, ret=True)]
        det = _detector(insns, functions=[(0x1080, 0x1088)], aliases={0x1040: 0x1080})
        self.assertTrue(det._pass_late_tail_jumps())
        self.assertIn(0x1060, det._alias_entries)

    def test_branches_left_to_other_passes(self):
        # jcc into a gap, jmp inside its own body, jmp into another body,
        # jmp to a function start, jmp off an instruction boundary.
        insns = [_Insn(0x1000, jcc=0x1040), _Insn(0x1004, jmp=0x1000),
                 _Insn(0x1008, jmp=0x1084), _Insn(0x100C, jmp=0x1080),
                 _Insn(0x1010, jmp=0x1042),
                 _Insn(0x1040), _Insn(0x1044, ret=True),
                 _Insn(0x1080), _Insn(0x1084), _Insn(0x1088, ret=True)]
        det = _detector(insns, functions=[(0x1000, 0x1014), (0x1080, 0x108C)])
        self.assertFalse(det._pass_late_tail_jumps())
        self.assertEqual(det._alias_entries, {})

    def test_known_targets_are_not_registered_again(self):
        insns = [_Insn(0x1000), _Insn(0x1004, jmp=0x1040),
                 _Insn(0x1040), _Insn(0x1044, ret=True)]
        det = _detector(insns, functions=[(0x1000, 0x1008)], aliases={0x1040: 0x1100})
        self.assertFalse(det._pass_late_tail_jumps())


if __name__ == "__main__":
    unittest.main()
