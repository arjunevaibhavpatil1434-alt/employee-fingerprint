"use strict";

/* ============================================================
 * Data sheet: spreadsheet view of the logs and employees
 * ============================================================ */

const $ = (id) => document.getElementById(id);

const FINGERS = ["thumb", "index", "middle", "ring", "little"];
const REASONS = {
  repeat_scan: "Repeat scan within 30 s (no punch)",
  unknown_sensor_slot: "Finger not registered",
  employee_inactive: "Employee inactive",
  biometric_record_inactive: "Fingerprint disabled",
};

// Column definitions: label, value(row), optional kind ("num" | "badge")
const TABLES = {
  logs: {
    columns: [
      { label: "Date & time", value: (r) => stamp(r.created_at) },
      { label: "Status", value: (r) => r.punch ? `punch ${r.punch}`
          : r.reason === "repeat_scan" ? "repeat" : r.status, kind: "badge" },
      { label: "Employee ID", value: (r) => r.employee_id, kind: "num" },
      { label: "Code", value: (r) => r.employee_code },
      { label: "Name", value: (r) => r.full_name ? r.full_name + (r.employee_deleted ? " (deleted)" : "") : "Unknown finger" },
      { label: "Department", value: (r) => r.department },
      { label: "Finger", value: (r) => r.finger_index ? finger(r.finger_index) : "" },
      { label: "Reason", value: (r) => r.reason ? (REASONS[r.reason] || r.reason) : "" },
      { label: "Terminal", value: (r) => r.device_id },
      { label: "Slot", value: (r) => r.sensor_slot, kind: "num" },
      { label: "Match", value: (r) => r.match_score, kind: "num" },
    ],
  },
  attendance: {
    columns: [
      { label: "Date", value: (r) => r.date },
      { label: "Day", value: (r) => new Date(`${r.date}T12:00`).toLocaleDateString([], { weekday: "short" }) },
      { label: "Code", value: (r) => r.employee_code },
      { label: "Name", value: (r) => r.full_name + (r.employee_deleted ? " (deleted)" : "") },
      { label: "Department", value: (r) => r.department },
      { label: "Status", value: (r) => r.missed_punch_out ? "No punch out"
          : r.state === "off" ? (r.day_type === "holiday" ? `Holiday: ${r.day_label}` : "Weekend")
          : ATT[r.state] },
      { label: "First in", value: (r) => clock(r.first_in) },
      { label: "Last out", value: (r) => clock(r.last_out) },
      { label: "Hours", value: (r) => r.punches && !r.missed_punch_out ? +(r.worked_seconds / 3600).toFixed(2) : "", kind: "num" },
      { label: "Punches", value: (r) => r.punches, kind: "num" },
      { label: "All punch times", value: (r) => (r.sessions || []).map((x) =>
          `IN ${clock(x.in) || "?"} → OUT ${x.out ? clock(x.out) : "—"}`).join("  |  ") },
    ],
  },
  employees: {
    columns: [
      { label: "Employee ID", value: (r) => r.id, kind: "num" },
      { label: "Code", value: (r) => r.employee_code },
      { label: "Name", value: (r) => r.full_name },
      { label: "Department", value: (r) => r.department },
      { label: "Email", value: (r) => r.email },
      { label: "Phone", value: (r) => r.phone },
      { label: "Fingers", value: (r) => r.fingers, kind: "num" },
      { label: "Last check-in", value: (r) => r.last_seen ? stamp(r.last_seen) : "" },
      { label: "Added", value: (r) => stamp(r.created_at) },
    ],
  },
};

const ATT = { in_office: "In office", present: "Present", absent: "Absent" };

function clock(iso) {
  return iso ? new Date(iso).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", hour12: false }) : "";
}

let table = "logs";
let rows = [];
let sort = { column: 0, dir: "descending" };

function stamp(iso) {
  const d = new Date(iso);
  const pad = (n) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ` +
         `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}

function finger(index) {
  return `${index <= 5 ? "Right" : "Left"} ${FINGERS[(index - 1) % 5]}`;
}

// Start of a local calendar day, as ISO (UTC) for the API
function dayIso(value, nextDay) {
  const [y, m, d] = value.split("-").map(Number);
  return new Date(y, m - 1, d + (nextDay ? 1 : 0)).toISOString();
}

function logQuery(forCsv) {
  const params = new URLSearchParams({ type: $("f-type").value });
  const from = $("f-from").value;
  const to = $("f-to").value;

  // The CSV endpoint takes plain dates in the PC's timezone; the JSON
  // endpoint takes exact instants so the browser's timezone is used
  if (from) params.set("from", forCsv ? from : dayIso(from, false));
  if (to) params.set("to", forCsv ? to : dayIso(to, true));
  if (!forCsv) params.set("limit", "20000");
  return params;
}

async function load() {
  if (!rows.length) $("summary").textContent = "Loading…";

  const today = stamp(new Date().toISOString()).slice(0, 10);
  const range = new URLSearchParams({
    from: $("f-from").value || today.slice(0, 8) + "01",   // this month
    to: $("f-to").value || today,
  });

  const url = {
    logs: `/api/v1/logs?${logQuery(false)}`,
    attendance: `/api/v1/attendance?${range}`,
    employees: "/api/v1/employees",
  }[table];

  $("download").href = {
    logs: `/api/v1/export/logs.csv?${logQuery(true)}`,
    attendance: `/api/v1/export/attendance.csv?${range}`,
    employees: "/api/v1/export/employees.csv",
  }[table];

  try {
    const response = await fetch(url);
    if (response.status === 401) {
      location.href = "./?next=sheet.html";      // log in, then come back
      return;
    }
    const data = await response.json();
    if (!response.ok) throw new Error(data.detail || `Server error ${response.status}`);

    rows = { logs: data.logs, attendance: data.rows, employees: data.employees }[table];
    render();
  } catch (err) {
    rows = [];
    render();
    $("summary").textContent = `Couldn't load data: ${err.message}`;
  }
}

function render() {
  const { columns } = TABLES[table];
  const query = $("f-search").value.trim().toLowerCase();

  // Search across every visible cell
  let shown = rows.filter((row) => !query ||
    columns.some((c) => String(c.value(row) ?? "").toLowerCase().includes(query)));

  const col = columns[sort.column];
  const sign = sort.dir === "ascending" ? 1 : -1;
  shown = shown.slice().sort((a, b) => {
    const x = col.value(a) ?? "";
    const y = col.value(b) ?? "";
    return (typeof x === "number" && typeof y === "number" ? x - y : String(x).localeCompare(String(y))) * sign;
  });

  // Header
  const head = document.createElement("tr");
  const corner = document.createElement("th");
  corner.className = "rownum";
  corner.textContent = "#";
  head.append(corner);

  columns.forEach((c, i) => {
    const th = document.createElement("th");
    th.textContent = c.label;
    th.scope = "col";
    if (i === sort.column) th.setAttribute("aria-sort", sort.dir);
    th.addEventListener("click", () => {
      sort = { column: i, dir: i === sort.column && sort.dir === "ascending" ? "descending" : "ascending" };
      render();
    });
    head.append(th);
  });

  $("sheet").tHead.replaceChildren(head);

  // Body
  const body = document.createDocumentFragment();

  shown.forEach((row, n) => {
    const tr = document.createElement("tr");
    const num = document.createElement("td");
    num.className = "rownum";
    num.textContent = n + 1;
    tr.append(num);

    for (const c of columns) {
      const td = document.createElement("td");
      const value = c.value(row);

      if (c.kind === "badge") {
        const badge = document.createElement("span");
        badge.className = `badge ${value.replace(" ", "-")}`;
        badge.textContent = value.charAt(0).toUpperCase() + value.slice(1);
        td.append(badge);
      } else {
        td.textContent = value ?? "";
        if (c.kind === "num") td.className = "num";
      }
      tr.append(td);
    }
    body.append(tr);
  });

  $("sheet").tBodies[0].replaceChildren(body);
  $("empty").hidden = shown.length > 0;

  const noun = { logs: "log entries", attendance: "attendance rows", employees: "employees" }[table];
  $("summary").textContent = shown.length === rows.length
    ? `${rows.length} ${noun}`
    : `${shown.length} of ${rows.length} ${noun} match "${$("f-search").value.trim()}"`;
}

/* ---------- Events ---------- */

document.querySelectorAll(".sheet-tabs .filter").forEach((tab) => {
  tab.addEventListener("click", () => {
    table = tab.dataset.table;
    sort = { column: 0, dir: table === "employees" ? "ascending" : "descending" };

    document.querySelectorAll(".sheet-tabs .filter").forEach((t) =>
      t.setAttribute("aria-pressed", String(t === tab)));
    document.querySelectorAll("[data-for]").forEach((el) => {
      el.hidden = !el.dataset.for.split(" ").includes(table);
    });

    load();

// Live: reload as soon as the server reports a change (long-poll)
(async function live() {
  let version = null;
  for (;;) {
    if (document.hidden) {
      await new Promise((r) => setTimeout(r, 1500));
      continue;
    }
    try {
      const response = await fetch(`/api/v1/changes${version === null ? "" : `?since=${version}&wait=8`}`);
      if (!response.ok) throw new Error(String(response.status));
      const data = await response.json();
      if (version !== null && data.version !== version) load();
      version = data.version;
    } catch {
      await new Promise((r) => setTimeout(r, 3000));
    }
  }
})();
  });
});

for (const id of ["f-type", "f-from", "f-to"]) $(id).addEventListener("change", load);
$("f-search").addEventListener("input", render);
$("toolbar").addEventListener("submit", (e) => e.preventDefault());

load();

// Live: reload as soon as the server reports a change (long-poll)
(async function live() {
  let version = null;
  for (;;) {
    if (document.hidden) {
      await new Promise((r) => setTimeout(r, 1500));
      continue;
    }
    try {
      const response = await fetch(`/api/v1/changes${version === null ? "" : `?since=${version}&wait=8`}`);
      if (!response.ok) throw new Error(String(response.status));
      const data = await response.json();
      if (version !== null && data.version !== version) load();
      version = data.version;
    } catch {
      await new Promise((r) => setTimeout(r, 3000));
    }
  }
})();
