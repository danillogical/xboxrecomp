import unittest

from .disasm import BasicBlock, Instruction
from .lifter import Lifter, lift_basic_block


def _lift(*instructions):
    lifted, _ = lift_basic_block(
        Lifter(), BasicBlock(start=0, instructions=list(instructions)))
    return "\n".join(lifted)


class DirectionFlagLifterTest(unittest.TestCase):
    """EFLAGS.DF decides which way the string instructions walk.

    It used to lift to a comment, so every one of them walked forwards. The
    instruction that exposed it is MSVC's strrchr, which is `std` followed by
    `repne scasb` from the terminator *backwards*; scanning forwards ran off
    the end of the string and returned a pointer into whatever followed it.
    """

    def test_cld_and_std_assign_the_flag(self):
        generated = _lift(
            Instruction(0, 1, "std", "", "fd"),
            Instruction(1, 1, "cld", "", "fc"),
        )

        self.assertIn("g_df = 1;", generated)
        self.assertIn("g_df = 0;", generated)

    def test_repne_scasb_steps_by_the_direction_flag(self):
        generated = _lift(Instruction(0, 2, "repne scasb", "", "f2ae"))

        self.assertIn("RECOMP_DF_STEP(1)", generated)
        # The whole point: no unconditional forward step survives.
        self.assertNotIn("edi++;", generated)

    def test_rep_movsb_uses_the_block_copy_only_when_it_cannot_overlap(self):
        generated = _lift(Instruction(0, 2, "rep movsb", "", "f3a4"))

        # memcpy is still there for the common case, but guarded: it is only
        # reached when the ranges provably do not overlap.
        self.assertIn("memcpy(_d, _s, _n)", generated)
        self.assertIn("_d + _n <= _s || _s + _n <= _d", generated)

    def test_rep_movsb_propagates_an_overlapping_forward_copy(self):
        """The LZ run case: distance 1, length N repeats one byte N times.

        rep movs copies an element at a time, so a forward copy whose
        destination is inside the source reads what it has just written.
        memcpy is undefined there and a vectorised one reads ahead, which
        silently corrupts every run a decompressor emits.
        """
        generated = _lift(Instruction(0, 2, "rep movsb", "", "f3a4"))

        # An explicit ascending byte loop, not memmove: memmove would give the
        # copy-through-a-temporary answer, which is a different result.
        self.assertIn("for (_i = 0; _i < _n; _i++) _d[_i] = _s[_i];", generated)
        self.assertNotIn("memmove", generated)

    def test_rep_movsb_backward_case_is_still_a_loop(self):
        generated = _lift(Instruction(0, 2, "rep movsb", "", "f3a4"))

        self.assertIn("MEM8(edi - _i) = MEM8(esi - _i)", generated)

    def test_rep_movsd_overlap_steps_by_dwords(self):
        # Element granularity matters: a dword-at-a-time propagation is not
        # the same sequence of bytes as a byte-at-a-time one.
        generated = _lift(Instruction(0, 2, "rep movsd", "", "f3a5"))

        self.assertIn("MEM32(edi + _i*4) = MEM32(esi + _i*4)", generated)
        self.assertIn("_d + _n <= _s || _s + _n <= _d", generated)

    def test_unprefixed_string_ops_step_by_the_direction_flag(self):
        for mnemonic, size in (("movsb", 1), ("stosw", 2), ("lodsd", 4)):
            with self.subTest(mnemonic=mnemonic):
                generated = _lift(Instruction(0, 1, mnemonic, "", "a4"))
                self.assertIn(f"RECOMP_DF_STEP({size})", generated)

    def test_rep_movs_avoids_the_block_copy_over_a_device_window(self):
        """A copy over MMIO has to be walked element by element.

        MMIO is emulated by decoding the faulting *guest* instruction, so a
        copy that reaches a host memcpy raises the fault inside that library
        instead, at a RIP where the decoder sees the library's own encoding --
        an AVX form, which it cannot read, so the fault escapes and the
        process dies. JSRF's first one is `rep movsd` with ESI = 0xFE830200
        (the APU's GP window) and EDI a RAM buffer: the ranges provably do not
        overlap, so the block copy was taken and the title died at a RIP in
        the CRT (logs/runs/20260922-162546-668-a2c-175300).
        """
        for mnemonic, element in (("rep movsb", "MEM8"), ("rep movsw", "MEM16"),
                                  ("rep movsd", "MEM32")):
            with self.subTest(mnemonic=mnemonic):
                generated = _lift(Instruction(0, 2, mnemonic, "", "f3a5"))

                # Both ends are checked: a device window on either side of the
                # copy is unemulatable through memcpy.
                self.assertIn("recomp_range_is_mmio(edi, _n)", generated)
                self.assertIn("recomp_range_is_mmio(esi, _n)", generated)
                # ...and the element-wise fallback it lands on is still there.
                self.assertIn(f"{element}(", generated)

    def test_rep_stosb_avoids_memset_over_a_device_window(self):
        generated = _lift(Instruction(0, 2, "rep stosb", "", "f3aa"))

        self.assertIn("recomp_range_is_mmio(edi, ecx)", generated)
        self.assertIn("MEM8(edi + _i) = LO8(eax)", generated)


if __name__ == "__main__":
    unittest.main()
