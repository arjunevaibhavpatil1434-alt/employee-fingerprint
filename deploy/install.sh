#!/usr/bin/env bash
# One-time setup on the server: start the services and schedule the
# nightly backup and attendance reports. Safe to run again.
set -euo pipefail
cd "$(dirname "$0")"

# Create these first, or Docker creates them owned by root
mkdir -p ../backups ../reports

docker compose up -d

# Nightly backup at 02:00 India time = 20:30 UTC (this server runs on UTC);
# replaces an older entry if present
line="30 20 * * * $(pwd)/backup.sh >> $(pwd)/../backups/backup.log 2>&1"
# (no crontab yet is fine: crontab -l fails, so tolerate that)
{ crontab -l 2>/dev/null || true; } | { grep -v -e "fingerprint_system/deploy/backup.sh" -e "report.py" || true; } > /tmp/ea-cron.$$
echo "$line" >> /tmp/ea-cron.$$

# Attendance reports for the finished day at 00:15 India time = 18:45 UTC
echo "45 18 * * * cd $(pwd) && TZ=Asia/Kolkata python3 report.py >> $(pwd)/../reports/report.log 2>&1" >> /tmp/ea-cron.$$
crontab /tmp/ea-cron.$$
rm -f /tmp/ea-cron.$$

docker compose ps
echo
echo "Web app:  http://$(hostname -I | awk '{print $1}'):8080"
echo "Terminal API: port 8100"
