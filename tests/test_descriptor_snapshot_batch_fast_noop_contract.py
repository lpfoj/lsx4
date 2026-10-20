import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CACHE_CPP = (
    ROOT / "funnel-arm" / "src" / "video_core" / "buffer_cache" / "buffer_cache.cpp"
)


class DescriptorSnapshotBatchFastNoopContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = CACHE_CPP.read_text(encoding="utf-8")

    def body_between(self, start: str, end: str) -> str:
        return self.source.split(start, 1)[1].split(end, 1)[0]

    @staticmethod
    def matching_brace(source: str, opening: int) -> int:
        depth = 0
        for index in range(opening, len(source)):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    return index
        raise AssertionError("unterminated C++ block")

    def is_inside_trace_gate(self, position: int) -> bool:
        for marker in (
            "if (trace_enabled",
            "if (read_only_rename_trace_enabled",
        ):
            search_from = 0
            while True:
                start = self.source.find(marker, search_from)
                if start < 0:
                    break
                opening = self.source.find("{", start)
                closing = self.matching_brace(self.source, opening)
                if opening < position < closing:
                    return True
                search_from = start + len(marker)
        return False

    def test_descriptor_rename_gates_are_explicitly_hard_disabled(self):
        for gate in (
            "ExecutorUniformReadOnlyRenameGateEnabled",
            "ExecutorStorageReadOnlyRenameGateEnabled",
        ):
            self.assertRegex(
                self.source,
                rf"constexpr bool {gate}\s*=\s*false;",
            )

        usage_gate = self.body_between(
            "bool ExecutorReadOnlyRenameUsageEnabled(",
            "bool ExecutorDescriptorSnapshotBatchRequired(const bool diagnostics_enabled)",
        )
        self.assertIn("ReadOnlyBufferUsage::Uniform", usage_gate)
        self.assertIn("ReadOnlyBufferUsage::Storage", usage_gate)
        self.assertIn("!ExecutorUniformReadOnlyRenameGateEnabled", usage_gate)
        self.assertIn("!ExecutorStorageReadOnlyRenameGateEnabled", usage_gate)

    def test_fast_noop_preserves_future_gates_and_diagnostic(self):
        required = self.body_between(
            "bool ExecutorDescriptorSnapshotBatchRequired(const bool diagnostics_enabled) noexcept",
            "bool ExecutorShouldLogBufferReadbackOrdinal",
        )
        for token in (
            "ExecutorUniformReadOnlyRenameGateEnabled",
            "ExecutorStorageReadOnlyRenameGateEnabled",
            "diagnostics_enabled",
        ):
            self.assertIn(token, required)
        self.assertIn(
            "return DescriptorRenameGateCanBeActive || diagnostics_enabled;",
            required,
        )
        self.assertNotIn("ExecutorReplayActive", required)

    def test_replay_cannot_reach_descriptor_arena_when_gates_are_disabled(self):
        obtain = self.body_between(
            "std::pair<Buffer*, u32> BufferCache::ObtainReadOnlyBuffer(",
            "std::pair<Buffer*, u32> BufferCache::ObtainBufferForImage(",
        )
        usage_exit = obtain.index("if (!ExecutorReadOnlyRenameUsageEnabled(usage))")
        replay_state = obtain.index(
            "const bool replay_memory = Libraries::GnmDriver::ExecutorReplayActive()"
        )
        arena_selection = obtain.index("EnsureDescriptorSnapshotBuffer()")
        self.assertLess(usage_exit, replay_state)
        self.assertLess(replay_state, arena_selection)

        descriptor_definition = obtain.index(
            "const bool descriptor_request = usage == ReadOnlyBufferUsage::Uniform ||"
        )
        descriptor_alias = obtain.index("const bool descriptor_usage = descriptor_request")
        stream_default = obtain.index("StreamBuffer* snapshot_buffer = &stream_buffer")
        self.assertLess(descriptor_definition, usage_exit)
        self.assertLess(usage_exit, descriptor_alias)
        self.assertLess(descriptor_alias, stream_default)
        self.assertLess(stream_default, arena_selection)

    def test_batch_entry_points_noop_before_side_effects(self):
        methods = {
            "BeginReadOnlyDescriptorSnapshotBatch": "RetireReadOnlyDescriptorSnapshotBatch",
            "RetireReadOnlyDescriptorSnapshotBatch": "CancelReadOnlyDescriptorSnapshotBatch",
            "CancelReadOnlyDescriptorSnapshotBatch": "#endif",
        }
        side_effects = {
            "BeginReadOnlyDescriptorSnapshotBatch": (
                "ASSERT(!descriptor_snapshot_batch_active)",
                "++descriptor_snapshot_batches_started",
            ),
            "RetireReadOnlyDescriptorSnapshotBatch": (
                "RetireLease",
                "descriptor_snapshot_batch_lease = {}",
            ),
            "CancelReadOnlyDescriptorSnapshotBatch": (
                "CancelLease",
                "descriptor_snapshot_batch_lease = {}",
            ),
        }
        begin_guard = re.compile(
            r"if\s*\(\s*!descriptor_snapshot_batch_active\s*&&\s*"
            r"!descriptor_snapshot_batch_lease\s*&&\s*"
            r"!ExecutorDescriptorSnapshotBatchRequired\("
            r"read_only_rename_trace_enabled\)\s*\)\s*\{\s*return;",
            re.DOTALL,
        )
        inactive_guard = re.compile(
            r"if\s*\(\s*!descriptor_snapshot_batch_active\s*&&\s*"
            r"!descriptor_snapshot_batch_lease\s*\)\s*\{\s*return;",
            re.DOTALL,
        )

        for method, next_method in methods.items():
            with self.subTest(method=method):
                body = self.body_between(
                    f"void BufferCache::{method}",
                    next_method,
                )
                expected_guard = (
                    begin_guard
                    if method == "BeginReadOnlyDescriptorSnapshotBatch"
                    else inactive_guard
                )
                match = expected_guard.search(body)
                self.assertIsNotNone(match)
                for side_effect in side_effects[method]:
                    self.assertLess(match.end(), body.index(side_effect))

    def test_report_counters_are_trace_only_but_budget_and_leases_are_not(self):
        self.assertIn(
            "read_only_rename_trace_enabled = ExecutorReadOnlyRenameTraceEnabled();",
            self.source,
        )

        obtain = self.body_between(
            "std::pair<Buffer*, u32> BufferCache::ObtainReadOnlyBuffer(",
            "std::pair<Buffer*, u32> BufferCache::ObtainBufferForImage(",
        )
        self.assertIn("const bool trace_enabled = read_only_rename_trace_enabled;", obtain)
        self.assertRegex(
            obtain,
            r"if \(trace_enabled\) \{\s*\+\+read_only_rename_attempts;\s*"
            r"\+\+read_only_rename_usage_counts",
        )
        self.assertRegex(
            obtain,
            r"read_only_rename_tick_bytes \+= size;\s*if \(trace_enabled\)",
        )

        report_fields = (
            "read_only_rename_usage_counts",
            "read_only_rename_success_usage_counts",
            "read_only_rename_fallback_counts",
            "read_only_rename_attempts",
            "read_only_rename_ranges",
            "read_only_rename_bytes",
            "descriptor_snapshot_selected_bytes",
            "descriptor_snapshot_batches_started",
            "descriptor_snapshot_batches_retired",
            "descriptor_snapshot_batches_canceled",
            "descriptor_snapshot_allocation_failures",
            "descriptor_snapshot_exhaustions",
            "descriptor_snapshot_fallback_attempts",
            "descriptor_snapshot_fallback_successes",
            "descriptor_snapshot_high_water_bytes",
            "descriptor_snapshot_max_batch_bytes",
            "descriptor_snapshot_batch_initial_tick",
            "descriptor_snapshot_max_tick_lag",
        )
        for field in report_fields:
            mutations = list(
                re.finditer(
                    rf"(?:\+\+\s*{field}|"
                    rf"{field}(?:\[[^;\n]*\])?\s*(?:\+=|=(?!=)))",
                    self.source,
                )
            )
            self.assertTrue(mutations, f"missing mutation for {field}")
            for mutation in mutations:
                self.assertTrue(
                    self.is_inside_trace_gate(mutation.start()),
                    f"ungated report mutation for {field}: "
                    f"{self.source[mutation.start():mutation.end()]}",
                )

        budget = obtain.split("static constexpr u64 MaxRenameBytesPerTick", 1)[1].split(
            "const u64 alignment", 1
        )[0]
        self.assertIn("read_only_rename_tick = current_tick", budget)
        self.assertIn("read_only_rename_tick_bytes = 0", budget)
        self.assertIn("std::min(read_only_rename_tick_bytes", budget)
        self.assertNotIn("if (trace_enabled)", budget)

        retire = self.body_between(
            "void BufferCache::RetireReadOnlyDescriptorSnapshotBatch",
            "void BufferCache::CancelReadOnlyDescriptorSnapshotBatch",
        )
        cancel = self.body_between(
            "void BufferCache::CancelReadOnlyDescriptorSnapshotBatch",
            "#endif",
        )
        self.assertLess(
            retire.index("RetireLease"),
            retire.index("if (read_only_rename_trace_enabled)"),
        )
        self.assertLess(
            cancel.index("CancelLease"),
            cancel.index("if (read_only_rename_trace_enabled)"),
        )
        for body in (retire, cancel):
            self.assertRegex(
                body,
                r"descriptor_snapshot_batch_lease = \{\};\s*"
                r"if \(read_only_rename_trace_enabled\)",
            )
            self.assertRegex(
                body,
                r"descriptor_snapshot_batch_active = false;",
            )


if __name__ == "__main__":
    unittest.main()
