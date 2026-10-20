import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
CORE = (
    ROOT / "src/executor/dynamic_translation/retiring_execution_core.cpp"
).read_text(encoding="utf-8")
SELECTOR = CORE.split(
    "std::optional<std::uint64_t> SelectColdSplitPublicationFallthrough(", 1
)[1].split("class RetiringTranslationRuntime final", 1)[0]
FLAT_SELECTOR = " ".join(SELECTOR.split())


class JitColdPublicationContinuationContractTest(unittest.TestCase):
    def test_positive_candidate_is_full_generic_six_op_shape(self):
        for token in (
            "block.instructions.size() < 6",
            "TryResolveBranchTarget",
            "kMaximumShortForwardSkip = 64",
            "X86_MNEMONIC_JE",
            "X86_MNEMONIC_MOV",
            "X86_MNEMONIC_LEA",
            "X86_MNEMONIC_SHL",
            "X86_MNEMONIC_ADD",
            "slot_scale.operands[1].imm.value.u == 3",
            "JitGuestGprIndex",
            "SignedMemoryDisplacement(increment_expression) != 1",
            "SignedMemoryDisplacement(backing_pointer) != 0",
            "X86_ATTRIB_HAS_LOCK",
        ):
            self.assertIn(token, SELECTOR)
        self.assertGreaterEqual(SELECTOR.count("X86_MNEMONIC_MOV"), 2)
        for forbidden in (
            "CUSA",
            "Resogun",
            "c85150",
            "789c",
            "X86_REGISTER_R15",
            "X86_REGISTER_RCX",
            "X86_REGISTER_RDX",
        ):
            self.assertNotIn(forbidden, SELECTOR)

    def test_near_misses_are_rejected_by_exact_roles_and_relationships(self):
        for guard in (
            "static_cast<X86Mnemonic>(terminator.mnemonic) != X86_MNEMONIC_JE",
            "publication.operands[0].type == X86_OPERAND_TYPE_MEMORY",
            "publication.operands[0].size == 32",
            "publication.operands[1].type == X86_OPERAND_TYPE_REGISTER",
            "publication.operands[1].size == 32",
            "count_memory.mem.index != X86_REGISTER_NONE",
            "increment_expression.mem.index != X86_REGISTER_NONE",
            "backing_pointer.mem.index != X86_REGISTER_NONE",
            "published_count.mem.index != X86_REGISTER_NONE",
            "*loaded_count_gpr != *increment_base_gpr",
            "*loaded_count_gpr != *scaled_slot_gpr",
            "*loaded_count_gpr != *addressed_slot_gpr",
            "*incremented_count_gpr != *published_value_gpr",
            "*count_object_gpr != *backing_object_gpr",
            "*count_object_gpr != *publication_object_gpr",
            "SignedMemoryDisplacement(count_memory) != SignedMemoryDisplacement(published_count)",
            "count_memory.mem.segment != backing_pointer.mem.segment",
            "count_memory.mem.segment != published_count.mem.segment",
        ):
            self.assertIn(guard, FLAT_SELECTOR)
        self.assertNotIn("IsConditionalBranchMnemonic", SELECTOR)
        self.assertNotIn("std::ranges::any_of", SELECTOR)
        self.assertNotIn("operand.size == 32 || operand.size == 64", SELECTOR)

    def test_fallthrough_is_resolved_before_source_edge_publication(self):
        resolver = CORE.split(
            "JitExecutionLookup ResolveExecutableRegionLocked(", 1
        )[1].split("OutboundEdgeState* CreateOutboundEdge(", 1)[0]
        select = resolver.index("SelectColdSplitPublicationFallthrough(*decoded)")
        recurse = resolver.index(
            "ResolveExecutableRegionLocked(\n"
            "                    *split_publication_fallthrough, lock, false)",
            select,
        )
        publish = resolver.index("InstallPublishedDestination(", recurse)

        self.assertLess(select, recurse)
        self.assertLess(recurse, publish)
        self.assertIn(
            "secondary_outbound_edge->target.load(\n"
            "                std::memory_order_acquire) == nullptr",
            resolver[select:recurse],
        )
        for counter in (
            "g_jit_cold_split_publication_prime_attempts",
            "g_jit_cold_split_publication_prime_connected",
            "g_jit_cold_split_publication_prime_fallbacks",
        ):
            self.assertIn(counter, resolver)
        self.assertIn('"coldSplitPublicationPrime"', CORE)

    def test_prime_is_bounded_to_one_cold_level_and_has_no_hot_edge_check(self):
        resolver = CORE.split(
            "JitExecutionLookup ResolveExecutableRegionLocked(", 1
        )[1].split("OutboundEdgeState* CreateOutboundEdge(", 1)[0]
        self.assertIn("prime_split_publication_fallthrough", resolver)
        self.assertIn("*split_publication_fallthrough, lock, false", resolver)

        generated_tail = CORE.split("const auto emit_static_edge_tail =", 1)[1].split(
            "const auto emit_machine_authoritative_guard_resident_tail", 1
        )[0]
        self.assertNotIn("SelectColdSplitPublicationFallthrough", generated_tail)
        self.assertNotIn("split_publication_fallthrough", generated_tail)
        for forbidden in ("mutex", "condition_variable", "wait(", "sleep"):
            self.assertNotIn(forbidden, generated_tail)


if __name__ == "__main__":
    unittest.main()
