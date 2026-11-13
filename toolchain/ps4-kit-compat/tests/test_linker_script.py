import importlib.util
from pathlib import Path
import unittest

MODULE = Path(__file__).parents[1] / "generate-link-script.py"
spec = importlib.util.spec_from_file_location("ps4_linker", MODULE)
ps4_linker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ps4_linker)


class LinkerScriptTests(unittest.TestCase):
    def setUp(self):
        self.source = """SECTIONS
{
  .init_array : { *(.init_array); }
  .data : { *(.data); }
  .tdata : { *(.tdata); }
  .tbss : { *(.tbss); }
}
"""

    def test_priority_arrays_and_suffixed_data_are_collected(self):
        result = ps4_linker.generate(self.source)
        self.assertIn("SORT_BY_INIT_PRIORITY(.init_array.* .ctors.*)", result)
        self.assertIn("SORT_BY_INIT_PRIORITY(.fini_array.* .dtors.*)", result)
        for section in (".data", ".tdata", ".tbss"):
            self.assertIn(f"*({section} {section}.*)", result)

    def test_missing_marker_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "init-array"):
            ps4_linker.generate("SECTIONS { .data : { *(.data); } }")

    def test_duplicate_marker_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "exactly one"):
            ps4_linker.generate(self.source.replace(
                "  .data", "  .init_array : { *(.init_array); }\n  .data"
            ))


if __name__ == "__main__":
    unittest.main()
