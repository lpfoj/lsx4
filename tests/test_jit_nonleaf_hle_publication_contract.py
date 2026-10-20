import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
CORE = (
    ROOT / "src/executor/dynamic_translation/retiring_execution_core.cpp"
).read_text(encoding="utf-8")


class JitNonleafHlePublicationContractTest(unittest.TestCase):
    def test_nonleaf_hle_is_classified_once_while_compiling_the_block(self):
        classification = CORE.split(
            "JitLeafHlePltTarget generic_leaf_hle_target{};", 1
        )[1].split("constexpr std::uint32_t kNoCachedGprOffset", 1)[0]

        leaf = classification.index("const bool generic_has_leaf_hle_call")
        nonleaf = classification.index("const bool generic_has_nonleaf_hle_call")
        self.assertLess(leaf, nonleaf)
        nonleaf_body = classification[nonleaf:]
        for token in (
            "generic_has_call_terminator",
            "!generic_has_dynamic_control_terminator",
            "!generic_has_leaf_hle_call",
            "IsGuestImportPltStub(branch_target)",
            "IsDirectHleBoundary(branch_target)",
        ):
            self.assertIn(token, nonleaf_body)

    def test_unresolved_import_plt_is_a_compile_time_hle_boundary(self):
        classifier = CORE.split(
            "bool IsGuestImportPltStub(const std::uint64_t guest_rip)", 1
        )[1].split("bool IsDirectHleBoundary", 1)[0]
        for token in (
            "ReadGuestBytesForHostInspection",
            "opcode[0] == 0xff",
            "opcode[1] == 0x25",
        ):
            self.assertIn(token, classifier)
        for forbidden in ("CUSA", "mutex", ".wait(", "std::lock"):
            self.assertNotIn(forbidden, classifier)

        self.assertIn(
            "kJitPersistentNativeSemanticAbiVersion =\n"
            "    0x202608230068ull",
            CORE,
        )

    def test_prewarm_uses_serialized_leaf_hle_fact_not_unpublished_got(self):
        prewarm = CORE.split(
            "void PrewarmPersistentNativeCache(", 1
        )[1].split("void FinishPersistentNativePrewarm", 1)[0]
        flag_gate = prewarm.index(
            "stable.native_flags & kJitNativeFlagLeafHleFused"
        )
        restore = prewarm.index("RestorePersistentNativeBlock", flag_gate)
        self.assertLess(flag_gate, restore)
        self.assertIn("continue;", prewarm[flag_gate:restore])

    def test_prewarm_never_restores_rip_relative_import_control(self):
        classifier = CORE.split(
            "bool IsRipRelativeIndirectControlBlock(", 1
        )[1].split("bool IsDirectHleBoundary", 1)[0]
        for token in (
            "X86_MNEMONIC_CALL",
            "X86_MNEMONIC_JMP",
            "X86_OPERAND_TYPE_MEMORY",
            "IsGuestInstructionPointerRegister",
        ):
            self.assertIn(token, classifier)
        for forbidden in (
            "ReadGuestBytesForHostInspection",
            "executor_jit_",
            "CUSA",
        ):
            self.assertNotIn(forbidden, classifier)

        prewarm = CORE.split(
            "void PrewarmPersistentNativeCache(", 1
        )[1].split("void FinishPersistentNativePrewarm", 1)[0]
        structural_gate = prewarm.index(
            "IsRipRelativeIndirectControlBlock(decoded)"
        )
        restore = prewarm.index("RestorePersistentNativeBlock", structural_gate)
        self.assertLess(structural_gate, restore)
        self.assertIn("continue;", prewarm[structural_gate:restore])

        byte_gate = prewarm.index("GuestBytesMatchDecoded(decoded)")
        hash_gate = prewarm.index("decoded_hash != stable.guest_hash", byte_gate)
        self.assertLess(byte_gate, structural_gate)
        self.assertLess(hash_gate, restore)

    def test_rip_relative_import_native_is_not_recaptured(self):
        capture = CORE.split(
            "const bool should_capture_persistent_native", 1
        )[1].split("const bool captured_persistent_native", 1)[0]
        self.assertIn("!IsRipRelativeIndirectControlBlock(*decoded)", capture)

    def test_nonleaf_hle_publishes_before_any_cross_block_call_edge(self):
        exits = CORE.split(
            "} else if (generic_has_jump_terminator ||", 1
        )[1].split("} else if (generic_has_ret_terminator)", 1)[0]
        call_exit = exits.split("if (generic_has_call_terminator)", 1)[1].split(
            "} else if (generic_has_dynamic_control_terminator &&\n"
            "                            generic_has_jump_terminator",
            1,
        )[0]

        boundary = call_exit.index("if (generic_has_nonleaf_hle_call)")
        publish = call_exit.index("emit_epilogue_return_reg(code->x10)", boundary)
        ordinary_edge = call_exit.index("emit_static_edge_tail(branch_target)", publish)
        self.assertLess(boundary, publish)
        self.assertLess(publish, ordinary_edge)

        generated_boundary_arm = call_exit[boundary:call_exit.index(
            "else if (generic_has_dynamic_control_terminator", boundary
        )]
        for forbidden in (
            "IsDirectHleBoundary",
            "executor_jit_",
            "EmitLeafHleGuard",
            "mutex",
            ".lock(",
            "std::lock",
            "emit_static_edge_tail",
        ):
            self.assertNotIn(forbidden, generated_boundary_arm)

    def test_leaf_hle_fusion_and_ordinary_guest_call_chaining_remain_enabled(self):
        classification = CORE.index("const bool generic_has_nonleaf_hle_call")
        guard = CORE.index("EmitLeafHleGuard", classification)
        leaf_start = CORE.rindex("if (generic_has_leaf_hle_call)", 0, guard)
        leaf_end = CORE.index(
            "if (emitted && generic_has_call_terminator)", guard
        )
        leaf_path = CORE[leaf_start:leaf_end]
        self.assertIn("EmitLeafHleGuard", leaf_path)
        self.assertIn("generic_leaf_hle_target.native_function", leaf_path)
        self.assertIn("emit_static_edge_tail(fallthrough)", leaf_path)

        exits = CORE.split(
            "} else if (generic_has_jump_terminator ||", 1
        )[1].split("} else if (generic_has_ret_terminator)", 1)[0]
        self.assertIn("!emit_static_edge_tail(branch_target)", exits)
        self.assertNotIn("CUSA", exits)


if __name__ == "__main__":
    unittest.main()
