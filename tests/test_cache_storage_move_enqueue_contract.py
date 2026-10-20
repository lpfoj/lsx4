from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "funnel-arm/src/video_core/cache_storage.cpp"
).read_text(encoding="utf-8")


class CacheStorageMoveEnqueueContractTests(unittest.TestCase):
    def test_blob_payload_is_moved_only_after_authoritative_admission(self):
        start = SOURCE.index("bool WriteVector(")
        end = SOURCE.index("template <typename T>\nbool LoadVector", start)
        body = SOURCE[start:end]

        lock = body.index("std::scoped_lock lock{submit_mutex}")
        admission = body.index("if (!opened.load())", lock)
        move = body.index("data = std::move(v)")
        enqueue = body.index("req_queue.emplace(std::move(request))")
        self.assertLess(lock, admission)
        self.assertLess(admission, move)
        self.assertLess(move, enqueue)

    def test_background_task_owns_blob_without_synchronous_copy(self):
        start = SOURCE.index("bool WriteVector(")
        end = SOURCE.index("template <typename T>\nbool LoadVector", start)
        body = SOURCE[start:end]
        self.assertIn("path = std::move(path_)", body)
        self.assertIn("data = std::move(v)", body)
        self.assertIn("file.Write(data)", body)
        self.assertNotIn("[=]", body)
        self.assertNotIn("file.Write(v)", body)


if __name__ == "__main__":
    unittest.main()
