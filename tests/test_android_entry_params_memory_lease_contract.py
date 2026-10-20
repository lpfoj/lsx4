import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(
    encoding="utf-8"
)


class AndroidEntryParamsMemoryLeaseContractTest(unittest.TestCase):
    def test_entry_arguments_have_widened_read_storage(self):
        self.assertIn("ExecutorEntryArgumentCapacity = 256", RUNTIME)
        self.assertIn(
            "alignas(16) std::array<std::array<char, "
            "ExecutorEntryArgumentCapacity>, 33>",
            RUNTIME,
        )
        prepare = RUNTIME.split("void PrepareEntryParamsForModule", 1)[1].split(
            "Libraries::Kernel::Pthread* AllocatePrimaryGuestThread", 1
        )[0]
        self.assertIn("g_entry_arg_storage[index].data()", prepare)
        self.assertIn("g_entry_arg_storage[index].size()", prepare)
        self.assertNotIn("std::strlen(argument) + 1", prepare)

    def test_registered_host_ranges_are_read_only_leases(self):
        acquire = RUNTIME.split(
            "int32_t ExecutorFexAcquireGuestMemoryLease", 1
        )[1].split("void ExecutorFexReleaseGuestMemoryLease", 1)[0]
        registered = acquire.split(
            "if (!write) {", 1
        )[1].split("const std::uint32_t external_attempts", 1)[0]
        self.assertIn("g_box64_guest_readable_ranges", registered)
        self.assertIn("size <= range.size - (guest_address - range.base)", registered)
        self.assertIn("std::move(readable_lock)", registered)
        self.assertNotIn("PrepareBulkGuestWrite", registered)
        self.assertNotIn("EnterBulkGuestWrite", registered)
        self.assertNotIn("BeginBulkGuestWrite", registered)


if __name__ == "__main__":
    unittest.main()
