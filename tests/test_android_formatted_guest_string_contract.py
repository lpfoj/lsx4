import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
STUBS = (ROOT / "funnel-arm/src/core/aerolib/stubs.cpp").read_text(encoding="utf-8")


class AndroidFormattedGuestStringContract(unittest.TestCase):
    def test_formatted_strings_restore_truncated_mspace_pointers_and_read_safely(self):
        body = STUBS.split("std::string ExecutorReadGuestString", 1)[1].split(
            "s64 ExecutorLibcVasprintf", 1
        )[0]
        self.assertIn("ExecutorCanonicalizeTruncatedMspaceCString", body)
        self.assertIn("ExecutorGuestPageReader reader", body)
        self.assertIn("reader.ReadSpan", body)
        self.assertLess(
            body.index("ExecutorCanonicalizeTruncatedMspaceCString"),
            body.index("reader.ReadSpan"),
        )

    def test_percent_s_uses_the_common_guest_string_reader(self):
        body = STUBS.split("s64 ExecutorLibcVasprintf", 1)[1].split(
            "static s64 ExecutorLibcCopyFormattedResult", 1
        )[0]
        self.assertIn("out += ExecutorReadGuestString(sp, 1 << 16);", body)


if __name__ == "__main__":
    unittest.main()
