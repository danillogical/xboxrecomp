"""An MSVC table indexed from one must not be discarded for its slot zero."""
import unittest
from unittest.mock import patch

from . import config
from .config import va_to_file_offset
from .translator import FunctionTranslator

TABLE = 0x00020100
LOWER, UPPER = 0x00020000, 0x00020100


class JumpTableSlotOneTest(unittest.TestCase):
    def setUp(self):
        sections = patch.object(config, "_SECTIONS", [
            config.Section(".text", LOWER, 0x200, 0, 0x200, True),
        ])
        sections.start()
        self.addCleanup(sections.stop)

    def test_slot_zero_overlap_retries_from_slot_one(self):
        image = bytearray(b"\xCC" * 0x200)
        # Slot zero overlaps the preceding instruction; slots one to three are cases.
        entries = [0xCCCCCCCC, LOWER + 0x10, LOWER + 0x20, LOWER + 0x30]
        offset = va_to_file_offset(TABLE)
        for index, target in enumerate(entries):
            image[offset + index * 4:offset + index * 4 + 4] = target.to_bytes(4, "little")
        translator = FunctionTranslator(bytes(image), {})
        self.assertEqual(translator._read_local_jump_table(TABLE, LOWER, UPPER), entries[1:])


if __name__ == "__main__":
    unittest.main()
