from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "shader_recompiler"
    / "ir"
    / "passes"
    / "flatten_extended_userdata_pass.cpp"
).read_text(encoding="utf-8")
WALKER = SOURCE.split("void RunPortableSrtWalker", 1)[1]


class PortableSrtScratchDefaultInitContractTests(unittest.TestCase):
    def test_candidate_is_default_off_and_marker_gated(self):
        gate = SOURCE.split(
            "bool ExecutorPortableSrtScratchDefaultInitEnabled()", 1
        )[1].split("void RecordPortableSrtPrecomputedLayoutProof", 1)[0]
        self.assertIn("run-enable-portable-srt-scratch-default-init", gate)
        self.assertIn("if (!selected)", gate)
        self.assertIn("return false;", gate)
        self.assertIn("legacyFallback=1", gate)

    def test_only_trivial_overwritten_scratch_uses_default_init(self):
        self.assertEqual(WALKER.count("boost::container::default_init"), 3)
        self.assertIn(
            "node_bulk_valid.resize(srt.portable_nodes.size(), "
            "boost::container::default_init)",
            WALKER,
        )
        self.assertIn(
            "bulk_node_words.resize(total_bulk_dwords, boost::container::default_init)",
            WALKER,
        )
        self.assertIn(
            "bases.resize(srt.portable_nodes.size(), boost::container::default_init)",
            WALKER,
        )

    def test_bases_are_zeroed_before_every_possible_early_continue(self):
        bases_tail = WALKER.split(
            "bases.assign(srt.portable_nodes.size(), 0);", 1
        )[1]
        loop = bases_tail.split(
            "for (size_t i = 0; i < srt.portable_nodes.size(); ++i)", 1
        )[1].split("u32 filled = 0", 1)[0]
        self.assertLess(loop.index("bases[i] = 0;"), loop.index("const auto& n"))
        self.assertLess(loop.index("bases[i] = 0;"), loop.index("continue;"))
        self.assertIn("bases.assign(srt.portable_nodes.size(), 0);", WALKER)

    def test_bulk_validity_is_initialized_per_node_before_use(self):
        bases_tail = WALKER.split(
            "bases.assign(srt.portable_nodes.size(), 0);", 1
        )[1]
        loop = bases_tail.split(
            "for (size_t i = 0; i < srt.portable_nodes.size(); ++i)", 1
        )[1].split("u32 filled = 0", 1)[0]
        self.assertLess(loop.index("node_bulk_valid[i] = 0;"), loop.index("node_bulk_valid[parent]"))
        self.assertIn("node_bulk_valid.assign(srt.portable_nodes.size(), 0);", WALKER)
        self.assertIn("bulk_reader_enabled && node_bulk_valid", WALKER)

    def test_bulk_words_are_consumed_only_after_a_successful_full_read(self):
        self.assertIn(
            "node_bulk_valid[i] =\n                memory_reader(ptr, node_words,",
            WALKER,
        )
        self.assertIn("bulk_reader_enabled && node_bulk_valid[parent] != 0", WALKER)
        self.assertIn("bulk_reader_enabled && node_bulk_valid[c.node] != 0", WALKER)
        self.assertEqual(WALKER.count("memory_reader(ptr, node_words,"), 1)


if __name__ == "__main__":
    unittest.main()
