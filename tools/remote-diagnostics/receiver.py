#!/usr/bin/env python3

import hashlib
import hmac
import json
import os
import re
import secrets
import tempfile
import zipfile
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse


DATA_DIR = Path(os.environ.get("LSX4_DIAGNOSTICS_DATA_DIR", "/srv/lsx4-diagnostics"))
TOKEN_FILE = Path(os.environ.get(
    "LSX4_DIAGNOSTICS_TOKEN_FILE", "/etc/lsx4-diagnostics/upload.token"))
MAX_BODY = int(os.environ.get("LSX4_DIAGNOSTICS_MAX_BODY", str(8 * 1024 * 1024)))
MAX_UNCOMPRESSED = 32 * 1024 * 1024
MAX_ARCHIVE_FILES = 96
MAX_REPORTS_PER_INSTALL = 30


def utc_now():
    return datetime.now(timezone.utc)


def safe_component(value, fallback="unknown", limit=80):
    value = re.sub(r"[^A-Za-z0-9_.-]+", "-", str(value or "")).strip("-.")
    return (value or fallback)[:limit]


def load_token():
    return TOKEN_FILE.read_text(encoding="utf-8").strip()


def validate_archive(path):
    if not zipfile.is_zipfile(path):
        raise ValueError("body is not a ZIP archive")
    with zipfile.ZipFile(path, "r") as archive:
        entries = archive.infolist()
        if not entries or len(entries) > MAX_ARCHIVE_FILES:
            raise ValueError("invalid archive entry count")
        total = 0
        for entry in entries:
            name = entry.filename.replace("\\", "/")
            if name.startswith("/") or ".." in name.split("/"):
                raise ValueError("unsafe archive path")
            total += entry.file_size
            if total > MAX_UNCOMPRESSED:
                raise ValueError("archive expands beyond the limit")


def prune_reports(install_dir):
    reports = sorted(install_dir.glob("*.zip"), key=lambda item: item.stat().st_mtime,
                     reverse=True)
    for archive in reports[MAX_REPORTS_PER_INSTALL:]:
        metadata = archive.with_suffix(".json")
        archive.unlink(missing_ok=True)
        metadata.unlink(missing_ok=True)


class DiagnosticsHandler(BaseHTTPRequestHandler):
    server_version = "lsx4-diagnostics/1.0"

    def log_message(self, _format, *_args):
        return

    def send_json(self, status, payload):
        raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        if urlparse(self.path).path == "/health":
            self.send_json(200, {"ok": True, "time": utc_now().isoformat()})
            return
        self.send_json(404, {"ok": False, "error": "not found"})

    def do_POST(self):
        if urlparse(self.path).path != "/upload":
            self.send_json(404, {"ok": False, "error": "not found"})
            return
        expected = "Bearer " + load_token()
        supplied = self.headers.get("Authorization", "")
        if not hmac.compare_digest(expected, supplied):
            self.send_json(401, {"ok": False, "error": "unauthorized"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        if length <= 0 or length > MAX_BODY:
            self.send_json(413, {"ok": False, "error": "invalid body size"})
            return

        incoming = DATA_DIR / ".incoming"
        incoming.mkdir(parents=True, exist_ok=True, mode=0o700)
        temporary = None
        try:
            digest = hashlib.sha256()
            with tempfile.NamedTemporaryFile(dir=incoming, prefix="upload-", suffix=".tmp",
                                             delete=False) as output:
                temporary = Path(output.name)
                remaining = length
                while remaining:
                    chunk = self.rfile.read(min(256 * 1024, remaining))
                    if not chunk:
                        raise ValueError("truncated request body")
                    output.write(chunk)
                    digest.update(chunk)
                    remaining -= len(chunk)
            validate_archive(temporary)

            install_id = safe_component(self.headers.get("X-LSX4-Install"), "anonymous")
            process_name = safe_component(self.headers.get("X-LSX4-Process"), "process")
            session_id = safe_component(self.headers.get("X-LSX4-Session"), "session")
            reason = safe_component(self.headers.get("X-LSX4-Reason"), "periodic")
            now = utc_now()
            report_id = "%s-%s-%s-%s" % (
                now.strftime("%Y%m%dT%H%M%S.%fZ"), process_name, session_id,
                secrets.token_hex(4))
            install_dir = DATA_DIR / install_id
            install_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
            target = install_dir / (report_id + ".zip")
            os.replace(temporary, target)
            temporary = None
            target.chmod(0o600)

            metadata = {
                "report_id": report_id,
                "received_at": now.isoformat(),
                "install_id": install_id,
                "process": process_name,
                "session": session_id,
                "reason": reason,
                "build": str(self.headers.get("X-LSX4-Build", ""))[:160],
                "device": str(self.headers.get("X-LSX4-Device", ""))[:240],
                "remote_ip": self.headers.get("X-Real-IP") or self.client_address[0],
                "size": target.stat().st_size,
                "sha256": digest.hexdigest(),
            }
            metadata_path = target.with_suffix(".json")
            metadata_path.write_text(json.dumps(metadata, ensure_ascii=False, indent=2),
                                     encoding="utf-8")
            metadata_path.chmod(0o600)
            prune_reports(install_dir)
            self.send_json(201, {"ok": True, "report_id": report_id})
        except Exception as error:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
            self.send_json(400, {"ok": False, "error": str(error)[:300]})


def main():
    DATA_DIR.mkdir(parents=True, exist_ok=True, mode=0o700)
    if not load_token():
        raise RuntimeError("empty upload token")
    server = ThreadingHTTPServer(("127.0.0.1", 8092), DiagnosticsHandler)
    server.serve_forever()


if __name__ == "__main__":
    main()
