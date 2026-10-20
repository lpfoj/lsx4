import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUFFER_CACHE = (
    ROOT / "funnel-arm/src/video_core/buffer_cache/buffer_cache.cpp"
).read_text(encoding="utf-8")
PAGE_MANAGER = (
    ROOT / "funnel-arm/src/video_core/page_manager.cpp"
).read_text(encoding="utf-8")
JIT = (
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


class SignedZeroVbufProvenanceContractTests(unittest.TestCase):
    def test_normal_vertex_prepare_has_no_provenance_probe(self):
        prepare = cpp_function(
            BUFFER_CACHE,
            "BufferCache::PreparedVertexBuffers BufferCache::PrepareVertexBuffers",
        )
        diagnostic = prepare.index("if (executor_probe_bound)")
        probe = prepare.index("ExecutorProbeBoundGpuBuffer", diagnostic)
        self.assertLess(diagnostic, probe)
        self.assertNotIn("TryArmDiagnosticWriteProvenance", prepare)
        self.assertEqual(
            BUFFER_CACHE.count("ExecutorSignedZeroVbufWriteProvenanceRequested()"),
            2,
        )

    def test_marker_is_explicit_default_off_and_read_only_after_exact_match(self):
        gate = cpp_function(
            BUFFER_CACHE, "bool ExecutorSignedZeroVbufWriteProvenanceRequested"
        )
        self.assertIn("static const bool requested", gate)
        self.assertIn("diag-signed-zero-vbuf-write-provenance", gate)
        self.assertIn("std::fopen", gate)
        self.assertIn("return false", gate)

        oracle = cpp_function(
            BUFFER_CACHE, "void BufferCache::ExecutorProbeBoundGpuBuffer"
        )
        exact = oracle.index("exact_signed_zero_vertex_payload")
        gate_call = oracle.index(
            "ExecutorSignedZeroVbufWriteProvenanceRequested()", exact
        )
        arm = oracle.index("TryArmDiagnosticWriteProvenance", gate_call)
        self.assertLess(exact, gate_call)
        self.assertLess(gate_call, arm)

    def test_selection_is_exact_48_byte_signed_zero_and_one_shot(self):
        payload = cpp_function(
            BUFFER_CACHE, "bool ExecutorIsSignedZeroPositionPayload"
        )
        self.assertIn("bytes.size() != ExecutorSignedZeroPositionBytes", payload)
        self.assertIn("0x80000000u : 0u", payload)

        oracle = cpp_function(
            BUFFER_CACHE, "void BufferCache::ExecutorProbeBoundGpuBuffer"
        )
        for required in (
            "size == ExecutorSignedZeroPositionBytes",
            "probe_size == ExecutorSignedZeroPositionBytes",
            'std::strcmp(role, "vertex_range") == 0',
            "slot == 0",
            "first_diff == probe_size",
        ):
            self.assertIn(required, oracle)
        claim = oracle.index(
            "executor_signed_zero_vbuf_provenance_claimed.compare_exchange_strong"
        )
        arm = oracle.index("TryArmDiagnosticWriteProvenance", claim)
        self.assertLess(claim, arm)
        self.assertIn(
            "guest_addr, ExecutorSignedZeroPositionBytes", oracle[arm:]
        )
        self.assertNotIn("19aa2cc1", oracle.lower())

    def test_jit_writer_pc_comes_from_host_pc_sidecar(self):
        provenance = cpp_function(
            JIT, "executor_jit_signal_context_fault_provenance"
        )
        self.assertIn("ResolveSignalFaultPublicationLookup(raw_context)", provenance)
        self.assertIn("FaultDescriptorFromPublication(publication)", provenance)
        self.assertIn("descriptor->guest_rip", provenance)
        self.assertIn("descriptor->bytes[index]", provenance)
        self.assertNotIn("CurrentPublishedFaultInstruction", provenance)

    def test_fault_hit_logs_backend_pc_and_source_instruction_then_disarms(self):
        handler = cpp_function(PAGE_MANAGER, "static bool GuestFaultSignalHandler")
        jit = handler.index("executor_jit_signal_context_fault_provenance")
        fex = handler.index("executor_lsx4_android_fex_signal_context_guest_rip", jit)
        log = handler.index("[EXECUTOR_WRITE_PROVENANCE]", fex)
        self.assertLess(jit, fex)
        self.assertLess(fex, log)
        for required in (
            'backend=%s',
            'guestRip=0x%llx',
            'instruction=%016llx:%016llx',
            'exactHit=%u',
            "executor_write_provenance_page_begin.store(0",
            "executor_write_provenance_page_end.store(0",
            "executor_write_provenance_exact_begin.store(",
            "executor_write_provenance_exact_end.store(",
        ):
            self.assertIn(required, handler)


if __name__ == "__main__":
    unittest.main()
