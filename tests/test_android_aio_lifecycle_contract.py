import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AIO = (ROOT / "funnel-arm/src/core/libraries/kernel/aio.cpp").read_text(
    encoding="utf-8"
)
FS_H = (ROOT / "funnel-arm/src/core/file_sys/fs.h").read_text(encoding="utf-8")
FS_CPP = (ROOT / "funnel-arm/src/core/file_sys/fs.cpp").read_text(
    encoding="utf-8"
)
KERNEL_FS = (
    ROOT / "funnel-arm/src/core/libraries/kernel/file_system.cpp"
).read_text(encoding="utf-8")


class AndroidAioLifecycleContractTests(unittest.TestCase):
    def test_terminal_ids_are_reused_only_after_worker_retirement(self):
        reserve = AIO.split("static s32 ReserveAioId", 1)[1].split(
            "static void BindAioResults", 1
        )[0]
        self.assertIn("id_active[index]", reserve)
        self.assertIn("ORBIS_KERNEL_AIO_STATE_COMPLETED", reserve)
        self.assertIn("ORBIS_KERNEL_AIO_STATE_ABORTED", reserve)
        self.assertIn("id_active[index] = true", reserve)
        self.assertNotIn("compare_exchange", reserve)
        self.assertNotIn("terminal_pass", reserve)
        self.assertEqual(reserve.count("attempt < MAX_QUEUE - 1"), 1)

    def test_result_record_precedes_terminal_id_publication(self):
        store = AIO.split("static bool StoreAioResult", 1)[1].split(
            "static void RetireCanceledAioRequest", 1
        )[0]
        self.assertLess(
            store.index("result->returnValue = return_value"),
            store.index("id_state[index].store(requested_state"),
        )
        self.assertLess(
            store.index("result->state = requested_state"),
            store.index("id_state[index].store(requested_state"),
        )

    def test_cancelled_worker_retires_without_retouching_guest_result(self):
        retire = AIO.split("static void RetireCanceledAioRequest", 1)[1].split(
            "static void AbortAioBatch", 1
        )[0]
        self.assertIn("id_active[index] = false", retire)
        self.assertNotIn("result->", retire)
        abort = AIO.split("static void AbortAioBatch", 1)[1].split(
            "static void SubmitAio", 1
        )[0]
        self.assertLess(
            abort.index("ORBIS_KERNEL_AIO_STATE_PROCESSING"),
            abort.index("result->returnValue"),
        )

    def test_batch_failure_makes_every_pending_result_terminal(self):
        abort = AIO.split("static void AbortAioBatch", 1)[1].split(
            "static void SubmitAio", 1
        )[0]
        self.assertIn("index < static_cast<s32>(requests.size())", abort)
        self.assertIn("result->state = ORBIS_KERNEL_AIO_STATE_ABORTED", abort)
        self.assertIn("id_active[state_index] = false", abort)

    def test_delete_validation_and_clear_share_one_lock(self):
        delete_one = AIO.split("sceKernelAioDeleteRequest", 1)[1].split(
            "sceKernelAioDeleteRequests", 1
        )[0]
        critical = delete_one.split("std::lock_guard lock{aio_state_mutex}", 1)[1]
        self.assertLess(critical.index("id_active[index]"), critical.index("id_results[index].clear"))
        self.assertLess(critical.index("id_results[index].clear"), critical.index("id_state[index].store(0"))

    def test_async_file_lease_prevents_fd_close_and_reuse(self):
        self.assertIn("class AsyncFileLease", FS_H)
        self.assertIn("AcquireFileForAsync", FS_H)
        acquire = FS_CPP.split("AsyncFileLease HandleTable::AcquireFileForAsync", 1)[1]
        self.assertIn("file->async_closing", acquire)
        self.assertIn("++file->async_users", acquire)
        take = FS_CPP.split("std::unique_ptr<File> HandleTable::TakeHandle", 1)[1].split(
            "AsyncFileLease HandleTable::AcquireFileForAsync", 1
        )[0]
        self.assertLess(take.index("file->async_closing = true"), take.index("async_condition.wait"))
        self.assertLess(take.index("async_condition.wait"), take.index("m_files[static_cast<std::size_t>(d)] = nullptr"))
        self.assertIn("auto file_owner = h->TakeHandle(fd)", KERNEL_FS)
        self.assertIn("AcquireAioFiles(req, size)", AIO)

    def test_session_registration_drains_old_worker_before_slot_reset(self):
        register = AIO.split("void RegisterAio", 1)[1]
        self.assertLess(register.index("worker->Drain()"), register.index("id_index.store(0"))
        self.assertLess(register.index("worker->Drain()"), register.index("id_results[index].clear"))

    def test_cpu_switch_still_selects_real_inline_or_worker_execution(self):
        dispatch = AIO.split("static void SubmitAio", 1)[1].split(
            "sceKernelAioInitializeImpl", 1
        )[0]
        self.assertIn("policy.subsystem_multithreading != 0", dispatch)
        self.assertIn("policy.separate_io_thread != 0", dispatch)
        self.assertIn("GetAioWorker().Submit", dispatch)
        self.assertIn("job();", dispatch)


if __name__ == "__main__":
    unittest.main()
