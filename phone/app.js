"use strict";

/* ============================================================
 * Constants
 * ============================================================ */

const SERVICE_UUID = "6e1a0001-4b5c-4c1e-9f3a-2d7c5e8b9a01";
const COMMAND_UUID = "6e1a0002-4b5c-4c1e-9f3a-2d7c5e8b9a01";
const STATUS_UUID  = "6e1a0003-4b5c-4c1e-9f3a-2d7c5e8b9a01";

// Finger index sent to the server: 1-5 right thumb..little, 6-10 left
const FINGERS = ["Thumb", "Index", "Middle", "Ring", "Little"];

const STORE_COMPANY = "cbio.companyId";
const STORE_ACTIVITY = "cbio.activity";

const $ = (id) => document.getElementById(id);

/* ============================================================
 * Small helpers
 * ============================================================ */

// Browser storage can be unavailable (private mode, blocked site data)
function storageGet(key, fallback) {
  try {
    const value = localStorage.getItem(key);
    return value === null ? fallback : JSON.parse(value);
  } catch {
    return fallback;
  }
}

function storageSet(key, value) {
  try {
    localStorage.setItem(key, JSON.stringify(value));
  } catch {
    /* not persisted; the app still works */
  }
}

let toastTimer = null;

function toast(message, isError = false) {
  const el = $("toast");
  el.textContent = message;
  el.classList.toggle("err", isError);
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, 4000);
}

function log(message) {
  const time = new Date().toLocaleTimeString();
  const pre = $("log");
  pre.textContent += `[${time}] ${message}\n`;
  pre.scrollTop = pre.scrollHeight;
}

function setDot(dot, state) {
  dot.classList.remove("ok", "err", "busy");
  if (state) dot.classList.add(state);
}

/* ============================================================
 * Server API (same origin, forwarded by serve.py)
 * ============================================================ */

function errorText(body, status) {
  const detail = body && body.detail;
  if (typeof detail === "string") {
    // Database errors arrive as long driver messages; keep the useful line
    if (/duplicate key|already exists/i.test(detail)) return "An employee with this code already exists.";
    if (/foreign key/i.test(detail)) return "Company ID not found on the server.";
    return detail.split("\n")[0].slice(0, 200);
  }
  if (Array.isArray(detail) && detail.length) {
    const first = detail[0];
    const field = Array.isArray(first.loc) ? first.loc[first.loc.length - 1] : "field";
    return `${field}: ${first.msg}`;
  }
  return `Server error (${status})`;
}

async function api(path, body) {
  const options = body === undefined
    ? { method: "GET" }
    : { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) };

  let response;
  try {
    response = await fetch(path, options);
  } catch {
    throw new Error("Can't reach the app server. Is serve.py running on the PC?");
  }

  let data = null;
  try { data = await response.json(); } catch { /* empty or non-JSON body */ }

  if (response.status === 401 && !path.startsWith("/api/v1/auth/")) {
    showLogin();
    throw new Error("Please log in");
  }

  if (!response.ok) throw new Error(errorText(data, response.status));
  return data;
}

/* ============================================================
 * Navigation
 * ============================================================ */

const VIEWS = ["dashboard", "employees", "enroll", "attendance", "logs"];

function showView() {
  const name = VIEWS.includes(location.hash.slice(1)) ? location.hash.slice(1) : "dashboard";

  for (const view of VIEWS) {
    $(`view-${view}`).hidden = view !== name;
  }

  document.querySelectorAll(".tabbar a").forEach((a) => {
    if (a.dataset.tab === name) a.setAttribute("aria-current", "page");
    else a.removeAttribute("aria-current");
  });

  document.title = `${$(`view-${name}`).dataset.title} · Employees Access`;
  window.scrollTo(0, 0);

  if (name === "dashboard") refreshStatus();
  if (name === "logs") refreshLogs();
  if (name === "employees") loadPeople();
  if (name === "attendance") {
    loadAttendance();
    loadCalendar();
    loadReports();
  }
}

window.addEventListener("hashchange", showView);

/* ============================================================
 * Dashboard
 * ============================================================ */

function setTile(id, state, text, detail) {
  const tile = $(id);
  setDot(tile.querySelector(".dot"), state);
  tile.querySelector(".tile-state").textContent = text;
  if (detail !== undefined) tile.querySelector(".tile-detail").textContent = detail;
}

let refreshing = false;

async function refreshStatus() {
  if (refreshing) return;
  refreshing = true;

  setTile("tile-server", "busy", "Checking…");
  setTile("tile-database", "busy", "Checking…");

  try {
    await api("/health");
    setTile("tile-server", "ok", "Online", "On the PC");
  } catch (err) {
    setTile("tile-server", "err", "Unreachable", err.message);
  }

  try {
    await api("/health/database");
    setTile("tile-database", "ok", "Connected", "SQLite");
  } catch (err) {
    setTile("tile-database", "err", "Unavailable", err.message);
  }

  await refreshToday();

  $("last-checked").textContent = `Last checked ${new Date().toLocaleTimeString()}`;
  refreshing = false;
}

$("refresh").addEventListener("click", refreshStatus);

setInterval(() => {
  if (!document.hidden && !$("view-dashboard").hidden) refreshStatus();
}, 30000);

function addActivity(title, detail) {
  const items = storageGet(STORE_ACTIVITY, []);
  items.unshift({ title, detail, at: Date.now() });
  storageSet(STORE_ACTIVITY, items.slice(0, 15));
  renderActivity();
}

function renderActivity() {
  const items = storageGet(STORE_ACTIVITY, []);
  const list = $("activity");
  list.replaceChildren();

  if (!items.length) {
    const li = document.createElement("li");
    li.className = "empty";
    li.textContent = "No activity yet on this phone.";
    list.append(li);
    return;
  }

  for (const item of items) {
    const li = document.createElement("li");
    const what = document.createElement("div");
    const title = document.createElement("div");
    const detail = document.createElement("div");
    const when = document.createElement("div");

    what.className = "what";
    title.textContent = item.title;
    detail.className = "muted";
    detail.textContent = item.detail;
    when.className = "when";
    when.textContent = new Date(item.at).toLocaleString([], {
      month: "short", day: "numeric", hour: "2-digit", minute: "2-digit",
    });

    what.append(title, detail);
    li.append(what, when);
    list.append(li);
  }
}

const companyInput = $("company-id");
companyInput.value = storageGet(STORE_COMPANY, 1);
companyInput.addEventListener("change", () => {
  const value = parseInt(companyInput.value, 10);
  if (value > 0) storageSet(STORE_COMPANY, value);
});

/* ============================================================
 * Employees
 * ============================================================ */

let lastEmployeeId = null;

$("employee-form").addEventListener("submit", async (e) => {
  e.preventDefault();

  const form = e.target;
  const value = (name) => form.elements[name].value.trim();
  const errorBox = $("employee-error");
  errorBox.hidden = true;

  // Required fields
  let invalid = null;
  for (const name of ["full_name", "employee_code"]) {
    const input = form.elements[name];
    const missing = input.value.trim() === "";
    input.setAttribute("aria-invalid", missing ? "true" : "false");
    if (missing && !invalid) invalid = input;
  }
  if (invalid) {
    errorBox.textContent = "Please fill in the required fields.";
    errorBox.hidden = false;
    invalid.focus();
    return;
  }

  const companyId = parseInt(companyInput.value, 10);
  if (!(companyId > 0)) {
    errorBox.textContent = "Set a valid Company ID on the Dashboard first.";
    errorBox.hidden = false;
    return;
  }

  const payload = {
    company_id: companyId,
    employee_code: value("employee_code").toUpperCase(),
    full_name: value("full_name"),
    department: value("department") || null,
    email: value("email") || null,
    phone: value("phone") || null,
  };

  const submit = $("employee-submit");
  submit.disabled = true;
  submit.textContent = "Creating…";

  try {
    const result = await api("/api/v1/employees", payload);
    const employee = result.employee;

    lastEmployeeId = employee.id;
    $("employee-result-name").textContent = employee.full_name;
    $("employee-result-meta").textContent =
      `Employee ID ${employee.id} · ${employee.employee_code}` +
      (employee.department ? ` · ${employee.department}` : "");
    $("employee-result").hidden = false;

    addActivity(`Added ${employee.full_name}`, `Employee ID ${employee.id} · ${employee.employee_code}`);
    loadPeople();
    form.reset();
    toast("Employee created");
  } catch (err) {
    errorBox.textContent = err.message;
    errorBox.hidden = false;
  } finally {
    submit.disabled = false;
    submit.textContent = "Create employee";
  }
});

$("employee-enroll").addEventListener("click", () => {
  if (lastEmployeeId) $("enroll-employee").value = lastEmployeeId;
  $("employee-result").hidden = true;
  location.hash = "#enroll";
});

/* ============================================================
 * Finger picker
 * ============================================================ */

let selectedFinger = 2; // right index

function buildFingerPicker() {
  const groups = [
    { el: $("fingers-left"), base: 6, hand: "Left" },
    { el: $("fingers-right"), base: 1, hand: "Right" },
  ];

  for (const { el, base, hand } of groups) {
    FINGERS.forEach((name, i) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "finger";
      button.setAttribute("role", "radio");
      button.setAttribute("aria-label", `${hand} ${name.toLowerCase()}`);
      button.dataset.index = base + i;
      button.textContent = name;
      button.addEventListener("click", () => selectFinger(base + i));
      el.append(button);
    });
  }

  selectFinger(selectedFinger);
}

function selectFinger(index) {
  selectedFinger = index;
  document.querySelectorAll(".finger").forEach((b) => {
    b.setAttribute("aria-checked", String(Number(b.dataset.index) === index));
  });
}

function fingerName(index) {
  return `${index > 5 ? "Left" : "Right"} ${FINGERS[(index - 1) % 5].toLowerCase()}`;
}

/* ============================================================
 * Bluetooth terminal
 * ============================================================ */

let device = null;
let commandChar = null;
let enrolling = false;
let pending = null; // { employeeId, finger }

function updateConnectionUi() {
  const connected = Boolean(commandChar);
  const name = device ? device.name : "CBIO-ENTRY-001";

  setDot($("ble-chip-dot"), connected ? "ok" : null);
  $("ble-chip-text").textContent = connected ? "Terminal connected" : "Terminal offline";

  setTile("tile-terminal", connected ? "ok" : null,
          connected ? "Connected" : "Not connected", `${name} · Bluetooth`);
  $("tile-connect").textContent = connected ? "Disconnect" : "Connect";

  $("enroll-connect-banner").hidden = connected;
  $("enroll-submit").disabled = !connected || enrolling;
  $("enroll-submit").textContent = enrolling ? "Enrolling…" : "Start enrollment";
}

// Set when the user disconnects on purpose, so we don't reconnect
let userDisconnected = false;
let reconnectTimer = null;
let reconnectAttempt = 0;

const RECONNECT_DELAYS = [1000, 2000, 4000, 8000, 15000];

// Discovers the service and subscribes to status updates
async function attachGatt() {
  setTile("tile-terminal", "busy", reconnectAttempt ? "Reconnecting…" : "Connecting…");
  log(`Connecting to ${device.name}…`);

  const server = await device.gatt.connect();
  const service = await server.getPrimaryService(SERVICE_UUID);
  const command = await service.getCharacteristic(COMMAND_UUID);
  const status = await service.getCharacteristic(STATUS_UUID);

  // First protected read triggers pairing (code shown on the terminal)
  const initial = new TextDecoder().decode(await status.readValue());
  log(`Paired. Status: ${initial}`);

  // The same characteristic object comes back after a reconnect; listen once
  status.removeEventListener("characteristicvaluechanged", onStatusChanged);
  status.addEventListener("characteristicvaluechanged", onStatusChanged);
  await status.startNotifications();

  commandChar = command;
  reconnectAttempt = 0;
  updateConnectionUi();
}

function onStatusChanged(e) {
  handleTerminalStatus(new TextDecoder().decode(e.target.value));
}

function onDisconnected() {
  log("Disconnected");
  commandChar = null;
  if (enrolling) showProgress("err", 0, "Connection lost", "The terminal disconnected during enrollment.");
  enrolling = false;
  updateConnectionUi();

  if (!userDisconnected) scheduleReconnect();
}

function scheduleReconnect() {
  clearTimeout(reconnectTimer);

  if (!device || userDisconnected) return;

  // Chrome drops Bluetooth while the page is hidden; retry when it's visible again
  if (document.hidden) return;

  if (reconnectAttempt >= RECONNECT_DELAYS.length) {
    log("Gave up reconnecting. Tap Connect to try again.");
    reconnectAttempt = 0;
    return;
  }

  const delay = RECONNECT_DELAYS[reconnectAttempt++];
  log(`Reconnecting in ${delay / 1000}s (attempt ${reconnectAttempt})…`);
  setTile("tile-terminal", "busy", "Reconnecting…");

  reconnectTimer = setTimeout(async () => {
    if (!device || device.gatt.connected || userDisconnected) return;
    try {
      await attachGatt();
      toast("Terminal reconnected");
    } catch (err) {
      log(`Reconnect failed: ${err.message}`);
      scheduleReconnect();
    }
  }, delay);
}

document.addEventListener("visibilitychange", () => {
  if (!document.hidden && device && !device.gatt.connected && !userDisconnected) {
    reconnectAttempt = 0;
    scheduleReconnect();
  }
});

async function connectTerminal() {
  if (!navigator.bluetooth) {
    toast(window.isSecureContext
      ? "This browser has no Bluetooth support. Use Chrome on Android."
      : "Chrome blocks Bluetooth on this address. Enable the 'insecure origins' flag for it, or use localhost.",
      true);
    return;
  }

  try {
    const chosen = await navigator.bluetooth.requestDevice({
      filters: [{ services: [SERVICE_UUID] }, { namePrefix: "CBIO" }],
      optionalServices: [SERVICE_UUID],
    });

    if (chosen !== device) {
      if (device) device.removeEventListener("gattserverdisconnected", onDisconnected);
      device = chosen;
      device.addEventListener("gattserverdisconnected", onDisconnected);
    }

    userDisconnected = false;
    reconnectAttempt = 0;
    clearTimeout(reconnectTimer);

    await attachGatt();
    toast("Terminal connected");
  } catch (err) {
    log(`Connection failed: ${err.message}`);
    commandChar = null;
    updateConnectionUi();
    if (err.name !== "NotFoundError") toast(`Couldn't connect: ${err.message}`, true);
  }
}

// After a reload, reconnect to a terminal this site was already allowed to use
async function restoreTerminal() {
  if (!navigator.bluetooth || !navigator.bluetooth.getDevices) return;

  try {
    const known = (await navigator.bluetooth.getDevices())
      .filter((d) => (d.name || "").startsWith("CBIO"));

    if (!known.length) return;

    device = known[0];
    device.addEventListener("gattserverdisconnected", onDisconnected);
    log(`Found ${device.name} from last time`);

    reconnectAttempt = 0;
    scheduleReconnect();
  } catch (err) {
    log(`Couldn't restore the terminal: ${err.message}`);
  }
}

function toggleConnection() {
  if (device && device.gatt.connected) {
    userDisconnected = true;
    clearTimeout(reconnectTimer);
    device.gatt.disconnect();
  } else {
    connectTerminal();
  }
}

$("ble-chip").addEventListener("click", toggleConnection);
$("tile-connect").addEventListener("click", toggleConnection);
$("banner-connect").addEventListener("click", connectTerminal);

/* ============================================================
 * Enrollment
 * ============================================================ */

// Explains the server's most common refusals
function hintFor(message) {
  if (/not online/i.test(message)) return "Restart the terminal (EN button) while the tunnel is running so it reports online, then try again.";
  if (/not registered/i.test(message)) return "The terminal isn't registered on the server yet.";
  if (/employee not found/i.test(message)) return "Use the employee's numeric ID (e.g. 2), not the employee code (e.g. 0052). Or add the employee first.";
  if (/different companies/i.test(message)) return "The employee belongs to a different company than this terminal.";
  if (/already enrolled/i.test(message)) return "Choose a different finger for this employee.";
  if (/unreachable|offline/i.test(message)) return "Check that the SSH tunnel and the terminal's Wi-Fi are working.";
  if (/did not match|unclear/i.test(message)) return "Press the same finger flat on the sensor both times.";
  return "";
}

function showProgress(kind, step, title, detail) {
  const card = $("progress");
  card.hidden = false;
  card.classList.toggle("ok", kind === "ok");
  card.classList.toggle("err", kind === "err");

  card.querySelectorAll(".stepper li").forEach((li) => {
    const n = Number(li.dataset.step);
    li.classList.toggle("done", kind === "ok" || n < step);
    li.classList.toggle("active", kind !== "ok" && n === step);
  });

  $("progress-title").textContent = title;
  $("progress-detail").textContent = detail || "";

  const hint = kind === "err" ? hintFor(detail || "") : "";
  $("progress-hint").textContent = hint;
  $("progress-hint").hidden = !hint;
}

function finishEnrollment() {
  enrolling = false;
  updateConnectionUi();
}

function handleTerminalStatus(text) {
  log(`← ${text}`);

  let s;
  try { s = JSON.parse(text); } catch { return; }

  switch (s.state) {
    case "queued":
      showProgress(null, 0, "Request accepted", "Waiting for the terminal…");
      break;
    case "contacting_server":
      showProgress(null, 0, "Checking employee", "The terminal is contacting the server…");
      break;
    case "place_finger":
      showProgress(null, s.step,
        s.step === 2 ? "Place the same finger again" : "Place finger on the terminal",
        `Scan ${s.step} of 2`);
      break;
    case "remove_finger":
      showProgress(null, 2, "Lift the finger", "Then place it on the sensor again");
      break;
    case "saving":
      showProgress(null, 3, "Saving", "Storing on the terminal and the server…");
      break;
    case "done":
      showProgress("ok", 3, "Enrollment complete", `${s.name} · sensor slot ${s.slot}`);
      if (pending) addActivity(`Enrolled ${s.name}`, `${fingerName(pending.finger)} · slot ${s.slot}`);
      toast("Fingerprint enrolled");
      pending = null;
      finishEnrollment();
      break;
    case "error":
      showProgress("err", 0, "Enrollment failed", s.message);
      pending = null;
      finishEnrollment();
      break;
  }
}

$("enroll-form").addEventListener("submit", async (e) => {
  e.preventDefault();

  const input = $("enroll-employee");
  const employeeId = parseInt(input.value, 10);

  if (!(employeeId > 0)) {
    input.setAttribute("aria-invalid", "true");
    input.focus();
    toast("Enter a valid employee ID", true);
    return;
  }
  input.setAttribute("aria-invalid", "false");

  const command = JSON.stringify({ employee_id: employeeId, finger_index: selectedFinger });

  enrolling = true;
  pending = { employeeId, finger: selectedFinger };
  updateConnectionUi();
  showProgress(null, 0, "Sending request", `Employee ${employeeId} · ${fingerName(selectedFinger)}`);
  log(`→ ${command}`);

  try {
    await commandChar.writeValueWithResponse(new TextEncoder().encode(command));
  } catch (err) {
    log(`Write failed: ${err.message}`);
    showProgress("err", 0, "Couldn't send request", err.message);
    pending = null;
    finishEnrollment();
  }
});

/* ============================================================
 * Logs
 * ============================================================ */

const LOG_REASONS = {
  repeat_scan: "Repeat scan within 30 s (no punch)",
  unknown_sensor_slot: "Finger not registered",
  employee_inactive: "Employee inactive",
  biometric_record_inactive: "Fingerprint disabled",
};

const LOG_STATUS = { allowed: "Allowed", denied: "Denied", enrolled: "Enrolled" };

let logType = "all";
let logsLoading = false;

// "Punch in" / "Punch out" for attendance scans, else the plain status
function entryLabel(entry) {
  if (entry.punch) return entry.punch === "in" ? "Punch in" : "Punch out";
  if (entry.reason === "repeat_scan") return "Repeat";
  return LOG_STATUS[entry.status] || entry.status;
}

function initials(name) {
  return name.split(/\s+/).filter(Boolean).slice(0, 2).map((w) => w[0].toUpperCase()).join("");
}

// Entries already shown in each list, so live updates can highlight new ones
const shownLogKeys = new WeakMap();

function logKey(entry) {
  return `${entry.created_at}|${entry.seq}|${entry.status}`;
}

function renderLogs(entries, list = $("logs"), emptyText = null) {
  const before = shownLogKeys.get(list);
  shownLogKeys.set(list, new Set(entries.map(logKey)));
  list.replaceChildren();

  if (!entries.length) {
    const li = document.createElement("li");
    li.className = "empty";
    li.textContent = emptyText ||
      (logType === "all" ? "No log entries yet." : `No ${LOG_STATUS[logType].toLowerCase()} entries.`);
    list.append(li);
    return;
  }

  for (const entry of entries) {
    const li = document.createElement("li");
    const avatar = document.createElement("div");
    const what = document.createElement("div");
    const title = document.createElement("div");
    const detail = document.createElement("div");
    const side = document.createElement("div");
    const badge = document.createElement("span");
    const when = document.createElement("div");

    avatar.className = `avatar ${entry.status}`;
    if (entry.full_name) {
      avatar.textContent = initials(entry.full_name);
    } else {
      avatar.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="9"/><path d="M9.5 9.5a2.5 2.5 0 1 1 3.5 2.3c-.6.3-1 .8-1 1.5v.4M12 17h.01"/></svg>';
    }

    what.className = "what";
    title.textContent = entry.full_name
      ? entry.full_name + (entry.employee_deleted ? " (deleted)" : "")
      : "Unknown finger";

    const parts = [];
    if (entry.employee_code) parts.push(entry.employee_code);
    if (entry.department) parts.push(entry.department);
    if (entry.status === "enrolled" && entry.finger_index) parts.push(fingerName(entry.finger_index));
    if (entry.reason) parts.push(LOG_REASONS[entry.reason] || entry.reason);
    parts.push(`Slot ${entry.sensor_slot}`);
    detail.className = "muted";
    detail.textContent = parts.join(" · ");

    badge.className = `badge ${entry.punch ? `punch-${entry.punch}`
      : entry.reason === "repeat_scan" ? "repeat" : entry.status}`;
    badge.textContent = entryLabel(entry);

    const at = new Date(entry.created_at);
    const today = at.toDateString() === new Date().toDateString();
    when.className = "when";
    when.textContent = at.toLocaleString([], today
      ? { hour: "2-digit", minute: "2-digit" }
      : { month: "short", day: "numeric", hour: "2-digit", minute: "2-digit" });

    what.append(title, detail);
    side.className = "side";
    side.append(badge, when);
    li.append(avatar, what, side);
    if (before && !before.has(logKey(entry))) li.classList.add("fresh");
    list.append(li);
  }
}

/* Dashboard hero: clock, greeting and today's numbers */

function tickClock() {
  const now = new Date();
  const hour = now.getHours();

  $("greeting").textContent =
    hour < 12 ? "Good morning" : hour < 17 ? "Good afternoon" : "Good evening";
  $("today-date").textContent =
    now.toLocaleDateString([], { weekday: "long", day: "numeric", month: "long" });
  $("clock").textContent =
    now.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
}

async function refreshToday() {
  try {
    const [logs, today] = await Promise.all([
      api("/api/v1/logs?type=all&limit=5"),
      api("/api/v1/attendance"),
    ]);

    $("stat-in").textContent = today.summary.in_office;
    $("stat-present").textContent = today.summary.present;
    $("stat-absent").textContent = today.summary.absent;

    renderLogs(logs.logs, $("latest"), "No punches yet today.");
  } catch {
    for (const id of ["stat-in", "stat-present", "stat-absent"]) $(id).textContent = "–";
  }
}

tickClock();
setInterval(tickClock, 10000);

async function refreshLogs() {
  if (logsLoading) return;
  logsLoading = true;

  try {
    const data = await api(`/api/v1/logs?type=${logType}&limit=200`);
    renderLogs(data.logs);
    showLiveStatus();
  } catch (err) {
    $("logs-updated").textContent = `Couldn't load logs: ${err.message}`;
  } finally {
    logsLoading = false;
  }
}

document.querySelectorAll(".filter").forEach((button) => {
  button.addEventListener("click", () => {
    logType = button.dataset.type;
    document.querySelectorAll(".filter").forEach((b) =>
      b.setAttribute("aria-pressed", String(b === button)));
    refreshLogs();
  });
});

$("logs-refresh").addEventListener("click", refreshLogs);

/* CSV export of the logs, following the current filter */

function csvCell(value) {
  const text = value === null || value === undefined ? "" : String(value);
  return /[",\n\r]/.test(text) ? `"${text.replace(/"/g, '""')}"` : text;
}

function localStamp(iso) {
  const d = new Date(iso);
  const pad = (n) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ` +
         `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}

/* Date range for the export: "YYYY-MM-DD" values of the date inputs */

function dateValue(d) {
  return localStamp(d.toISOString()).slice(0, 10);
}

// Start of a local calendar day as a Date
function dayStart(value) {
  const [y, m, d] = value.split("-").map(Number);
  return new Date(y, m - 1, d);
}

// Preset ranges as date input values ("" = no limit)
function rangeFor(range) {
  const today = new Date();
  const y = today.getFullYear();
  const m = today.getMonth();

  switch (range) {
    case "today": return { from: dateValue(today), to: dateValue(today) };
    case "week":  return { from: dateValue(new Date(y, m, today.getDate() - 6)), to: dateValue(today) };
    case "month": return { from: dateValue(new Date(y, m, 1)), to: dateValue(today) };
    default:      return { from: "", to: "" };
  }
}

function setRange(range) {
  const { from, to } = rangeFor(range);
  $("export-from").value = from;
  $("export-to").value = to;
  markPreset();
}

// Highlights the preset that matches the chosen dates, if any
function markPreset() {
  const from = $("export-from").value;
  const to = $("export-to").value;

  document.querySelectorAll(".preset").forEach((button) => {
    const r = rangeFor(button.dataset.range);
    button.setAttribute("aria-pressed", String(r.from === from && r.to === to));
  });
}

function toggleExport() {
  const panel = $("export-panel");
  panel.hidden = !panel.hidden;
  $("logs-csv").setAttribute("aria-expanded", String(!panel.hidden));

  if (!panel.hidden) {
    $("export-type").textContent = logType === "all" ? "All entries" : `${LOG_STATUS[logType]} only`;
    $("export-error").hidden = true;
    if (!$("export-from").value && !$("export-to").value) setRange("week");
  }
}

async function downloadCsv(e) {
  e.preventDefault();

  const from = $("export-from").value;
  const to = $("export-to").value;
  const errorBox = $("export-error");
  errorBox.hidden = true;

  if (from && to && from > to) {
    errorBox.textContent = "The From date must be on or before the To date.";
    errorBox.hidden = false;
    return;
  }

  const params = new URLSearchParams({ type: logType, limit: "20000" });
  if (from) params.set("from", dayStart(from).toISOString());
  if (to) {
    // Up to the end of the To day
    const end = dayStart(to);
    end.setDate(end.getDate() + 1);
    params.set("to", end.toISOString());
  }

  const button = $("export-download");
  button.disabled = true;

  try {
    const data = await api(`/api/v1/logs?${params}`);

    if (!data.logs.length) {
      errorBox.textContent = "No log entries in this date range.";
      errorBox.hidden = false;
      return;
    }

    const header = ["Date & time", "Status", "Employee ID", "Employee code", "Name",
                    "Department", "Finger", "Reason", "Terminal", "Sensor slot", "Match score"];

    // Oldest first reads naturally in a spreadsheet
    const rows = data.logs.slice().reverse().map((entry) => [
      localStamp(entry.created_at),
      entryLabel(entry),
      entry.employee_id,
      entry.employee_code,
      entry.full_name || "Unknown finger",
      entry.department,
      entry.finger_index ? fingerName(entry.finger_index) : "",
      entry.reason ? (LOG_REASONS[entry.reason] || entry.reason) : "",
      entry.device_id,
      entry.sensor_slot,
      entry.match_score,
    ]);

    // BOM so Excel reads names with accents correctly
    const csv = "\uFEFF" + [header, ...rows].map((r) => r.map(csvCell).join(",")).join("\r\n");

    const range = from || to
      ? `${from || "start"}_to_${to || dateValue(new Date())}`
      : "all-time";
    const filename = `employee-logs-${logType}-${range}.csv`;

    if (window.AndroidApp) {
      // The Android app's WebView can't download blob: links
      if (!window.AndroidApp.saveFile(filename, "text/csv", csv)) throw new Error("Couldn't save the file");
      toast(`Saved ${rows.length} entries to Downloads`);
      return;
    }

    const url = URL.createObjectURL(new Blob([csv], { type: "text/csv;charset=utf-8" }));
    const link = document.createElement("a");
    link.href = url;
    link.download = filename;
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 10000);

    toast(`Downloaded ${rows.length} log entries`);
  } catch (err) {
    errorBox.textContent = `Couldn't export: ${err.message}`;
    errorBox.hidden = false;
  } finally {
    button.disabled = false;
  }
}

document.querySelectorAll(".preset").forEach((button) =>
  button.addEventListener("click", () => setRange(button.dataset.range)));
$("export-from").addEventListener("change", markPreset);
$("export-to").addEventListener("change", markPreset);
$("export-panel").addEventListener("submit", downloadCsv);
$("logs-csv").addEventListener("click", toggleExport);


// Live updates do the work; this only covers a dropped connection
setInterval(() => {
  if (!document.hidden && !$("view-logs").hidden && !liveConnected) refreshLogs();
}, 30000);

/* ============================================================
 * Attendance (punch in / punch out)
 * ============================================================ */

const ATT_STATE = {
  in_office: "In office",
  present: "Present",
  absent: "Absent",
};

// "Weekend" or the holiday's name for a day off
function offLabel(row) {
  return row.day_type === "holiday" ? row.day_label : "Weekend";
}

let attLoading = false;

function todayValue() {
  return dateValue(new Date());
}

function shiftDay(value, days) {
  const d = dayStart(value);
  d.setDate(d.getDate() + days);
  return dateValue(d);
}

function timeOf(iso) {
  return iso ? new Date(iso).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" }) : "";
}

function hoursText(seconds) {
  const minutes = Math.floor(seconds / 60);
  return `${Math.floor(minutes / 60)}h ${String(minutes % 60).padStart(2, "0")}m`;
}

function dayLabel(value) {
  const today = todayValue();
  if (value === today) return "Today";
  if (value === shiftDay(today, -1)) return "Yesterday";
  return dayStart(value).toLocaleDateString([], { weekday: "long", day: "numeric", month: "short" });
}

async function loadAttendance() {
  if (attLoading) return;
  attLoading = true;

  const date = $("att-date").value || todayValue();
  $("att-date").value = date;
  $("att-day-label").textContent = dayLabel(date);
  $("att-next").disabled = date >= todayValue();

  try {
    const data = await api(`/api/v1/attendance?date=${date}`);

    const banner = $("att-off");
    banner.hidden = data.day_type === "working";
    banner.className = `off-banner ${data.day_type}`;
    banner.textContent = data.day_type === "holiday"
      ? `Holiday: ${data.day_label}. Employees who didn't come in aren't marked absent.`
      : `${data.day_label} is a weekly off day. Employees who didn't come in aren't marked absent.`;

    $("att-in").textContent = data.summary.in_office;
    $("att-present").textContent = data.summary.present;
    $("att-absent").textContent = data.summary.absent;
    renderAttendance(data.employees);
    $("att-updated").textContent = date === todayValue()
      ? `Updated ${new Date().toLocaleTimeString()} · refreshes every 15 seconds`
      : `Attendance for ${dayStart(date).toLocaleDateString([], { dateStyle: "full" })}`;
  } catch (err) {
    $("att-updated").textContent = `Couldn't load attendance: ${err.message}`;
  } finally {
    attLoading = false;
  }
}

function renderAttendance(rows) {
  const list = $("att-list");
  list.replaceChildren();

  if (!rows.length) {
    const li = document.createElement("li");
    li.className = "empty";
    li.textContent = "No employees yet.";
    list.append(li);
    return;
  }

  // In office first, then present, then absent
  const order = { in_office: 0, present: 1, absent: 2, off: 3 };
  rows = rows.slice().sort((a, b) => order[a.state] - order[b.state] ||
    a.full_name.localeCompare(b.full_name));

  for (const row of rows) {
    const li = document.createElement("li");
    const avatar = document.createElement("div");
    const what = document.createElement("div");
    const title = document.createElement("div");
    const detail = document.createElement("div");
    const side = document.createElement("div");
    const badge = document.createElement("span");
    const hours = document.createElement("div");

    avatar.className = `avatar att-${row.state}`;
    avatar.textContent = initials(row.full_name);

    what.className = "what";
    title.textContent = row.full_name + (row.employee_deleted ? " (deleted)" : "");

    const parts = [row.employee_code];
    if (row.first_in) parts.push(`In ${timeOf(row.first_in)}`);
    if (row.last_out && row.state !== "in_office") parts.push(`Out ${timeOf(row.last_out)}`);
    if (row.missed_punch_out) parts.push("No punch out");
    if (!row.punches && row.department) parts.push(row.department);
    detail.className = "muted";
    detail.textContent = parts.join(" · ");

    badge.className = `badge att-${row.missed_punch_out ? "missed" : row.state}`;
    badge.textContent = row.missed_punch_out ? "No punch out"
      : row.state === "off" ? offLabel(row) : ATT_STATE[row.state];

    hours.className = "when";
    // A day left without a punch out has no reliable total
    hours.textContent = row.missed_punch_out ? "–" : row.punches ? hoursText(row.worked_seconds) : "";

    what.append(title, detail);
    if (row.sessions && row.sessions.length) what.append(timeline(row.sessions));

    side.className = "side";
    side.append(badge, hours);
    li.append(avatar, what, side);
    list.append(li);
  }
}

// Every IN -> OUT stretch: "IN 09:00 → OUT 12:00 · 3h 00m"
function timeline(sessions) {
  const ol = document.createElement("ol");
  ol.className = "timeline";

  for (const session of sessions) {
    const li = document.createElement("li");

    const inEl = document.createElement("span");
    inEl.className = "t-in";
    inEl.textContent = session.in ? `IN ${timeOf(session.in)}` : "no IN";

    const arrow = document.createElement("span");
    arrow.className = "t-arrow";
    arrow.textContent = "→";

    const outEl = document.createElement("span");
    outEl.className = session.out ? "t-out" : "t-open";
    outEl.textContent = session.out ? `OUT ${timeOf(session.out)}` : "still in";

    li.append(inEl, arrow, outEl);

    if (session.seconds !== null && session.seconds !== undefined) {
      const length = document.createElement("span");
      length.className = "t-len";
      length.textContent = hoursText(session.seconds);
      li.append(length);
    }
    ol.append(li);
  }
  return ol;
}

/* Automatic reports (saved nightly on the server) */

let reports = null;
let reportsShowAll = false;

async function loadReports() {
  try {
    reports = await api("/api/v1/reports");
    renderReports();
  } catch {
    /* shown again on the next visit */
  }
}

function reportRow(report) {
  const li = document.createElement("li");

  const label = document.createElement("span");
  label.className = "report-label";
  label.textContent = report.type === "monthly"
    ? new Date(`${report.period}-01T12:00`).toLocaleDateString([], { month: "long", year: "numeric" })
    : dayStart(report.period).toLocaleDateString([], { weekday: "short", day: "numeric", month: "short", year: "numeric" });

  li.append(label);

  for (const [key, text] of [["summary", "Summary"], ["punches", "Punch times"]]) {
    if (!report[key]) continue;
    const link = document.createElement("a");
    link.className = "report-link";
    link.href = `/api/v1/reports/download?name=${encodeURIComponent(report[key])}`;
    link.textContent = text;
    li.append(link);
  }
  return li;
}

function renderReports() {
  const monthly = $("reports-monthly");
  const daily = $("reports-daily");
  monthly.replaceChildren();
  daily.replaceChildren();

  if (!reports.daily.length && !reports.monthly.length) {
    const li = document.createElement("li");
    li.className = "muted";
    li.textContent = "No reports yet. The first one is saved tonight after midnight.";
    daily.append(li);
  }

  for (const r of reports.monthly.slice(0, 3)) {
    const li = reportRow(r);
    li.classList.add("monthly");
    monthly.append(li);
  }

  const days = reportsShowAll ? reports.daily : reports.daily.slice(0, 7);
  for (const r of days) daily.append(reportRow(r));

  $("reports-more").hidden = reportsShowAll || reports.daily.length <= 7;
}

$("reports-more").addEventListener("click", () => {
  reportsShowAll = true;
  renderReports();
});

/* Weekends and holidays */

const SHORT_DAYS = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"];
let calendar = null;

async function loadCalendar() {
  try {
    calendar = await api("/api/v1/calendar");
    renderCalendar();
  } catch {
    /* shown again on the next visit */
  }
}

function renderCalendar() {
  const box = $("cal-weekend");
  box.replaceChildren();

  SHORT_DAYS.forEach((label, day) => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "weekday";
    button.textContent = label;
    button.setAttribute("aria-pressed", String(calendar.weekend_days.includes(day)));
    button.setAttribute("aria-label", `${calendar.day_names[day]} off`);
    button.addEventListener("click", () => toggleWeekend(day));
    box.append(button);
  });

  const list = $("hol-list");
  list.replaceChildren();

  const today = todayValue();
  const upcoming = calendar.holidays.filter((h) => h.date >= today);
  const past = calendar.holidays.filter((h) => h.date < today).reverse().slice(0, 5);

  if (!calendar.holidays.length) {
    const li = document.createElement("li");
    li.className = "muted";
    li.textContent = "No holidays added yet.";
    list.append(li);
  }

  for (const h of [...upcoming, ...past]) {
    const li = document.createElement("li");
    li.classList.toggle("past", h.date < today);

    const when = document.createElement("span");
    when.className = "hol-date";
    when.textContent = dayStart(h.date).toLocaleDateString([], { weekday: "short", day: "numeric", month: "short", year: "numeric" });

    const name = document.createElement("span");
    name.className = "hol-name";
    name.textContent = h.name;

    const remove = document.createElement("button");
    remove.type = "button";
    remove.className = "btn-danger";
    remove.setAttribute("aria-label", `Remove ${h.name}`);
    remove.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M6 6l12 12M18 6L6 18"/></svg>';
    remove.addEventListener("click", async () => {
      if (!confirm(`Remove the holiday "${h.name}"?`)) return;
      try {
        calendar = await api("/api/v1/holidays/delete", { date: h.date });
        renderCalendar();
        loadAttendance();
      } catch (err) {
        toast(`Couldn't remove: ${err.message}`, true);
      }
    });

    li.append(when, name, remove);
    list.append(li);
  }
}

async function toggleWeekend(day) {
  const days = calendar.weekend_days.includes(day)
    ? calendar.weekend_days.filter((d) => d !== day)
    : [...calendar.weekend_days, day];

  try {
    calendar = await api("/api/v1/calendar/weekend", { weekend_days: days });
    renderCalendar();
    loadAttendance();
  } catch (err) {
    toast(err.message, true);
  }
}

$("hol-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const errorBox = $("hol-error");
  errorBox.hidden = true;

  const date = $("hol-date").value;
  const name = $("hol-name").value.trim();
  if (!date || !name) {
    errorBox.textContent = "Pick a date and type the holiday's name.";
    errorBox.hidden = false;
    return;
  }

  try {
    calendar = await api("/api/v1/holidays", { date, name });
    $("hol-form").reset();
    renderCalendar();
    loadAttendance();
    toast(`Added ${name}`);
  } catch (err) {
    errorBox.textContent = err.message;
    errorBox.hidden = false;
  }
});

function updateAttendanceCsv() {
  const from = $("att-from").value || todayValue();
  const to = $("att-to").value || todayValue();
  $("att-csv").href = `/api/v1/export/attendance.csv?from=${from}&to=${to}`;
  $("att-punches-csv").href = `/api/v1/export/punches.csv?from=${from}&to=${to}`;
}

$("att-date").addEventListener("change", loadAttendance);
$("att-prev").addEventListener("click", () => {
  $("att-date").value = shiftDay($("att-date").value || todayValue(), -1);
  loadAttendance();
});
$("att-next").addEventListener("click", () => {
  $("att-date").value = shiftDay($("att-date").value || todayValue(), 1);
  loadAttendance();
});
$("att-refresh").addEventListener("click", loadAttendance);

// Report defaults to this month so far
$("att-from").value = dateValue(new Date(new Date().getFullYear(), new Date().getMonth(), 1));
$("att-to").value = todayValue();
updateAttendanceCsv();
$("att-from").addEventListener("change", updateAttendanceCsv);
$("att-to").addEventListener("change", updateAttendanceCsv);
for (const id of ["att-csv", "att-punches-csv"]) {
  $(id).addEventListener("click", (e) => {
    if ($("att-from").value && $("att-to").value && $("att-from").value > $("att-to").value) {
      e.preventDefault();
      toast("The From date must be on or before the To date.", true);
    }
  });
}

setInterval(() => {
  if (!document.hidden && !$("view-attendance").hidden &&
      ($("att-date").value || todayValue()) === todayValue()) {
    loadAttendance();
  }
}, 15000);

/* ============================================================
 * Employee list (live) and delete
 * ============================================================ */

let people = [];
let peopleLoading = false;
let peopleFailed = false;

async function loadPeople() {
  if (peopleLoading) return;
  peopleLoading = true;

  try {
    people = (await api("/api/v1/employees")).employees;
    peopleFailed = false;
    renderPeople();
    $("people-updated").textContent = `Updated ${new Date().toLocaleTimeString()} · live`;
  } catch (err) {
    peopleFailed = true;
    $("people-updated").textContent = `Couldn't load employees: ${err.message}`;
  } finally {
    setDot($("people-live").querySelector(".dot"), peopleFailed ? "err" : "ok");
    peopleLoading = false;
  }
}

function renderPeople() {
  const list = $("people");
  const query = $("people-search").value.trim().toLowerCase();
  const shown = people.filter((p) => !query ||
    [p.full_name, p.employee_code, p.department, p.email, p.phone, String(p.id)]
      .some((v) => (v || "").toLowerCase().includes(query)));

  list.replaceChildren();

  if (!shown.length) {
    const li = document.createElement("li");
    li.className = "empty";
    li.textContent = people.length ? "No employees match your search." : "No employees yet. Add one above.";
    list.append(li);
    return;
  }

  for (const person of shown) {
    const li = document.createElement("li");
    const avatar = document.createElement("div");
    const what = document.createElement("div");
    const title = document.createElement("div");
    const detail = document.createElement("div");
    const remove = document.createElement("button");

    avatar.className = `avatar ${person.fingers ? "allowed" : ""}`;
    avatar.textContent = initials(person.full_name);

    what.className = "what";
    title.textContent = person.full_name;

    const parts = [`ID ${person.id}`, person.employee_code];
    if (person.department) parts.push(person.department);
    parts.push(person.fingers === 1 ? "1 finger" : `${person.fingers} fingers`);
    if (person.last_seen) {
      parts.push(`last in ${new Date(person.last_seen).toLocaleString([], {
        month: "short", day: "numeric", hour: "2-digit", minute: "2-digit" })}`);
    }
    detail.className = "muted";
    detail.textContent = parts.join(" · ");

    remove.type = "button";
    remove.className = "btn-danger";
    remove.setAttribute("aria-label", `Delete ${person.full_name}`);
    remove.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"/></svg>';
    remove.addEventListener("click", () => deletePerson(person, remove));

    what.append(title, detail);
    li.append(avatar, what, remove);
    list.append(li);
  }
}

async function deletePerson(person, button) {
  const fingers = person.fingers
    ? `\n\nTheir ${person.fingers === 1 ? "fingerprint" : `${person.fingers} fingerprints`} will stop working at the terminal immediately.`
    : "";

  if (!confirm(`Delete ${person.full_name} (${person.employee_code})?${fingers}\n\nTheir past check-ins stay in the logs.`)) {
    return;
  }

  button.disabled = true;

  try {
    await api("/api/v1/employees/delete", { employee_id: person.id });
    addActivity(`Deleted ${person.full_name}`, `Employee ID ${person.id} · ${person.employee_code}`);
    toast(`${person.full_name} deleted`);
    await loadPeople();
  } catch (err) {
    toast(`Couldn't delete: ${err.message}`, true);
    button.disabled = false;
  }
}

$("people-search").addEventListener("input", renderPeople);

setInterval(() => {
  if (!document.hidden && !$("view-employees").hidden && !liveConnected) loadPeople();
}, 30000);

/* ============================================================
 * Live updates
 *
 * /api/v1/changes answers as soon as anything changes on the server
 * (a punch, a denied scan, an enrollment, an employee or holiday edit),
 * or after 8 s with no change; then the page asks again. When the
 * answer shows a change, the visible page reloads its data.
 * ============================================================ */

let liveVersion = null;
let liveConnected = false;
let lastChange = null;
let newestSeen = null;          // created_at of the newest log entry announced

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function showLiveStatus() {
  const line = $("logs-updated");
  line.replaceChildren();

  const dot = document.createElement("span");
  dot.className = `dot ${liveConnected ? "ok" : "busy"}`;
  line.append(dot, liveConnected
    ? ` Live${lastChange ? ` · last update ${lastChange.toLocaleTimeString()}` : ""}`
    : " Reconnecting…");
  line.classList.add("live-line");
}

function setLive(connected) {
  if (connected === liveConnected) return;
  liveConnected = connected;
  showLiveStatus();
}

async function announceNewest() {
  try {
    const [entry] = (await api("/api/v1/logs?type=all&limit=1")).logs;
    if (!entry || entry.created_at === newestSeen) return;
    const first = newestSeen === null;
    newestSeen = entry.created_at;
    if (first) return;

    const who = entry.full_name || "Unknown finger";
    toast(entry.status === "denied"
      ? `${who}: denied (${LOG_REASONS[entry.reason] || entry.reason})`
      : `${who}: ${entryLabel(entry)}`, entry.status === "denied");
  } catch {
    /* the next change tries again */
  }
}

function onLiveChange() {
  lastChange = new Date();

  const view = VIEWS.find((v) => !$(`view-${v}`).hidden);
  if (view === "logs") refreshLogs();
  if (view === "dashboard") refreshToday();
  if (view === "employees") loadPeople();
  if (view === "attendance") loadAttendance();
  announceNewest();
}

async function liveLoop() {
  for (;;) {
    // Pause while hidden or logged out
    if (document.hidden || !$("login").hidden) {
      setLive(false);
      await sleep(1500);
      continue;
    }

    try {
      const query = liveVersion === null ? "" : `?since=${liveVersion}&wait=8`;
      const response = await fetch(`/api/v1/changes${query}`);
      if (!response.ok) throw new Error(String(response.status));

      const { version } = await response.json();
      if (liveVersion === null) {
        announceNewest();               // remember what's newest now
      } else if (version !== liveVersion) {
        onLiveChange();
      }
      liveVersion = version;
      setLive(true);
    } catch {
      setLive(false);
      await sleep(3000);
    }
  }
}

/* ============================================================
 * Admin login
 * ============================================================ */

function showLogin(notSetUp = false) {
  const overlay = $("login");
  $("login-setup").hidden = !notSetUp;
  $("login-password").disabled = notSetUp;
  $("login-submit").disabled = notSetUp;
  $("login-sub").hidden = notSetUp;

  if (overlay.hidden) {
    overlay.hidden = false;
    $("login-error").hidden = true;
    if (!notSetUp) setTimeout(() => $("login-password").focus(), 50);
  }
}

async function checkLogin() {
  try {
    const response = await fetch("/api/v1/auth/me");
    if (response.status === 503) showLogin(true);
    else if (response.status === 401) showLogin();
    return response.ok;
  } catch {
    return true;    // server down: the dashboard tiles explain that
  }
}

$("login-form").addEventListener("submit", async (e) => {
  e.preventDefault();

  const errorBox = $("login-error");
  const submit = $("login-submit");
  errorBox.hidden = true;
  submit.disabled = true;
  submit.textContent = "Checking…";

  try {
    await api("/api/v1/auth/login", { password: $("login-password").value });

    $("login-password").value = "";
    $("login").hidden = true;

    // Came here from the data sheet: go back to it
    if (new URLSearchParams(location.search).get("next") === "sheet.html") {
      location.href = "sheet.html";
      return;
    }

    toast("Logged in");
    showView();
  } catch (err) {
    errorBox.textContent = err.message === "password_not_set"
      ? "No admin password has been set on the server yet."
      : err.message;
    errorBox.hidden = false;
    $("login-password").select();
  } finally {
    submit.disabled = false;
    submit.textContent = "Log in";
  }
});

$("login-reveal").addEventListener("click", () => {
  const input = $("login-password");
  const show = input.type === "password";
  input.type = show ? "text" : "password";
  $("login-reveal").setAttribute("aria-pressed", String(show));
  $("login-reveal").setAttribute("aria-label", show ? "Hide password" : "Show password");
});

// Inside the Android app: pick a different PC
if (window.AndroidApp) {
  $("change-server").hidden = false;
  $("change-server").addEventListener("click", () => window.AndroidApp.changeServer());
}

$("logout").addEventListener("click", async () => {
  try { await api("/api/v1/auth/logout", {}); } catch { /* logged out either way */ }
  showLogin();
});

/* ============================================================
 * Start-up
 * ============================================================ */

buildFingerPicker();
renderActivity();
updateConnectionUi();
checkLogin().then((ok) => { if (ok) showView(); });
liveLoop();
restoreTerminal();

if ("serviceWorker" in navigator && window.isSecureContext) {
  navigator.serviceWorker.register("sw.js").catch((err) => log(`Offline cache unavailable: ${err.message}`));
}
