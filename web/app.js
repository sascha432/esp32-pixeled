// The web UI of pixeled (docs/ui/web/web-app.md): one page, the views follow the hash (#/wifi). Plain
// JavaScript, no framework; Bootstrap 5.3 only as CSS. tools/build_web.py drops the `// #if CONFIG_*`
// blocks of options the firmware lacks (none yet) and minifies the file. Everything comes from the
// device API (components/webserver). Texts from outside (network names) are escaped.
"use strict";

const $ = (selector, root = document) => root.querySelector(selector);
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const esc = (value) =>
  String(value ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);

class HttpError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

let session = {};

// GET, or POST with a form (an object, or FormData for files); JSON answers. A 401 leads to the login.
async function api(path, form) {
  const options = { cache: "no-store" };
  if (form !== undefined) {
    options.method = "POST";
    options.body = form instanceof FormData ? form : new URLSearchParams(form);
  }
  const response = await fetch(path, options);
  const body = await response.json().catch(() => ({}));
  if (response.status === 401 && path !== "/api/login") {
    session.logged_in = false;
    go("login");
    throw new HttpError(401, "login required");
  }
  if (!response.ok) throw new HttpError(response.status, body.error || response.statusText);
  return body;
}

function go(name) {
  if (location.hash !== "#/" + name) location.hash = "#/" + name;
  else route();
}

function toast(text, kind = "secondary") {
  const t = $("#toast");
  t.className = `app-toast alert alert-${kind} py-2 shadow-sm`;
  t.textContent = text;
  t.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => (t.hidden = true), 4000);
}

// runs `action` for a button: disabled while it runs, errors as a toast
async function busy(button, action) {
  button.disabled = true;
  try {
    await action();
  } catch (e) {
    if (e.status !== 401) toast(e.message, "danger");
  } finally {
    button.disabled = false;
  }
}

// The live socket (docs/ui/web/web-app.md section 4.1): one WebSocket for the app instead of
// pages that ask every few seconds. live(path, onData) follows one GET: the device sends its answer
// at once and again whenever it changes, `onData` gets it. A page follows up to 3 paths (the LEDs
// page: the light, the sound, the presets). A route change stops them (liveStop), a hidden tab
// pauses them; a lost socket comes back by itself after 1, 2, 4 ... 10 s and asks again. The
// recovery app has no live socket (`"live": false` in /api/status): when one does not open and the
// device says so, live() asks with the GETs every 3 s instead (a tick skipped while the last waits
// or the tab is hidden). A device that restarts says nothing: the socket keeps trying.
const liveState = { socket: null, follows: new Map(), retry: 1000, timer: null, polling: false, poll: null, seen: 0, queued: false, epoch: 0 };

// The device sends at least `{}` every 10 s; a socket silent for 25 s died without a close (the
// device restarted, the laptop slept) and is opened again.
setInterval(() => {
  const socket = liveState.socket;
  if (!socket || socket.readyState !== WebSocket.OPEN || Date.now() - liveState.seen <= 25000) return;
  // its close handshake can wait long on a dead link: let it go and open a new one at once
  socket.onclose = socket.onmessage = null;
  socket.close();
  liveState.socket = null;
  liveConnect();
}, 5000);

// the paths followed, in one frame; several live() calls of a page go out together
function liveSend() {
  if (liveState.queued) return;
  liveState.queued = true;
  queueMicrotask(() => {
    liveState.queued = false;
    const socket = liveState.socket;
    if (socket && socket.readyState === WebSocket.OPEN) socket.send(document.hidden ? "" : [...liveState.follows.keys()].join(" "));
  });
}

function livePoll() {
  clearInterval(liveState.poll);
  let waiting = false;
  liveState.poll = setInterval(() => {
    if (!liveState.follows.size || waiting || document.hidden) return;
    waiting = true;
    Promise.all([...liveState.follows].map(([path, onData]) =>
      api(path).then((state) => liveState.follows.get(path) === onData && onData(state)).catch(() => {})))
      .finally(() => (waiting = false));
  }, 3000);
}

function liveConnect() {
  clearTimeout(liveState.timer);
  if (liveState.socket || !liveState.follows.size || liveState.polling) return;
  const socket = new WebSocket(`${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/api/live`);
  liveState.socket = socket;
  let opened = false;
  socket.onopen = () => {
    opened = true;
    liveState.seen = Date.now();
    liveState.retry = 1000;
    liveSend();
  };
  socket.onmessage = (event) => {
    liveState.seen = Date.now();
    let message;
    try { message = JSON.parse(event.data); } catch (e) { return; }
    // a frame of the page before is still on its way after a route change
    const onData = liveState.follows.get(message.path);
    if (!onData) return;
    Promise.resolve().then(() => onData(message.state)).catch(() => {});
  };
  socket.onclose = () => {
    liveState.socket = null;
    if (!liveState.follows.size) return;
    if (!opened) {
      fetch("/api/status", { cache: "no-store" }).then((r) => r.json()).then((s) => {
        if (s.live !== false || liveState.polling) return;
        liveState.polling = true;
        clearTimeout(liveState.timer);
        livePoll();
      }).catch(() => {});
    }
    liveState.timer = setTimeout(liveConnect, liveState.retry);
    liveState.retry = Math.min(liveState.retry * 2, 10000);
  };
}

function live(path, onData) {
  liveState.follows.set(path, onData);
  if (liveState.socket) liveSend();
  else liveConnect();
}

function liveStop() {
  liveState.follows.clear();
  liveSend();
}

document.addEventListener("visibilitychange", () => {
  if (liveState.socket) liveSend();
  else if (!document.hidden) liveConnect();
});

// Puts the page in and returns its handle: `page.live(path, onData)` follows a GET for this page
// only. A view that was left while it still loaded its data (its awaits) would otherwise register
// its paths after the next page's and crowd them out (the device takes the first 3).
function render(html) {
  $("#page").innerHTML = html;
  window.scrollTo(0, 0);
  const epoch = ++liveState.epoch;
  return { live: (path, onData) => { if (epoch === liveState.epoch) live(path, onData); } };
}

const duration = (s) =>
  s >= 86400 ? `${Math.floor(s / 86400)} d ${Math.floor((s % 86400) / 3600)} h`
    : s >= 3600 ? `${Math.floor(s / 3600)} h ${Math.floor((s % 3600) / 60)} min`
    : s >= 60 ? `${Math.floor(s / 60)} min`
    : `${s} s`;

function bars(rssi) {
  const n = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
  return `<span class="bars" title="${rssi} dBm">${[6, 9, 12, 14].map((h, i) =>
    `<i class="${i < n ? "on" : ""}" style="height:${h}px"></i>`).join("")}</span>`;
}

const lock = '<svg width="12" height="12" viewBox="0 0 16 16" fill="currentColor" aria-label="secured"><path d="M8 1a3 3 0 0 0-3 3v3H4a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1V8a1 1 0 0 0-1-1h-1V4a3 3 0 0 0-3-3m-2 6V4a2 2 0 1 1 4 0v3z"/></svg>';
const wifiIcon = '<svg width="18" height="18" viewBox="0 0 16 16" fill="currentColor"><path d="M8 12.5a1.25 1.25 0 1 1 0 2.5 1.25 1.25 0 0 1 0-2.5M3.3 9.6a6.6 6.6 0 0 1 9.4 0l-1 1a5.2 5.2 0 0 0-7.4 0zM.7 7a10.3 10.3 0 0 1 14.6 0l-1 1A8.9 8.9 0 0 0 1.7 8z"/></svg>';

// -------------------------------------------------------------------------------------- frame

// Icons of the menu, 24 x 24 strokes in the text's color.
const navIcons = {
  leds: '<circle cx="12" cy="12" r="4"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3M5 5l2 2M17 17l2 2M5 19l2-2M17 7l2-2"/>',
  sound: '<path d="M4 14v-4M8 18V6M12 21V3M16 17V7M20 14v-4"/>',
  "led-settings": '<path d="M4 6h10M18 6h2M4 12h4M12 12h8M4 18h12M20 18h0"/><circle cx="16" cy="6" r="2"/><circle cx="10" cy="12" r="2"/><circle cx="18" cy="18" r="2"/>',
  layout: '<rect x="3" y="3" width="8" height="8" rx="1"/><rect x="13" y="3" width="8" height="8" rx="1"/><rect x="3" y="13" width="8" height="8" rx="1"/><rect x="13" y="13" width="8" height="8" rx="1"/>',
  buttons: '<circle cx="12" cy="12" r="8"/><circle cx="12" cy="12" r="3"/>',
  wifi: '<path d="M2 9a16 16 0 0 1 20 0M5.5 12.5a11 11 0 0 1 13 0M9 16a5 5 0 0 1 6 0"/><circle cx="12" cy="19.5" r="1"/>',
  hotspot: '<circle cx="12" cy="12" r="2"/><path d="M8.5 8.5a5 5 0 0 0 0 7M15.5 8.5a5 5 0 0 1 0 7M5.6 5.6a9 9 0 0 0 0 12.8M18.4 5.6a9 9 0 0 1 0 12.8"/>',
  mqtt: '<path d="M4 4a16 16 0 0 1 16 16M4 10a10 10 0 0 1 10 10M4 16a4 4 0 0 1 4 4"/>',
  status: '<path d="M3 12h4l3-7 4 14 3-7h4"/>',
  time: '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
  logs: '<path d="M6 3h12v18H6zM9 8h6M9 12h6M9 16h4"/>',
  system: '<rect x="4" y="4" width="16" height="16" rx="2"/><path d="M9 9h6v6H9z"/>',
  more: '<circle cx="5" cy="12" r="1.5"/><circle cx="12" cy="12" r="1.5"/><circle cx="19" cy="12" r="1.5"/>',
  auto: '<circle cx="12" cy="12" r="9"/><path d="M12 3a9 9 0 0 1 0 18z" fill="currentColor"/>',
  light: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M2 12h2M20 12h2M5 5l1.5 1.5M17.5 17.5L19 19M5 19l1.5-1.5M17.5 6.5L19 5"/>',
  dark: '<path d="M20 14.5A8 8 0 0 1 9.5 4a8 8 0 1 0 10.5 10.5z"/>',
  logout: '<path d="M15 4h4v16h-4M10 8l-4 4 4 4M6 12h10"/>',
};
const navIcon = (name) => `<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"
  stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${navIcons[name]}</svg>`;

// The menu in groups (web-design.md section 3): [route, menu text, rail text, the feature it needs
// (`features` of /api/status)]. From 800 px a rail of icons with the rail text, the groups apart by
// a line; on a phone the tab bar below (tabPages and More) and More's sheet with every page. An
// entry whose feature the device lacks is hidden, a group without a shown entry too (the recovery
// app has none of the LED pages).
const menuGroups = [
  ["Light", [["leds", "Control", "Control", "leds"], ["sound", "Sound", "Sound", "audio"]]],
  ["Hardware", [["led-settings", "LED settings", "Settings", "leds"], ["layout", "Layout", "Layout", "leds"],
    ["buttons", "Buttons", "Buttons", "buttons"]]],
  ["Network", [["wifi", "Wi-Fi", "Wi-Fi"], ["hotspot", "Hotspot", "Hotspot"], ["mqtt", "MQTT", "MQTT"]]],
  ["Device", [["status", "Status", "Status"], ["time", "Time", "Time"], ["logs", "Logs", "Logs"], ["system", "System", "System"]]],
];
const tabPages = ["leds", "sound", "layout", "wifi"];

// a menu entry: the icon, the rail's short text, the sheet's full one; a dot for a state (Wi-Fi)
const navItem = ([page, title, short, feature]) => `<a class="app-nav-item" href="#/${page}" data-page="${page}"
  ${feature ? `data-feature="${feature}" hidden` : ""} title="${esc(title)}">${navIcon(page)}<span class="app-nav-dot" hidden></span>
  <span class="app-nav-short">${esc(short)}</span><span class="app-nav-long">${esc(title)}</span></a>`;

// shows the entries of the features the device has; a firmware without the list shows them all
function showFeatures(features) {
  const has = (f) => !f || !features || features.includes(f);
  for (const link of document.querySelectorAll("[data-feature]")) link.hidden = !has(link.dataset.feature);
  for (const group of document.querySelectorAll("#menu-pages [data-group]")) {
    group.hidden = ![...group.querySelectorAll("a")].some((link) => !link.hidden);
  }
}

// the sheet of every page on a phone (More); from 800 px the rail is always there. More is the
// active tab while the sheet is open and on a page that has no tab of its own.
function openSheet(open) {
  $("#menu").hidden = !open;
  const page = location.hash.replace(/^#\/?/, "") || "leds";
  $("#tab-more").classList.toggle("active", open || !tabPages.includes(page));
}

// pages that use the whole width
const widePages = ["layout"];

function frame(loggedIn, step) {
  $("#icon-wifi").hidden = !loggedIn;
  $("#tabbar").hidden = !loggedIn;
  $("#step").hidden = !step;
  $("#step").textContent = step || "";
  if (!loggedIn) openSheet(false);
  $("#menu").classList.toggle("d-none", !loggedIn);
  document.body.classList.toggle("app-logged-in", loggedIn);
}

async function updateHeader() {
  return showHeader(await api("/api/status"));
}

// the device name and the Wi-Fi icon, from /api/status
function showHeader(s) {
  $("#device-name").textContent = s.device.name;
  document.title = s.device.name;
  showFeatures(s.features);
  const icon = $("#icon-wifi");
  icon.innerHTML = wifiIcon;
  icon.className = "state-icon " + (s.wifi.state === "connected" ? "ok" : s.wifi.error ? "error" : "warn");
  icon.title = s.wifi.state === "connected" ? `Wi-Fi: ${s.wifi.ssid}, ${s.wifi.rssi} dBm` : `Wi-Fi: ${s.wifi.error || s.wifi.state}`;
  // the same state as a dot on the Wi-Fi entries of the rail and the tab bar
  for (const dot of document.querySelectorAll('[data-page="wifi"] .app-nav-dot')) {
    dot.hidden = false;
    dot.className = `app-nav-dot ${icon.className.split(" ")[1]}`;
  }
  return s;
}

// ------------------------------------------------------------------------------------- router

async function route() {
  const name = location.hash.replace(/^#\/?/, "") || "leds";
  liveStop();
  // a wide page (the LED layout) uses the whole width, every other page one column
  $("#page").classList.toggle("app-wide", widePages.includes(name));
  $("#page").style.maxWidth = "";
  try {
    session = await api("/api/session");
  } catch (e) {
    render(`<div class="alert alert-warning">The device does not answer. ${esc(e.message)}</div>`);
    return;
  }
  if (session.setup) return setupRoute(name);
  if (!session.logged_in) return loginPage();
  if (name === "setup-done") return setupDone();
  frame(true);
  for (const link of document.querySelectorAll("#menu-pages a, #tabbar a")) {
    link.classList.toggle("active", link.dataset.page === name);
  }
  openSheet(false);
  updateHeader().catch(() => {});
  await (views[name] || views.status)();
}

// -------------------------------------------------------------------------------------- setup

function setupRoute(name) {
  frame(false, name === "setup-login" ? "2 / 3" : "1 / 3");
  return name === "setup-login" ? setupLogin() : setupWifi();
}

function networkPicker(scan, saved) {
  const items = scan.map((n, i) => `
    <button type="button" class="list-group-item list-group-item-action d-flex align-items-center gap-2" data-index="${i}">
      <span class="flex-grow-1 text-truncate">${esc(n.ssid)}</span>
      ${n.saved ? '<span class="badge text-bg-secondary">saved</span>' : ""}
      ${n.secure ? lock : ""} ${bars(n.rssi)}
    </button>`).join("");
  return `
    <div class="list-group mb-3" id="scan-list">${items || '<div class="list-group-item text-body-secondary">Searching ...</div>'}
      <button type="button" class="list-group-item list-group-item-action" data-other="1">Other network ...</button>
    </div>
    <div id="ssid-row" class="mb-3" hidden>
      <label class="form-label" for="ssid">Network name</label>
      <input id="ssid" class="form-control" autocomplete="off" maxlength="32">
    </div>
    <label class="form-label" for="wifi-password">Password</label>
    <div class="input-group mb-3">
      <input id="wifi-password" type="password" class="form-control" autocomplete="off" maxlength="64">
      <button id="show-password" class="btn btn-outline-secondary" type="button">Show</button>
    </div>`;
}

// wires a picker made by networkPicker(); returns a function that gives the chosen name
function wirePicker(scan) {
  let chosen = "";
  $("#scan-list").addEventListener("click", (event) => {
    const item = event.target.closest("button");
    if (!item) return;
    for (const b of $("#scan-list").querySelectorAll("button")) b.classList.remove("active");
    item.classList.add("active");
    const other = item.dataset.other === "1";
    $("#ssid-row").hidden = !other;
    chosen = other ? "" : scan[Number(item.dataset.index)].ssid;
    $("#wifi-password").placeholder = !other && scan[Number(item.dataset.index)].saved
      ? "empty keeps the saved password" : "";
    (other ? $("#ssid") : $("#wifi-password")).focus();
  });
  $("#show-password").onclick = () => {
    const field = $("#wifi-password");
    field.type = field.type === "password" ? "text" : "password";
    $("#show-password").textContent = field.type === "password" ? "Show" : "Hide";
  };
  return () => ($("#ssid-row").hidden ? chosen : $("#ssid").value.trim());
}

async function scanNetworks() {
  await api("/api/wifi/scan", {}).catch(() => {});
  await sleep(4000);
  return api("/api/wifi");
}

async function setupWifi() {
  render(`<h1>Choose your Wi-Fi</h1><div class="card"><div class="card-body" id="setup-wifi">
    <p class="text-body-secondary">Searching for networks ...</p></div></div>`);
  let w = await api("/api/wifi");
  if (w.scan.length === 0) w = await scanNetworks();
  const draw = () => {
    $("#setup-wifi").innerHTML = `
      ${w.saved.length ? `<p>Saved: ${w.saved.map((n) => esc(n.ssid)).join(", ")}. You can add another one or go on.</p>` : ""}
      <div class="d-flex justify-content-end mb-2"><button id="rescan" class="btn btn-sm btn-outline-secondary" type="button">Rescan</button></div>
      ${networkPicker(w.scan)}
      <div class="d-flex gap-2 justify-content-end">
        ${w.saved.length ? '<button id="next" class="btn btn-outline-secondary" type="button">Next</button>' : ""}
        <button id="connect" class="btn btn-primary" type="button">Connect</button>
      </div>`;
    const chosen = wirePicker(w.scan);
    $("#rescan").onclick = (e) => busy(e.target, async () => { w = await scanNetworks(); draw(); });
    if ($("#next")) $("#next").onclick = () => go("setup-login");
    $("#connect").onclick = (e) => busy(e.target, async () => {
      const ssid = chosen();
      if (!ssid) throw new Error("choose a network");
      await api("/api/wifi/add", { ssid, password: $("#wifi-password").value });
      toast(`Saved ${ssid}, connecting ...`, "success");
      go("setup-login");
    });
  };
  draw();
}

function setupLogin() {
  render(`<h1>Web UI login</h1><div class="card"><div class="card-body">
    <p class="text-body-secondary">The login protects these pages. It is kept on the device.</p>
    <label class="form-label" for="new-user">Username</label>
    <input id="new-user" class="form-control mb-3" value="admin" autocomplete="username" maxlength="32">
    <label class="form-label" for="new-password">Password (8 to 64 characters)</label>
    <input id="new-password" type="password" class="form-control mb-3" autocomplete="new-password" maxlength="64">
    <label class="form-label" for="new-password2">Password again</label>
    <input id="new-password2" type="password" class="form-control mb-3" autocomplete="new-password" maxlength="64">
    <div class="d-flex gap-2 justify-content-end">
      <button class="btn btn-outline-secondary" type="button" onclick="go('setup')">Back</button>
      <button id="create" class="btn btn-primary" type="button">Create login</button>
    </div></div></div>`);
  $("#create").onclick = (e) => busy(e.target, async () => {
    if ($("#new-password").value !== $("#new-password2").value) throw new Error("the passwords differ");
    await api("/api/setup/user", { username: $("#new-user").value, password: $("#new-password").value, remember: "1" });
    go("setup-done");
  });
}

async function setupDone() {
  frame(false, "3 / 3");
  const page = render(`<h1>Done</h1><div class="card"><div class="card-body" id="done">Checking the Wi-Fi ...</div></div>`);
  const draw = (s) => {
    const w = s.wifi;
    const box = $("#done");
    if (!box) return;
    const keep = $("#keep-hotspot")?.checked ?? s.hotspot.on;
    box.innerHTML = (w.state === "connected"
      ? `<p>Connected to <b>${esc(w.ssid)}</b>.</p>
         <p>Join ${esc(w.ssid)} with this phone and open<br>
         <a href="http://${esc(s.device.name)}.local/">http://${esc(s.device.name)}.local</a><br>
         <a href="http://${esc(w.ip)}/">http://${esc(w.ip)}</a></p>`
      : `<p>${w.error ? `<span class="text-danger">${esc(w.error)}</span>` : `Wi-Fi: ${esc(w.state)} ${esc(w.ssid)} ...`}</p>
         <p><a href="#/setup">Change the Wi-Fi</a></p>`) +
      `<div class="form-check form-switch mb-3">
         <input class="form-check-input" type="checkbox" role="switch" id="keep-hotspot" ${keep ? "checked" : ""}>
         <label class="form-check-label" for="keep-hotspot">Keep the hotspot on</label>
       </div>
       <div class="d-flex justify-content-end"><button id="finish" class="btn btn-primary" type="button">Done</button></div>`;
    $("#finish").onclick = (e) => busy(e.target, async () => {
      if ($("#keep-hotspot").checked) await api("/api/hotspot", { on: "1" });
      go("status");
    });
  };
  draw(await api("/api/status"));
  page.live("/api/status", draw);
}

// -------------------------------------------------------------------------------------- login

function loginPage() {
  frame(false);
  render(`<h1>Log in</h1><div class="card"><div class="card-body"><form id="login-form">
    <label class="form-label" for="user">Username</label>
    <input id="user" class="form-control mb-3" autocomplete="username" required>
    <label class="form-label" for="password">Password</label>
    <input id="password" type="password" class="form-control mb-3" autocomplete="current-password" required>
    <div class="form-check mb-3"><input class="form-check-input" type="checkbox" id="remember">
      <label class="form-check-label" for="remember">Remember me</label></div>
    <div class="d-flex justify-content-end"><button class="btn btn-primary" type="submit">Log in</button></div>
    </form></div></div>
    <p class="small text-body-secondary">Problems with these pages? <a href="/recovery">Recovery page</a></p>`);
  $("#login-form").onsubmit = (event) => {
    event.preventDefault();
    busy($("#login-form button"), async () => {
      await api("/api/login", { username: $("#user").value, password: $("#password").value,
                                remember: $("#remember").checked ? "1" : "0" });
      go(location.hash.replace(/^#\/?/, "") === "login" ? "status" : location.hash.replace(/^#\/?/, ""));
    });
  };
  $("#user").focus();
}

// -------------------------------------------------------------------------------------- views

const facts = (rows) => `<dl class="facts">${rows.map(([k, v]) => `<dt>${esc(k)}</dt><dd>${v}</dd>`).join("")}</dl>`;
const card = (title, body) => `<div class="card"><div class="card-body"><h2 class="h6 card-title">${esc(title)}</h2>${body}</div></div>`;

function notInRecovery(title) {
  render(`<h1>${esc(title)}</h1>${card(title, '<p class="text-body-secondary mb-0">The recovery app drives no LEDs. Start the firmware (System).</p>')}`);
}

// A script loaded once, on the first visit of the page that needs it.
const loadedScripts = {};
function loadScript(src) {
  return (loadedScripts[src] ||= loadScriptNow(src));
}
function loadScriptNow(src) {
  return new Promise((resolve, reject) => {
    const script = document.createElement("script");
    script.src = src;
    script.onload = resolve;
    script.onerror = () => reject(new Error(`${src} could not be loaded`));
    document.head.appendChild(script);
  });
}

// The LED pages are in leds.js, loaded with the first visit (the setup on a phone stays small).
async function ledPage(view, title) {
  await loadScript("leds.js");
  if ((await api("/api/update")).app === "recovery") return notInRecovery(title);
  await window.pxLeds[view]();
}

const views = {
  leds: () => ledPage("leds", "Control"),
  sound: () => ledPage("sound", "Sound"),
  "led-settings": () => ledPage("ledSettings", "LED settings"),
  layout: () => ledPage("layout", "Layout"),
  buttons: () => ledPage("buttons", "Buttons"),
  async status() {
    const page = render(`<h1>Status</h1><div id="status-cards">Loading ...</div>`);
    const draw = (s) => {
      showHeader(s);
      const w = s.wifi, h = s.hotspot, d = s.device;
      const box = $("#status-cards");
      if (!box) return;
      box.innerHTML =
        card("Wi-Fi", facts([
          ["State", esc(w.state) + (w.error ? ` <span class="text-danger">${esc(w.error)}</span>` : "")],
          ["Network", esc(w.ssid || "-")], ["Address", esc(w.ip || "-")],
          ["Signal", w.state === "connected" ? `${bars(w.rssi)} ${w.rssi} dBm` : "-"],
        ])) +
        card("Hotspot", facts([
          ["State", h.on ? "on" : "off"], ["Name", esc(h.ssid)], ["Devices", h.clients],
          ["Turns off in", h.off_in_s ? duration(h.off_in_s) : "-"],
        ])) +
        card("Device", facts([
          ["Firmware", esc(d.firmware)], ["Built", esc(d.built)], ["Uptime", duration(d.uptime_s)],
          ["Free memory", `${d.heap_internal_kb} KB internal, ${d.heap_psram_kb} KB PSRAM`],
          ["File system", esc(s.fs.state) + (s.fs.state === "mounted" ? `, ${s.fs.used_kb} of ${s.fs.total_kb} KB used` : "")],
        ]));
    };
    draw(await api("/api/status"));
    page.live("/api/status", draw);
  },

  async wifi() {
    const page = render(`<h1>Wi-Fi</h1><div id="wifi-saved">Loading ...</div>
      <div class="card"><div class="card-body"><h2 class="h6 card-title d-flex">Add a network
        <button id="rescan" class="btn btn-sm btn-outline-secondary ms-auto" type="button">Scan</button></h2>
        <div id="wifi-add"></div></div></div>`);
    let w = await api("/api/wifi");
    // a saved network's password is being typed in its row
    let editing = false;
    const drawSaved = () => {
      const rows = w.saved.map((n, i) => `
        <li class="list-group-item d-flex align-items-center gap-2">
          <span class="text-body-secondary">${i + 1}</span>
          <span class="flex-grow-1 text-truncate">${esc(n.ssid)}</span>
          ${n.password_needed ? `<span class="badge text-bg-warning" title="From a backup without its password; not tried until it has one">Password needed</span>
            <button class="btn btn-sm btn-outline-primary" data-password="${i}">Enter</button>` : ""}
          <button class="btn btn-sm btn-outline-secondary" data-move="-1" data-index="${i}" ${i === 0 ? "disabled" : ""} aria-label="up">&uarr;</button>
          <button class="btn btn-sm btn-outline-secondary" data-move="1" data-index="${i}" ${i === w.saved.length - 1 ? "disabled" : ""} aria-label="down">&darr;</button>
          <button class="btn btn-sm btn-outline-danger" data-forget="${i}">Forget</button>
        </li>`).join("");
      $("#wifi-saved").innerHTML = card(`Saved networks (${w.saved.length} of 5)`, `
        <ul class="list-group mb-3">${rows || '<li class="list-group-item text-body-secondary">none</li>'}</ul>
        <div class="d-flex gap-2 align-items-center">
          <label class="text-body-secondary small" for="policy">Use</label>
          <select id="policy" class="form-select form-select-sm w-auto">
            <option value="order" ${w.policy === "order" ? "selected" : ""}>the first one in range</option>
            <option value="strongest" ${w.policy === "strongest" ? "selected" : ""}>the strongest one</option>
          </select>
          <button id="reconnect" class="btn btn-sm btn-outline-secondary ms-auto" type="button">Reconnect</button>
        </div>`);
      $("#wifi-saved").onclick = (event) => {
        const b = event.target.closest("button[data-index],button[data-forget],button[data-password]");
        if (!b) return;
        if (b.dataset.password !== undefined) {
          // the password in place of the badge, saved like a new network (it connects at once)
          const ssid = w.saved[Number(b.dataset.password)].ssid;
          const item = b.closest("li");
          item.innerHTML = `<span class="text-truncate">${esc(ssid)}</span>
            <input type="password" class="form-control form-control-sm flex-grow-1" placeholder="password" autocomplete="off">
            <button class="btn btn-sm btn-primary" type="button">Save</button>
            <button class="btn btn-sm btn-outline-secondary" type="button">Cancel</button>`;
          const [input, save, cancel] = item.querySelectorAll("input,button");
          editing = true;
          input.focus();
          cancel.onclick = (e) => { e.stopPropagation(); editing = false; drawSaved(); };
          save.onclick = (e) => {
            e.stopPropagation();
            busy(save, async () => {
              await api("/api/wifi/add", { ssid, password: input.value });
              toast(`Saved ${ssid}`, "success");
              editing = false;
              drawSaved(); // a push that came meanwhile was not drawn
            });
          };
          input.onkeydown = (e) => { if (e.key === "Enter") save.click(); };
          return;
        }
        busy(b, async () => {
          if (b.dataset.forget !== undefined) {
            const ssid = w.saved[Number(b.dataset.forget)].ssid;
            if (!confirm(`Forget ${ssid}? The device no longer connects to it.`)) return;
            await api("/api/wifi/remove", { index: b.dataset.forget });
          } else {
            await api("/api/wifi/move", { index: b.dataset.index, delta: b.dataset.move });
          }
          // the list follows with the next push
        });
      };
      $("#policy").onchange = (event) => api("/api/wifi/policy", { policy: event.target.value })
        .then(() => toast("saved", "success"), (e) => toast(e.message, "danger"));
      $("#reconnect").onclick = (e) => busy(e.target, async () => {
        await api("/api/wifi/reconnect", {});
        toast("reconnecting ...");
      });
    };
    const drawAdd = () => {
      $("#wifi-add").innerHTML = networkPicker(w.scan) +
        '<div class="d-flex justify-content-end"><button id="save-network" class="btn btn-primary" type="button">Save network</button></div>';
      const chosen = wirePicker(w.scan);
      $("#save-network").onclick = (e) => busy(e.target, async () => {
        const ssid = chosen();
        if (!ssid) throw new Error("choose a network");
        await api("/api/wifi/add", { ssid, password: $("#wifi-password").value });
        toast(`Saved ${ssid}`, "success");
        // done with the picker: the next push may draw it with the network's "saved" tag
        $("#wifi-password").value = "";
        document.activeElement.blur();
      });
    };
    drawSaved();
    drawAdd();
    $("#rescan").onclick = (e) => busy(e.target, async () => { w = await scanNetworks(); drawAdd(); });
    // A push draws what changed. Not the list while a password is typed into it, not the picker
    // while it has the focus or a password: a choice half made stays.
    let shown = JSON.stringify(w);
    // what was not drawn while picking, once the picker is left without a password typed
    let pickerBehind = false;
    page.live("/api/wifi", (data) => {
      const before = JSON.parse(shown);
      shown = JSON.stringify(data);
      w = data;
      const savedChanged = JSON.stringify([before.saved, before.policy]) !== JSON.stringify([w.saved, w.policy]);
      if (savedChanged && !editing) drawSaved();
      const add = $("#wifi-add");
      const busyPicking = add.contains(document.activeElement) || ($("#wifi-password")?.value ?? "") !== "";
      if ((savedChanged || JSON.stringify(before.scan) !== JSON.stringify(w.scan)) && !busyPicking) drawAdd();
      else if (busyPicking) pickerBehind = true;
    });
    $("#wifi-add").addEventListener("focusout", (event) => {
      if (!pickerBehind || $("#wifi-add").contains(event.relatedTarget) || $("#wifi-password")?.value) return;
      pickerBehind = false;
      drawAdd();
    });
  },

  async hotspot() {
    const page = render(`<h1>Hotspot</h1><div id="hotspot">Loading ...</div>`);
    const draw = (h) => {
      const box = $("#hotspot");
      if (!box || box.contains(document.activeElement)) return;
      box.innerHTML = card("Hotspot", `
        <div class="form-check form-switch mb-3">
          <input class="form-check-input" type="checkbox" role="switch" id="hotspot-on" ${h.kept_on ? "checked" : ""}>
          <label class="form-check-label" for="hotspot-on">Keep the hotspot on</label>
        </div>
        ${facts([
          ["State", h.on ? "on" + (h.off_in_s ? `, turns off in ${duration(h.off_in_s)}` : "") : "off"],
          ["Name", esc(h.ssid)], ["Password", (h.open ? "none during the setup, then " : "") + `<code>${esc(h.password)}</code>`], ["Devices", h.clients],
        ])}
        <div class="d-flex justify-content-end mt-3"><button id="new-password" class="btn btn-outline-secondary" type="button">New password</button></div>`) +
        '<p class="small text-body-secondary">The hotspot is on by itself while no Wi-Fi is set up and for 15 minutes after the setup. ' +
        "A device that has no Wi-Fi for 5 minutes opens it too (with the password), and the hotspot button (the Buttons page) held while the device starts " +
        "opens it without one; both stay until 15 minutes after the Wi-Fi is back.</p>";
      // the new state follows with the next push (a focused switch is not drawn over: blur it)
      $("#hotspot-on").onchange = (event) => api("/api/hotspot", { on: event.target.checked ? "1" : "0" })
        .then(() => event.target.blur(), (e) => toast(e.message, "danger"));
      $("#new-password").onclick = (e) => busy(e.target, async () => {
        if (!confirm("New hotspot password? Devices on the hotspot have to join again.")) return;
        await api("/api/hotspot/password", {});
        e.target.blur();
      });
    };
    // a push that came while a control had the focus is drawn when the focus leaves
    let latest = await api("/api/hotspot");
    draw(latest);
    page.live("/api/hotspot", (h) => draw((latest = h)));
    $("#hotspot").addEventListener("focusout", (event) => {
      if (!$("#hotspot").contains(event.relatedTarget)) setTimeout(() => draw(latest));
    });
  },

  // MQTT and Home Assistant discovery (docs/ui/web/web-app.md section 4)
  async mqtt() {
    const page = render(`<h1>MQTT</h1><div id="mqtt-status">Loading ...</div><div id="mqtt-form"></div>`);
    let m;
    try {
      m = await api("/api/mqtt");
    } catch (e) {
      if (e.status !== 404) throw e;
      render(`<h1>MQTT</h1>${card("MQTT", '<p class="text-body-secondary mb-0">Not in the recovery app. Start the firmware (System).</p>')}`);
      return;
    }
    const states = { off: ["off", "secondary"], connecting: ["connecting", "info"], connected: ["connected", "success"],
      disconnected: ["disconnected", "warning"] };
    const drawStatus = () => {
      const [text, kind] = states[m.state] || [m.state, "secondary"];
      $("#mqtt-status").innerHTML = card("Connection", facts([
        ["State", `<span class="badge text-bg-${kind}">${esc(text)}</span>` +
          (m.error ? ` <span class="text-danger">${esc(m.error)}</span>` : "")],
        ["Connected for", m.state === "connected" ? duration(m.connected_s) : "-"],
        ["Topics", `<code>${esc(m.topics)}/...</code>`],
      ]));
    };
    drawStatus();
    $("#mqtt-form").innerHTML = card("Broker", `
      <form id="mqtt" autocomplete="off">
        <div class="form-check form-switch mb-3">
          <input class="form-check-input" type="checkbox" role="switch" id="mqtt-on" ${m.enabled ? "checked" : ""}>
          <label class="form-check-label" for="mqtt-on">Connect</label>
        </div>
        <label class="form-label" for="mqtt-uri">Address</label>
        <input id="mqtt-uri" class="form-control mb-1" value="${esc(m.uri)}" placeholder="mqtt://homeassistant.local:1883" spellcheck="false" autocapitalize="off">
        <div class="form-text mb-3">mqtt:// or mqtts:// with the host and the port of the broker (Home Assistant's Mosquitto add-on: port 1883).</div>
        <div class="row g-3 mb-3">
          <div class="col-sm-6"><label class="form-label" for="mqtt-user">Username</label>
            <input id="mqtt-user" class="form-control" value="${esc(m.username)}" spellcheck="false" autocapitalize="off"></div>
          <div class="col-sm-6"><label class="form-label" for="mqtt-password">Password ${m.password_set ? '<span class="badge text-bg-secondary">stored</span>' : ""}</label>
            <input id="mqtt-password" class="form-control" type="password" placeholder="${m.password_set ? "empty keeps the stored one" : ""}"></div>
        </div>
        <div class="row g-3">
          <div class="col-sm-6"><label class="form-label" for="mqtt-prefix">Discovery prefix</label>
            <input id="mqtt-prefix" class="form-control" value="${esc(m.prefix)}" spellcheck="false" autocapitalize="off"></div>
          <div class="col-sm-6"><label class="form-label" for="mqtt-base">Topics (empty: pixeled/device name)</label>
            <input id="mqtt-base" class="form-control" value="${esc(m.base)}" spellcheck="false" autocapitalize="off"></div>
        </div>
        <div class="d-flex justify-content-end gap-2 mt-3">
          ${m.password_set ? '<button id="mqtt-forget" class="btn btn-outline-danger" type="button">Forget the password</button>' : ""}
          <button class="btn btn-primary" type="submit">Save and connect</button>
        </div>
      </form>`) +
      '<p class="small text-body-secondary">Home Assistant finds the device by itself (MQTT discovery): a light with the animations as effects, the speed, the power, the motion sensor and a restart button.</p>';
    $("#mqtt").onsubmit = (e) => {
      e.preventDefault();
      busy(e.submitter || $("#mqtt button[type=submit]"), async () => {
        m = await api("/api/mqtt", {
          enabled: $("#mqtt-on").checked ? "1" : "0", uri: $("#mqtt-uri").value.trim(),
          username: $("#mqtt-user").value.trim(), password: $("#mqtt-password").value,
          prefix: $("#mqtt-prefix").value.trim(), base: $("#mqtt-base").value.trim(),
        });
        $("#mqtt-password").value = "";
        drawStatus();
        toast("saved", "success");
      });
    };
    const forget = $("#mqtt-forget");
    if (forget) forget.onclick = (e) => busy(e.target, async () => {
      if (!confirm("Forget the stored MQTT password?")) return;
      m = await api("/api/mqtt", { forget_password: "1" });
      go("mqtt");
    });
    page.live("/api/mqtt", (state) => { m = state; drawStatus(); });
  },

  // the clock: time zone and SNTP servers (docs/ui/web/web-app.md section 4)
  async time() {
    const page = render(`<h1>Time</h1><div id="time-status">Loading ...</div><div id="time-form"></div>`);
    let t, zones;
    try {
      [t, zones] = await Promise.all([api("/api/time"), api("/api/time/zones")]);
    } catch (e) {
      if (e.status !== 404) throw e;
      render(`<h1>Time</h1>${card("Time", '<p class="text-body-secondary mb-0">Not in the recovery app. Start the firmware (System).</p>')}`);
      return;
    }
    const drawStatus = () => {
      const ago = t.last_sync ? duration(Math.max(0, t.now - t.last_sync)) + " ago" : "not yet";
      $("#time-status").innerHTML = card("Clock", facts([
        ["Time", t.synced ? esc(t.local) : '<span class="badge text-bg-warning">not set</span>'],
        // a name with the rule it stands for; a rule of its own once
        ["Time zone", `${esc(t.zone || "UTC")}${t.zone && t.zone !== t.rule
          ? ` <span class="small text-body-secondary font-monospace">${esc(t.rule)}</span>` : ""}`],
        ["Last answer", t.synced ? ago : "-"],
      ]));
    };
    drawStatus();
    // the zones grouped by their region ("Europe/Berlin": Europe), names without one last
    const groups = {};
    for (const z of zones) (groups[z.includes("/") ? z.split("/")[0] : "Other"] ||= []).push(z);
    const option = (z) => `<option value="${esc(z)}" ${z === t.zone ? "selected" : ""}>${esc(z.replace(/_/g, " "))}</option>`;
    const browserZone = Intl.DateTimeFormat().resolvedOptions().timeZone;
    // a zone that is not in the list is a POSIX TZ rule of its own (not a possible zone name)
    const customValue = "*custom";
    const custom = !!t.zone && !zones.includes(t.zone);
    $("#time-form").innerHTML = card("Settings", `
      <form id="time" autocomplete="off">
        <label class="form-label" for="time-zone">Time zone</label>
        <div class="d-flex gap-2 mb-3">
          <select id="time-zone" class="form-select">
            <option value="" ${t.zone ? "" : "selected"}>UTC (none set)</option>
            ${Object.keys(groups).sort((a, b) => (a === "Other") - (b === "Other") || a.localeCompare(b))
              .map((g) => `<optgroup label="${esc(g)}">${groups[g].map(option).join("")}</optgroup>`).join("")}
            <option value="${customValue}" ${custom ? "selected" : ""}>Custom rule (POSIX TZ)</option>
          </select>
          ${zones.includes(browserZone) ? `<button id="time-browser" class="btn btn-outline-secondary text-nowrap" type="button"
            title="${esc(browserZone)}">This browser's</button>` : ""}
        </div>
        <div id="time-custom" class="mb-3 ${custom ? "" : "d-none"}">
          <label class="form-label" for="time-rule">POSIX TZ rule</label>
          <input id="time-rule" class="form-control font-monospace" value="${esc(t.rule)}" placeholder="CET-1CEST,M3.5.0,M10.5.0/3"
            maxlength="63" spellcheck="false" autocapitalize="off">
          <div class="form-text">For a zone the list has wrong or not at all, such as after a change of the law. The
            standard name and its offset west of UTC, then the daylight saving name and when it starts and ends.</div>
        </div>
        <div class="row g-3">
          ${t.servers.map((s, i) => `<div class="col-sm-6"><label class="form-label" for="time-server${i + 1}">Time server ${i + 1}</label>
            <input id="time-server${i + 1}" class="form-control" value="${esc(s)}" placeholder="${esc(t.defaults[i] || "")}"
              maxlength="63" spellcheck="false" autocapitalize="off"></div>`).join("")}
        </div>
        <div class="form-text">Host names or addresses of NTP servers, such as the router. Both empty: ${t.defaults.map(esc).join(" and ")}.</div>
        <div class="d-flex justify-content-end mt-3"><button class="btn btn-primary" type="submit">Save</button></div>
      </form>`);
    const showCustom = () => $("#time-custom").classList.toggle("d-none", $("#time-zone").value !== customValue);
    $("#time-zone").onchange = showCustom;
    const browser = $("#time-browser");
    if (browser) browser.onclick = () => { $("#time-zone").value = browserZone; showCustom(); };
    $("#time").onsubmit = (e) => {
      e.preventDefault();
      busy(e.submitter || $("#time button[type=submit]"), async () => {
        const zone = $("#time-zone").value;
        const form = { zone: zone === customValue ? $("#time-rule").value.trim() : zone };
        t.servers.forEach((_, i) => (form[`server${i + 1}`] = $(`#time-server${i + 1}`).value.trim()));
        t = await api("/api/time", form);
        drawStatus();
        toast("saved", "success");
      });
    };
    page.live("/api/time", (state) => { t = state; drawStatus(); });
  },
  async logs() {
    render(`<h1>Logs</h1><div class="card"><div class="card-body">
      <div class="d-flex justify-content-between align-items-center gap-2 mb-2">
        <span id="log-count" class="small text-body-secondary"></span>
        <select id="log-filter" class="form-select form-select-sm w-auto" aria-label="Show">
          <option value="E">Errors</option><option value="W">Warnings and errors</option>
          <option value="I" selected>Everything</option>
        </select>
      </div>
      <div class="d-flex mb-3">
        <pre id="log-lines" class="app-log flex-grow-1 mb-0">Loading ...</pre>
        <div id="log-grip" class="app-grip" title="Drag to resize, double-click for the default"></div>
      </div>
      <div class="d-flex flex-wrap gap-2 align-items-center">
        <button id="log-refresh" class="btn btn-outline-secondary" type="button">Refresh</button>
        <button id="log-download" class="btn btn-outline-secondary" type="button">Download</button>
        <div class="form-check form-switch ms-auto mb-0">
          <input id="log-debug" class="form-check-input" type="checkbox" role="switch">
          <label class="form-check-label" for="log-debug">Debug</label>
        </div>
      </div>
      <p class="small text-body-secondary mt-2 mb-0">Errors and warnings are kept in the device's memory, the
        oldest go first. Debug keeps the info messages as well. The log starts again with each restart.</p>
    </div></div>`);
    let entries = [];
    const rank = { E: 0, W: 1, I: 2, D: 3, V: 4 };
    const pad = (n) => String(n).padStart(2, "0");
    // the device's clock once it is set, else the time since the start
    const when = (e) => {
      if (e.unix) {
        const d = new Date(e.unix * 1000);
        const time = `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
        return d.toDateString() === new Date().toDateString() ? time
          : `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${time}`;
      }
      const s = Math.floor(e.ms / 1000);
      return `+${Math.floor(s / 3600)}:${pad(Math.floor(s / 60) % 60)}:${pad(s % 60)}`;
    };
    const draw = () => {
      const max = rank[$("#log-filter").value];
      const shown = entries.filter((e) => rank[e.level] <= max);
      $("#log-count").textContent = `${shown.length} of ${entries.length} messages`;
      $("#log-lines").innerHTML = shown.length ? shown.map((e) =>
        `<span class="log-${e.level}">${esc(when(e))} ${e.level} ${esc(e.tag)}  ${esc(e.text)}</span>`).join("\n")
        : '<span class="text-body-secondary">No messages.</span>';
      const box = $("#log-lines");
      box.scrollTop = box.scrollHeight;
    };
    const load = async () => {
      const [settings, response] = await Promise.all([api("/api/log"), fetch("/api/log.txt", { cache: "no-store" })]);
      if (response.status === 401) return go("login");
      if (!response.ok) throw new Error(`the log could not be loaded (${response.status})`);
      $("#log-debug").checked = settings.debug;
      // `<unix time or 0> <level> (<ms since the start>) <tag>: <message>`
      entries = (await response.text()).split("\n").map((line) => {
        const m = /^(\d+) ([EWIDV]) \((\d+)\) ([^:]*): ?(.*)$/.exec(line);
        return m && { unix: Number(m[1]), level: m[2], ms: Number(m[3]), tag: m[4], text: m[5] };
      }).filter(Boolean);
      draw();
    };
    // the border right of the box drags the page's width, kept in this browser; a double-click goes
    // back to the 800 px column. The page stays centered: it grows by twice the drag.
    const page = $("#page"), grip = $("#log-grip");
    const size = (w) => { page.style.maxWidth = w ? `${Math.max(480, w)}px` : ""; };
    size(Number(localStorage.getItem("logWidth")) || 0);
    grip.onpointerdown = (e) => {
      const x0 = e.clientX, w0 = page.offsetWidth;
      grip.setPointerCapture(e.pointerId);
      grip.onpointermove = (m) => size(w0 + 2 * (m.clientX - x0));
      grip.onpointerup = grip.onpointercancel = () => {
        grip.onpointermove = null;
        localStorage.setItem("logWidth", page.offsetWidth);
      };
    };
    grip.ondblclick = () => { size(0); localStorage.removeItem("logWidth"); };
    $("#log-filter").onchange = draw;
    $("#log-refresh").onclick = (e) => busy(e.target, load);
    $("#log-download").onclick = () => {
      const text = entries.map((e) => `${when(e)} ${e.level} (${e.ms}) ${e.tag}: ${e.text}`).join("\n") + "\n";
      const link = document.createElement("a");
      link.href = URL.createObjectURL(new Blob([text], { type: "text/plain" }));
      const d = new Date();
      link.download = `${document.title || "pixeled"}-log-${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}-${pad(d.getHours())}${pad(d.getMinutes())}.txt`;
      link.click();
      URL.revokeObjectURL(link.href);
    };
    $("#log-debug").onchange = (e) => busy(e.target, async () => {
      try {
        await api("/api/log", { debug: e.target.checked ? 1 : 0 });
      } catch (err) {
        e.target.checked = !e.target.checked;
        throw err;
      }
      toast(e.target.checked ? "debug on: info messages are kept from now on" : "debug off", "success");
    });
    await load();
  },

  async system() {
    const [s, u] = await Promise.all([api("/api/status"), api("/api/update")]);
    // the recovery app has no backup (its /api/backup answers 404)
    const recovery = u.app === "recovery";
    render(`<h1>System</h1>` +
      card("Device", facts([
        ["Firmware", esc(s.device.firmware)], ["Built", esc(s.device.built)], ["Uptime", duration(s.device.uptime_s)],
        ["Free memory", `${s.device.heap_internal_kb} KB internal, ${s.device.heap_psram_kb} KB PSRAM`],
        ["Firmware update", u.firmware_direct ? "direct" : "through the recovery app"],
      ])) +
      (recovery ? "" : card("Backup", `
        <p class="small text-body-secondary">The settings and the presets in one YAML file. Wi-Fi passwords and the
          MQTT password are not in it; they may be added to the file by hand before an import.</p>
        <div class="d-flex gap-2 flex-wrap mb-3">
          <a class="btn btn-outline-secondary" href="/api/backup.yaml">Export all</a>
          <a class="btn btn-outline-secondary" href="/api/backup.yaml?parts=settings">Settings only</a>
          <a class="btn btn-outline-secondary" href="/api/backup.yaml?parts=presets">Presets only</a>
        </div>
        <label class="form-label" for="backup-file">Import a backup</label>
        <input id="backup-file" type="file" class="form-control mb-2" accept=".yaml,.yml">
        <div class="d-flex gap-2 justify-content-end">
          <button id="backup-check" class="btn btn-outline-secondary" type="button">Check</button>
          <button id="backup-import" class="btn btn-primary" type="button">Import</button>
        </div>
        <div id="backup-report" class="mt-3"></div>`)) +
      (session.logged_in ? card("Password", `
        <p class="small text-body-secondary">The password of the web UI login <code>${esc(session.user)}</code>. Every other
          browser is logged out; this one stays logged in until it is closed.</p>
        <input id="pw-current" type="password" class="form-control mb-2" placeholder="Current password" autocomplete="current-password">
        <input id="pw-new" type="password" class="form-control mb-2" placeholder="New password, 8 to 64 characters" autocomplete="new-password">
        <input id="pw-repeat" type="password" class="form-control mb-2" placeholder="New password again" autocomplete="new-password">
        <div class="d-flex justify-content-end"><button id="pw-go" class="btn btn-primary" type="button">Change password</button></div>
        <div class="d-flex justify-content-between align-items-center gap-2 border-top pt-3 mt-3">
          <span class="small text-body-secondary">Ends the session of every browser, this one too.</span>
          <button id="logout-all" class="btn btn-outline-secondary text-nowrap" type="button">Log out everywhere</button>
        </div>`) : "") +
      card("Update", `
        <label class="form-label" for="fw-file">Firmware image</label>
        <input id="fw-file" type="file" class="form-control mb-2" accept=".bin">
        <input id="fw-url" class="form-control mb-2" placeholder="or a URL: http(s)://.../pixeled.bin" autocomplete="off">
        <div class="d-flex justify-content-end mb-3"><button id="fw-go" class="btn btn-primary" type="button">Update firmware</button></div>
        <label class="form-label" for="fs-file">File system image (these web pages)</label>
        <input id="fs-file" type="file" class="form-control mb-2" accept=".bin">
        <input id="fs-url" class="form-control mb-2" placeholder="or a URL" autocomplete="off">
        <div class="d-flex justify-content-end"><button id="fs-go" class="btn btn-primary" type="button">Update file system</button></div>
        <div class="progress mt-3" id="progress" hidden><div class="progress-bar" style="width:0%"></div></div>
        <p class="mt-2 mb-0" id="update-message"></p>`) +
      card("File system", `
        <p class="mb-2">${esc(s.fs.state)}${s.fs.state === "mounted" ? `, ${s.fs.used_kb} of ${s.fs.total_kb} KB used` : ""}</p>
        <div class="d-flex gap-2 flex-wrap">
          <a class="btn btn-outline-secondary" href="/api/fs/files.zip">Download the files (.zip)</a>
          <button id="format" class="btn btn-outline-danger ms-auto" type="button">Format</button>
        </div>`) +
      card("Restart and reset", `
        <div class="d-flex gap-2 flex-wrap">
          <button id="restart" class="btn btn-outline-secondary" type="button">Restart</button>
          ${recovery && u.firmware_present ? '<button id="to-firmware" class="btn btn-outline-secondary" type="button">Start the firmware</button>' : ""}
          <button id="reset" class="btn btn-outline-danger ms-auto" type="button">Factory reset</button>
        </div>`));
    if (!recovery) {
      $("#backup-check").onclick = (e) => busy(e.target, () => importBackup(true));
      $("#backup-import").onclick = (e) => busy(e.target, () => importBackup(false));
    }
    if ($("#pw-go")) {
      if ($("#logout-all")) $("#logout-all").onclick = (e) => busy(e.target, async () => {
        if (!confirm("Log out on every browser and device?")) return;
        await api("/api/logout-all", {}).catch(() => {});
        go("login");
      });
      $("#pw-go").onclick = (e) => busy(e.target, async () => {
        const current = $("#pw-current").value, password = $("#pw-new").value;
        if (password.length < 8 || password.length > 64) throw new Error("the new password needs 8 to 64 characters");
        if (password !== $("#pw-repeat").value) throw new Error("the two new passwords differ");
        await api("/api/password", { current, password });
        for (const id of ["#pw-current", "#pw-new", "#pw-repeat"]) $(id).value = "";
        toast("password changed, other browsers are logged out");
      });
    }
    $("#fw-go").onclick = (e) => busy(e.target, () => runUpdate("firmware"));
    $("#fs-go").onclick = (e) => busy(e.target, () => runUpdate("filesystem"));
    $("#format").onclick = (e) => busy(e.target, async () => {
      if (!confirm("Format the file system? Every file on it is deleted, also these web pages: the device then shows its built-in page.")) return;
      await api("/api/fs/format", { confirm: "yes" });
      toast("formatted", "warning");
      setTimeout(() => location.reload(), 1500);
    });
    $("#restart").onclick = (e) => busy(e.target, async () => {
      if (!confirm("Restart the device?")) return;
      await api("/api/restart", {});
      toast("restarting ...");
    });
    if ($("#to-firmware")) {
      // back from the recovery app; the page then shows the firmware's System page
      $("#to-firmware").onclick = (e) => busy(e.target, async () => {
        await api("/api/restart", { to: "firmware" });
        toast("starting the firmware ...");
        await waitForApp("main");
        location.reload();
      });
    }
    $("#reset").onclick = (e) => busy(e.target, async () => {
      if (!confirm("Factory reset? All settings, Wi-Fi networks and the web UI login are deleted.")) return;
      if (!confirm("Really? The device starts like new, with its hotspot.")) return;
      await api("/api/factory-reset", { confirm: "yes" });
      toast("erased, restarting ...", "warning");
    });
  },
};

// --------------------------------------------------------------------------------- backup

// The file goes up as it is; the answer lists the sections, the problems (nothing imported then),
// what the user should know and what was ignored.
async function importBackup(checkOnly) {
  const file = $("#backup-file").files[0];
  if (!file) throw new Error("choose a backup file");
  if (!checkOnly && !confirm("Import the backup? What is in the file replaces the settings and the presets of this device.")) return;
  const response = await fetch("/api/backup/import" + (checkOnly ? "?check=1" : ""),
    { method: "POST", body: await file.text(), cache: "no-store" });
  const r = await response.json().catch(() => ({}));
  if (response.status === 401) {
    session.logged_in = false;
    go("login");
    return;
  }
  if (!Array.isArray(r.problems)) throw new Error(r.error || response.statusText);
  const list = (title, items, kind) => items.length
    ? `<div class="mb-2"><strong class="${kind}">${title}</strong><ul class="mb-0 small">${items.map((i) => `<li>${esc(i)}</li>`).join("")}</ul></div>`
    : "";
  const sections = esc(r.sections.join(", ") || "nothing");
  const head = r.problems.length ? '<p class="text-danger mb-2">Not imported, nothing was changed: fix the file first.</p>'
    : r.imported ? `<p class="text-success mb-2">Imported: ${sections}.</p>`
    : `<p class="text-success mb-2">The file is fine (${sections}). Import applies it.</p>`;
  $("#backup-report").innerHTML = head + list("Problems", r.problems, "text-danger") +
    list("Please note", r.notes, "") + list("Ignored", r.ignored, "text-body-secondary");
  if (r.imported) toast("backup imported", "success");
}

// --------------------------------------------------------------------------------- update

function progress(done, total) {
  const box = $("#progress");
  if (!box) return;
  box.hidden = !total;
  if (total) box.firstElementChild.style.width = Math.round((100 * done) / total) + "%";
}

function updateMessage(text) {
  if ($("#update-message")) $("#update-message").textContent = text;
}

async function waitForApp(app) {
  for (let i = 0; i < 60; i++) {
    await sleep(2000);
    try {
      const u = await (await fetch("/api/update", { cache: "no-store" })).json();
      // a new firmware is kept only after it confirmed itself
      if (u.app === app && u.confirmed !== false) return u;
    } catch (e) { /* restarting */ }
  }
  throw new Error("the device did not come back, reload the page");
}

// XMLHttpRequest, because fetch() reports no upload progress
function upload(url, file) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", url);
    xhr.upload.onprogress = (e) => progress(e.loaded, e.total);
    xhr.onload = () => {
      let body = {};
      try { body = JSON.parse(xhr.responseText); } catch (e) { /* no JSON */ }
      if (xhr.status === 200) resolve(body); else reject(new Error(body.error || xhr.statusText));
    };
    xhr.onerror = () => reject(new Error("the connection broke off"));
    xhr.send(file);
  });
}

async function runUpdate(target) {
  const file = $(target === "firmware" ? "#fw-file" : "#fs-file").files[0];
  const url = $(target === "firmware" ? "#fw-url" : "#fs-url").value.trim();
  if (!file && !url) throw new Error("choose an image or enter a URL");
  // the device restarts: nothing to follow meanwhile
  liveStop();
  try {
    let u = await api("/api/update");
    if (target === "firmware" && !u.firmware_direct) {
      updateMessage("restarting into the recovery app ...");
      await api("/api/restart", { to: "recovery" });
      await waitForApp("recovery");
    }
    if (file) {
      updateMessage(`uploading ${Math.round(file.size / 1024)} KB ...`);
      await upload("/api/update/" + target, file);
    } else {
      await api("/api/update/url", { target, url });
      for (;;) {
        await sleep(1000);
        u = await api("/api/update").catch(() => null);
        if (!u) continue;
        progress(u.done, u.total || 0);
        updateMessage(`downloading ${Math.round(u.done / 1024)} KB ...`);
        if (u.state === "done") break;
        if (u.state === "failed") throw new Error(u.error);
      }
    }
    progress(0, 0);
    if (target === "firmware") {
      updateMessage("written, restarting into the new firmware ...");
      u = await waitForApp("main");
      updateMessage(`running firmware ${u.firmware_version}`);
    } else {
      updateMessage("file system updated, reloading the page ...");
      await sleep(1500);
      location.reload();
    }
  } catch (e) {
    progress(0, 0);
    updateMessage(e.message);
    throw e;
  }
}

// --------------------------------------------------------------------------------------- start

// the entries of a feature stay hidden until /api/status says the device has it (no flicker)
$("#menu-pages").innerHTML = menuGroups.map(([group, pages]) => `<div data-group="${esc(group)}" ${pages.some((p) => p[3]) ? "hidden" : ""}>
  <div class="app-menu-heading">${esc(group)}</div>${pages.map(navItem).join("")}</div>`).join("");
const allPages = menuGroups.flatMap(([, pages]) => pages);
$("#tabbar").innerHTML = tabPages.map((page) => navItem(allPages.find((p) => p[0] === page))).join("") +
  `<button id="tab-more" class="app-nav-item" type="button" aria-label="More pages">${navIcon("more")}<span class="app-nav-short">More</span></button>`;
$("#tab-more").onclick = () => openSheet($("#menu").hidden);

// the appearance: one button that goes Auto, Light, Dark
const themes = { auto: "Auto", light: "Light", dark: "Dark" };
let theme = localStorage.getItem("theme") || "auto";
function applyTheme() {
  const dark = theme === "dark" || (theme === "auto" && matchMedia("(prefers-color-scheme: dark)").matches);
  document.documentElement.setAttribute("data-bs-theme", dark ? "dark" : "light");
  $("#theme").innerHTML = `${navIcon(theme)}<span class="app-nav-short">${themes[theme]}</span><span class="app-nav-long">Appearance: ${themes[theme]}</span>`;
  $("#theme").title = `Appearance: ${themes[theme]} (click for the next)`;
}
$("#theme").onclick = () => {
  theme = { auto: "light", light: "dark", dark: "auto" }[theme] || "auto";
  localStorage.setItem("theme", theme);
  applyTheme();
};
applyTheme();
matchMedia("(prefers-color-scheme: dark)").addEventListener("change", applyTheme);

$("#logout").innerHTML = `${navIcon("logout")}<span class="app-nav-short">Log out</span><span class="app-nav-long">Log out</span>`;
$("#logout").onclick = async () => {
  await api("/api/logout", {}).catch(() => {});
  go("login");
};

window.addEventListener("hashchange", route);
route();
