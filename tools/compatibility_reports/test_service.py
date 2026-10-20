import contextlib
import copy
import http.client
import io
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
import uuid

from service import Store, Server, validate, restore, DEVICE_TEXT


def report():
    return {"schema_version": 1, "client_report_id": str(uuid.uuid4()),
            "game": {"title": "Compatibility test", "title_id": "CUSA99999", "platform": "PS4"},
            "result": "supported", "app_version": "test-1", "device": {
                **{name: None for name in DEVICE_TEXT}, "android_api": 35, "ram_total_mib": None}}


class ReportsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = Store(Path(self.temp.name) / "db.sqlite3")

    def tearDown(self):
        self.temp.cleanup()

    def test_nullable_graphics_both_results_and_exact_fields(self):
        for result in ("supported", "unsupported"):
            data = report()
            data["result"] = result
            self.assertEqual(validate(json.dumps(data).encode()), data)
            receipt = self.store.receive(data)
            with self.store.connect() as db:
                stored = json.loads(db.execute("SELECT payload FROM reports WHERE report_id=?",
                                              (receipt["report_id"],)).fetchone()[0])
                self.assertEqual(stored, data)

    def test_idempotency_and_conflicting_reuse(self):
        data = report()
        first = self.store.receive(data)
        self.assertEqual(first, self.store.receive(data))
        data["result"] = "unsupported"
        with self.assertRaises(ValueError):
            self.store.receive(data)
        with self.store.connect() as db:
            self.assertEqual(db.execute("SELECT count(*) FROM reports").fetchone()[0], 1)

    def test_ps5_platform_identity(self):
        data = report()
        data["game"].update(platform="PS5", title_id="PPSA99999")
        self.assertEqual(validate(json.dumps(data).encode()), data)
        data["game"]["title_id"] = "CUSA99999"
        with self.assertRaises(ValueError):
            validate(json.dumps(data).encode())

    def test_concurrent_duplicate(self):
        data, results = report(), []
        threads = [threading.Thread(target=lambda: results.append(self.store.receive(data))) for _ in range(8)]
        for t in threads: t.start()
        for t in threads: t.join()
        self.assertEqual(len(results), 8)
        self.assertEqual(len({r["report_id"] for r in results}), 1)

    def test_strict_schema(self):
        valid = report()
        variants = []
        for level, key, value in ((None, "extra", "path"), ("device", "serial", "secret"),
                                  (None, "result", "unclassified"), (None, "schema_version", True),
                                  ("device", "android_api", "35"), ("device", "model", "a" * 129),
                                  ("game", "title_id", "CUSA00000' OR 1=1--"),
                                  (None, "client_report_id", "installation-id"),
                                  ("game", "title", "\ud800")):
            data = copy.deepcopy(valid)
            (data if level is None else data[level])[key] = value
            variants.append(json.dumps(data).encode())
        variants += [b'{"a":1,"a":2}', b'\xff', b'[]', b'null', b'{"a":NaN}', b'{' + b' ' * 4096]
        for body in variants:
            with self.subTest(body=body[:50]), self.assertRaises((ValueError, UnicodeError)):
                validate(body)

    def test_old_reports_survive_restart_and_backup_restore(self):
        receipt = self.store.receive(report())
        with self.store.connect() as db:
            db.execute("UPDATE reports SET received_unix=?", (int(time.time()) - 3650 * 86400,))
        again = Store(self.store.path)
        backup = Path(self.temp.name) / "backup.sqlite3"
        again.backup(backup)
        restored = Path(self.temp.name) / "restored.sqlite3"
        restore(backup, restored)
        with Store(restored).connect() as db:
            self.assertEqual(db.execute("SELECT report_id FROM reports").fetchone()[0], receipt["report_id"])
            self.assertLess(db.execute("SELECT received_unix FROM reports").fetchone()[0],
                            int(time.time()) - 3600 * 86400)
        self.assertEqual(again.statistics()['total_reports'], 1)

    def test_sql_data_stays_data(self):
        data = report()
        data["game"]["title"] = "'); DROP TABLE reports; --"
        self.store.receive(data)
        with self.store.connect() as db:
            self.assertEqual(db.execute("SELECT count(*) FROM reports").fetchone()[0], 1)

    def test_identical_devices_count_separate_reports_but_retry_is_idempotent(self):
        first, second = report(), report()
        first['device'].update(manufacturer='Xiaomi', model='Same model', soc='SM8650')
        second['device'] = copy.deepcopy(first['device'])
        a, b = self.store.receive(first), self.store.receive(second)
        self.assertNotEqual(a['report_id'], b['report_id'])
        self.assertEqual(a, self.store.receive(first))
        stats = self.store.statistics()
        self.assertEqual(stats['counting_unit'], 'reports')
        self.assertEqual(stats['total_reports'], 2)
        self.assertEqual(len(stats['groups']), 1)
        self.assertEqual(stats['groups'][0]['playable_reports'], 2)
        second['client_report_id'] = str(uuid.uuid4())
        second['result'] = 'unsupported'
        self.store.receive(second)
        self.assertEqual(self.store.statistics()['groups'][0]['not_playable_reports'], 1)

    def test_http_contract_and_private_logs(self):
        server = Server(("127.0.0.1", 0), self.store)
        thread = threading.Thread(target=server.serve_forever)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            thread.start()
            def request(body, method="POST", path="/api/v1/compatibility-reports"):
                conn = http.client.HTTPConnection(*server.server_address, timeout=5)
                conn.request(method, path, body, {"Content-Type": "application/json"})
                response = conn.getresponse()
                result = response.status, response.read()
                conn.close()
                return result
            try:
                data = json.dumps(report()).encode()
                status, receipt = request(data)
                self.assertEqual(status, 201)
                self.assertEqual(request(data), (201, receipt))
                self.assertEqual(request(b'{}')[0], 400)
                self.assertEqual(request(b'x' * 4097)[0], 413)
                self.assertEqual(request(b'', "GET")[0], 404)

                for _ in range(130):
                    self.assertEqual(request(json.dumps(report()).encode())[0], 201)
                self.assertEqual(self.store.statistics()['total_reports'], 131)
                self.assertEqual(request(data, path="/admin")[0], 404)
            finally:
                server.shutdown()
                server.server_close()
                thread.join()
        for line in output.getvalue().splitlines():
            self.assertEqual(set(json.loads(line)), {"status", "duration_ms", "request_id"})
        self.assertNotIn("127.0.0.1", output.getvalue())
        self.assertNotIn("Compatibility test", output.getvalue())


if __name__ == "__main__":
    unittest.main()
