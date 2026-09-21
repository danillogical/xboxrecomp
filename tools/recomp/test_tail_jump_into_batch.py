"""A jump that leaves one entry but lands inside another is not a tail call.

The function database contains tail_jump_alias entries that begin *inside* a
function. A tail jump in the CRT lands mid-body to share the victim's tail, so
the detector records the target as an entry of its own whose end is the
enclosing function's end -- 2539 of JSRF's 2548 overlapping entries share the
parent's end exactly.

Once such an alias is translated, its own span starts late, so a jump back into
the parent's earlier blocks is outside the alias span. _is_external_target said
"external", and _lift_jmp emitted

    g_seh_ebp = ebp; sub_X(); return;

for a target that was never a function. Two failures from one line: the guest
frame is abandoned in a spot that expects to continue with it, and the symbol
does not exist, so the generated tree fails to link. JSRF hit 473 of them, all
pointing at correct generated code.

JSRF's own case, sub_000110D0, is the shape to keep in mind. In
src/recomp/gen/recomp_0000.c the alias sub_00011105 is emitted at line 418; the
label loc_000110D0 sits at line 183 inside sub_000110A0; and line 464 read
`... sub_000110D0(); return;` -- a call to a label that was 281 lines above it
in the same file.
"""
import unittest

from tools.recomp.lifter import Lifter


class _Insn:
    def __init__(self, mnemonic, op_str="", jump_target=None):
        self.mnemonic = mnemonic
        self.op_str = op_str
        self.jump_target = jump_target
        self.operands = []
        self.address = 0
        self.is_jump = mnemonic == "jmp"
        self.is_cond_jump = False


class TailJumpIntoBatchTest(unittest.TestCase):

    def _lifter(self, func_start, func_end, spans):
        lifter = Lifter(func_db={}, label_db={}, abi_db={}, xbe_data=b"",
                        manual_functions=set())
        lifter.func_start = func_start
        lifter.func_end = func_end
        lifter.set_batch_spans(spans)
        return lifter

    def test_target_inside_the_parent_is_not_external(self):
        """The alias begins at 0x11105; its parent body starts at 0x110A0.

        0x110D0 is inside the parent and outside the alias, and it is the case
        that produced `sub_000110D0(); return;` in the JSRF tree.
        """
        lifter = self._lifter(0x0011105, 0x0011214,
                              [(0x00110A0, 0x0011214), (0x0011105, 0x0011214)])
        self.assertFalse(lifter._is_external_target(0x00110D0))

    def test_the_jump_lifts_as_a_goto(self):
        lifter = self._lifter(0x0011105, 0x0011214,
                              [(0x00110A0, 0x0011214), (0x0011105, 0x0011214)])
        out = lifter._lift_jmp(_Insn("jmp", jump_target=0x00110D0), [])
        joined = "\n".join(out)
        self.assertIn("goto loc_000110D0;", joined)
        self.assertNotIn("sub_000110D0()", joined)

    def test_a_real_tail_call_is_still_a_tail_call(self):
        """Coverage must not collapse into 'never tail call'.

        A jump to an address no entry in the batch covers is genuinely
        external and still has to reuse the caller's frame.
        """
        lifter = self._lifter(0x0011105, 0x0011214,
                              [(0x00110A0, 0x0011214), (0x0011105, 0x0011214)])
        self.assertTrue(lifter._is_external_target(0x00123456))
        out = lifter._lift_jmp(_Insn("jmp", jump_target=0x00123456), [])
        joined = "\n".join(out)
        self.assertIn("g_seh_ebp = ebp;", joined)
        self.assertIn("return;", joined)

    def test_without_batch_spans_the_old_behaviour_is_unchanged(self):
        """A caller that installs no spans keeps the single-function test.

        translate_batch_split is the only caller that installs them; the
        single-function path has no batch to consult and must still treat a
        target outside its own span as external.
        """
        lifter = Lifter(func_db={}, label_db={}, abi_db={}, xbe_data=b"",
                        manual_functions=set())
        lifter.func_start = 0x0011105
        lifter.func_end = 0x0011214
        self.assertTrue(lifter._is_external_target(0x00110D0))

    def test_a_target_inside_the_current_function_is_never_external(self):
        lifter = self._lifter(0x00110A0, 0x0011214, [(0x00110A0, 0x0011214)])
        self.assertFalse(lifter._is_external_target(0x00110D0))

    def test_spans_are_sorted_and_empty_ones_dropped(self):
        lifter = Lifter(func_db={}, label_db={}, abi_db={}, xbe_data=b"",
                        manual_functions=set())
        lifter.set_batch_spans([(0x2000, 0x3000), (0x1000, 0x1000),
                                (0x1500, 0x1800)])
        self.assertEqual(lifter.batch_spans, [(0x1500, 0x1800),
                                              (0x2000, 0x3000)])
        self.assertEqual(lifter._batch_span_starts, [0x1500, 0x2000])

    def test_the_last_span_is_inclusive_of_its_start_only(self):
        """A target exactly at a span's `end` is outside; at `start` is inside.

        The current function is placed elsewhere (0x7000) so the single-span
        check does not answer first, which is what makes this about the batch
        lookup rather than about func_start/func_end.
        """
        lifter = self._lifter(0x7000, 0x7010, [(0x4000, 0x5000)])
        self.assertTrue(lifter._is_external_target(0x5000))
        lifter2 = self._lifter(0x7000, 0x7010, [(0x5000, 0x6000)])
        self.assertFalse(lifter2._is_external_target(0x5000))
        # one byte before the end is inside
        self.assertFalse(lifter2._is_external_target(0x5FFF))


if __name__ == "__main__":
    unittest.main()
