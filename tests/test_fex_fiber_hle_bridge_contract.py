import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NATIVE = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")
BRIDGE_H = (ROOT / "src/executor/fex_fiber_hle_bridge.h").read_text(
    encoding="utf-8"
)
BRIDGE_CPP = (ROOT / "src/executor/fex_fiber_hle_bridge.cpp").read_text(
    encoding="utf-8"
)
SMOKE = (ROOT / "tests/fex_fiber_hle_bridge_smoke.cpp").read_text(
    encoding="utf-8"
)


def section(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class FexFiberHleBridgeContractTests(unittest.TestCase):
    def test_only_the_five_fiber_nids_are_name_fallbacks(self):
        nid_table = section(
            BRIDGE_CPP,
            "ContextReplacingFiberNids{",
            "};",
        )
        expected = {
            "a0LLrZWac0M",
            "PFT2S-tJ7Uk",
            "B0ZX2hx9DMw",
            "avfGJ94g36Q",
            "ZqhZFuzKT6U",
        }
        found = {
            line.split('"', 2)[1]
            for line in nid_table.splitlines()
            if '"' in line
        }
        self.assertEqual(found, expected)
        self.assertIn("std::find", BRIDGE_CPP)
        self.assertNotIn("strstr", BRIDGE_CPP)

    def test_production_classification_uses_immutable_wrapper_identity(self):
        metadata = section(
            NATIVE,
            "metadata.fex_context_replacing_fiber_target =",
            "metadata.pthread_create_target",
        )
        for target in (
            "sceFiberRun",
            "sceFiberSwitch",
            "sceFiberReturnToThread",
            "TraceFiberRunImpl",
            "TraceFiberSwitchImpl",
        ):
            self.assertIn(f"HOST_CALL(Libraries::Fiber::{target})", NATIVE)
        self.assertIn("IsContextReplacingFiberNid(metadata.name)", metadata)
        self.assertNotIn("SymbolNameContains", metadata)
        self.assertNotIn("CUSA", metadata)

    def test_context_cost_and_nonlocal_commit_are_fiber_only(self):
        dispatch = section(
            NATIVE,
            'extern "C" int32_t ExecutorFexDispatchHle',
            "void ExecutorFexThreadEvent",
        )
        guarded_region = dispatch.index("try {")
        metadata_lookup = dispatch.index("GetExecutorHleTargetMetadata(")
        boundary = dispatch.index(
            "std::optional<ScopedExecutorHleBoundary> hle_boundary"
        )
        full_boundary = dispatch.index("hle_boundary.emplace(")
        guest_return = dispatch.index("GuestReturnAddress(call->guest_rsp)")
        classify = dispatch.index("fex_context_replacing_fiber_target")
        publish = dispatch.index("fiber_context_bridge.emplace")
        invoke = dispatch.index("InvokeAndroidX64HleTypedTargetResolved")
        commit = dispatch.index("fiber_context_bridge->Commit()")
        ordinary_result = dispatch.index("result->rax = typed_result.rax")
        self.assertLess(guarded_region, metadata_lookup)
        self.assertLess(metadata_lookup, boundary)


        self.assertLess(boundary, full_boundary)
        self.assertLess(full_boundary, guest_return)
        self.assertLess(guest_return, publish)
        self.assertLess(classify, publish)
        self.assertLess(publish, invoke)
        self.assertLess(invoke, commit)
        self.assertLess(commit, ordinary_result)
        self.assertIn("g_fex_hle_dispatch_depth != 1", dispatch)
        self.assertEqual(dispatch.count("GetExecutorHleTargetMetadata("), 1)

    def test_nested_publication_is_restored_on_commit_and_cancel(self):
        for bridge_marker in (
            "previous_live_storage_",
            "previous_continuation_storage_",
            "inherited_replacement_request_",
            "RestorePublication();",
        ):
            self.assertIn(bridge_marker, BRIDGE_H + BRIDGE_CPP)
        for smoke_marker in (
            "restore-outer-live",
            "restore-outer-continuation",
            "restore-outer-replacement",
            "cancel-restores-storage",
            "get-failure-no-publication",
        ):
            self.assertIn(smoke_marker, SMOKE)

    def test_v3_never_commits_a_partial_vector_or_x87_state(self):
        flags = section(BRIDGE_H, "CommitContextFlags =", ";")
        self.assertIn("LSX4_FEX_CONTEXT_GPR_VALID", flags)
        self.assertIn("LSX4_FEX_CONTEXT_FLAGS_VALID", flags)
        self.assertIn("LSX4_FEX_CONTEXT_TLS_VALID", flags)
        self.assertNotIn("LSX4_FEX_CONTEXT_XMM_VALID", flags)
        commit_context = section(
            BRIDGE_CPP,
            "Lsx4FexGuestContextV3 MakeCommitContext",
            "} // namespace",
        )
        self.assertNotIn("context.xmm", commit_context)
        self.assertNotIn("context.mxcsr", commit_context)
        self.assertIn("no-partial-vector-commit", SMOKE)
        self.assertIn("unavailable-ymm-upper-zero", SMOKE)

    def test_bridge_adds_no_lock_fence_or_title_route(self):
        bridge = BRIDGE_H + BRIDGE_CPP
        for forbidden in (
            "std::mutex",
            "scoped_lock",
            "lock_guard",
            "atomic_thread_fence",
            "CUSA",
            "title_id",
        ):
            self.assertNotIn(forbidden, bridge)


if __name__ == "__main__":
    unittest.main()
