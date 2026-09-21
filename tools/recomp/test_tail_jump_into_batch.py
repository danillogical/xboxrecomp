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

    def _lifter(self, func_start, func_end, spans, entry_methods=None):
        """Build a lifter whose func_db marks the given entries.

        entry_methods maps address -> detection_method. Anything absent from it
        but present as a span start gets a normal method, so a caller only has
        to name the aliases.
        """
        methods = dict(entry_methods or {})
        func_db = {}
        for start, _end in spans:
            func_db[start] = {
                "name": f"sub_{start:08X}",
                "end": _end,
                "detection_method": methods.get(start, "call_target"),
            }
        lifter = Lifter(func_db=func_db, label_db={}, abi_db={}, xbe_data=b"",
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
                              [(0x00110A0, 0x0011214), (0x0011105, 0x0011214)],
                              {0x0011105: "tail_jump_alias"})
        self.assertFalse(lifter._is_external_target(0x00110D0))

    def test_the_jump_lifts_as_a_goto(self):
        lifter = self._lifter(0x0011105, 0x0011214,
                              [(0x00110A0, 0x0011214), (0x0011105, 0x0011214)],
                              {0x0011105: "tail_jump_alias"})
        out = lifter._lift_jmp(_Insn("jmp", jump_target=0x00110D0), [])
        joined = "\n".join(out)
        self.assertIn("goto loc_000110D0;", joined)
        self.assertNotIn("sub_000110D0()", joined)

    def test_a_real_entry_inside_a_batch_span_is_still_a_tail_call(self):
        """A function start is inside its own span, and must stay a tail call.

        This is the 456-forward-jump class. `0x00011C0E jmp 0x12890` targets
        `sub_00012890`, a real `call_target` entry also reached by a direct
        `RECOMP_ABI_CALL`. Asking only "is it inside some batch span" made the
        generator emit `goto loc_00012890`, which is a cross-function goto --
        invalid C -- so the label validator deleted the jump.
        """
        lifter = self._lifter(0x0011BE0, 0x0011C13,
                              [(0x0011BE0, 0x0011C13), (0x0012890, 0x00128B1)])
        self.assertTrue(lifter._is_external_target(0x0012890))
        out = lifter._lift_jmp(_Insn("jmp", jump_target=0x0012890), [])
        joined = "\n".join(out)
        self.assertIn("g_seh_ebp = ebp;", joined)
        self.assertIn("sub_00012890();", joined)
        self.assertNotIn("goto loc_00012890", joined)

    def test_an_alias_target_in_batch_stays_a_goto(self):
        """The discriminator is the *method*, not merely being an entry.

        An alias is an entry too, and it is exactly what must not become a
        tail call: it has no body of its own.
        """
        lifter = self._lifter(0x0011BE0, 0x0011C13,
                              [(0x0011BE0, 0x0011C13), (0x0012890, 0x00128B1)],
                              {0x0012890: "tail_jump_alias"})
        self.assertFalse(lifter._is_external_target(0x0012890))

    def test_a_real_tail_call_is_still_a_tail_call(self):
        """Coverage must not collapse into 'never tail call'.

        A jump to an address no entry in the batch covers is genuinely
        external and still has to reuse the caller's frame.
        """
        lifter = self._lifter(0x0011105, 0x0011214,
                              [(0x00110A0, 0x0011214), (0x0011105, 0x0011214)],
                              {0x0011105: "tail_jump_alias"})
        self.assertTrue(lifter._is_external_target(0x00123456))
        out = lifter._lift_jmp(_Insn("jmp", jump_target=0x00123456), [])
        joined = "\n".join(out)
        self.assertIn("g_seh_ebp = ebp;", joined)
        self.assertIn("return;", joined)

    def test_a_mid_body_target_with_no_entry_is_intra_batch(self):
        """Inside a span, not an entry at all -- a fragment interior.

        There is no symbol to call, but the owner's body emits the label.
        """
        lifter = self._lifter(0x0011105, 0x0011214, [(0x00110A0, 0x0011214)])
        self.assertFalse(lifter._is_external_target(0x00110D0))

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

        Note what `0x5000` is now: it is a span *start*, so it is a real entry
        and the answer is True (tail call) for a reason that has nothing to do
        with the batch bounds. To test the bounds themselves the probe has to
        be an address inside the span that is not an entry, which is the
        mid-body case below.
        """
        lifter = self._lifter(0x7000, 0x7010, [(0x4000, 0x5000)])
        # 0x5000 is this span's end and no other span starts there: outside.
        self.assertTrue(lifter._is_external_target(0x5000))
        lifter2 = self._lifter(0x7000, 0x7010, [(0x5000, 0x6000)])
        # one byte before the end is inside the span and not an entry
        self.assertFalse(lifter2._is_external_target(0x5FFF))
        # the start itself is an entry, so it is external as a tail call
        self.assertTrue(lifter2._is_external_target(0x5000))


if __name__ == "__main__":
    unittest.main()
