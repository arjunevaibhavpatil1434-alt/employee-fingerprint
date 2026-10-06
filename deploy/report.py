#!/usr/bin/env python3
"""
Saves attendance reports as CSV files in ../reports, using the same
report code as the web app. Cron runs it every night (see install.sh).

    report.py                  yesterday's daily reports, and last month's
                               monthly reports when today is the 1st
    report.py --date 2026-10-05
    report.py --month 2026-09

Files:
    reports/daily/attendance-YYYY-MM-DD.csv     one row per employee
    reports/daily/punch-times-YYYY-MM-DD.csv    every IN and OUT
    reports/monthly/attendance-YYYY-MM.csv
    reports/monthly/punch-times-YYYY-MM.csv

Run with TZ set to the office timezone (install.sh does), since "a day"
means midnight to midnight there.
"""

import argparse
import os
import sys
from datetime import date, datetime, timedelta

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "backend"))

import server  # noqa: E402  (the backend's report functions)

REPORTS = os.path.join(ROOT, "reports")


def save(folder, name, csv_file):
    os.makedirs(os.path.join(REPORTS, folder), exist_ok=True)
    path = os.path.join(REPORTS, folder, name)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(csv_file.data)
    os.replace(tmp, path)          # never leave a half-written report
    print(f"Saved {os.path.relpath(path, ROOT)}")


def daily(db, day):
    span = {"from": day.isoformat(), "to": day.isoformat()}
    save("daily", f"attendance-{day}.csv", server.export_attendance(db, span)[1])
    save("daily", f"punch-times-{day}.csv", server.export_punches(db, span)[1])


def monthly(db, year, month):
    first = date(year, month, 1)
    last = (first + timedelta(days=32)).replace(day=1) - timedelta(days=1)
    span = {"from": first.isoformat(), "to": last.isoformat()}
    tag = first.strftime("%Y-%m")
    save("monthly", f"attendance-{tag}.csv", server.export_attendance(db, span)[1])
    save("monthly", f"punch-times-{tag}.csv", server.export_punches(db, span)[1])


def main():
    parser = argparse.ArgumentParser(description="Save attendance reports")
    parser.add_argument("--date", help="daily reports for this day (YYYY-MM-DD)")
    parser.add_argument("--month", help="monthly reports for this month (YYYY-MM)")
    args = parser.parse_args()

    db = server.Database(server.DEFAULT_DB)
    today = datetime.now().astimezone().date()

    if args.date:
        daily(db, date.fromisoformat(args.date))
    if args.month:
        year, month = (int(x) for x in args.month.split("-"))
        monthly(db, year, month)

    if not args.date and not args.month:
        daily(db, today - timedelta(days=1))
        if today.day == 1:
            previous = today - timedelta(days=1)
            monthly(db, previous.year, previous.month)


if __name__ == "__main__":
    main()
