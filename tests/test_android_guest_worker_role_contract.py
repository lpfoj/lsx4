import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")


class AndroidGuestWorkerRoleContractTests(unittest.TestCase):
    def test_progress_services_are_not_counted_as_background_pool_workers(self):
        start = RUNTIME.index("bool IsCpuLimitedGuestWorkerName")
        end = RUNTIME.index("} // namespace", start)
        body = RUNTIME[start:end]
        self.assertIn('normalized.starts_with("unitygfx")', body)
        self.assertIn('normalized.find("gfxdeviceworker")', body)
        self.assertIn('normalized.find("enlightenworker")', body)
        self.assertIn("return !graphics_service && !enlighten_service;", body)

    def test_background_worker_limit_and_wait_remain_enabled(self):
        self.assertIn('normalized.find("jobthread")', RUNTIME)
        self.assertIn('normalized.find("job thread")', RUNTIME)
        self.assertIn("admission.condition.wait(lock", RUNTIME)
        self.assertIn("admission.active.size() < limit", RUNTIME)


if __name__ == "__main__":
    unittest.main()
