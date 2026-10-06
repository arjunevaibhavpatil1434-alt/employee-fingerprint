# Running Employees Access on the server

Everything runs on `server@192.168.88.84` from `~/fingerprint_system`.

| What | Where |
|---|---|
| Web app (phones, browsers, Android app) | http://192.168.88.84:8080 |
| Data sheet | http://192.168.88.84:8080/sheet.html |
| Android app download | http://192.168.88.84:8080/download/employees-access.apk |
| Terminal API (ESP32 firmware `SERVER_BASE_URL`) | http://192.168.88.84:8100 |
| Database | `~/fingerprint_system/backend/biometric.db` |
| Nightly backups (02:00 IST, kept 30 days) | `~/fingerprint_system/backups/` |
| Nightly attendance reports (00:15 IST) | `~/fingerprint_system/reports/` (also in the web app: Attendance → Automatic reports) |

Both services run in Docker (`ea-backend`, `ea-web`) with `restart: unless-stopped`,
so they start again after a crash or a server reboot. The old prototype
containers (`cbio-api` on port 8000, `cbio-postgres`) are separate and untouched.

## Everyday commands (on the server)

```sh
cd ~/fingerprint_system/deploy

docker compose ps                  # are both running?
docker compose logs -f backend     # live server log (punches, enrollments)
docker compose restart             # restart after changing backend/ or phone/ code
docker compose down                # stop
./install.sh                       # start + (re)install the backup schedule

./backup.sh                        # backup right now
TZ=Asia/Kolkata python3 report.py --date 2026-10-05   # (re)make a day's reports
TZ=Asia/Kolkata python3 report.py --month 2026-09     # (re)make a month's reports
python3 ../backend/server.py --set-password   # change the admin password
```

## Restoring a backup

```sh
cd ~/fingerprint_system
docker compose -f deploy/docker-compose.yml stop backend
cp backups/biometric-YYYYMMDD-HHMM.db backend/biometric.db
docker compose -f deploy/docker-compose.yml start backend
```

## Firmware

The ESP32 must be plugged into the machine that flashes it. The firmware
source is in `firmware/hello_world`; build and flash with ESP-IDF
(`idf.py build flash`). Its server address is `SERVER_BASE_URL` in
`main/hello_world_main.c`.
