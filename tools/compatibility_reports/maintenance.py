"""Daily private SQLite backup; report history has no automatic expiry."""
from datetime import datetime, timezone
from pathlib import Path
import os
import time
from service import Store

os.umask(0o077)
store = Store("/var/lib/lsx4-reports/reports.sqlite3")
directory = Path("/var/lib/lsx4-reports/backups")
directory.mkdir(mode=0o700, exist_ok=True)
stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
store.backup(directory / (stamp + ".sqlite3"))
for path in directory.glob("*.sqlite3"):
    if path.is_file() and not path.is_symlink() and path.stat().st_mtime < time.time() - 7 * 86400:
        path.unlink()
