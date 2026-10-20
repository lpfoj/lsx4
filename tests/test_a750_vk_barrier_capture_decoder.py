import struct
import unittest

from tools.decode_a750_vk_barrier_capture import CAPTURE_SIZE, MAGIC, decode_capture


class A750BarrierCaptureDecoderTests(unittest.TestCase):
    def test_decodes_committed_capture(self):
        values = [MAGIC, 1, CAPTURE_SIZE, 42, 42, 7, 0x1234, 0x5678, 0, 0, 0, 1, 0, 0, 0, 1]
        payload = bytearray(CAPTURE_SIZE)
        struct.pack_into("<16Q", payload, 0, *values)
        image_offset = struct.calcsize("<16Q") + 4 * struct.calcsize("<7Q")
        struct.pack_into("<13Q", payload, image_offset, 0xABC, 1, 2, 4, 8, 0, 1, 1, 0, 1, 0, 1, 0)
        decoded = decode_capture(payload)
        self.assertTrue(decoded["committed"])
        self.assertEqual(decoded["caller_hex"], "0x1234")
        self.assertEqual(decoded["images"][0]["handle"], 0xABC)


if __name__ == "__main__":
    unittest.main()
