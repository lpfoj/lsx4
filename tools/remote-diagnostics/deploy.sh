#!/bin/sh
set -eu

if ! id -u lsx4-diag >/dev/null 2>&1; then
    useradd --system --home /nonexistent --shell /usr/sbin/nologin lsx4-diag
fi

install -d -m 0755 /opt/lsx4-diagnostics
install -d -m 0750 -o root -g lsx4-diag /etc/lsx4-diagnostics
install -d -m 0700 -o lsx4-diag -g lsx4-diag /srv/lsx4-diagnostics
install -m 0755 /tmp/lsx4-receiver.py /opt/lsx4-diagnostics/receiver.py
install -m 0640 -o root -g lsx4-diag /tmp/lsx4-upload.token \
    /etc/lsx4-diagnostics/upload.token
install -m 0644 /tmp/lsx4-diagnostics.service \
    /etc/systemd/system/lsx4-diagnostics.service

systemctl daemon-reload
systemctl enable --now lsx4-diagnostics.service
ready=0
for attempt in 1 2 3 4 5 6 7 8 9 10; do
    if curl --fail --silent http://127.0.0.1:8092/health >/dev/null; then
        ready=1
        break
    fi
    sleep 1
done
if [ "$ready" -ne 1 ]; then
    systemctl --no-pager --full status lsx4-diagnostics.service || true
    exit 1
fi

install -m 0644 /tmp/lsx4-nginx-rate.conf \
    /etc/nginx/conf.d/lsx4-diagnostics-rate.conf
install -m 0644 /tmp/lsx4-nginx-location.conf \
    /etc/nginx/snippets/lsx4-diagnostics.conf

site=/etc/nginx/sites-available/llayer
backup="${site}.before-lsx4-diagnostics"
if [ ! -f "$backup" ]; then
    cp -a "$site" "$backup"
fi
python3 - "$site" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
include = "    include /etc/nginx/snippets/lsx4-diagnostics.conf;"
if include not in text:
    needle = "    location / { try_files $uri $uri/ =404; }"
    if needle not in text:
        raise SystemExit("nginx insertion point was not found")
    text = text.replace(needle, include + "\n\n" + needle, 1)
    path.write_text(text, encoding="utf-8")
PY

if ! nginx -t; then
    cp -a "$backup" "$site"
    nginx -t
    exit 1
fi
systemctl reload nginx
curl --fail --silent https://llayer.tech/lsx4-diag/health
