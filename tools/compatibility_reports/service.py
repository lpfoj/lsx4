"""Private compatibility report inbox. Python 3.12 standard library only."""
import argparse
from contextlib import contextmanager, closing
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import sqlite3
import threading
import time
import uuid
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = "1.1.0"
ROUTE = "/api/v1/compatibility-reports"
BODY_LIMIT = 4096
MAX_ROWS = 100_000
MAX_DB_BYTES = 128 * 1024 * 1024
TOP = {"schema_version", "client_report_id", "game", "result", "app_version", "device"}
DEVICE_TEXT = {"manufacturer": 128, "model": 128, "android_version": 64,
               "soc": 128, "cpu_abi": 64, "gpu_renderer": 256, "gpu_vendor": 128,
               "vulkan_device": 256, "vulkan_api_version": 64, "vulkan_driver_version": 64}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate field")
        result[key] = value
    return result


def fields(value, expected):
    if type(value) is not dict or set(value) != expected:
        raise ValueError("fields")


def text(value, limit, nullable=False):
    if value is None and nullable:
        return
    if type(value) is not str or not value.strip() or len(value) > limit:
        raise ValueError("text")

    value.encode("utf-8", "strict")
    if any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise ValueError("control")


def integer(value, low, high, nullable=False):
    if value is None and nullable:
        return
    if type(value) is not int or not low <= value <= high:
        raise ValueError("integer")


def validate(body):
    if len(body) > BODY_LIMIT:
        raise ValueError("size")
    value = json.loads(body.decode("utf-8", "strict"), object_pairs_hook=unique_object,
                       parse_constant=lambda _: (_ for _ in ()).throw(ValueError("constant")))
    fields(value, TOP)
    integer(value["schema_version"], 1, 1)
    text(value["client_report_id"], 36)
    report_uuid = uuid.UUID(value["client_report_id"])
    if str(report_uuid) != value["client_report_id"] or report_uuid.version != 4:
        raise ValueError("uuid")
    if value["result"] not in ("supported", "unsupported"):
        raise ValueError("result")
    text(value["app_version"], 64)
    game = value["game"]
    fields(game, {"title", "title_id", "platform"})
    text(game["title"], 256)
    if game["platform"] not in ("PS4", "PS5"):
        raise ValueError("platform")
    prefix = "CUSA" if game["platform"] == "PS4" else "PPSA"
    if type(game["title_id"]) is not str or not re.fullmatch(prefix + r"[0-9]{5}", game["title_id"]):
        raise ValueError("title_id")
    device = value["device"]
    fields(device, set(DEVICE_TEXT) | {"android_api", "ram_total_mib"})
    for name, limit in DEVICE_TEXT.items():
        text(device[name], limit, nullable=True)
    integer(device["android_api"], 26, 100)
    integer(device["ram_total_mib"], 1, 1_048_576, nullable=True)
    return value


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


class Store:
    def __init__(self, path):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        with self.connect() as db:
            version = db.execute("PRAGMA user_version").fetchone()[0]
            if version not in (0, 1):
                raise RuntimeError("unsupported database version")
            db.execute("PRAGMA journal_mode=WAL")
            db.execute("PRAGMA max_page_count=32768")
            db.executescript("""
                CREATE TABLE IF NOT EXISTS reports (
                    report_id TEXT PRIMARY KEY, client_report_id TEXT UNIQUE NOT NULL,
                    received_at TEXT NOT NULL, received_unix INTEGER NOT NULL,
                    title_id TEXT NOT NULL, payload TEXT NOT NULL);
                CREATE INDEX IF NOT EXISTS reports_title ON reports(title_id);
                CREATE INDEX IF NOT EXISTS reports_time ON reports(received_unix);
                PRAGMA user_version=1;
            """)
        os.chmod(self.path, 0o600)

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.path, timeout=3, isolation_level="DEFERRED")
        db.execute("PRAGMA busy_timeout=3000")
        db.execute("PRAGMA secure_delete=ON")
        db.execute("PRAGMA max_page_count=32768")
        try:
            with db:
                yield db
        finally:
            db.close()

    def receive(self, value):
        payload = canonical(value)
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            previous = db.execute("SELECT report_id,received_at,payload FROM reports WHERE client_report_id=?",
                                  (value["client_report_id"],)).fetchone()
            if previous:
                if previous[2] != payload:
                    raise ValueError("uuid reused with different content")
                return {"report_id": previous[0], "received_at": previous[1]}
            if (db.execute("SELECT count(*) FROM reports").fetchone()[0] >= MAX_ROWS
                    or self.path.stat().st_size >= MAX_DB_BYTES):
                raise sqlite3.OperationalError("quota")
            now = int(time.time())
            received = datetime.fromtimestamp(now, timezone.utc).isoformat().replace("+00:00", "Z")
            report_id = str(uuid.uuid4())
            db.execute("INSERT INTO reports VALUES (?,?,?,?,?,?)", (report_id,
                       value["client_report_id"], received, now, value["game"]["title_id"], payload))
            return {"report_id": report_id, "received_at": received}

    def statistics(self):
        """Private aggregate of reports, never an estimate of unique people."""
        with self.connect() as db:
            rows = db.execute("""SELECT title_id,
                json_extract(payload,'$.game.platform'),
                json_extract(payload,'$.device.manufacturer'),
                json_extract(payload,'$.device.model'),
                json_extract(payload,'$.device.soc'),
                sum(json_extract(payload,'$.result')='supported'),
                sum(json_extract(payload,'$.result')='unsupported'), count(*)
                FROM reports GROUP BY 1,2,3,4,5 ORDER BY 1,2,3,4,5""").fetchall()
        keys = ('title_id', 'platform', 'manufacturer', 'model', 'soc',
                'playable_reports', 'not_playable_reports', 'reports')
        return {'counting_unit': 'reports', 'total_reports': sum(row[7] for row in rows),
                'groups': [dict(zip(keys, row)) for row in rows]}

    def backup(self, target):
        target = Path(target)
        if target.exists() or target.resolve() == self.path.resolve():
            raise ValueError("backup destination must be new")
        target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        with self.connect() as db, closing(sqlite3.connect(target)) as dest:
            db.backup(dest)
            if dest.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
                raise RuntimeError("invalid backup")
        os.chmod(target, 0o600)


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    def __init__(self, address, store):
        self.store = store
        self.slots = threading.BoundedSemaphore(16)
        super().__init__(address, Handler)

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, address)
        except Exception:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()

    def handle_error(self, request, client_address):

        pass


class Handler(BaseHTTPRequestHandler):
    server_version = "CompatibilityReports"
    sys_version = ""
    def setup(self):
        super().setup()
        self.connection.settimeout(5)

    def log_message(self, *args):
        pass

    def respond(self, status, payload, started):
        body = json.dumps(payload, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        try:
            self.wfile.write(body)
        except (OSError, socket.timeout):
            pass
        print(json.dumps({"status": status, "duration_ms": round((time.monotonic() - started) * 1000),
                          "request_id": str(uuid.uuid4())}), flush=True)

    def do_POST(self):
        start = time.monotonic()
        if self.path != ROUTE:
            return self.respond(404, {"error": "not_found"}, start)
        lengths = self.headers.get_all("Content-Length", [])
        if self.headers.get("Transfer-Encoding") or len(lengths) != 1 or not lengths[0].isascii() or not lengths[0].isdigit():
            return self.respond(400, {"error": "invalid_request"}, start)
        if len(lengths[0]) > 10 or int(lengths[0]) > BODY_LIMIT:
            return self.respond(413, {"error": "body_too_large"}, start)
        if self.headers.get("Content-Type", "").lower() not in ("application/json", "application/json; charset=utf-8"):
            return self.respond(400, {"error": "invalid_request"}, start)
        try:
            length = int(lengths[0])
            body = self.rfile.read(length)
            if len(body) != length:
                raise ValueError("incomplete body")
            value = validate(body)
            receipt = self.server.store.receive(value)
        except (ValueError, UnicodeError, RecursionError, TypeError, KeyError, OverflowError):
            return self.respond(400, {"error": "invalid_report"}, start)
        except socket.timeout:
            return self.respond(408, {"error": "request_timeout"}, start)
        except (sqlite3.Error, OSError):
            return self.respond(503, {"error": "temporarily_unavailable"}, start)
        self.respond(201, receipt, start)

    def do_GET(self):
        self.respond(404, {"error": "not_found"}, time.monotonic())


def restore(source, destination):
    """Restore into a NEW path; service must be stopped before activating it."""
    source, destination = Path(source), Path(destination)
    if destination.exists() or not source.is_file():
        raise ValueError("restore requires existing source and new destination")
    with closing(sqlite3.connect(f"file:{source.as_posix()}?mode=ro", uri=True)) as src:
        if src.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
            raise ValueError("invalid backup")
        with closing(sqlite3.connect(destination)) as dst:
            src.backup(dst)
    os.chmod(destination, 0o600)


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", default="/var/lib/lsx4-reports/reports.sqlite3")
    actions = parser.add_subparsers(dest="action", required=True)
    serve = actions.add_parser("serve")
    serve.add_argument("--port", type=int, default=8765)
    actions.add_parser("count")
    actions.add_parser("stats")
    delete = actions.add_parser("delete")
    delete.add_argument("report_id")
    show = actions.add_parser("show")
    show.add_argument("report_id")
    backup = actions.add_parser("backup")
    backup.add_argument("destination")
    restored = actions.add_parser("restore")
    restored.add_argument("source")
    restored.add_argument("destination")
    args = parser.parse_args()
    if args.action == "restore":
        restore(args.source, args.destination)
        return
    store = Store(args.db)
    if args.action == "serve":
        Server(("127.0.0.1", args.port), store).serve_forever()
    elif args.action == "stats":
        print(json.dumps(store.statistics(), ensure_ascii=False))
    elif args.action == "backup":
        store.backup(args.destination)
    else:
        with store.connect() as db:
            if args.action == "delete":
                uuid.UUID(args.report_id)
                print(db.execute("DELETE FROM reports WHERE report_id=?", (args.report_id,)).rowcount)
            elif args.action == "count":
                print(db.execute("SELECT count(*) FROM reports").fetchone()[0])
            else:
                row = db.execute("SELECT payload FROM reports WHERE report_id=?", (args.report_id,)).fetchone()
                if not row:
                    raise SystemExit("report not found")
                print(row[0])


if __name__ == "__main__":
    main()
