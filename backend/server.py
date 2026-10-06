#!/usr/bin/env python3
"""
Employees Access backend: employees, terminals, fingerprint slots and
access events, stored in SQLite. Standard library only.

    python3 server.py                     # 0.0.0.0:8000, ./biometric.db
    python3 server.py --port 8000 --db /path/to/biometric.db
    python3 server.py --set-password      # set the admin password, then exit

The admin endpoints (employees, logs, exports) need a login; the terminal's
endpoints (heartbeat, enrollment, access events) and /health do not.

Endpoints (JSON in, JSON out; errors are {"detail": "..."}):

    GET  /health
    GET  /health/database
    GET  /api/v1/employees
    POST /api/v1/employees              phone app: create an employee
    POST /api/v1/employees/delete       phone app: {"employee_id": 3}
    POST /api/v1/devices/heartbeat      terminal: reports online at boot
    POST /api/v1/enrollment/start       terminal: validate + assign slot
    POST /api/v1/enrollment/complete    terminal: slot now holds the finger
    POST /api/v1/access-events          terminal: who does this slot belong to
    GET  /api/v1/logs?type=&from=&to=&limit=   web page: access attempts + enrollments
    GET  /api/v1/export/logs.csv?type=&from=&to=  spreadsheet download (dates: YYYY-MM-DD)
    GET  /api/v1/export/employees.csv             spreadsheet download
    POST /api/v1/auth/login             {"password": "..."} -> session cookie
    POST /api/v1/auth/logout
    GET  /api/v1/auth/me                200 when logged in, else 401
    GET  /api/v1/changes?since=N&wait=8 live updates: answers when data changes
    GET  /api/v1/attendance?date=YYYY-MM-DD       punch in/out summary for a day
    GET  /api/v1/attendance?from=&to=             the same for every day in a range
    GET  /api/v1/calendar                         weekend days + holidays
    POST /api/v1/calendar/weekend                 {"weekend_days": [5, 6]}  (Monday = 0)
    POST /api/v1/holidays                         {"date": "2026-10-20", "name": "Diwali"}
    POST /api/v1/holidays/delete                  {"date": "2026-10-20"}
    GET  /api/v1/export/attendance.csv?from=&to=  one row per employee per day
    GET  /api/v1/export/punches.csv?from=&to=     one row per punch (every IN and OUT)
    GET  /api/v1/reports                          nightly saved reports (deploy/report.py)
    GET  /api/v1/reports/download?name=daily/attendance-2026-10-05.csv
"""

import argparse
import csv
import getpass
import hashlib
import hmac
import io
import json
import re
import os
import secrets
import sqlite3
import sys
import threading
import time
from datetime import datetime, timedelta, timezone
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

DEFAULT_DB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "biometric.db")
DEFAULT_COMPANY_ID = 1
SENSOR_CAPACITY = 1000          # R307S template slots; slot 0 is never used
MAX_BODY = 16 * 1024

SCHEMA = """
CREATE TABLE IF NOT EXISTS companies (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS employees (
    id            INTEGER PRIMARY KEY,
    company_id    INTEGER NOT NULL REFERENCES companies(id),
    employee_code TEXT NOT NULL,
    full_name     TEXT NOT NULL,
    department    TEXT,
    email         TEXT,
    phone         TEXT,
    active        INTEGER NOT NULL DEFAULT 1,
    created_at    TEXT NOT NULL,
    deleted_at    TEXT            -- set when deleted; kept so old logs keep the name
);

-- A deleted employee's code can be given to someone new
CREATE UNIQUE INDEX IF NOT EXISTS employees_code_current
    ON employees (company_id, employee_code) WHERE deleted_at IS NULL;

CREATE TABLE IF NOT EXISTS devices (
    device_id        TEXT PRIMARY KEY,
    company_id       INTEGER NOT NULL REFERENCES companies(id),
    firmware_version TEXT,
    last_seen        TEXT
);

CREATE TABLE IF NOT EXISTS biometric_records (
    id           INTEGER PRIMARY KEY,
    employee_id  INTEGER NOT NULL REFERENCES employees(id),
    device_id    TEXT NOT NULL REFERENCES devices(device_id),
    finger_index INTEGER NOT NULL,
    sensor_slot  INTEGER NOT NULL,
    active       INTEGER NOT NULL DEFAULT 1,
    created_at   TEXT NOT NULL,
    UNIQUE (device_id, sensor_slot),
    UNIQUE (device_id, employee_id, finger_index)
);

CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS holidays (
    date TEXT PRIMARY KEY,          -- YYYY-MM-DD
    name TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS sessions (
    token_hash TEXT PRIMARY KEY,    -- SHA-256 of the cookie value
    expires_at REAL NOT NULL        -- Unix time
);

CREATE TABLE IF NOT EXISTS access_events (
    id          INTEGER PRIMARY KEY,
    device_id   TEXT NOT NULL,
    sensor_slot INTEGER NOT NULL,
    match_score INTEGER,
    employee_id INTEGER,
    status      TEXT NOT NULL,
    reason      TEXT,
    punch       TEXT,           -- 'in' / 'out' for attendance; NULL otherwise
    created_at  TEXT NOT NULL
);
"""


class Download:
    """Handler result sent as a CSV file download instead of JSON."""

    def __init__(self, filename, data):
        self.filename = filename
        self.data = data


class CsvFile(Download):
    """A CSV built from a header and rows."""

    def __init__(self, filename, header, rows):
        self.filename = filename
        out = io.StringIO()
        writer = csv.writer(out, lineterminator="\r\n")
        writer.writerow(header)
        writer.writerows(rows)
        # BOM so Excel reads non-ASCII names correctly
        self.data = ("\ufeff" + out.getvalue()).encode("utf-8")


class ApiError(Exception):
    def __init__(self, status, detail):
        super().__init__(detail)
        self.status = status
        self.detail = detail


def now():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


class Database:
    def __init__(self, path):
        self.conn = sqlite3.connect(path, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self._migrate()
        self.conn.execute("PRAGMA foreign_keys = ON")
        self.conn.executescript(SCHEMA)
        self.conn.execute(
            "INSERT OR IGNORE INTO companies (id, name) VALUES (?, ?)",
            (DEFAULT_COMPANY_ID, "Default company"),
        )
        self.conn.commit()
        # One request at a time: keeps slot allocation race-free
        self.lock = threading.Lock()

    def _migrate(self):
        self._migrate_employees()

        events = [r["name"] for r in self.conn.execute("PRAGMA table_info(access_events)")]
        if events and "punch" not in events:
            self.conn.execute("ALTER TABLE access_events ADD COLUMN punch TEXT")
            self.conn.commit()

    def _migrate_employees(self):
        """Adds deleted_at to employees created by older versions.

        The old table had UNIQUE (company_id, employee_code), which SQLite
        cannot drop in place, so the table is rebuilt with the same rows.
        """
        columns = [r["name"] for r in self.conn.execute("PRAGMA table_info(employees)")]
        if not columns or "deleted_at" in columns:
            return

        self.conn.executescript("""
            PRAGMA foreign_keys = OFF;
            BEGIN;
            CREATE TABLE employees_new (
                id            INTEGER PRIMARY KEY,
                company_id    INTEGER NOT NULL REFERENCES companies(id),
                employee_code TEXT NOT NULL,
                full_name     TEXT NOT NULL,
                department    TEXT,
                email         TEXT,
                phone         TEXT,
                active        INTEGER NOT NULL DEFAULT 1,
                created_at    TEXT NOT NULL,
                deleted_at    TEXT
            );
            INSERT INTO employees_new
                (id, company_id, employee_code, full_name, department,
                 email, phone, active, created_at)
            SELECT id, company_id, employee_code, full_name, department,
                   email, phone, active, created_at
            FROM employees;
            DROP TABLE employees;
            ALTER TABLE employees_new RENAME TO employees;
            COMMIT;
            PRAGMA foreign_keys = ON;
        """)

    def ensure_device(self, device_id, firmware_version=None):
        self.conn.execute(
            """INSERT INTO devices (device_id, company_id, firmware_version, last_seen)
               VALUES (?, ?, ?, ?)
               ON CONFLICT (device_id) DO UPDATE SET
                   firmware_version = COALESCE(excluded.firmware_version, firmware_version),
                   last_seen = excluded.last_seen""",
            (device_id, DEFAULT_COMPANY_ID, firmware_version, now()),
        )
        return self.conn.execute(
            "SELECT * FROM devices WHERE device_id = ?", (device_id,)
        ).fetchone()


def require_str(body, key, optional=False):
    value = body.get(key)
    if value is None or (isinstance(value, str) and value.strip() == ""):
        if optional:
            return None
        raise ApiError(422, f"{key} is required")
    if not isinstance(value, str):
        raise ApiError(422, f"{key} must be a string")
    return value.strip()


def require_int(body, key, low=None, high=None):
    value = body.get(key)
    if not isinstance(value, int) or isinstance(value, bool):
        raise ApiError(422, f"{key} must be a whole number")
    if (low is not None and value < low) or (high is not None and value > high):
        raise ApiError(422, f"{key} is out of range")
    return value


def employee_json(row):
    return {
        "id": row["id"],
        "company_id": row["company_id"],
        "employee_code": row["employee_code"],
        "full_name": row["full_name"],
        "department": row["department"],
        "email": row["email"],
        "phone": row["phone"],
        "active": bool(row["active"]),
        "created_at": row["created_at"],
    }


# ------------------------------------------------------------
# Handlers: (db, body) -> (status, response)
# ------------------------------------------------------------

def list_employees(db, _body):
    rows = db.conn.execute(
        """SELECT e.*,
                  (SELECT COUNT(*) FROM biometric_records b
                   WHERE b.employee_id = e.id AND b.active = 1) AS fingers,
                  (SELECT MAX(a.created_at) FROM access_events a
                   WHERE a.employee_id = e.id AND a.status = 'allowed') AS last_seen
           FROM employees e
           WHERE e.deleted_at IS NULL
           ORDER BY e.id"""
    ).fetchall()
    return 200, {"employees": [
        dict(employee_json(r), fingers=r["fingers"], last_seen=r["last_seen"]) for r in rows
    ]}


def delete_employee(db, body):
    """Removes an employee: their fingerprint slots are freed at once (the
    terminal then reports "finger not registered"); their access history
    stays, still showing their name."""
    employee_id = require_int(body, "employee_id", low=1)

    employee = db.conn.execute(
        "SELECT * FROM employees WHERE id = ? AND deleted_at IS NULL", (employee_id,)
    ).fetchone()
    if employee is None:
        raise ApiError(404, f"Employee not found (id {employee_id})")

    slots = [r["sensor_slot"] for r in db.conn.execute(
        "SELECT sensor_slot FROM biometric_records WHERE employee_id = ?", (employee_id,))]

    db.conn.execute("DELETE FROM biometric_records WHERE employee_id = ?", (employee_id,))
    db.conn.execute(
        "UPDATE employees SET active = 0, deleted_at = ? WHERE id = ?", (now(), employee_id))
    db.conn.commit()

    return 200, {"status": "deleted", "employee_id": employee_id,
                 "full_name": employee["full_name"], "freed_slots": slots}


def create_employee(db, body):
    company_id = require_int(body, "company_id", low=1)
    code = require_str(body, "employee_code").upper()
    name = require_str(body, "full_name")

    if db.conn.execute("SELECT 1 FROM companies WHERE id = ?", (company_id,)).fetchone() is None:
        raise ApiError(404, f"Company ID {company_id} not found on the server.")

    try:
        cur = db.conn.execute(
            """INSERT INTO employees
                   (company_id, employee_code, full_name, department, email, phone, created_at)
               VALUES (?, ?, ?, ?, ?, ?, ?)""",
            (company_id, code, name,
             require_str(body, "department", optional=True),
             require_str(body, "email", optional=True),
             require_str(body, "phone", optional=True),
             now()),
        )
    except sqlite3.IntegrityError:
        raise ApiError(409, f"Employee code {code} already exists")

    db.conn.commit()
    row = db.conn.execute("SELECT * FROM employees WHERE id = ?", (cur.lastrowid,)).fetchone()
    return 201, {"employee": employee_json(row)}


def heartbeat(db, body):
    device_id = require_str(body, "device_id")
    device = db.ensure_device(device_id, body.get("firmware_version"))
    db.conn.commit()
    return 200, {"status": "ok", "device_id": device_id, "company_id": device["company_id"]}


def load_enrollment(db, body):
    employee_id = require_int(body, "employee_id", low=1)
    device_id = require_str(body, "device_id")
    finger_index = require_int(body, "finger_index", low=1, high=10)

    employee = db.conn.execute(
        "SELECT * FROM employees WHERE id = ? AND deleted_at IS NULL", (employee_id,)
    ).fetchone()
    if employee is None:
        raise ApiError(404, f"Employee not found (id {employee_id})")
    if not employee["active"]:
        raise ApiError(409, "Employee is inactive")

    device = db.ensure_device(device_id)
    if device["company_id"] != employee["company_id"]:
        raise ApiError(409, "Employee and terminal belong to different companies")

    existing = db.conn.execute(
        """SELECT sensor_slot FROM biometric_records
           WHERE device_id = ? AND employee_id = ? AND finger_index = ? AND active = 1""",
        (device_id, employee_id, finger_index),
    ).fetchone()
    if existing is not None:
        raise ApiError(409, f"This finger is already enrolled (slot {existing['sensor_slot']})")

    return employee, device_id, finger_index


def enrollment_start(db, body):
    employee, device_id, _finger = load_enrollment(db, body)

    used = {r[0] for r in db.conn.execute(
        "SELECT sensor_slot FROM biometric_records WHERE device_id = ?", (device_id,))}
    slot = next((s for s in range(1, SENSOR_CAPACITY) if s not in used), None)
    if slot is None:
        raise ApiError(409, "The terminal's fingerprint memory is full")

    db.conn.commit()
    return 200, {"sensor_slot": slot, "full_name": employee["full_name"],
                 "employee_id": employee["id"]}


def enrollment_complete(db, body):
    employee, device_id, finger_index = load_enrollment(db, body)
    slot = require_int(body, "sensor_slot", low=1, high=SENSOR_CAPACITY - 1)

    try:
        db.conn.execute(
            """INSERT INTO biometric_records
                   (employee_id, device_id, finger_index, sensor_slot, created_at)
               VALUES (?, ?, ?, ?, ?)""",
            (employee["id"], device_id, finger_index, slot, now()),
        )
    except sqlite3.IntegrityError:
        raise ApiError(409, f"Sensor slot {slot} is already assigned")

    db.conn.commit()
    return 201, {"status": "enrolled", "sensor_slot": slot, "full_name": employee["full_name"]}


# ------------------------------------------------------------
# Attendance: recognised scans alternate IN, OUT, IN, OUT through the
# day (this server's timezone); the first scan of each day is IN.
# Worked time is the sum of the IN -> OUT pairs, so breaks don't count.
# ------------------------------------------------------------

DUPLICATE_SECONDS = 30          # a second scan this soon is a double scan


def local_midnight(day):
    """UTC ISO string for the start of a local calendar day (a date)."""
    start = datetime(day.year, day.month, day.day).astimezone()
    return start.astimezone(timezone.utc).isoformat(timespec="seconds")


def day_window(day):
    return local_midnight(day), local_midnight(day + timedelta(days=1))


def hhmm(stored):
    return datetime.fromisoformat(stored).astimezone().strftime("%H:%M")


def duration_text(seconds):
    minutes = int(seconds // 60)
    return f"{minutes // 60}h {minutes % 60:02d}m"


def punches_between(db, start, end, employee_id=None):
    sql = """SELECT employee_id, punch, created_at FROM access_events
             WHERE punch IS NOT NULL AND created_at >= ? AND created_at < ?"""
    args = [start, end]
    if employee_id is not None:
        sql += " AND employee_id = ?"
        args.append(employee_id)
    return db.conn.execute(sql + " ORDER BY created_at", args).fetchall()


def summarize(punches, until=None):
    """First in, last out and worked time from one employee's punches for
    one day (oldest first). Worked time adds up each IN -> OUT pair; an IN
    without an OUT counts up to `until` (now, for today) or not at all."""
    first_in = last_out = None
    worked = 0.0
    open_since = None
    sessions = []           # every IN -> OUT stretch, in order

    for p in punches:
        moment = datetime.fromisoformat(p["created_at"])
        if p["punch"] == "in":
            first_in = first_in or p["created_at"]
            open_since = moment
            sessions.append({"in": p["created_at"], "out": None, "seconds": None})
        else:
            last_out = p["created_at"]
            if open_since is not None:
                seconds = (moment - open_since).total_seconds()
                worked += seconds
                sessions[-1].update(out=p["created_at"], seconds=int(seconds))
                open_since = None
            else:
                # OUT without an IN (shouldn't happen): keep the timestamp anyway
                sessions.append({"in": None, "out": p["created_at"], "seconds": None})

    in_office = open_since is not None
    if in_office and until is not None:
        worked += (until - open_since).total_seconds()

    return {"first_in": first_in, "last_out": last_out, "worked_seconds": int(worked),
            "in_office": in_office, "punches": len(punches), "sessions": sessions}


def access_event(db, body):
    device_id = require_str(body, "device_id")
    slot = require_int(body, "sensor_slot", low=0)
    score = body.get("match_score")
    db.ensure_device(device_id)

    row = db.conn.execute(
        """SELECT b.active AS record_active, e.id AS employee_id, e.full_name, e.active
           FROM biometric_records b JOIN employees e ON e.id = b.employee_id
           WHERE b.device_id = ? AND b.sensor_slot = ?""",
        (device_id, slot),
    ).fetchone()

    if row is None:
        status, reason, employee_id, name = "denied", "unknown_sensor_slot", None, None
    elif not row["active"]:
        status, reason, employee_id, name = "denied", "employee_inactive", row["employee_id"], row["full_name"]
    elif not row["record_active"]:
        status, reason, employee_id, name = "denied", "biometric_record_inactive", row["employee_id"], row["full_name"]
    else:
        status, reason, employee_id, name = "allowed", None, row["employee_id"], row["full_name"]

    stamp = now()
    response = {"status": status, "reason": reason}
    punch = None

    if status == "allowed":
        today = datetime.now().astimezone().date()
        start, end = day_window(today)
        todays = punches_between(db, start, end, employee_id)
        last = todays[-1] if todays else None

        seconds_since = (datetime.fromisoformat(stamp) -
                         datetime.fromisoformat(last["created_at"])).total_seconds() if last else None

        if last is not None and seconds_since < DUPLICATE_SECONDS:
            # Double scan: report the punch just made, record no new punch
            reason = "repeat_scan"
            response.update(punch=last["punch"], duplicate=True,
                            detail=f"Punched {last['punch']} at {hhmm(last['created_at'])}")
        else:
            # Alternate: in, out, in, out... (first scan of the day is in)
            punch = "out" if last is not None and last["punch"] == "in" else "in"
            response.update(punch=punch, duplicate=False,
                            detail=f"{'In' if punch == 'in' else 'Out'} at {hhmm(stamp)}")
            if punch == "out":
                worked = summarize(list(todays) + [{"punch": "out", "created_at": stamp}])
                response["worked"] = f"{duration_text(worked['worked_seconds'])} today"

    db.conn.execute(
        """INSERT INTO access_events
               (device_id, sensor_slot, match_score, employee_id, status, reason, punch, created_at)
           VALUES (?, ?, ?, ?, ?, ?, ?, ?)""",
        (device_id, slot, score if isinstance(score, int) else None,
         employee_id, status, reason, punch, stamp),
    )
    db.conn.commit()

    if name:
        response["full_name"] = name
    return 200, response


def parse_day(value, key):
    try:
        return datetime.strptime(value, "%Y-%m-%d").date()
    except (TypeError, ValueError):
        raise ApiError(422, f"{key} must be a date like 2026-10-05")


# ------------------------------------------------------------
# Office calendar: weekends and holidays are not counted as absent
# ------------------------------------------------------------

DAY_NAMES = ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"]
DEFAULT_WEEKEND = [5, 6]        # Saturday, Sunday (Monday = 0)


def weekend_days(db):
    row = db.conn.execute("SELECT value FROM settings WHERE key = 'weekend_days'").fetchone()
    if row is None:
        return list(DEFAULT_WEEKEND)
    return [int(d) for d in row["value"].split(",") if d != ""]


def day_type(db, day, weekend=None):
    """("holiday", name), ("weekend", day name) or ("working", None)."""
    holiday = db.conn.execute("SELECT name FROM holidays WHERE date = ?",
                              (day.isoformat(),)).fetchone()
    if holiday:
        return "holiday", holiday["name"]
    if day.weekday() in (weekend if weekend is not None else weekend_days(db)):
        return "weekend", DAY_NAMES[day.weekday()]
    return "working", None


def get_calendar(db, _query):
    holidays = db.conn.execute("SELECT date, name FROM holidays ORDER BY date").fetchall()
    return 200, {"weekend_days": weekend_days(db), "day_names": DAY_NAMES,
                 "holidays": [dict(h) for h in holidays]}


def set_weekend(db, body):
    days = body.get("weekend_days")
    if not isinstance(days, list) or not all(isinstance(d, int) and 0 <= d <= 6 for d in days):
        raise ApiError(422, "weekend_days must be a list of numbers 0 (Monday) to 6 (Sunday)")
    if len(set(days)) == 7:
        raise ApiError(422, "At least one day must be a working day")
    db.conn.execute("INSERT OR REPLACE INTO settings (key, value) VALUES ('weekend_days', ?)",
                    (",".join(str(d) for d in sorted(set(days))),))
    db.conn.commit()
    return get_calendar(db, {})


def add_holiday(db, body):
    day = parse_day(body.get("date"), "date")
    name = require_str(body, "name")[:60]
    db.conn.execute("INSERT OR REPLACE INTO holidays (date, name) VALUES (?, ?)",
                    (day.isoformat(), name))
    db.conn.commit()
    return get_calendar(db, {})


def delete_holiday(db, body):
    day = parse_day(body.get("date"), "date")
    db.conn.execute("DELETE FROM holidays WHERE date = ?", (day.isoformat(),))
    db.conn.commit()
    return get_calendar(db, {})


def attendance_for_day(db, day):
    """One row per current employee (and anyone deleted who punched that day)."""
    start, end = day_window(day)
    is_today = day == datetime.now().astimezone().date()
    until = datetime.now(timezone.utc) if is_today else None

    by_employee = {}
    for p in punches_between(db, start, end):
        by_employee.setdefault(p["employee_id"], []).append(p)

    people = db.conn.execute(
        """SELECT id, employee_code, full_name, department, deleted_at FROM employees
           WHERE deleted_at IS NULL OR id IN (%s) ORDER BY full_name COLLATE NOCASE"""
        % ",".join("?" * len(by_employee)) if by_employee else
        """SELECT id, employee_code, full_name, department, deleted_at FROM employees
           WHERE deleted_at IS NULL ORDER BY full_name COLLATE NOCASE""",
        list(by_employee),
    ).fetchall()

    kind, label = day_type(db, day)

    rows = []
    for person in people:
        summary = summarize(by_employee.get(person["id"], []), until)
        # No punches on a weekend or holiday is a day off, not an absence
        state = ("in_office" if summary["in_office"] and is_today
                 else "present" if summary["punches"]
                 else "off" if kind != "working" else "absent")
        rows.append({
            "day_type": kind, "day_label": label,
            "employee_id": person["id"], "employee_code": person["employee_code"],
            "full_name": person["full_name"], "department": person["department"],
            "employee_deleted": person["deleted_at"] is not None,
            "state": state, "missed_punch_out": summary["in_office"] and not is_today,
            **summary,
        })
    return rows


def attendance_range(db, query):
    """Rows for every day from..to, each with its date (data sheet)."""
    first = parse_day(query.get("from"), "from")
    last = parse_day(query.get("to") or datetime.now().astimezone().date().isoformat(), "to")
    if first > last:
        raise ApiError(422, "from must be on or before to")
    if (last - first).days >= MAX_EXPORT_DAYS:
        raise ApiError(422, f"Choose at most {MAX_EXPORT_DAYS} days")

    rows, day = [], first
    while day <= last:
        rows += [dict(r, date=day.isoformat()) for r in attendance_for_day(db, day)]
        day += timedelta(days=1)
    return 200, {"from": first.isoformat(), "to": last.isoformat(), "rows": rows}


def attendance(db, query):
    if query.get("from"):
        return attendance_range(db, query)

    day = parse_day(query.get("date") or datetime.now().astimezone().date().isoformat(), "date")
    rows = attendance_for_day(db, day)
    kind, label = day_type(db, day)
    return 200, {
        "date": day.isoformat(),
        "day_type": kind,
        "day_label": label,
        "employees": rows,
        "summary": {
            "in_office": sum(r["state"] == "in_office" for r in rows),
            "present": sum(r["state"] in ("in_office", "present") for r in rows),
            "absent": sum(r["state"] == "absent" for r in rows),
            "off": sum(r["state"] == "off" for r in rows),
        },
    }


LOG_TYPES = ("all", "allowed", "denied", "enrolled")
MAX_LOGS = 20000             # enough for a full CSV export


def parse_time(query, key):
    """Optional ISO 8601 timestamp, returned in the stored UTC format."""
    value = query.get(key)
    if not value:
        return None
    try:
        moment = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        raise ApiError(422, f"{key} must be an ISO 8601 date and time")
    if moment.tzinfo is None:
        raise ApiError(422, f"{key} must include a timezone (e.g. Z or +05:30)")
    return moment.astimezone(timezone.utc).isoformat(timespec="seconds")


def list_logs(db, query):
    """Access attempts and completed enrollments, newest first.

    Optional filters: type, from (inclusive) and to (exclusive).
    """
    kind = query.get("type", "all")
    if kind not in LOG_TYPES:
        raise ApiError(422, f"type must be one of {', '.join(LOG_TYPES)}")
    try:
        limit = min(max(int(query.get("limit", 100)), 1), MAX_LOGS)
    except ValueError:
        raise ApiError(422, "limit must be a whole number")

    start = parse_time(query, "from")
    end = parse_time(query, "to")

    # Stored times share one UTC format, so text comparison orders them
    conditions, params = [], []
    if kind != "all":
        conditions.append("status = ?")
        params.append(kind)
    if start:
        conditions.append("created_at >= ?")
        params.append(start)
    if end:
        conditions.append("created_at < ?")
        params.append(end)

    where = f"WHERE {' AND '.join(conditions)}" if conditions else ""

    rows = db.conn.execute(
        f"""SELECT * FROM (
                SELECT a.created_at, a.id AS seq, a.status, a.reason, a.device_id, a.sensor_slot,
                       a.match_score, NULL AS finger_index, a.punch,
                       e.id AS employee_id, e.full_name, e.employee_code, e.department,
                       e.deleted_at IS NOT NULL AS employee_deleted
                FROM access_events a LEFT JOIN employees e ON e.id = a.employee_id
                UNION ALL
                SELECT b.created_at, b.id, 'enrolled', NULL, b.device_id, b.sensor_slot,
                       NULL, b.finger_index, NULL,
                       e.id, e.full_name, e.employee_code, e.department,
                       e.deleted_at IS NOT NULL
                FROM biometric_records b JOIN employees e ON e.id = b.employee_id
            )
            {where}
            ORDER BY created_at DESC, seq DESC     -- same second: newest row first
            LIMIT ?""",
        (*params, limit),
    ).fetchall()

    return 200, {"logs": [dict(r) for r in rows]}


# ------------------------------------------------------------
# CSV exports (times in this PC's local timezone)
# ------------------------------------------------------------

FINGERS = ["thumb", "index", "middle", "ring", "little"]
REASONS = {
    "repeat_scan": "Repeat scan within 30 s (no punch)",
    "unknown_sensor_slot": "Finger not registered",
    "employee_inactive": "Employee inactive",
    "biometric_record_inactive": "Fingerprint disabled",
}


def local_time(stored):
    return datetime.fromisoformat(stored).astimezone().strftime("%Y-%m-%d %H:%M:%S")


def day_bound(query, key, next_day):
    """A YYYY-MM-DD date (local midnight) or a full ISO timestamp."""
    value = query.get(key)
    if not value or len(value) != 10:
        return value
    try:
        day = datetime.strptime(value, "%Y-%m-%d") + timedelta(days=1 if next_day else 0)
    except ValueError:
        raise ApiError(422, f"{key} must be a date like 2026-10-05")
    return day.astimezone().isoformat()


def export_logs(db, query):
    first, last = query.get("from"), query.get("to")
    span = f"{first or 'start'}_to_{last or 'now'}" if first or last else "all-time"

    query = dict(query, limit=str(MAX_LOGS))
    query["from"] = day_bound(query, "from", next_day=False)
    query["to"] = day_bound(query, "to", next_day=True)   # the To day is included
    query = {k: v for k, v in query.items() if v}

    _, result = list_logs(db, query)

    rows = [
        [local_time(e["created_at"]), e["status"].capitalize(), e["employee_id"],
         e["employee_code"], e["full_name"] or "Unknown finger", e["department"],
         (f"{'Right' if e['finger_index'] <= 5 else 'Left'} {FINGERS[(e['finger_index'] - 1) % 5]}"
          if e["finger_index"] else ""),
         REASONS.get(e["reason"], e["reason"] or ""), e["device_id"],
         e["sensor_slot"], e["match_score"]]
        for e in reversed(result["logs"])              # oldest first
    ]

    header = ["Date & time", "Status", "Employee ID", "Employee code", "Name", "Department",
              "Finger", "Reason", "Terminal", "Sensor slot", "Match score"]
    return 200, CsvFile(f"employee-logs-{query.get('type', 'all')}-{span}.csv", header, rows)


def export_employees(db, _query):
    rows = db.conn.execute(
        """SELECT e.id, e.employee_code, e.full_name, e.department, e.email, e.phone,
                  e.active, e.created_at,
                  (SELECT COUNT(*) FROM biometric_records b
                   WHERE b.employee_id = e.id AND b.active = 1) AS fingers
           FROM employees e WHERE e.deleted_at IS NULL ORDER BY e.id"""
    ).fetchall()

    header = ["Employee ID", "Employee code", "Name", "Department", "Email", "Phone",
              "Active", "Fingers enrolled", "Added"]
    return 200, CsvFile("employees.csv", header, [
        [r["id"], r["employee_code"], r["full_name"], r["department"], r["email"],
         r["phone"], "Yes" if r["active"] else "No", r["fingers"], local_time(r["created_at"])]
        for r in rows
    ])


MAX_EXPORT_DAYS = 366


def export_attendance(db, query):
    today = datetime.now().astimezone().date()
    first = parse_day(query.get("from") or today.isoformat(), "from")
    last = parse_day(query.get("to") or today.isoformat(), "to")
    if first > last:
        raise ApiError(422, "from must be on or before to")
    if (last - first).days >= MAX_EXPORT_DAYS:
        raise ApiError(422, f"Choose at most {MAX_EXPORT_DAYS} days")

    states = {"in_office": "In office", "present": "Present", "absent": "Absent"}

    def status_text(r):
        if r["state"] == "off":
            return "Weekend" if r["day_type"] == "weekend" else f"Holiday - {r['day_label']}"
        text = states[r["state"]] + (" - no punch out" if r["missed_punch_out"] else "")
        if r["day_type"] == "holiday":
            text += f" (holiday - {r['day_label']})"
        elif r["day_type"] == "weekend":
            text += " (weekend)"
        return text

    rows = []
    day = first
    while day <= last:
        for r in attendance_for_day(db, day):
            rows.append([
                day.isoformat(), day.strftime("%A"), r["employee_id"], r["employee_code"],
                r["full_name"] + (" (deleted)" if r["employee_deleted"] else ""), r["department"],
                status_text(r),
                hhmm(r["first_in"]) if r["first_in"] else "",
                hhmm(r["last_out"]) if r["last_out"] else "",
                round(r["worked_seconds"] / 3600, 2) if r["punches"] and not r["missed_punch_out"] else "",
                duration_text(r["worked_seconds"]) if r["punches"] and not r["missed_punch_out"] else "",
                r["punches"],
                punch_times_text(r["sessions"]),
            ])
        day += timedelta(days=1)

    header = ["Date", "Day", "Employee ID", "Employee code", "Name", "Department", "Status",
              "First in", "Last out", "Hours worked", "Worked (h m)", "Punches", "All punch times"]
    return 200, CsvFile(f"attendance-{first}_to_{last}.csv", header, rows)


def punch_times_text(sessions):
    """IN 09:00 - OUT 12:00 | IN 12:45 - OUT 16:45 | IN 17:15 - (no out)"""
    parts = []
    for s in sessions:
        start = f"IN {hhmm(s['in'])}" if s["in"] else "(no in)"
        end = f"OUT {hhmm(s['out'])}" if s["out"] else "(no out)"
        parts.append(f"{start} - {end}")
    return " | ".join(parts)


def export_punches(db, query):
    """One row per punch: every IN and OUT timestamp in the range."""
    today = datetime.now().astimezone().date()
    first = parse_day(query.get("from") or today.isoformat(), "from")
    last = parse_day(query.get("to") or today.isoformat(), "to")
    if first > last:
        raise ApiError(422, "from must be on or before to")
    if (last - first).days >= MAX_EXPORT_DAYS:
        raise ApiError(422, f"Choose at most {MAX_EXPORT_DAYS} days")

    start, _ = day_window(first)
    _, end = day_window(last)
    events = db.conn.execute(
        """SELECT a.created_at, a.punch, a.device_id, e.id, e.employee_code, e.full_name,
                  e.department, e.deleted_at
           FROM access_events a JOIN employees e ON e.id = a.employee_id
           WHERE a.punch IS NOT NULL AND a.created_at >= ? AND a.created_at < ?
           ORDER BY e.full_name COLLATE NOCASE, e.id, a.created_at""",
        (start, end),
    ).fetchall()

    rows, open_in = [], {}
    for ev in events:
        moment = datetime.fromisoformat(ev["created_at"]).astimezone()
        key = (ev["id"], moment.date())
        duration = ""
        if ev["punch"] == "in":
            open_in[key] = moment
        elif key in open_in:
            duration = duration_text((moment - open_in.pop(key)).total_seconds())
        rows.append([
            moment.strftime("%Y-%m-%d"), moment.strftime("%A"), moment.strftime("%H:%M:%S"),
            ev["punch"].upper(), ev["id"], ev["employee_code"],
            ev["full_name"] + (" (deleted)" if ev["deleted_at"] else ""), ev["department"],
            duration, ev["device_id"],
        ])

    header = ["Date", "Day", "Time", "Punch", "Employee ID", "Employee code", "Name",
              "Department", "Time inside (on OUT)", "Terminal"]
    return 200, CsvFile(f"punch-times-{first}_to_{last}.csv", header, rows)


# ------------------------------------------------------------
# Saved reports (written nightly by deploy/report.py)
# ------------------------------------------------------------

REPORTS_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "reports")
REPORT_NAME = re.compile(r"^(daily|monthly)/(attendance|punch-times)-\d{4}-\d{2}(-\d{2})?\.csv$")


def list_reports(db, _query):
    """Saved report files, newest first, grouped by day or month."""
    groups = {}
    for folder in ("daily", "monthly"):
        path = os.path.join(REPORTS_DIR, folder)
        if not os.path.isdir(path):
            continue
        for name in os.listdir(path):
            rel = f"{folder}/{name}"
            match = REPORT_NAME.match(rel)
            if not match:
                continue
            kind = match.group(2)                       # attendance / punch-times
            period = name[len(kind) + 1:-len(".csv")]   # 2026-10-05 / 2026-10
            entry = groups.setdefault((folder, period), {"type": folder, "period": period})
            entry["summary" if kind == "attendance" else "punches"] = rel

    reports = sorted(groups.values(), key=lambda r: r["period"], reverse=True)
    return 200, {"daily": [r for r in reports if r["type"] == "daily"][:62],
                 "monthly": [r for r in reports if r["type"] == "monthly"][:24]}


def download_report(db, query):
    name = query.get("name", "")
    if not REPORT_NAME.match(name):
        raise ApiError(404, "No such report")
    path = os.path.join(REPORTS_DIR, name)
    if not os.path.isfile(path):
        raise ApiError(404, "No such report")
    with open(path, "rb") as f:
        return 200, Download(os.path.basename(name), f.read())


# ------------------------------------------------------------
# Live updates: pages long-poll /api/v1/changes and refresh the moment
# anything that shows up in them changes (a punch, an enrollment...).
# ------------------------------------------------------------

LIVE_WAIT_MAX = 8.0             # seconds; below serve.py's 10 s proxy timeout

changes = threading.Condition()
change_version = 0              # bumped on every change; restarts at 0 with the server

# Successful requests that change what the pages show
CHANGE_ROUTES = {
    ("POST", "/api/v1/access-events"),
    ("POST", "/api/v1/enrollment/complete"),
    ("POST", "/api/v1/employees"),
    ("POST", "/api/v1/employees/delete"),
    ("POST", "/api/v1/calendar/weekend"),
    ("POST", "/api/v1/holidays"),
    ("POST", "/api/v1/holidays/delete"),
}


def announce_change():
    global change_version
    with changes:
        change_version += 1
        changes.notify_all()


def wait_for_change(since, wait):
    """Returns the current version, waiting up to `wait` seconds for it to
    differ from `since` (answers at once if it already does)."""
    with changes:
        changes.wait_for(lambda: change_version != since, timeout=wait)
        return change_version


# ------------------------------------------------------------
# Admin login
# ------------------------------------------------------------

SESSION_COOKIE = "ea_session"
SESSION_SECONDS = 12 * 3600
PBKDF2_ROUNDS = 240_000
MIN_PASSWORD_LEN = 8

# Wrong passwords per client address: after MAX_FAILS, wait LOCK_SECONDS
MAX_FAILS = 5
LOCK_SECONDS = 60
login_failures = {}          # ip -> (count, locked_until)


def hash_password(password, salt=None):
    salt = salt or secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, PBKDF2_ROUNDS)
    return f"pbkdf2_sha256${PBKDF2_ROUNDS}${salt.hex()}${digest.hex()}"


def check_password(password, stored):
    try:
        _, rounds, salt, digest = stored.split("$")
        test = hashlib.pbkdf2_hmac("sha256", password.encode(), bytes.fromhex(salt), int(rounds))
    except ValueError:
        return False
    return hmac.compare_digest(test.hex(), digest)


def stored_password(db):
    row = db.conn.execute("SELECT value FROM settings WHERE key = 'admin_password'").fetchone()
    return row["value"] if row else None


def token_hash(token):
    return hashlib.sha256(token.encode()).hexdigest()


def session_valid(db, token):
    if not token:
        return False
    row = db.conn.execute(
        "SELECT expires_at FROM sessions WHERE token_hash = ?", (token_hash(token),)
    ).fetchone()
    return row is not None and row["expires_at"] > time.time()


def new_session(db):
    token = secrets.token_urlsafe(32)
    db.conn.execute("DELETE FROM sessions WHERE expires_at <= ?", (time.time(),))
    db.conn.execute("INSERT INTO sessions (token_hash, expires_at) VALUES (?, ?)",
                    (token_hash(token), time.time() + SESSION_SECONDS))
    db.conn.commit()
    return token


def set_password_interactive(db):
    if sys.stdin.isatty():
        first = getpass.getpass("New admin password: ")
        second = getpass.getpass("Repeat it: ")
    else:
        first = second = sys.stdin.readline().rstrip("\n")

    if first != second:
        sys.exit("Passwords do not match - nothing changed.")
    if len(first) < MIN_PASSWORD_LEN:
        sys.exit(f"Use at least {MIN_PASSWORD_LEN} characters - nothing changed.")

    db.conn.execute("INSERT OR REPLACE INTO settings (key, value) VALUES ('admin_password', ?)",
                    (hash_password(first),))
    db.conn.execute("DELETE FROM sessions")      # log everyone out
    db.conn.commit()
    print("Admin password saved. Everyone has been logged out.")


ROUTES = {
    ("GET", "/health"): lambda db, body: (200, {"status": "ok"}),
    ("GET", "/health/database"): lambda db, body: (
        200, {"status": "ok" if db.conn.execute("SELECT 1").fetchone() else "error"}),
    ("GET", "/api/v1/employees"): list_employees,
    ("POST", "/api/v1/employees"): create_employee,
    ("POST", "/api/v1/employees/delete"): delete_employee,
    ("POST", "/api/v1/devices/heartbeat"): heartbeat,
    ("POST", "/api/v1/enrollment/start"): enrollment_start,
    ("POST", "/api/v1/enrollment/complete"): enrollment_complete,
    ("POST", "/api/v1/access-events"): access_event,
    ("GET", "/api/v1/logs"): list_logs,
    ("GET", "/api/v1/export/logs.csv"): export_logs,
    ("GET", "/api/v1/export/employees.csv"): export_employees,
    ("GET", "/api/v1/attendance"): attendance,
    ("GET", "/api/v1/export/attendance.csv"): export_attendance,
    ("GET", "/api/v1/export/punches.csv"): export_punches,
    ("GET", "/api/v1/reports"): list_reports,
    ("GET", "/api/v1/reports/download"): download_report,
    ("GET", "/api/v1/calendar"): get_calendar,
    ("POST", "/api/v1/calendar/weekend"): set_weekend,
    ("POST", "/api/v1/holidays"): add_holiday,
    ("POST", "/api/v1/holidays/delete"): delete_holiday,
}

# Need an admin login; everything else is used by the terminal or is public
ADMIN_ROUTES = {
    ("GET", "/api/v1/employees"),
    ("POST", "/api/v1/employees"),
    ("POST", "/api/v1/employees/delete"),
    ("GET", "/api/v1/logs"),
    ("GET", "/api/v1/export/logs.csv"),
    ("GET", "/api/v1/export/employees.csv"),
    ("GET", "/api/v1/attendance"),
    ("GET", "/api/v1/export/attendance.csv"),
    ("GET", "/api/v1/export/punches.csv"),
    ("GET", "/api/v1/reports"),
    ("GET", "/api/v1/reports/download"),
    ("GET", "/api/v1/calendar"),
    ("POST", "/api/v1/calendar/weekend"),
    ("POST", "/api/v1/holidays"),
    ("POST", "/api/v1/holidays/delete"),
}


class ApiHandler(BaseHTTPRequestHandler):
    db = None

    def _send_json(self, status, payload, cookie=None):
        data = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        if cookie is not None:
            self.send_header("Set-Cookie", cookie)
        self.end_headers()
        self.wfile.write(data)

    def _session_token(self):
        cookie = SimpleCookie(self.headers.get("Cookie", ""))
        return cookie[SESSION_COOKIE].value if SESSION_COOKIE in cookie else None

    def _client(self):
        # serve.py adds X-Forwarded-For; only trust it from this machine
        forwarded = self.headers.get("X-Forwarded-For")
        if forwarded and self.client_address[0] in ("127.0.0.1", "::1"):
            return forwarded.split(",")[0].strip()
        return self.client_address[0]

    def _read_json(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length > MAX_BODY:
            return None
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            return None
        return body if isinstance(body, dict) else None

    def _auth(self, method, path):
        """Login endpoints. Returns True when the request was handled."""
        if (method, path) == ("GET", "/api/v1/auth/me"):
            with self.db.lock:
                configured = stored_password(self.db) is not None
                ok = session_valid(self.db, self._session_token())
            if not configured:
                self._send_json(503, {"detail": "password_not_set"})
            else:
                self._send_json(200 if ok else 401, {"logged_in": ok})
            return True

        if (method, path) == ("POST", "/api/v1/auth/logout"):
            with self.db.lock:
                token = self._session_token()
                if token:
                    self.db.conn.execute("DELETE FROM sessions WHERE token_hash = ?",
                                         (token_hash(token),))
                    self.db.conn.commit()
            self._send_json(200, {"logged_in": False},
                            cookie=f"{SESSION_COOKIE}=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict")
            return True

        if (method, path) == ("GET", "/api/v1/changes"):
            with self.db.lock:
                ok = session_valid(self.db, self._session_token())
            if not ok:
                self._send_json(401, {"detail": "Login required"})
                return True
            query = {k: v[0] for k, v in parse_qs(urlsplit(self.path).query).items()}
            try:
                since = int(query["since"]) if query.get("since") else None
                wait = min(max(float(query.get("wait", 0)), 0.0), LIVE_WAIT_MAX)
            except ValueError:
                self._send_json(422, {"detail": "since and wait must be numbers"})
                return True
            version = change_version if since is None else wait_for_change(since, wait)
            self._send_json(200, {"version": version})
            return True

        if (method, path) != ("POST", "/api/v1/auth/login"):
            return False

        client = self._client()
        count, locked_until = login_failures.get(client, (0, 0))
        if locked_until > time.time():
            wait = int(locked_until - time.time()) + 1
            self._send_json(429, {"detail": f"Too many wrong passwords. Try again in {wait} seconds."})
            return True

        body = self._read_json() or {}
        password = body.get("password") if isinstance(body.get("password"), str) else ""

        with self.db.lock:
            stored = stored_password(self.db)
            if stored is None:
                self._send_json(503, {"detail": "password_not_set"})
                return True

            if not check_password(password, stored):
                count += 1
                login_failures[client] = (0, time.time() + LOCK_SECONDS) \
                    if count >= MAX_FAILS else (count, 0)
                self._send_json(401, {"detail": "Wrong password"})
                return True

            login_failures.pop(client, None)
            token = new_session(self.db)

        self._send_json(200, {"logged_in": True},
                        cookie=f"{SESSION_COOKIE}={token}; Path=/; Max-Age={SESSION_SECONDS}; "
                               "HttpOnly; SameSite=Strict")
        return True

    def _dispatch(self, method):
        url = urlsplit(self.path)
        path = url.path.rstrip("/") or "/"

        if self._auth(method, path):
            return

        handler = ROUTES.get((method, path))
        if handler is None:
            self._send_json(404, {"detail": "Not found"})
            return

        if (method, path) in ADMIN_ROUTES:
            with self.db.lock:
                allowed = session_valid(self.db, self._session_token())
            if not allowed:
                self._send_json(401, {"detail": "Login required"})
                return

        # GET handlers receive the query string, POST handlers the JSON body
        body = {key: values[0] for key, values in parse_qs(url.query).items()}
        if method == "POST":
            length = int(self.headers.get("Content-Length") or 0)
            if length > MAX_BODY:
                self._send_json(413, {"detail": "Request too large"})
                return
            try:
                body = json.loads(self.rfile.read(length) or b"{}")
            except ValueError:
                self._send_json(400, {"detail": "Body must be JSON"})
                return
            if not isinstance(body, dict):
                self._send_json(400, {"detail": "Body must be a JSON object"})
                return

        try:
            with self.db.lock:
                status, payload = handler(self.db, body)
        except ApiError as err:
            self.db.conn.rollback()
            status, payload = err.status, {"detail": err.detail}

        if status < 400 and (method, path) in CHANGE_ROUTES:
            announce_change()

        if isinstance(payload, Download):
            self.send_response(status)
            self.send_header("Content-Type", "text/csv; charset=utf-8")
            self.send_header("Content-Disposition", f'attachment; filename="{payload.filename}"')
            self.send_header("Content-Length", str(len(payload.data)))
            self.end_headers()
            self.wfile.write(payload.data)
            return

        self._send_json(status, payload)

    def do_GET(self):
        self._dispatch("GET")

    def do_POST(self):
        self._dispatch("POST")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--db", default=DEFAULT_DB)
    parser.add_argument("--set-password", action="store_true",
                        help="set the admin password for the web page, then exit")
    args = parser.parse_args()

    ApiHandler.db = Database(args.db)

    if args.set_password:
        set_password_interactive(ApiHandler.db)
        return

    if stored_password(ApiHandler.db) is None:
        print("WARNING: no admin password yet - the web page stays locked until you run:\n"
              f"    python3 {os.path.basename(__file__)} --set-password")

    server = ThreadingHTTPServer((args.bind, args.port), ApiHandler)
    print(f"Backend on http://{args.bind}:{args.port}  (database {args.db})")
    server.serve_forever()


if __name__ == "__main__":
    main()
