import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT
    / "src"
    / "executor"
    / "dynamic_translation"
    / "retiring_execution_core.cpp"
)


class JitBadAccessReplayDescriptorContractTests(unittest.TestCase):
    def test_fault_publication_retains_decoded_semantics_for_exact_replay(self) -> None:
        text = SOURCE.read_text(encoding="utf-8")
        preserve_begin = text.index("const auto preserve_fault_descriptor")
        preserve_end = text.index(
            "const auto preserve_semantic_descriptor", preserve_begin
        )
        preserve = text[preserve_begin:preserve_end]

        self.assertIn("NativeFaultDescriptor descriptor = instruction;", preserve)
        self.assertNotIn("descriptor.guest_rip = instruction.guest_rip;", preserve)
        self.assertNotIn("descriptor.mnemonic = instruction.mnemonic;", preserve)

    def test_exact_replay_consumes_the_published_decoded_instruction(self) -> None:
        text = SOURCE.read_text(encoding="utf-8")

        self.assertIn("TryReplayIgnoredBadAccess(", text)
        self.assertIn("deferred_fault.instruction_descriptor", text)
        self.assertIn("InterpretDecodedInstruction(&replay_state, instruction)", text)


if __name__ == "__main__":
    unittest.main()
