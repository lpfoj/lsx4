from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "externals/LibAtrac9/C/src/decinit.c"
).read_text(encoding="utf-8")
TABLES = (
    ROOT / "externals/LibAtrac9/C/src/tables.h"
).read_text(encoding="utf-8")


def function_body(signature: str) -> str:
    start = SOURCE.index(signature)
    while SOURCE[start + len(signature) :].lstrip().startswith(";"):
        start = SOURCE.index(signature, start + len(signature))
    opening = SOURCE.index("{", start)
    depth = 0
    for index in range(opening, len(SOURCE)):
        char = SOURCE[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[opening + 1 : index]
    raise AssertionError(f"unterminated function: {signature}")


class Atrac9StaticTableInitContract(unittest.TestCase):
    def test_decoder_uses_the_once_gate_after_config_validation(self) -> None:
        init = function_body("At9Status InitDecoder")
        self.assertIn("EnsureStaticTablesInitialized();", init)
        self.assertLess(init.index("InitConfigData"), init.index("InitFrame"))
        self.assertLess(init.index("InitFrame"), init.index("EnsureStaticTablesInitialized"))
        self.assertNotIn("InitMdctTables", init)
        self.assertNotIn("InitHuffmanCodebooks", init)
        self.assertNotIn("GenerateGradientCurves", init)

    def test_once_initializer_builds_every_immutable_table(self) -> None:
        initializer = function_body("static void InitStaticTables(void)")
        self.assertIn("InitMdctTables();", initializer)
        self.assertIn("InitHuffmanCodebooks();", initializer)
        self.assertIn("GenerateGradientCurves();", initializer)

    def test_all_supported_mdct_windows_are_generated_once(self) -> None:
        mdct = function_body("static void InitMdctTables(void)")
        self.assertIn("for (int i = 0; i < 9; i++)", mdct)
        self.assertIn("GenerateTrigTables(i);", mdct)
        self.assertIn("GenerateShuffleTable(i);", mdct)
        self.assertIn(
            "for (int frameSizePower = 6; frameSizePower <= 8; frameSizePower++)",
            mdct,
        )
        self.assertEqual(mdct.count("GenerateMdctWindow(frameSizePower);"), 1)
        self.assertEqual(mdct.count("GenerateImdctWindow(frameSizePower);"), 1)
        self.assertIn("extern double MdctWindow[3][256];", TABLES)
        self.assertIn("extern double ImdctWindow[3][256];", TABLES)

    def test_android_and_posix_use_pthread_once(self) -> None:
        self.assertIn("#include <pthread.h>", SOURCE)
        self.assertIn(
            "static pthread_once_t StaticTablesOnce = PTHREAD_ONCE_INIT;", SOURCE
        )
        self.assertIn(
            "(void)pthread_once(&StaticTablesOnce, InitStaticTables);", SOURCE
        )

    def test_non_posix_platforms_keep_thread_safe_once_semantics(self) -> None:
        self.assertIn("INIT_ONCE_STATIC_INIT", SOURCE)
        self.assertIn("InitOnceExecuteOnce", SOURCE)
        self.assertIn("atomic_compare_exchange_strong_explicit", SOURCE)
        self.assertIn("memory_order_acq_rel", SOURCE)
        self.assertIn("memory_order_release", SOURCE)
        self.assertIn("memory_order_acquire", SOURCE)


if __name__ == "__main__":
    unittest.main()
