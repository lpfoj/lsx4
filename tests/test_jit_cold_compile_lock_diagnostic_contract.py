from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
CORE = (
    ROOT / "src/executor/dynamic_translation/retiring_execution_core.cpp"
).read_text(encoding="utf-8")


def cpp_function(source: str, signature: str) -> str:
    start = source.index(signature)
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


class JitColdCompileLockDiagnosticContractTest(unittest.TestCase):
    def test_diagnostic_is_explicitly_gated_and_cached(self):
        gate = cpp_function(CORE, "bool JitColdCompileLockDiagnosticEnabled()")
        self.assertIn('"diag-jit-cold-compile-lock"', gate)
        self.assertIn('"EXECUTOR_DIAG_JIT_COLD_COMPILE_LOCK"', gate)
        self.assertIn("static const bool enabled = toggle.Enabled();", gate)

        resolve = cpp_function(
            CORE,
            "JitExecutionLookup ResolveExecutableRegion(const std::uint64_t guest_rip)",
        )
        self.assertIn(
            "const bool diagnose_lock = JitColdCompileLockDiagnosticEnabled();",
            resolve,
        )
        self.assertIn("if (diagnose_lock)", resolve)

    def test_timing_covers_wait_and_locked_transaction(self):
        resolve = cpp_function(
            CORE,
            "JitExecutionLookup ResolveExecutableRegion(const std::uint64_t guest_rip)",
        )
        wait_start = resolve.index("const auto wait_started")
        lock = resolve.index("std::unique_lock lock{block_cache_mutex_}")
        acquired = resolve.index("const auto lock_acquired")
        transaction = resolve.index("ResolveExecutableRegionLocked(guest_rip, lock, true)")
        completed = resolve.index("const auto completed")
        self.assertLess(wait_start, lock)
        self.assertLess(lock, acquired)
        self.assertLess(acquired, transaction)
        self.assertLess(transaction, completed)
        self.assertIn("lock_acquired - wait_started", resolve)
        self.assertIn("completed - lock_acquired", resolve)

    def test_thresholds_and_record_cap_are_bounded(self):
        resolve = cpp_function(
            CORE,
            "JitExecutionLookup ResolveExecutableRegion(const std::uint64_t guest_rip)",
        )
        self.assertIn("wait_us >= 100 || transaction_us >= 2000", resolve)
        self.assertIn(
            "static std::atomic<std::uint32_t> diagnostic_records{0};", resolve
        )
        self.assertIn(
            "diagnostic_records.fetch_add(1, std::memory_order_relaxed) < 4096",
            resolve,
        )

    def test_record_has_thread_rip_timing_and_outcome_fields(self):
        resolve = cpp_function(
            CORE,
            "JitExecutionLookup ResolveExecutableRegion(const std::uint64_t guest_rip)",
        )
        for field in (
            "[EXECUTOR_JIT_COLD_COMPILE_LOCK]",
            "tid=%u",
            "thread=%s",
            "rip=0x%llx",
            "waitUs=%lld",
            "transactionUs=%lld",
            "cachedBefore=%u",
            "lockOwnedEnd=%u",
            "executable=%u",
        ):
            self.assertIn(field, resolve)
        self.assertIn("JitHostTid()", resolve)
        self.assertIn("JitThreadName(thread_name)", resolve)
        self.assertIn("lock.owns_lock()", resolve)
        self.assertIn("result.block.entry != nullptr", resolve)

    def test_log_helpers_are_declared_before_the_inline_resolver(self):
        resolver = CORE.index(
            "JitExecutionLookup ResolveExecutableRegion(const std::uint64_t guest_rip)"
        )
        self.assertLess(CORE.index("std::uint32_t JitHostTid();"), resolver)
        self.assertLess(CORE.index("void JitThreadName(std::span<char> output);"), resolver)


if __name__ == "__main__":
    unittest.main()
