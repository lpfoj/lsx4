#!/bin/sh
set -eu
test "$(id -u)" = 0
stage=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test -f "$stage/service.py"
stamp=$(date -u +%Y%m%dT%H%M%SZ)
backup=/etc/caddy/Caddyfile.before-reports-$stamp
cp -p /etc/caddy/Caddyfile "$backup"
chmod 600 "$backup"
getent passwd lsx4-reports >/dev/null || useradd --system --home-dir /var/lib/lsx4-reports --shell /usr/sbin/nologin lsx4-reports
install -d -o root -g root -m 755 /opt/lsx4-reports
install -d -o lsx4-reports -g lsx4-reports -m 700 /var/lib/lsx4-reports /var/lib/lsx4-reports/backups
install -o root -g root -m 644 "$stage/service.py" "$stage/maintenance.py" /opt/lsx4-reports/
for unit in lsx4-reports.service lsx4-reports-maintenance.service lsx4-reports-maintenance.timer; do
    install -o root -g root -m 644 "$stage/$unit" "/etc/systemd/system/$unit"
done
systemctl daemon-reload
systemctl enable --now lsx4-reports.service
python3 - "$backup" <<'PY'
from pathlib import Path
import sys
p = Path('/etc/caddy/Caddyfile')
old = p.read_text()
if '# LSX4 compatibility reports v1' in old:
    raise SystemExit('Already configured: review before reinstallation')
opening = 'lsx45.app, www.lsx45.app {'
if old.count(opening) != 1 or 'root * /var/www/lsx45.app/current' not in old:
    raise SystemExit('Unexpected Caddy configuration; preserved backup, no overwrite')
addition = '''
    # LSX4 compatibility reports v1 (exact path; private loopback service)
    @compatibility_reports path /api/v1/compatibility-reports
    handle @compatibility_reports {
        reverse_proxy 127.0.0.1:8765
    }
'''
p.write_text(old.replace(opening, opening + addition))
PY
if ! caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile; then
    cp -p "$backup" /etc/caddy/Caddyfile
    exit 1
fi
if ! systemctl reload caddy; then
    cp -p "$backup" /etc/caddy/Caddyfile
    systemctl reload caddy
    exit 1
fi
systemctl enable --now lsx4-reports-maintenance.timer
systemctl start lsx4-reports-maintenance.service
systemctl is-active lsx4-reports.service caddy
sha256sum /opt/lsx4-reports/service.py
printf 'Caddy backup: %s\n' "$backup"
