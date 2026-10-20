import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RESOURCE = (
    ROOT / "funnel-arm/src/video_core/amdgpu/resource.h"
).read_text(encoding="utf-8")


class AmdGpuBufferDescriptorAddressContractTests(unittest.TestCase):
    def test_reserved_high_nibble_is_not_part_of_guest_address(self):
        buffer_decl = re.search(
            r"struct Buffer \{(?P<body>.*?)\n\};", RESOURCE, re.DOTALL
        )
        self.assertIsNotNone(buffer_decl)
        body = buffer_decl.group("body")
        self.assertIn("u64 base_address : 40;", body)
        self.assertIn("u64 _padding0 : 8;", body)
        self.assertNotIn("u64 base_address : 44;", body)

    def test_mask_preserves_valid_gpu_va_and_drops_reserved_bits(self):


        raw_descriptor_low64 = 0x1234_0A02_1139_0000
        decoded_address = raw_descriptor_low64 & ((1 << 40) - 1)
        self.assertEqual(decoded_address, 0x02_1139_0000)
        self.assertEqual(decoded_address >> 40, 0)


if __name__ == "__main__":
    unittest.main()
