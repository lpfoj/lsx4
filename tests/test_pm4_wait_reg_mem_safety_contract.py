import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
LIVERPOOL = (
    ROOT / "funnel-arm" / "src" / "video_core" / "amdgpu" / "liverpool.cpp"
).read_text(encoding="utf-8")
MEMORY = (ROOT / "funnel-arm" / "src" / "core" / "memory.cpp").read_text(
    encoding="utf-8"
)


def cpp_function(source: str, signature: str) -> str:
    start = source.rindex(signature)
    brace = source.index("{", start)
    depth = 0
    for position in range(brace, len(source)):
        token = source[position]
        if token == "{":
            depth += 1
        elif token == "}":
            depth -= 1
            if depth == 0:
                return source[start : position + 1]
    raise AssertionError(f"unterminated function: {signature}")


class Pm4WaitRegMemSafetyContractTest(unittest.TestCase):
    def test_android_wait_read_never_dereferences_an_untracked_guest_address(self):
        read = cpp_function(LIVERPOOL, "static ExecutorWaitValueSource ExecutorReadWaitRegMemValue")

        self.assertIn("Function::Always", read)
        self.assertIn("wait_reg_mem.Reg() >= regs.size()", read)
        self.assertIn("ExecutorTryReadVoLabelAlias", read)
        self.assertIn("ExecutorReadManagedWaitValue", read)
        self.assertNotIn("std::memcpy(value, wait_addr", read)
        self.assertNotIn("*wait_addr", read)

    def test_managed_wait_read_is_locked_and_has_no_raw_pointer_fallback(self):
        read = cpp_function(LIVERPOOL, "static ExecutorWaitValueSource ExecutorReadManagedWaitValue")

        self.assertIn("AcquireSnapshotReadGuard", read)
        self.assertIn("TryCopyBackingMemory", read)
        self.assertIn("TryCopyReadableMemory", read)
        self.assertIn("ExecutorWaitValueSource::Unresolved", read)
        self.assertNotIn("std::memcpy", read)
        self.assertNotIn("atomic_ref", read)

    def test_graphics_wait_validates_packet_and_aborts_unresolved_submission(self):
        graphics = LIVERPOOL.split("Liverpool::Task Liverpool::ProcessGraphics", 1)[1]
        graphics = graphics.split("Liverpool::Task Liverpool::ProcessCompute", 1)[0]
        wait = graphics.rsplit("case PM4ItOpcode::WaitRegMem:", 1)[1].split(
            "case PM4ItOpcode::IndirectBuffer:", 1
        )[0]

        self.assertIn("ValidateWaitRegMemPacket", wait)
        self.assertIn("ExecutorWaitValueSource::Unresolved", wait)
        self.assertIn("state->abort = true", wait)
        self.assertIn("[EXECUTOR_PM4_WAIT_UNMAPPED]", wait)
        self.assertEqual(wait.count("wait_reg_mem->Test"), 1)
        self.assertGreater(wait.index("wait_reg_mem->Test"), wait.rindex("#else"))
        self.assertLess(wait.index("packet_words != WaitPacketWords"), wait.index("wait_reg_mem ="))

    def test_compute_wait_uses_the_same_safe_refresh(self):
        compute = LIVERPOOL.split("Liverpool::Task Liverpool::ProcessCompute", 1)[1]
        wait = compute.split("case PM4ItOpcode::WaitRegMem:", 1)[1].split(
            "case PM4ItOpcode::ReleaseMem:", 1
        )[0]

        self.assertIn("ValidateWaitRegMemPacket", wait)
        self.assertIn("ExecutorReadWaitRegMemValue", wait)
        self.assertIn("ExecutorWaitValueSource::Unresolved", wait)
        self.assertEqual(wait.count("wait_reg_mem->Test"), 1)
        self.assertGreater(wait.index("wait_reg_mem->Test"), wait.rindex("#else"))
        self.assertLess(
            wait.index("packet_total_words != WaitPacketWords"), wait.index("wait_reg_mem =")
        )

    def test_parked_wait_refresh_wakes_unmapped_queue_without_raw_read(self):
        refresh = cpp_function(LIVERPOOL, "void Liverpool::RefreshParkedMemoryWaitsForExecutor")

        self.assertIn("ExecutorReadManagedWaitValue", refresh)
        self.assertIn("ReleaseParkedMemoryWaitForExecutor", refresh)
        self.assertIn("submit_ready_mask.fetch_or", refresh)
        self.assertNotIn("ExecutorReadMappedWaitValueFast", refresh)

    def test_completion_wait_uses_physical_alias_only_after_exact_sidecar_misses(self):
        consume = cpp_function(
            LIVERPOOL, "u64 Liverpool::ConsumeCompletionWaitForExecutor"
        )

        exact_state = consume.index("consume_submission_write(address)")
        exact_pending = consume.index("find_pending_write(address)")
        alias_lookup = consume.index("FindMappedBackingAliases")
        self.assertLess(exact_state, exact_pending)
        self.assertLess(exact_pending, alias_lookup)
        self.assertIn("TestWaitRegMemValue", consume)
        self.assertIn("alias == address", consume)
        self.assertNotIn("WaitIdle", consume)
        self.assertNotIn("Finish", consume)
        self.assertNotIn("ReadMemory", consume)
        self.assertNotIn("FlushGpuCompletionBatch", consume)

    def test_physical_alias_lookup_requires_one_containing_backing_span(self):
        lookup = cpp_function(MEMORY, "bool MemoryManager::FindMappedBackingAliases")

        self.assertIn("AcquireSnapshotReadGuard", lookup)
        self.assertIn("alias_cache_mutex", lookup)
        self.assertIn("RebuildWritableAliasCacheLocked", lookup)
        self.assertIn("physical_addr < alias.physical_begin", lookup)
        self.assertIn("physical_end > alias.physical_end", lookup)
        self.assertIn("std::unique", lookup)


if __name__ == "__main__":
    unittest.main()
