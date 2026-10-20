import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FEX = ROOT.parent / "refs/PS4Run/repos/fex-emu-2608/FEXCore"
CORE = (FEX / "Source/Interface/Core/Core.cpp").read_text(encoding="utf-8")
STATE = (FEX / "include/FEXCore/Debug/InternalThreadState.h").read_text(
    encoding="utf-8"
)
CONTEXT = (FEX / "Source/Interface/Context/Context.h").read_text(
    encoding="utf-8"
)


class FexCompileScratchContractTests(unittest.TestCase):
    def test_debug_data_is_owned_once_per_guest_thread(self):
        self.assertIn("CompileDebugData", STATE)
        initialize = CORE.split("void ContextImpl::InitializeCompiler", 1)[1].split(
            "ContextImpl::CreateThread", 1
        )[0]
        self.assertEqual(
            initialize.count("make_unique<FEXCore::Core::DebugData>()"), 1
        )

    def test_compile_hot_path_clears_but_does_not_allocate_debug_data(self):
        compile_code = CORE.split("ContextImpl::CompileCodeResult ContextImpl::CompileCode", 1)[
            1
        ].split("uintptr_t ContextImpl::CompileBlock", 1)[0]
        self.assertIn("Thread->CompileDebugData.get()", compile_code)
        self.assertIn("DebugData->Subblocks.clear()", compile_code)
        self.assertIn("DebugData->GuestOpcodes.clear()", compile_code)
        self.assertNotIn("make_unique<FEXCore::Core::DebugData>", compile_code)
        self.assertIn("FEXCore::Core::DebugData* DebugData", CONTEXT)


if __name__ == "__main__":
    unittest.main()
