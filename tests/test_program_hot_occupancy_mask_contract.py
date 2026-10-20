from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
CACHE = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")


def sparse_probe_order(valid_mask: int, preferred_way: int, mru: bool) -> list[int]:
    remaining = valid_mask & 0xF
    order: list[int] = []
    if mru and preferred_way < 4 and remaining & (1 << preferred_way):
        order.append(preferred_way)
        remaining &= ~(1 << preferred_way)
    while remaining:
        bit = remaining & -remaining
        order.append(bit.bit_length() - 1)
        remaining &= remaining - 1
    return order


def first_invalid_way(valid_mask: int, preferred_way: int, mru: bool) -> int:
    invalid_mask = (~valid_mask) & 0xF
    if mru and preferred_way != 0 and invalid_mask & (1 << preferred_way):
        return preferred_way
    bit = invalid_mask & -invalid_mask
    return bit.bit_length() - 1


class ProgramHotOccupancyMaskContractTests(unittest.TestCase):
    def test_candidate_is_default_off_and_has_direct_witness(self):
        self.assertIn("run-enable-program-hot-occupancy-mask", CACHE)
        self.assertIn("ExecutorProgramHotOccupancyMaskEnabled()", CACHE)
        self.assertIn("program-hot-occupancy-mask.txt", CACHE)
        self.assertIn("skippedInvalidWays=%u", CACHE)
        self.assertIn("identityChecksUnchanged=1 byteProofUnchanged=1", CACHE)

    def test_sparse_probe_visits_each_valid_way_once_and_mru_first(self):
        self.assertEqual(sparse_probe_order(0b0000, 2, True), [])
        self.assertEqual(sparse_probe_order(0b1010, 3, True), [3, 1])
        self.assertEqual(sparse_probe_order(0b1010, 0, True), [1, 3])
        self.assertEqual(sparse_probe_order(0b1111, 2, True), [2, 0, 1, 3])
        self.assertEqual(sparse_probe_order(0b1111, 2, False), [0, 1, 2, 3])

    def test_invalid_victim_matches_legacy_mru_probe_order(self):
        self.assertEqual(first_invalid_way(0b0000, 0, True), 0)
        self.assertEqual(first_invalid_way(0b0101, 3, True), 3)
        self.assertEqual(first_invalid_way(0b0101, 2, True), 1)
        self.assertEqual(first_invalid_way(0b0101, 3, False), 1)

    def test_mask_tracks_both_entry_invalidation_and_insertion(self):
        self.assertIn("program_hot_valid_way_masks", CACHE)
        self.assertIn("program_hot_valid_way_mask &=", CACHE)
        self.assertIn("program_hot_valid_way_mask |=", CACHE)
        self.assertIn("candidate.valid = false", CACHE)
        self.assertIn("program_hot.valid = true", CACHE)

    def test_disabled_route_preserves_full_legacy_probe(self):
        get_program = CACHE.split("PipelineCache::GetProgram(", 1)[1]
        self.assertIn("program_hot_probe_count = ProgramHotWayCount", get_program)
        self.assertIn("std::swap(program_hot_probe_order[0]", get_program)
        self.assertIn("if (!candidate.valid)", get_program)


if __name__ == "__main__":
    unittest.main()
