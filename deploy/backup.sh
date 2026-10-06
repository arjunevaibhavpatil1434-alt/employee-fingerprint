#!/usr/bin/env bash
# Nightly copy of the database, kept for 30 days. Safe while the
# server is running (SQLite online backup). Installed in crontab by
# install.sh; run it by hand any time.
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p backups
target="backups/biometric-$(date +%Y%m%d-%H%M).db"

python3 - "$target" <<'PY'
import sqlite3, sys
src = sqlite3.connect("backend/biometric.db")
dst = sqlite3.connect(sys.argv[1])
src.backup(dst)
dst.close()
src.close()
PY

find backups -name 'biometric-*.db' -mtime +30 -delete
echo "Backup saved: $target"
