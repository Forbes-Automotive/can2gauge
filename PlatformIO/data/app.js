// can2gauge web UI - talks to the JSON API in src/can2gauge_API.cpp
const setIntervalDuration = 500; // status poll (ms)
let settingsCache = null;        // /api/settings
let catalogue = null;            // /api/signals
let themesCache = null;          // /api/themes
let statusCache = null;
let spritesCache = { sprites: [] }; // /api/sprites

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
const $ = (id) => document.getElementById(id);
async function api(path, method = "GET", body = null) {
  const opt = { method, headers: {} };
  if (body !== null) { opt.headers["Content-Type"] = "application/json"; opt.body = JSON.stringify(body); }
  const r = await fetch(path, opt);
  return r.json();
}
// 0.30.0: the Navigation card (0.30.5: its own Navigation tab): is the phone connected, and the turn it last sent
function renderNav(n) {
  if (!n || !$("navState")) return;
  let state = "";
  if (!n.mode) {
    state = "Nav link off";
  } else if (!n.started) {
    state = "Bluetooth not started" + (n.error ? " (" + n.error + ")" : "");
  } else if (!n.phone) {
    // 0.30.5: say whether the phone can see the gauge at all
    if (n.advertising) {
      state = "Waiting for the can2gauge Link app (the gauge is visible to the phone)";
    } else {
      state = "Waiting for the can2gauge Link app - not visible to the phone yet, retrying";
    }
    if (n.advertRestarts) state += " - visibility restarted " + n.advertRestarts + "x";
  } else if (n.navigating) {
    state = "Connected - navigating";
  } else {
    state = "Connected - no route in Google Maps";
  }
  $("navState").textContent = state;
  let now = "";
  if (n.test) {
    now = "Test turn on the glass.";
  } else if (n.navigating) {
    now = "Next: " + n.dist + "  " + n.instruction + (n.eta ? "  (" + n.eta + ")" : "");
    if (n.progress >= 0) now += "  - journey " + n.progress + "% done"; // 0.30.8: the ring on the glass
    if (n.card) now += "  - on the glass";
  }
  $("navNow").textContent = now;
  $("navRestartRow").hidden = !n.needsRestart;
}

async function action(name, extra = {}) {
  const res = await api("/api/action", "POST", Object.assign({ action: name }, extra));
  toast(res.status || res.error || "ok");
  return res;
}
function toast(msg) {
  let t = $("toast");
  if (!t) { t = document.createElement("div"); t.id = "toast"; t.style.cssText = "position:fixed;bottom:18px;left:50%;transform:translateX(-50%);background:var(--panel);border:1px solid var(--accent);color:var(--text);padding:8px 16px;border-radius:10px;font-size:13px;z-index:99;"; document.body.appendChild(t); }
  t.textContent = msg; t.style.display = "block";
  clearTimeout(t._h); t._h = setTimeout(() => t.style.display = "none", 2500);
}
// names that come over the air (BLE adverts, ESP-NOW heartbeats) never go into innerHTML unescaped
function esc(s) { return String(s == null ? "" : s).replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c])); }
function setVal(id, v) { const e = $(id); if (!e) return; if (e.dataset.hex) e.value = "0x" + Number(v).toString(16).toUpperCase(); else e.value = v; }
function getVal(id) { const e = $(id); if (!e) return undefined; if (e.dataset.hex) return parseInt(e.value, 16); if (e.type === "number" || e.type === "range" || e.tagName === "SELECT") { const n = Number(e.value); return isNaN(n) ? e.value : n; } return e.value; }
const uShown = (u) => (u === "C" ? "\u00b0C" : u === "F" ? "\u00b0F" : u === "deg" ? "\u00b0" : u || ""); // as the gauge shows units
// the adapter's state as the web UI says it. 0.37.1: "not found - stopped" says how to look again
function elmStateText(state) {
  if (state === "polling") return "connected";
  if (state === "not found - stopped") return "not found - stopped (Scan, or ignition off and on, looks again)";
  return state;
}
function fmt(v, d = 1) { return (Math.round(v * Math.pow(10, d)) / Math.pow(10, d) + 0).toString(); } // + 0 turns -0 into 0
// 0.17.0: a value with a fixed number of decimals, "0.00" and never "-0.00" (minus only below zero once rounded)
function fixedText(v, decimals) {
  let text = Number(v).toFixed(decimals);
  if (Number(text) === 0) {
    text = (0).toFixed(decimals);
  }
  return text;
}
// 0.17.1: a status shown with Every Word Capitalised ("switched power" -> "Switched Power", "listen-only" ->
// "Listen-Only"). Only words written all in small letters change: units (kB, ms, km/h, fps), numbers, hex (0x7E8) and
// words already in capitals (CAN, WiFi) stay as they are, and so do the small joining words unless they come first.
const TITLE_KEEP = ["a", "an", "the", "of", "to", "or", "and", "in", "for", "at", "per", // joining words
  "kb", "kbps", "ms", "s", "min", "h", "km", "fps", "sps", "mbar", "bar", "psi", "x", "y"]; // units
function titleCase(text) {
  if (text === undefined || text === null) return "";
  return String(text).replace(/\b[a-z]+\b/g, (word, at, whole) => {
    const isFirst = /^[^A-Za-z0-9]*$/.test(whole.slice(0, at)); // the very first word (nothing but spaces / brackets before it)
    if (!isFirst && TITLE_KEEP.includes(word)) return word;
    return word.charAt(0).toUpperCase() + word.slice(1);
  });
}
// a Live data value with its own decimals ("d", from the gauge: 0.36.14 every value, before only ADC sensors), else the
// way autoText shows it (an older gauge)
function sigText(signal, autoText) {
  if (signal.d !== undefined) {
    return fixedText(signal.v, signal.d);
  }
  return autoText(Number(signal.v));
}
function sigOptions(sel, current, allowNone = false) {
  sel.innerHTML = "";
  catalogue.signals.forEach(s => {
    if (s.key === "none" && !allowNone) return;
    if (/^me7_\d+$/.test(s.key) && s.key !== current) return; // 0.35.0: ME7 1-8 are never filled (ME7 variables are page values)
    if (!adcSensorFound(s.key) && s.key !== current) return; // 0.36.6: an adc2can board that isn't there
    const o = document.createElement("option"); o.value = s.key; o.textContent = s.name + (s.unit ? " (" + uShown(s.unit) + ")" : "");
    if (isAdc(s.key)) o.textContent = adcSensorText(Number(s.key.slice(3)) - 1); // 0.36.6: the same name everywhere
    if (s.key === current) o.selected = true; sel.appendChild(o);
  });
}

// ---------------------------------------------------------------------------
// navigation
// ---------------------------------------------------------------------------
document.querySelectorAll(".nav-tab").forEach(tab => tab.addEventListener("click", () => {
  document.querySelectorAll(".nav-tab").forEach(t => t.classList.remove("active"));
  document.querySelectorAll(".page").forEach(p => p.classList.remove("active"));
  tab.classList.add("active");
  $(tab.dataset.page + "-page").classList.add("active");
  if (tab.dataset.page === "kline") loadFiles();
  if (tab.dataset.page === "can") { canIdsTick = 0; loadDbcLibrary(); }
  if (tab.dataset.page === "gauges" && settingsCache) loadSourceData().then(() => { if (!$("pagesList").contains(document.activeElement)) renderPages(); });
  if (tab.dataset.page === "triggers" && settingsCache) refreshTriggerSources(); // 0.35.0
}));
document.querySelectorAll("[data-action]").forEach(b => b.addEventListener("click", () => action(b.dataset.action)));
document.querySelectorAll("[data-goto]").forEach(a => a.addEventListener("click", () => { // link to another tab
  const t = document.querySelector(`.nav-tab[data-page="${a.dataset.goto}"]`); if (t) t.click();
}));
document.querySelectorAll(".mode-btn").forEach(b => b.addEventListener("click", () => action("haldexMode", { mode: Number(b.dataset.mode) })));

// phone layout: on a narrow screen the tab row is hidden and the top bar's menu button opens the pages in groups
// (index.html's <style>). The list is built from the tabs each time it opens, so the tabs hidden in Settings > Menu and
// tabs and First Boot's tab (firstboot.js) follow along, and a tap just clicks the real tab.
const NAV_GROUPS = [
  ["Live", ["dashboard", "gauges", "log", "nav"]],
  ["Car data", ["can", "ecu", "kline", "ble", "adc", "oil", "dsg", "vdiag", "service", "haldex"]],
  ["Set up", ["firstboot", "triggers", "chain", "carscan", "settings"]], // 0.37.5: Scan My Car fourth
  ["Tools", ["diag", "ota"]],
];
function navSheetBuild() {
  // 0.23.0: the tool tabs (adv-tab) are left out in Basic
  const tabs = [...document.querySelectorAll(".nav-tab")].filter(t => t.style.display !== "none" && (showAdvanced || !t.classList.contains("adv-tab")));
  const grouped = NAV_GROUPS.flatMap(([, pages]) => pages);
  const groups = NAV_GROUPS.map(([name, pages]) => [name, pages.map(p => tabs.find(t => t.dataset.page === p)).filter(Boolean)]);
  const others = tabs.filter(t => !grouped.includes(t.dataset.page)); // a tab added later that has no group yet
  if (others.length) groups.push(["More", others]);
  // 0.23.0: the Advanced switch sits at the bottom of the menu, under Tools
  let advChecked = "";
  if (showAdvanced) advChecked = "checked";
  const advSwitch = `<div class="nav-adv"><label class="toggle"><input type="checkbox" id="navAdvanced" ${advChecked}><span class="toggle-slider"></span><span class="toggle-label">Advanced</span></label></div>`;
  $("navSheet").innerHTML = groups.filter(([, list]) => list.length).map(([name, list]) =>
    `<div class="nav-group"><h4>${name}</h4><div class="items">` +
    list.map(t => `<button class="nav-item${t.classList.contains("active") ? " active" : ""}" data-page="${t.dataset.page}">${t.textContent}</button>`).join("") +
    `</div></div>`).join("") + advSwitch;
}
function navSheetShow(open) {
  if (open) {
    navSheetBuild();
    $("navSheet").style.top = document.querySelector(".topbar").getBoundingClientRect().bottom + "px"; // just under the top bar
  }
  $("navSheet").classList.toggle("open", open);
  // 0.36.8: the page behind the menu stays put while it's open (index.html's html.nav-open / body.nav-open)
  document.documentElement.classList.toggle("nav-open", open);
  document.body.classList.toggle("nav-open", open);
  $("menuBtn").setAttribute("aria-expanded", open);
}
$("menuBtn").addEventListener("click", () => navSheetShow(!$("navSheet").classList.contains("open")));
$("navSheet").addEventListener("click", (e) => {
  if (e.target.closest(".nav-adv")) return; // the Advanced switch: the menu stays open
  const item = e.target.closest(".nav-item");
  if (item) document.querySelector(`.nav-tab[data-page="${item.dataset.page}"]`).click();
  navSheetShow(false); // a page picked, or a tap on the empty space: close
});
$("navSheet").addEventListener("change", navAdvancedChanged);

// ---------------------------------------------------------------------------
// 0.23.0: Basic / Advanced. Basic (the default) shows the everyday settings. The fine-tuning on a card sits in a
// <div class="adv"> behind a "More Options" button, and the tool tabs / inspection cards (class adv-tab / adv-card)
// only show in Advanced. The Advanced switch (phone menu, Settings > Device) opens everything; the browser remembers it.
// The hiding itself is CSS in index.html, keyed off the class "show-adv" on the page's body.
// ---------------------------------------------------------------------------
let showAdvanced = false; // true = the Advanced switch is on
let openGauge = -1;       // the gauge open on the Gauges tab (-1 = none), kept when the list is redrawn
let isGaugeFoldOpen = false; // that gauge's More Options are open

// turn Advanced on or off (isOn true / false), remember it in this browser and update both switches
function setAdvanced(isOn) {
  showAdvanced = isOn;
  document.body.classList.toggle("show-adv", isOn);
  if (settingsCache && catalogue && $("pagesList")) renderPages(); // 0.37.0: the pages' Source lists follow Basic / Advanced
  try {
    if (isOn) {
      localStorage.setItem("c2g.showAdvanced", "1");
    } else {
      localStorage.setItem("c2g.showAdvanced", "0");
    }
  } catch (e) { /* private window: not remembered */ }
  $("showAdvanced").checked = isOn;
  // a tool tab that just went away can't stay open: back to the Dashboard
  const activeTab = document.querySelector(".nav-tab.active");
  if (!isOn && activeTab && activeTab.classList.contains("adv-tab")) {
    document.querySelector('.nav-tab[data-page="dashboard"]').click();
  }
}

// the Advanced switch in the phone menu: redraw the menu so the tool tabs come and go
function navAdvancedChanged(event) {
  if (event.target.id !== "navAdvanced") return;
  setAdvanced(event.target.checked);
  navSheetBuild();
}

// the Advanced switch on Settings > Device
function settingsAdvancedChanged(event) {
  setAdvanced(event.target.checked);
}

// put a "More Options" button in front of every fold (<div class="adv">) inside root that doesn't have one yet
function setupFolds(root) {
  const folds = root.querySelectorAll(".adv");
  for (let i = 0; i < folds.length; i++) {
    const fold = folds[i];
    const before = fold.previousElementSibling;
    if (before && before.classList.contains("more-btn")) continue; // already done
    const button = document.createElement("button");
    button.type = "button";
    button.className = "more-btn";
    button.innerHTML = 'More Options <span class="chev">&#9662;</span>';
    button.setAttribute("aria-expanded", "false");
    if (fold.classList.contains("open")) {
      button.classList.add("open");
      button.firstChild.textContent = "Fewer Options ";
      button.setAttribute("aria-expanded", "true");
    }
    button.addEventListener("click", foldButtonClicked);
    fold.parentNode.insertBefore(button, fold);
  }
}

// a More Options button: open or close the fold just after it
function foldButtonClicked(event) {
  const button = event.currentTarget;
  const fold = button.nextElementSibling;
  const isOpen = !fold.classList.contains("open");
  fold.classList.toggle("open", isOpen);
  button.classList.toggle("open", isOpen);
  button.setAttribute("aria-expanded", String(isOpen));
  if (isOpen) {
    button.firstChild.textContent = "Fewer Options ";
  } else {
    button.firstChild.textContent = "More Options ";
  }
  if (fold.closest(".page-card")) isGaugeFoldOpen = isOpen; // the open gauge keeps it through a redraw
}

// the help text on each card goes behind an (i) button on the card's title. Only the plain help lines that sit
// straight in the card: ones with an id are live messages the page fills in, so they stay showing.
function setupHelpTips() {
  const cards = document.querySelectorAll(".page .card");
  for (let i = 0; i < cards.length; i++) {
    const card = cards[i];
    if (card.closest("#ota-page") || card.closest("#firstboot-page")) continue; // the shared OTA page and First Boot keep theirs
    const title = card.firstElementChild;
    if (!title || title.tagName !== "H2") continue;
    let tipCount = 0;
    for (let k = 0; k < card.children.length; k++) {
      const child = card.children[k];
      if (child.classList.contains("help") && !child.id) {
        child.classList.add("tip");
        tipCount = tipCount + 1;
      }
    }
    if (tipCount === 0) continue;
    const button = document.createElement("button");
    button.type = "button";
    button.className = "tip-btn"; // the "i" is drawn by the CSS, so the title's text stays the same
    button.setAttribute("aria-label", "Help");
    button.setAttribute("aria-expanded", "false");
    button.addEventListener("click", tipButtonClicked);
    title.classList.add("has-tip");
    title.appendChild(button);
  }
}

// an (i) button: show or hide its card's help text
function tipButtonClicked(event) {
  const card = event.currentTarget.closest(".card");
  const isOpen = !card.classList.contains("tips-open");
  card.classList.toggle("tips-open", isOpen);
  event.currentTarget.setAttribute("aria-expanded", String(isOpen));
}

// Settings: one section at a time, picked with the chips along the top (each card's data-sec says its section)
function showSettingsSection(name) {
  const chips = document.querySelectorAll("#settingsChips .sec-chip");
  for (let i = 0; i < chips.length; i++) {
    chips[i].classList.toggle("active", chips[i].dataset.sec === name);
  }
  const cards = document.querySelectorAll("#settings-page .card[data-sec]");
  for (let i = 0; i < cards.length; i++) {
    cards[i].classList.toggle("sec-hidden", cards[i].dataset.sec !== name);
  }
  try {
    localStorage.setItem("c2g.settingsSec", name);
  } catch (e) { /* private window: not remembered */ }
}

// a chip on the Settings tab
function settingsChipClicked(event) {
  const chip = event.target.closest(".sec-chip");
  if (chip) showSettingsSection(chip.dataset.sec);
}

// start-up: the folds, the (i) buttons, the Settings section and the Advanced switch as this browser last left them
function setupBasicAdvanced() {
  setupFolds(document);
  setupHelpTips();
  $("settingsChips").addEventListener("click", settingsChipClicked);
  let section = "display";
  let isAdvanced = false;
  try {
    const savedSection = localStorage.getItem("c2g.settingsSec");
    if (savedSection && document.querySelector('#settingsChips [data-sec="' + savedSection + '"]')) section = savedSection;
    isAdvanced = localStorage.getItem("c2g.showAdvanced") === "1";
  } catch (e) { /* private window: the defaults */ }
  showSettingsSection(section);
  $("showAdvanced").addEventListener("change", settingsAdvancedChanged);
  setAdvanced(isAdvanced);
}
setupBasicAdvanced();
// the menu button names the page you're on: every tab click (a tap, a link to another tab, firstboot.js) passes through
document.querySelector(".nav-tabs").addEventListener("click", (e) => {
  const tab = e.target.closest(".nav-tab");
  if (tab) $("menuPage").textContent = tab.textContent;
});
// phone: the status dots open out to the full badges and back
$("statusBar").addEventListener("click", () => $("statusBar").classList.toggle("expanded"));

// ---------------------------------------------------------------------------
// status polling
// ---------------------------------------------------------------------------
const liveKeys = ["rpm", "speed", "coolant", "boost", "map", "iat", "oiltemp", "oilLevel", "pedal", "throttle", "batt", "lambda", "gear", "tcuSpeed", "gboxTemp", "ambient", "lights", "output", "me7_1", "me7_2", "me7_3", "me7_4"];
// Live data: what the glass shows - a mode screen's rows (K-line block, CAN message, OBD page ...), or for a gauge page
// its own values first, then everything else live from the same source
function renderLive(s) {
  const sc = s.screen || { kind: "page" }, grid = $("liveGrid"), num = (v) => fmt(v, Math.abs(v) < 10 ? 2 : (Math.abs(v) < 100 ? 1 : 0));
  let head = "", tiles = [];
  const sigTile = (k) => {
    const raw = s.signals[k];
    if (!raw) return null;
    const shown = unitShow(k, raw.v, raw.u); // 0.27.0: in the unit picked for its quantity, as on the glass
    const v = Object.assign({}, raw, { v: shown.v, u: shown.u });
    return [v.n, sigText(v, num), uShown(v.u)];
  };
  if (sc.kind === "mode" && sc.title) {
    head = "On the gauge: " + sc.title + (sc.header ? "  \u2022  " + sc.header : "") + (sc.sub ? "  \u2022  " + sc.sub : "") + (sc.status ? "  (" + sc.status + ")" : "");
    tiles = sc.bytes ? sc.bytes.map((b, i) => ["Byte " + i, b.toString(16).toUpperCase().padStart(2, "0"), String(b)]) : (sc.rows || []).map(r => {
      const m = /^(-?[\d.]+)\s*(.*)$/.exec(r[1]); return m ? [r[0], m[1], m[2]] : [r[0], r[1], ""]; });
  } else if (sc.kind === "page" && settingsCache) {
    const p = settingsCache.pages[sc.item] || {}, src = p.source || "can", srcName = (SOURCES.find(x => x[0] === src) || ["", "CAN"])[1];
    head = "On the gauge: gauge " + (sc.item + 1) + " " + (p.label || "") + "  \u2022  source " + srcName;
    const own = ["user" + (sc.item * 2 + 1), "user" + (sc.item * 2 + 2), p.signal, p.signal2].filter(k => k && s.signals[k]);
    const same = Object.keys(s.signals).filter(k => !isUser(k) && (src === "gauge" ? SPECIFICS_KEYS.includes(k) : src === "obd" ? OBD_PIDS.some(x => x[2] === k) : src === "adc" ? isAdc(k) : src === "ecu" ? ECU_KEYS.includes(k) : src === "can" ? !GAUGE_KEYS.includes(k) && !isAdc(k) : false));
    tiles = [...new Set([...own, ...same])].map(sigTile).filter(Boolean);
  } else {
    head = "On the gauge: " + ({ haldex: "OpenHaldex", dsg: "the Gearbox screen", menu: "the menu", settings: "Settings", boot: "the boot logo" }[sc.kind] || sc.kind);
    const keys = sc.kind === "haldex" ? Object.keys(s.signals).filter(k => k.startsWith("hdx")) : Object.keys(s.signals).filter(k => !isUser(k));
    tiles = keys.map(sigTile).filter(Boolean);
  }
  $("liveHead").textContent = titleCase(head);
  // nothing to show: say why when it's an adc2can page and the board is heard (it just has no valid values)
  let emptyText = "Nothing live from this source yet.";
  const isAdcPage = sc.kind === "page" && settingsCache && (settingsCache.pages[sc.item] || {}).source === "adc";
  if (isAdcPage && s.adc && s.adc.heard) {
    emptyText = adcNoValuesText(s.adc) + ".";
  }
  const html = tiles.length ? tiles.map(([n, v, u]) => `<div class="gauge"><div class="gauge-label">${esc(n)}</div><div class="gauge-value sig-live">${esc(v)}</div><div class="gauge-unit">${esc(u)}</div></div>`).join("")
    : '<div class="help">' + esc(emptyText) + '</div>';
  if (grid.innerHTML !== html) grid.innerHTML = html;
}
// System tab (was Diag) > Display: the panel bring-up, frames going to the glass, backlight, LVGL memory and the touch controller
function renderDiagDisplay(d) {
  const el = $("diagDisplay");
  if (!el || !d) return;
  const t = d.touch || {};
  let setup = "OK";
  if (!d.ready) {
    setup = "Failed at \"" + (d.stepName || d.step) + "\"";
  }
  let touch = "Not Found";
  if (t.found) {
    touch = "Found (0x" + Number(t.chipId).toString(16).toUpperCase().padStart(2, "0") + ")";
  }
  let lastPress = "None";
  if (t.ageMs >= 0) {
    lastPress = t.x + ", " + t.y + " (" + (t.ageMs < 60000 ? (t.ageMs / 1000).toFixed(1) + " s" : Math.round(t.ageMs / 60000) + " min") + " ago)";
  }
  diagRows("diagDisplay", [
    ["Display set-up", setup], ["Panel", d.panel || "--"],
    ["Frames to glass", (d.scanHz ?? "--") + "/s" + (d.asleep ? " (asleep)" : "")],
    ["Register writes refused", d.spiErrors ?? "--"], ["Backlight", (d.backlight ?? "--") + " %"],
    ["IO expander", d.expander ? "OK" : "Not Answering"],
    ["LVGL memory", d.lvglMemUsedPct !== undefined ? d.lvglMemUsedPct + " % used, " + d.lvglMemFragPct + " % fragmented" : "--"],
    ["Touch", touch], ["Touch reads / errors", (t.reads ?? 0) + " / " + (t.readErrors ?? 0)],
    ["Presses", (t.presses ?? 0) + (t.pressedNow ? " (touching)" : "")], ["Last press", lastPress],
  ]);
}

// System tab (was Diag): the gauge's state at a glance, from the status poll, in blocks (Gauge, Communication, Power & inputs)
const CAN_PROFILES = ["VAG PQ", "OBD-II", "OpenHaldex only", "VAG MQB", "MaxxECU", "VAG MLB"];
// one Diag block: a list of [name, value] rows into the grid with this id, Every Word Capitalised (0.17.1)
function diagRows(id, rows) {
  let html = "";
  for (let i = 0; i < rows.length; i++) {
    html += "<div><span>" + esc(titleCase(rows[i][0])) + "</span><span>" + esc(titleCase(rows[i][1])) + "</span></div>";
  }
  $(id).innerHTML = html;
}
// ---------------------------------------------------------------------------
// 0.27.0: a unit per quantity (src/can2gauge_Units.cpp) - the web UI shows values the way the glass does
// ---------------------------------------------------------------------------
const UNIT_ROW_KEYS = ["speed", "boost", "pressure", "temp", "mixture", "distance"];
// Units: the rows show only for Custom
function unitRowsShow() {
  const custom = Number($("units.quick").value) === 2;
  $("unitRows").style.display = custom ? "" : "none";
}
function unitRowsSetup() {
  unitRowsShow();
  // Metric / Imperial set every row (lambda stays lambda), before the card's autoSave sends them
  $("units.quick").addEventListener("change", () => {
    const quick = Number($("units.quick").value);
    if (quick !== 2) UNIT_ROW_KEYS.forEach(k => setVal("units." + k, k === "mixture" ? 0 : quick));
    unitRowsShow();
  });
}
// which row a value belongs to, by its stored unit (as unitRowOf on the gauge): "" = never converted
function unitRowOf(key, unit) {
  if (unit === "km/h") return "speed";
  if (unit === "mbar") return "boost";
  if (unit === "bar") return key === "boost" ? "boost" : "pressure";
  if (unit === "C") return "temp";
  if (unit === "km") return "distance";
  if (key === "lambda" || key === "lambdaTarget" || unit === "lambda") return "mixture";
  return "";
}
// a stored value as the glass shows it: { v, u } in the unit picked for its quantity
function unitShow(key, value, unit) {
  const d = (settingsCache && settingsCache.units) || {};
  const row = unitRowOf(key, unit), choice = row ? Number(d[row] || 0) : 0, v = Number(value);
  if (!row || !choice) return { v: v, u: unit };
  if (row === "speed") return { v: v * 0.621371, u: "mph" };
  if (row === "temp") return { v: v * 9 / 5 + 32, u: "F" };
  if (row === "distance") return { v: v * 0.621371, u: "mi" };
  if (row === "mixture") return { v: v * 14.7, u: "AFR" };
  const bar = unit === "mbar" ? v / 1000 : v; // boost / pressures: in bar first
  if (choice === 2) return { v: bar * 100, u: "kPa" };
  return { v: bar * 14.5038, u: "psi" };
}
// a catalogue range end ("min" / "max" of a signal, stored unit) in the shown unit, rounded like the gauge does
function unitShowRange(key, value, unit, span) {
  const shown = unitShow(key, value, unit).v;
  const shownSpan = Math.abs(unitShow(key, span, unit).v - unitShow(key, 0, unit).v);
  if (!(shownSpan > 0)) return shown;
  const step = Math.pow(10, Math.floor(Math.log10(shownSpan))) / 20;
  return Math.round(Math.round(shown / step) * step * 1000) / 1000;
}

// 0.25.4: the gauge's note of memory requests that failed (this boot, and the boot before a reset): "No", or
// "2x, last 3072 B in async_tcp at 412 s" - the place to look after a freeze
function ramFailText(r) {
  if (!r) return "--";
  const one = (x) => x.count + "x, last " + x.size + " B in " + x.task + " at " + x.atS + " s";
  let text = r.count ? one(r) : "No";
  if (r.lastBoot) text += " (before the reset: " + one(r.lastBoot) + ")";
  return text;
}
function renderDiag(s) {
  if (!$("diag-page").classList.contains("active")) return;
  const up = (sec) => { const d = Math.floor(sec / 86400), h = Math.floor(sec / 3600) % 24, m = Math.floor(sec / 60) % 60; return (d ? d + "d " : "") + h + "h " + String(m).padStart(2, "0") + "m"; };
  const sig = (k, d = 0) => s.signals && s.signals[k] ? fixedText(Number(s.signals[k].v), d) + " " + uShown(s.signals[k].u) : "--"; // 0.36.14: 13.0 V, not 13
  const pw = s.power || {}, io = s.io || {}, c = s.can || {}, kl = s.kline || {}, e = s.elm || {}, w = s.wifi || {}, ch = s.chain || {};
  // what the glass shows: "gauge 3" for a gauge page, else the screen's name
  let glass = "--";
  if (s.screen) {
    glass = s.screen.kind;
    if (s.screen.kind === "page") glass = "gauge " + (s.screen.item + 1);
  }  diagRows("diagGauge", [
    ["Firmware", s.version], ["Uptime", up(s.uptime || 0)], ["Last reset", s.reset || "--"],
    ["Frame rate", (s.fps ?? "--") + " fps"], ["Free heap", Math.round((s.heap || 0) / 1024) + " kB"], ["Free PSRAM", Math.round((s.psram || 0) / 1024) + " kB"],
    ["Lowest heap", Math.round((s.heapMin || 0) / 1024) + " kB"], ["RAM ran out", ramFailText(s.ramFail)], // 0.25.4
    ["Board temp", sig("boardTemp")], ["Glass shows", glass], ["Screensaver", s.saver ? "On" : "Off"], ["Alert", s.alert || "None"],
  ]);
  diagRows("diagComms", [
    ["CAN", c.up ? (c.listenOnly ? "Listen-only (standby)" : "Up") : "Down"], ["CAN profile", CAN_PROFILES[c.profile] || "--"],
    ["CAN rx", (c.rxPerSec ?? 0) + "/s (" + (c.rx ?? 0) + ")"], ["CAN tx / errors", (c.tx ?? 0) + " / " + (c.err ?? 0)],
    ["CAN bus-off", c.busOff ?? 0], ["Termination", c.termination === 2 ? (c.terminationOn ? "Auto - On (120R)" : "Auto - Off") : (c.termination ? "On (120R)" : "Off")],
    ["OpenHaldex", c.haldexLink ? "Linked (" + (c.haldexVia || "") + ")" : "Not heard"],
    ["usb2ip", ecuDiagText(s.ecu)],
    ["K-line", kl.state || "--"], ["K-line module", kl.ecu || "--"],
    ["OBD adapter", e.enabled ? elmStateText(e.state || "--") : "Off"], ["Gearbox", s.dsg ? s.dsg.state : "--"],
    ["Daisy chain", ch.enabled ? ch.role + " (node " + ch.node + ")" : "Off"],
    ["WiFi clients", w.apClients ?? "--"], ["Home WiFi", w.staConnected ? w.staIP : "Not joined"],
  ]);
  let outputText = "Off";
  if (io.outputFault) {
    outputText = "Fault";
  } else if (s.outputs && s.outputs[0]) {
    outputText = "On";
  }
  diagRows("diagPower", [
    ["Ignition", settingsCache.power && settingsCache.power.ignSource ? (pw.ignition ? "On" : "Off") : "Switched power"],
    ["Gauge power", (pw.state || "--") + (pw.sleepInS ? ", sleeps in " + pw.sleepInS + " s" : "")], ["Ignition source", pw.detail || "--"],
    ["Illumination", io.dashLights ? "On" : "Off"], ["Dimmed", io.night ? "Yes" : "No"],
    ["Output", outputText],
    ["Battery", sig("batt", 1)],
  ]);
  renderDiagDisplay(s.display);
  const pf = s.perf;
  if (!pf) return;
  renderRotation(pf.rotation);
  const ms = (us) => (us / 1000).toFixed(1) + " ms";
  diagRows("diagPerf", [
    ["Drawing", (pf.mode || "--") + (pf.simd ? ", SIMD" : "")], ["Frame rate", pf.fps + " fps"], ["Frame (avg / slowest)", ms(pf.frameAvgUs) + " / " + ms(pf.frameMaxUs)],
    ["LVGL busy", pf.busyPct + " %"], ["Copy to panel", ms(pf.flushUs) + "/s"],
    ["Redrawn", Math.round(pf.pixels / 230400 * 10) / 10 + " screens/s"], ["UI tick (avg / slowest)", ms(pf.tickAvgUs) + " / " + ms(pf.tickMaxUs)],
    ["Sprite frames", pf.decodes + "/s"], ["Sprite decode (avg / slowest)", pf.decodes ? ms(pf.decodeAvgUs) + " / " + ms(pf.decodeMaxUs) : "--"],
    ["Sprite unzip (avg)", pf.decodes && pf.unzipAvgUs !== undefined ? ms(pf.unzipAvgUs) : "--"],
  ]);
  const b = pf.bench;
  $("benchResult").textContent = b.running ? "Running... leave the glass alone." : b.ageS < 0 ? "Not run yet." :
    `${b.screen} (${pf.mode}${pf.simd ? ", SIMD" : ""}): ${b.fps} fps, frame ${ms(b.frameAvgUs)} avg / ${ms(b.frameMaxUs)} slowest, copy ${ms(b.flushUs)} per s, LVGL ${b.busyPct} % busy` +
    (b.decodeAvgUs ? `, sprite decode ${ms(b.decodeAvgUs)}` : "") + ` (${b.ageS} s ago)`;
}
// 0.25.1: Settings > Display > Screen Rotation - the card only shows when the firmware has the feature (perf.rotation
// is missing when it was built with SCREEN_ROTATION 0); the line under it says whether the turn is running
const ROTATION_TEXT = ["", "Turning the picture on the glass.", "There's no free memory for the turned picture right now: restart the gauge to apply it.", "Only works in Direct drawing mode (System > Drawing)."];
function renderRotation(state) {
  if (state === undefined) return;
  $("rotateCard").style.display = "";
  $("rotState").textContent = ROTATION_TEXT[state] || "";
}
function rotationFill() {
  const d = settingsCache.display;
  $("rotUpsideDown").checked = d.upsideDown === 1;
  $("rotTrim").value = d.rotateTrim || 0;
  $("rotTrimVal").textContent = fmt(d.rotateTrim || 0);
}
function rotationCollect() {
  return { display: { upsideDown: $("rotUpsideDown").checked ? 1 : 0, rotateTrim: Number($("rotTrim").value) } };
}
function rotationSetup() {
  rotationFill();
  $("rotTrim").addEventListener("input", () => { $("rotTrimVal").textContent = fmt(Number($("rotTrim").value)); });
  autoSave($("rotateCard"), rotationCollect, "rotation saved");
  $("rotTrimZero").addEventListener("click", () => {
    $("rotTrim").value = 0;
    $("rotTrimVal").textContent = "0";
    $("rotTrim").dispatchEvent(new Event("change", { bubbles: true })); // saves it, as moving the slider does
  });
}
// Diag: the usb2ip link in a few words
function ecuDiagText(e) {
  if (!e) return "--";
  if (!e.wireless) return "Off";
  if (e.heard) return "Heard (" + e.kind + ")";
  return "Not heard";
}
function badge(id, text, cls) { const e = $(id); e.textContent = text; e.className = "status-badge " + (cls || ""); }
// a top-bar badge from a Dashboard tile's state: "CAN: On" green, "CAN: Off" / "CAN: N/A" red.
// Hidden when that thing is switched off in Settings > Device > Menu and Tabs (the car doesn't have it).
function stateBadge(id, label, menuKey, s) {
  const e = $(id);
  if (!menuOn(menuShown, menuKey)) {
    e.style.display = "none";
    return;
  }
  e.style.display = "";
  const tile = tileData(menuKey, s);
  let cls = "error";
  if (tile[2] === "on") cls = "ok";
  badge(id, label + ": " + STATE_TEXT[tile[2]], cls);
}
const HALDEX_HOLD_MS = 5000; // the OpenHaldex card stays up this long after the last time it was heard (no flicker)
let haldexSeenMs = 0;        // when the OpenHaldex was last heard (browser clock), 0 = never
// 0.37.0: Enable Other Scanners - one switch on Settings > Device and on the Diagnostics tab; the gauge's status says
// whether it's on (not saved: off after a restart)
function otherScannersShow(isOn) {
  document.querySelectorAll(".other-scanners").forEach(function (box) {
    if (document.activeElement !== box) box.checked = !!isOn;
  });
}
function otherScannersChanged(event) {
  const isOn = event.target.checked;
  otherScannersShow(isOn);
  action("otherScanners", { on: Number(isOn) }); // 1 = on, 0 = off
}
document.querySelectorAll(".other-scanners").forEach(function (box) { box.addEventListener("change", otherScannersChanged); });

async function pollStatus() {
  try {
    const s = await api("/api/status");
    statusCache = s;
    otherScannersShow(s.otherScanners); // 0.37.0
    renderDiag(s);
    stateBadge("canStatus", "CAN", "can", s);
    stateBadge("klineStatus", "K-line", "kline", s);
    if (s.elm) stateBadge("bleStatus", "OBD", "ble", s);
    if (s.chain) stateBadge("chainStatus", "Chain", "chain", s);
    // OpenHaldex: the badge and the Dashboard card only while one is heard
    if (s.can.haldexLink) haldexSeenMs = Date.now();
    const haldexFound = haldexSeenMs > 0 && Date.now() - haldexSeenMs < HALDEX_HOLD_MS;
    const showHaldex = haldexFound && menuOn(menuShown, "haldex");
    $("haldexStatus").style.display = showHaldex ? "" : "none";
    $("haldexCard").style.display = showHaldex ? "" : "none";
    if (showHaldex) badge("haldexStatus", "OpenHaldex: On", "ok");
    badge("verStatus", "v" + s.version);
    // haldex
    const t = s.signals.hdxTarget, a = s.signals.hdxActual;
    $("lockTarget").textContent = t ? fmt(t.v, 0) : "--";
    $("lockActual").textContent = a ? fmt(a.v, 0) : "--";
    $("engagementFill").style.width = (a ? a.v : 0) + "%";
    document.querySelectorAll(".mode-btn").forEach(b => b.classList.toggle("active", s.signals.hdxMode && Number(b.dataset.mode) === Math.round(s.signals.hdxMode.v)));
    $("hdxDashInfo").textContent = s.can.haldexLink ? "Linked (" + s.can.haldexVia + ")" + (s.signals.hdxSpeed ? "  \u2022  Speed " + fmt(Number(s.signals.hdxSpeed.v), 0) + " km/h" : "") +
      (s.signals.hdxPedal ? "  \u2022  Pedal " + fmt(Number(s.signals.hdxPedal.v), 0) + " %" : "") + (s.signals.hdxClutchTemp ? "  \u2022  Clutch " + fmt(Number(s.signals.hdxClutchTemp.v), 0) + " \u00b0C" : "")
      : "No OpenHaldex heard (CAN 0x" + (settingsCache ? settingsCache.haldex.broadcastId.toString(16).toUpperCase() : "6B0") + " / WiFi).";
    renderTiles(s);
    renderLive(s);
    renderNav(s.nav); // 0.30.0
    $("alertNow").textContent = (s.alert ? "Alert: " + s.alert : "No Alert") + "  •  Gauge " + (s.page + 1) + "  •  " + s.fps + " fps";
    // outputs live dots
    s.outputs.forEach((on, i) => showOutputState(i, on, s.io.outputFault)); // green = on, red = off, flashing orange = fault
    s.triggers.forEach((on, i) => { const e = $("trigdot" + i); if (e) e.classList.toggle("on", !!on); });
    // CAN stats
    renderCanRelay(s.can.relay);
    $("canRx").textContent = s.can.rx; $("canRate").textContent = s.can.rxPerSec; $("canTx").textContent = s.can.tx; $("canErr").textContent = s.can.err; $("canBusOff").textContent = s.can.busOff;
    renderCanObd(s.can);
    // K-line
    $("klState").textContent = titleCase(s.kline.state); $("klSamples").textContent = s.kline.samples; $("klRate").textContent = s.kline.me7Rate;
    klLineState = s.kline.state; // 0.17.6: for the ME7 logger card's state line
    $("klLog").textContent = s.kline.me7Logging ? s.kline.me7LogFile : "Not Logging";
    // 0.17.3: the module's part number goes in the box under Status (it sat above an empty box before); the ME7 logger
    // puts its live values there too
    klEcuText = s.kline.ecu ? "Module: " + s.kline.ecu : "";
    if (!settingsCache || settingsCache.kline.protocol != 3) me7Text = "";
    renderKlBox();
    $("wifiInfo").textContent = "AP Clients: " + s.wifi.apClients + "  |  Home WiFi: " + (s.wifi.staConnected ? s.wifi.staIP : "Not Joined");
    $("deviceInfo").textContent = "Uptime " + Math.floor(s.uptime / 60) + " min  |  Heap " + Math.round(s.heap / 1024) + " kB  |  PSRAM " + Math.round(s.psram / 1024) + " kB  |  " + s.fps + " fps";
    if ($("can-page").classList.contains("active")) { pollCanRecent(); if (canIdsTick++ % 4 === 0) pollCanIds(); }
    if ($("ecu-page").classList.contains("active")) pollEcu();
    if ($("kline-page").classList.contains("active") && $("klLive").checked) pollKlBlock();
    if ($("kline-page").classList.contains("active") && settingsCache && settingsCache.kline.protocol == 3) pollMe7();
    if ($("dsg-page").classList.contains("active")) pollDsg();
    if ($("vdiag-page").classList.contains("active")) pollVdiag(); // also keeps the VAG diag session open
    if ($("oil-page").classList.contains("active")) pollOil();
    if ($("ble-page").classList.contains("active")) pollElm();
    if ($("adc-page").classList.contains("active")) pollAdc();
    if ($("chain-page").classList.contains("active")) pollChain();
    if ($("haldex-page").classList.contains("active")) pollHaldex();
  } catch (e) { $("canStatus").style.display = ""; badge("canStatus", "No connection", "error"); }
}

// ---------------------------------------------------------------------------
// DSG / Bluetooth OBD / chain tabs
// ---------------------------------------------------------------------------
function dsgBoxes() { // only the settings of the diagnostic read in use
  const src = Number($("dsg.source").value);
  $("dsgUdsBox").style.display = src === 1 || src === 3 ? "" : "none";
  $("dsgTpBox").style.display = src === 2 || src === 4 ? "" : "none";
}
function dsgMimic(sel, gear) { // as the gauge's DSG screen: P R N D S M strip, the big selector + gear, gears 1-7
  const known = sel && sel !== "-", drive = known && !["P", "R", "N"].includes(sel);
  $("dsgStrip").innerHTML = ["P", "R", "N", "D", "S", "M"].map(l => `<span class="${l === sel ? "on" : ""}">${l}</span>`).join("");
  $("dsgBig").textContent = known ? sel + (drive && gear > 0 ? gear : "") : gear > 0 ? String(gear) : "--";
  $("dsgGears").innerHTML = [1, 2, 3, 4, 5, 6, 7].map(n => `<span class="${(drive || !known) && gear === n ? "on" : ""}">${n}</span>`).join("");
}
async function pollDsg() {
  const r = await api("/api/dsg");
  dsgMimic(r.selector, r.gear);
  $("dsgSel").textContent = r.selector !== "-" ? r.selector + (r.gear > 0 && !["P", "R", "N"].includes(r.selector) ? r.gear : "") : (r.gear > 0 ? r.gear : "--");
  $("dsgTcu").textContent = r.tcuSpeed !== undefined ? r.tcuSpeed : "--";
  $("dsgVeh").textContent = r.speed !== undefined ? r.speed : "--";
  $("dsgTemp").textContent = r.gboxTemp !== undefined ? r.gboxTemp : "--";
  // 0.26.0: lock-up (with the raw number the gearbox sent) and the input (turbine) speed with the converter slip
  $("dsgLockup").textContent = r.lockup !== undefined ? ["Open", "Slipping", "Locked"][r.lockup] || "--" : "--";
  $("dsgLockupRaw").textContent = (r.lockupRaw >= 0 ? "raw " + r.lockupRaw : "converter / clutch") + (r.selectorRaw >= 0 ? ", selector raw " + r.selectorRaw : "") + (r.shifting ? " - shifting" : "");
  $("dsgInput").textContent = r.inputRpm !== undefined ? r.inputRpm : "--";
  $("dsgSlip").textContent = r.slip !== undefined ? "rpm, slip " + r.slip : "rpm (turbine)";
  $("dsgState").textContent = (r.selector === "-" && r.gear === undefined ? "No selector / gear on the bus yet - a DSG or automatic sends them (VAG PQ: 0x440 / 0x540, MQB: 0x0AD, MLB: 0x102); a manual car has none.  |  " : "") +
    "Diagnostics: " + r.state + (r.detail ? " - " + r.detail : "") + "  |  ok " + r.ok + ", errors " + r.err +
    (r.nrc ? ", last NRC 0x" + r.nrc.toString(16).toUpperCase() : "") + (r.speedSupported ? "" : "  |  speed read not supported") +
    (r.master ? "" : "  |  follower: the chain master reads the gearbox");
  const f = (arr, title) => arr.length ? title + "\n" + arr.map((x, i) => "field " + (i + 1) + ": formula 0x" + x.formula.toString(16).padStart(2, "0") +
    "  a " + x.a + "  b " + x.b + "  (a*256+b = " + (x.a * 256 + x.b) + ")  = " + x.value + " " + x.unit).join("\n") : "";
  $("dsgFields").textContent = [f(r.fields, "TP2.0 speed block (tx " + r.tp20Tx.toString(16) + " / rx " + r.tp20Rx.toString(16) + ")"), f(r.gearFields, "TP2.0 gear block")].filter(x => x).join("\n\n");
}

async function pollElm() {
  const r = await api("/api/elm");
  window.lastElmStatus = r; // 0.34.0: vdid.js picks the car group from the VIN's make
  $("elmState").textContent = titleCase(elmStateText(r.state) + (r.scanning ? " (scanning)" : ""));
  $("elmRate").textContent = r.reqPerSec;
  $("elmVolts").textContent = r.connected ? r.volts : "--";
  $("elmMac").textContent = r.mac || "None (First Found)";
  $("elmRestartRow").hidden = !r.needsRestart; // 0.24.22: switched on after boot and Bluetooth didn't fit
  $("elmVehicle").textContent = r.vin ? "Detected: " + r.make + " " + r.model + (r.modelCode ? " (" + r.modelCode + ")" : "") + (r.year ? ", " + r.year : "") +
    "  |  VIN " + r.vin + "  |  " + r.protocol : (r.connected ? r.make + "  |  " + r.protocol : "");
  $("elmAdapter").textContent = r.connected ? "Adapter " + r.adapter + (r.stn ? " (STN)" : "") + "  |  " + r.link : (r.lastError ? "Last error: " + r.lastError : "");
  $("elmPids").textContent = r.supported.length ? "Supported mode 01 PIDs: " + r.supported.map(p => p.toString(16).toUpperCase().padStart(2, "0")).join(" ") +
    "  |  " + r.requests + " requests, " + r.noData + " NO DATA, " + r.errors + " errors" : "";
  renderObdLive(r);
  const list = $("elmFound");
  list.innerHTML = r.found.map((d, i) => `<div class="found-row"><span class="${d.likely ? "likely" : ""}">${esc(d.name || "(no name)")} <span class="help">${esc(d.mac)}  ${d.rssi} dBm${d.likely ? "  OBD adapter" : ""}</span></span><button class="btn-secondary" data-i="${i}">${d.mac === r.mac ? "Saved" : "Pair"}</button></div>`).join("");
  list.querySelectorAll("button").forEach(b => b.addEventListener("click", () => { const d = r.found[Number(b.dataset.i)]; elmAction("pair", { mac: d.mac, type: d.type }); }));
  const t = $("elmTerm"); const atBottom = t.scrollTop + t.clientHeight >= t.scrollHeight - 4;
  t.textContent = r.terminal || "--"; if (atBottom) t.scrollTop = t.scrollHeight;
  const ts = r.test;
  if (ts && (ts.running || ts.runs)) {
    const s = $("elmTestState");
    s.innerHTML = ts.running ? "Running <b>" + esc(ts.kind) + "</b>: " + esc(ts.step) + " ..." :
      "Last test (" + esc(ts.kind) + "): " + esc(ts.result).split(" | ").join("<br>") +
      (ts.file ? '<br><a href="/download?path=' + encodeURIComponent(ts.file) + '">Download ' + esc(ts.file) + "</a>" : "");
  }
  document.querySelectorAll("[data-elmtest]").forEach(b => b.disabled = !r.connected || (ts && ts.running));
}
function elmBoxes() { // WiFi fields only for a WiFi adapter, the Bluetooth scan / forget only for Bluetooth
  const wifi = Number($("elm.transport").value) === 1;
  $("elmWifiBox").style.display = wifi ? "" : "none";
  $("elmScan").style.display = $("elmForget").style.display = $("elmFound").style.display = wifi ? "none" : "";
}
async function elmWifiScan() { // the shared WiFi scan (wifi_manager): {scanning:true} until {networks:[...]}
  const box = $("elmWifiFound");
  box.textContent = "Scanning (the gauge's WiFi may pause for a few seconds) ...";
  let r = {};
  for (let i = 0; i < 15; i++) {
    try { r = await api("/api/wifi/scan"); } catch (e) { r = {}; }
    if (r.networks) break;
    await new Promise(res => setTimeout(res, 1000));
  }
  const nets = (r.networks || []).filter(n => n.ssid);
  const obd = /OBD|ELM|V-?LINK|VGATE|ICAR|KONNWEI|VEEPEAK|WIFI_OBD/i;
  nets.sort((a, b) => (obd.test(b.ssid) - obd.test(a.ssid)) || (b.rssi - a.rssi));
  box.innerHTML = nets.length ? nets.map((n, i) => `<div class="found-row"><span class="${obd.test(n.ssid) ? "likely" : ""}">${esc(n.ssid)} <span class="help">${n.rssi} dBm${n.secure ? "  password" : "  open"}${obd.test(n.ssid) ? "  OBD adapter?" : ""}</span></span><button class="btn-secondary" data-i="${i}">Use</button></div>`).join("") : "No networks found.";
  box.querySelectorAll("button").forEach(b => b.addEventListener("click", () => {
    $("elm.wifiSsid").value = nets[Number(b.dataset.i)].ssid;
    $("elm.wifiSsid").dispatchEvent(new Event("change", { bubbles: true })); // autoSave picks it up
  }));
}
async function elmAction(a, extra = {}) {
  const r = await api("/api/elm/action", "POST", Object.assign({ action: a }, extra));
  toast(r.status || r.error || "ok");
  if (a === "pair" && !r.error) $("elm.enabled").value = "1"; // pairing turns auto-connect on (the gauge did the same)
}
// Bluetooth OBD tab > Live OBD data: every standard OBD value the car answers, whatever the glass shows (the Dashboard's
// Live data follows the screen, so a gauge with no OBD page - or no screen - showed none of them). r.pids comes from
// /api/elm: [pid, name, unit, value], and asking for it makes the gauge poll every PID the car has while the tab is open
function renderObdLive(r) {
  const grid = $("elmLive");
  if (!grid) return;
  const rows = r.pids || [];
  const html = rows.length ? rows.map(([p, n, u, v]) => { const x = Number(v);
    return `<div class="gauge"><div class="gauge-label">${esc(n)} <span class="help">${p.toString(16).toUpperCase().padStart(2, "0")}</span></div><div class="gauge-value sig-live">${esc(fmt(x, Math.abs(x) < 10 ? 2 : (Math.abs(x) < 100 ? 1 : 0)))}</div><div class="gauge-unit">${esc(uShown(u))}</div></div>`; }).join("")
    : '<div class="help">No OBD values yet - they appear once the adapter is connected and the car answers (ignition on). The rarer ones take a few seconds to come round.</div>';
  if (grid.innerHTML !== html) grid.innerHTML = html;
}

async function pollChain() {
  const r = await api("/api/chain");
  $("chainMe").textContent = r.enabled ? "This gauge: G" + r.node + (r.name ? " (" + r.name + ")" : "") + ", " + r.role + ", group " + r.group.toString(16).toUpperCase() +
    "  |  ESP-NOW " + (r.espNowUp ? "up" : "down") + ", CAN link " + (r.canHeard ? "heard" : "not heard") + "  |  tx CAN " + r.txCan + " / ESP-NOW " + r.txNow +
    ", rx CAN " + r.rxCan + " / ESP-NOW " + r.rxNow + (r.rejected ? ", rejected " + r.rejected : "") + (r.collisions ? ", id clashes " + r.collisions : "") : "Chain off - this gauge runs standalone.";
  $("chainNodes").textContent = r.nodes.length ? r.nodes.map(n => ("G" + n.node).padEnd(4) + String(n.name || "").padEnd(16) + n.role.padEnd(11) + ("v" + (n.version || n.fw)).padEnd(9) +
    n.via.padEnd(13) + (n.rssi ? n.rssi + " dBm  " : "") + n.age + " ms ago, sharing " + n.owned).join("\n") +
    (r.shared.length ? "\n\nReceived: " + r.shared.map(s => s.key + " (G" + s.from + ")").join(", ") : "") : (r.enabled ? "no other gauges heard yet" : "--");
  // the row wave: where this gauge stands among the gauges in the row it can hear right now
  let rowText = "Not in a Row";
  if (!r.enabled && r.rowPosition) {
    rowText = "Chain Off";
  } else if (r.rowPosition && r.rowInRow) {
    rowText = r.rowPlace + " of " + r.rowGauges;
  } else if (r.rowPosition) {
    rowText = "No Others Heard";
  }
  $("rowStatus").textContent = rowText;
  renderChainTable(r);
  showChainUpdate(r);
}

// 0.29.0: Daisy Chain > Gauge Numbers. One line per gauge (this one first among equals), sorted by its number, with a
// Role and a Number picker that set that gauge from here. A pick is held on screen for a few seconds, until that
// gauge's heartbeat (once a second) shows it took it, so the picker doesn't jump back meanwhile.
const chainPending = {}; // "node:kind" -> { value, until }
const CHAIN_ROLE_NAMES = ["Auto", "Master", "Follower"];

// "0.29.0" or later can be set from another gauge
function chainVersionOk(version) {
  const parts = String(version || "").split(".").map(Number);
  if (parts.length < 3 || isNaN(parts[0])) return false;
  return parts[0] * 10000 + parts[1] * 100 + parts[2] >= 2900;
}

function renderChainTable(r) {
  const box = $("chainTable");
  if (!r.enabled) {
    if (box.textContent !== "Chain off.") box.textContent = "Chain off.";
    chainTableHtml = ""; // drawn again in full when the chain comes back on
    return;
  }
  // every gauge: this one, then the others heard
  const gauges = [{ node: r.node, name: r.name, version: r.version, roleSetting: r.roleSetting, row: r.rowPosition, role: r.role, hasShutdown: r.hasShutdown, mac: r.mac, ignSource: r.ignSource, sharedSame: true, me: true }];
  r.nodes.forEach(n => gauges.push({ node: n.node, name: n.name, version: n.version, roleSetting: n.roleSetting, row: n.row, role: n.role, hasShutdown: n.hasShutdown, mac: n.mac, ignSource: n.ignSource, sharedSame: n.sharedSame, me: false }));
  // a pick still waiting for the gauge's heartbeat to catch up shows as picked
  const now = Date.now();
  gauges.forEach(g => {
    const rolePick = chainPending[g.node + ":role"];
    if (rolePick && rolePick.until > now && rolePick.value !== g.roleSetting) g.roleSetting = rolePick.value;
    const rowPick = chainPending[g.node + ":row"];
    if (rowPick && rowPick.until > now && rowPick.value !== g.row) g.row = rowPick.value;
  });
  // numbered gauges first, left to right, then the ones without a number
  gauges.sort((a, b) => {
    const rowA = a.row || 99;
    const rowB = b.row || 99;
    if (rowA !== rowB) return rowA - rowB;
    return a.node - b.node;
  });
  const html = gauges.map(g => {
    let who = "<b>G" + g.node + (g.name ? " " + esc(g.name) : "") + "</b>";
    if (g.me) who += " (This Gauge)";
    if (g.version) who += " &middot; v" + esc(g.version);
    who += " &middot; " + esc(g.role) + " now";
    // 0.30.4: its MAC (to tell the gauges apart) and its part in the ignition
    const extra = [];
    if (g.mac) extra.push("MAC " + esc(g.mac));
    if (g.ignSource === 1 || g.ignSource === 2) extra.push("has the ignition");
    if (g.ignSource === 3) extra.push("follows the ignition");
    if (extra.length) who += '<br><span class="chain-mac">' + extra.join(" &middot; ") + "</span>";
    const notes = [];
    const sameNumber = gauges.filter(o => o !== g && g.row && o.row === g.row).map(o => "G" + o.node);
    if (sameNumber.length) notes.push("same number as " + sameNumber.join(", "));
    if (g.row && !g.hasShutdown) notes.push("no shutdown sprite, so it sits out Play It Now");
    if (!g.me && g.sharedSame === false) notes.push("shared settings not the same yet (catching up)");
    const canSet = g.me || chainVersionOk(g.version);
    if (!canSet) notes.push("update it to 0.29.0 or later to set it from here");
    if (notes.length) who += '<br><span class="chain-warn">' + notes.join("; ") + "</span>";
    const disabled = canSet ? "" : " disabled";
    let roleSelect = '<select data-node="' + g.node + '" data-kind="role"' + disabled + ">";
    CHAIN_ROLE_NAMES.forEach((name, value) => {
      roleSelect += '<option value="' + value + '"' + (value === g.roleSetting ? " selected" : "") + ">" + name + "</option>";
    });
    roleSelect += "</select>";
    let rowSelect = '<select data-node="' + g.node + '" data-kind="row"' + disabled + ">";
    rowSelect += '<option value="0"' + (!g.row ? " selected" : "") + ">No Number</option>";
    for (let i = 1; i <= 15; i++) {
      rowSelect += '<option value="' + i + '"' + (i === g.row ? " selected" : "") + ">No. " + i + "</option>"; // short: fits a phone
    }
    rowSelect += "</select>";
    return '<div class="chain-gauge' + (g.me ? " me" : "") + '"><div class="chain-num">' + (g.row || "?") + '</div><div class="chain-who">' + who +
      '</div><div class="chain-pick">' + roleSelect + rowSelect + "</div></div>";
  }).join("");
  // 0.29.1: compared with the last html we wrote, not box.innerHTML - the browser rewrites that (&middot; -> ·,
  // selected -> selected=""), so it never matched, the table was rebuilt every poll and an open picker closed itself.
  // Also left alone while one of its pickers is open / focused.
  if (box.contains(document.activeElement) && document.activeElement.tagName === "SELECT") return;
  if (chainTableHtml === html) return;
  chainTableHtml = html;
  box.innerHTML = html;
}
let chainTableHtml = ""; // what renderChainTable last put in the table

// a Role or Number picker changed: send it to that gauge (or set this one), then look again shortly
async function chainPicked(e) {
  const pick = e.target;
  if (!pick.dataset || !pick.dataset.kind) return;
  pick.blur(); // let go of it, so the table can update again (it isn't redrawn while a picker has focus)
  const node = Number(pick.dataset.node);
  const value = Number(pick.value);
  chainPending[node + ":" + pick.dataset.kind] = { value: value, until: Date.now() + 4000 };
  if (pick.dataset.kind === "role") {
    await action("chainSetRole", { node: node, role: value });
  } else {
    await action("chainSetRow", { node: node, row: value });
  }
  setTimeout(pollChain, 1500); // the gauge's next heartbeat carries the change
}

// a firmware version as one number, so two can be compared ("0.39.5" -> 3905); 0 when unknown
function chainVersionNumber(version) {
  const parts = String(version || "").split(".").map(Number);
  if (parts.length < 3 || isNaN(parts[0])) return 0;
  return parts[0] * 10000 + parts[1] * 100 + parts[2];
}

// Update All Gauges - what this gauge is doing, then each other gauge: its version and how its update went. The gauge
// with the newest firmware sends the update, whichever gauge the button is pressed on (0.39.5).
function showChainUpdate(r) {
  const u = r.update || { results: [] };
  let state = "Idle";
  if (u.sending) state = "Updating Others" + (u.text ? " - " + u.text : "") + (u.percent ? " (" + u.percent + " %)" : "");
  else if (u.receiving) state = (u.text || "Being Updated") + (u.percent ? " (" + u.percent + " %)" : "");
  else if (u.text) state = u.text;
  $("chainUpdState").textContent = "v" + (r.version || "?") + "  |  " + state;
  if (!r.enabled) { $("chainUpdList").textContent = "Chain off."; return; }
  const results = {};
  (u.results || []).forEach(x => results[x.node] = x);
  const lines = r.nodes.map(n => {
    let note = "";
    if (results[n.node]) note = results[n.node].text;
    else if (!n.canUpdate) note = n.version ? "only heard on CAN" : "older than 0.25.0";
    else if (!n.sameBoard) note = "different board";
    else if (n.version === r.version) note = "same version";
    else if (chainVersionNumber(n.version) > chainVersionNumber(r.version)) note = "newer: it sends the update";
    else note = "can be updated";
    // two lines a gauge, so it fits a phone: who and which version, then how it's going
    return "G" + n.node + " " + String(n.name || "") + "  " + (n.version ? "v" + n.version : "before 0.25.0") + "\n   " + note;
  });
  $("chainUpdList").textContent = lines.length ? lines.join("\n") : "no other gauges heard yet";
}

// ---------------------------------------------------------------------------
// OEM oil temp / level (Oil tab, STYLE_OIL pages)
// ---------------------------------------------------------------------------
function oilStyleIndex() { return catalogue.styles.indexOf("Oil (PQ)"); } // 0.31.0: was "Oil (OEM)"
// 0.31.0: the order the Style list shows them in. The gauge numbers its styles in the order they were added (the
// number is what's saved), so the list is sorted here by name; a style not named here goes on the end.
const STYLE_ORDER = ["Needle", "Arc", "Bar", "Graph", "Single", "Dual", "Triple", "Quad", "Haldex", "Gear", "Oil (PQ)", "Custom"];
function styleOptions(selected) {
  const order = (name) => (STYLE_ORDER.includes(name) ? STYLE_ORDER.indexOf(name) : STYLE_ORDER.length);
  const list = catalogue.styles.map((name, number) => ({ name, number })).sort((a, b) => order(a.name) - order(b.name));
  return list.map((x) => `<option value="${x.number}" ${selected == x.number ? "selected" : ""}>${x.name}</option>`).join("");
}
async function pollOil() {
  const r = await api("/api/oil");
  const imp = settingsCache && settingsCache.units.quick == 1;
  $("oilTemp").textContent = r.temp !== undefined ? fmt(imp ? r.temp * 9 / 5 + 32 : Number(r.temp), 0) : "--";
  $("oilTempUnit").textContent = imp ? "\u00b0F" : "\u00b0C";
  const lv = $("oilLevel");
  lv.textContent = r.levelOk === undefined ? "--" : r.levelOk ? "OK" : "LOW";
  lv.style.color = r.levelOk === undefined ? "" : r.levelOk ? "var(--ok)" : "var(--danger, #ff4d5e)";
  $("oilLevelSrc").textContent = r.source == 1 ? (r.connected && r.levelField === 255 ? "Not in Block" : "Cluster Block") : "K-line Only";
  const k = r.kombi2;
  if (r.source == 1) {
    $("oilSrc").textContent = "K-line 0x" + settingsCache.oil.clusterAddr.toString(16).toUpperCase();
    $("oilConn").textContent = r.connecting ? "Connecting..." : r.connected ? "Connected" : (r.pageShowing ? "Waiting" : "Idle (Not Shown)");
    $("oilStatusLine").textContent = "Cluster session: " + (r.state || "not opened yet") + (r.clusterId ? "  |  " + r.clusterId : "") +
      (r.connected ? "  |  block " + r.group + ", temp field " + (r.tempField + 1) + ", level field " + (r.levelField === 255 ? "none" : r.levelField + 1) : "") +
      "  |  logins " + r.connects + ", reads " + r.reads + ", errors " + r.errors + "  |  K-line task: " + r.klineState;
  } else {
    $("oilSrc").textContent = "CAN Kombi_2";
    $("oilConn").textContent = !k.canEnabled ? "CAN Off" : !k.pqProfile ? "Needs VAG PQ" : !k.seen ? "No 0x420 Yet" : k.ageMs > 2000 ? "0x420 Lost" : k.err ? "Sensor Fault" : "Receiving";
    $("oilStatusLine").textContent = k.seen ? "0x420: " + k.count + " frames, last " + k.ageMs + " ms ago, byte 3 raw " + k.raw + " (= " + (k.raw - 60) + " C), fault bit " + (k.err ? 1 : 0) + (k.raw === 255 ? " - 0xFF = no value" : "")
      : "No mKombi_2 (0x420) heard yet." + (!k.canEnabled ? " CAN is off." : !k.pqProfile ? " Select the VAG PQ profile on the CAN tab." : "");
  }
  $("oilPageState").textContent = r.page < 0 ? "None" : r.pageShowing ? "On Screen" : "Enabled";
  $("oilPageNo").textContent = r.page < 0 ? "Add One Below" : "Gauge " + (r.pageCfg + 1) + " (Position " + (r.page + 1) + ")";
  $("oilShowPage").disabled = r.page < 0;
  $("oilRaw").textContent = r.source == 1 && r.fields.length ? "Block " + r.group + " raw fields\n" + r.fields.map((x, i) => "field " + (i + 1) + ": formula 0x" + x.formula.toString(16).padStart(2, "0") +
    "  a " + String(x.a).padStart(3) + "  b " + String(x.b).padStart(3) + "  = " + x.value + " " + x.unit + (i === r.tempField ? "   <- oil temp" : i === r.levelField ? "   <- oil level" : "")).join("\n") : "";
}

function renderOil() {
  const o = settingsCache.oil;
  fillSimple(o, "oil");
  const sel = $("oilGroupSel");
  sel.value = o.group === 0 ? "0" : (o.group === 3 && o.tempField === 2 && o.levelField === 1) ? "3" : (o.group === 50 && o.tempField === 2 && o.levelField === 255) ? "50" : "custom";
  const sync = () => {
    const kl = Number($("oil.source").value) === 1;
    $("oilKlBox").style.display = kl ? "" : "none";
    $("oilCanBox").style.display = kl ? "none" : "";
    const custom = sel.value === "custom", auto = sel.value === "0";
    ["oil.group", "oil.tempField", "oil.levelField"].forEach(id => $(id).disabled = !custom);
    if (auto) $("oil.group").value = 0;
    else if (sel.value === "3") { $("oil.group").value = 3; $("oil.tempField").value = 2; $("oil.levelField").value = 1; }
    else if (sel.value === "50") { $("oil.group").value = 50; $("oil.tempField").value = 2; $("oil.levelField").value = 255; }
    else if (Number($("oil.group").value) === 0) $("oil.group").value = 3;
    const c = settingsCache.can;
    $("oilCanCheck").innerHTML = !c.enabled ? "<b>CAN is currently off.</b>" : c.profile != 0 ? "<b>The CAN profile is not VAG PQ right now.</b>" : "";
  };
  $("oil.source").onchange = sync; sel.onchange = sync;
  sync();
}
function collectOil() { return collectSimple("oil", ["source", "clusterAddr", "group", "tempField", "levelField", "levelOkZero", "pollMs"]); }

// enable a page with the Oil style: an existing oil page, else the first disabled page, else (after asking) the last page
async function addOilPage() {
  const st = oilStyleIndex(), pages = collectPages();
  let i = pages.findIndex(p => p.style === st);
  if (i < 0) i = pages.findIndex(p => !p.enabled);
  if (i < 0) { i = pages.length - 1; if (!confirm("All 8 gauges are in use - replace gauge " + (i + 1) + " with the oil gauge?")) return; }
  const c = catalogue.signals.find(x => x.key === "oiltemp");
  const span = c.max - c.min; // 0.27.0: a new page's range is in the shown unit (C or F)
  pages[i] = Object.assign(pages[i], { enabled: 1, style: st, signal: "oiltemp", signal2: "none", label: pages[i].style === st && pages[i].label ? pages[i].label : "OIL",
    min: pages[i].style === st ? pages[i].min : unitShowRange("oiltemp", c.min, c.unit, span), max: pages[i].style === st ? pages[i].max : unitShowRange("oiltemp", c.max, c.unit, span),
    redline: pages[i].style === st ? pages[i].redline : unitShowRange("oiltemp", 130, c.unit, span), decimals: 0 });
  await saveSettings({ pages }, "oil gauge added (gauge " + (i + 1) + ")");
  renderPages();
}

// DSG read presets
const speedPresets = { f40d: { svc: 0x22, id: 0xF40D, pos: 0, len: 1, scale: 1, offset: 0 }, "010d": { svc: 0x01, id: 0x0D, pos: 0, len: 1, scale: 1, offset: 0 } };
const gearPresets = { off: { svc: 0, id: 0, pos: 0, fmt: 0, scale: 1 }, "01a4": { svc: 0x01, id: 0xA4, pos: 1, fmt: 1, scale: 1 } };
function presetOf(spec, presets) {
  for (const k in presets) { const p = presets[k]; if (p.svc === spec.svc && (p.svc === 0 || p.id === spec.id)) return k; }
  return "custom";
}
// 0.26.0: what picking a Gearbox type fills in (docs/8HP_research.md). DIDs from OBDb; the ZF codes still want checking
// on a car (Audi 210F gear coding, 7078 lock-up values; BMW has no gear DID, so the gear stays off)
const READ_OFF = { svc: 0, id: 0, pos: 0, len: 1, fmt: 0, scale: 1, offset: 0 };
const gearboxPresets = {
  0: { txId: 0x7E1, rxId: 0x7E9, txExtAddr: 0, rxExtAddr: 0, // DSG: as before
    speed: speedPresets.f40d, gear: gearPresets.off, temp: READ_OFF, lockup: READ_OFF, selector: READ_OFF },
  1: { txId: 0x7E1, rxId: 0x7E9, txExtAddr: 0, rxExtAddr: 0, // ZF 8HP in an Audi / VW: module 02
    speed: speedPresets.f40d,
    gear: { svc: 0x22, id: 0x210F, pos: 0, len: 1, fmt: 0, scale: 1, offset: 0 },     // current gear (raw on Q5 / A5)
    temp: { svc: 0x22, id: 0x2104, pos: 0, len: 1, fmt: 0, scale: 1, offset: -40 },  // ATF temperature, A - 40
    lockup: { svc: 0x22, id: 0x7078, pos: 0, len: 1, fmt: 0, scale: 1, offset: 0 },  // lock-up status (values unknown: watch the raw number)
    selector: READ_OFF },
  2: { txId: 0x6F1, rxId: 0x618, txExtAddr: 0x18, rxExtAddr: 0xF1, // ZF 8HP in a BMW: the EGS, extended addressing
    speed: READ_OFF, gear: gearPresets.off,
    temp: { svc: 0x22, id: 0xDA12, pos: 0, len: 2, fmt: 0, scale: 1, offset: -48 },  // ATF temperature, 16-bit - 48
    lockup: { svc: 0x22, id: 0xDA22, pos: 1, len: 1, fmt: 2, scale: 1, offset: 0 },  // converter lock-up, byte 1 (table)
    selector: { svc: 0x22, id: 0xDA2E, pos: 0, len: 2, fmt: 2, scale: 1, offset: 0 } }, // P R N D + Sport (table)
};
const DSG_READ_ROWS = ["temp", "lockup", "selector"]; // the three 0.26.0 read rows, each prefix the settings key too
function applyGearboxPreset(type) {
  const p = gearboxPresets[type];
  if (!p) return;
  fillSimple({ txId: p.txId, rxId: p.rxId, txExtAddr: p.txExtAddr, rxExtAddr: p.rxExtAddr }, "dsg");
  fillSimple(p.speed, "spd"); fillSimple(p.gear, "gear");
  DSG_READ_ROWS.forEach(k => fillSimple(p[k], k));
  $("spdPreset").value = presetOf(p.speed, speedPresets);
  $("gearPreset").value = presetOf(p.gear, gearPresets);
  if (type === 2 && Number($("dsg.speedPref").value) === 1) setVal("dsg.speedPref", 0); // BMW: no gearbox speed read
}
function renderDsgBoxes() { dsgBoxes(); $("dsg.source").addEventListener("change", dsgBoxes); }
function renderDsg() {
  const g = settingsCache.dsg;
  fillSimple(g, "dsg"); fillSimple(g.speed, "spd"); fillSimple(g.gear, "gear");
  DSG_READ_ROWS.forEach(k => { if (g[k]) fillSimple(g[k], k); });
  $("spdPreset").value = presetOf(g.speed, speedPresets);
  $("gearPreset").value = presetOf(g.gear, gearPresets);
  $("spdPreset").onchange = e => { const p = speedPresets[e.target.value]; if (p) fillSimple(p, "spd"); };
  $("gearPreset").onchange = e => { const p = gearPresets[e.target.value]; if (p) fillSimple(p, "gear"); };
  $("dsg.gearbox").onchange = e => applyGearboxPreset(Number(e.target.value)); // runs before the section's autoSave
}
function collectDsg() {
  const d = collectSimple("dsg", ["gearbox", "source", "speedPref", "pollMs", "txId", "rxId", "txExtAddr", "rxExtAddr", "session", "tp20Module", "tp20Group", "tp20SpeedField", "tp20SpeedDiv", "tp20GearGroup", "tp20GearField"]);
  d.speed = collectSimple("spd", ["svc", "id", "pos", "len", "scale", "offset"]);
  d.gear = collectSimple("gear", ["svc", "id", "pos", "fmt", "scale"]);
  d.gear.len = 1;
  d.temp = collectSimple("temp", ["svc", "id", "pos", "len", "scale", "offset"]);
  d.temp.fmt = 0;
  d.lockup = collectSimple("lockup", ["svc", "id", "pos", "len", "fmt", "scale"]);
  d.selector = collectSimple("selector", ["svc", "id", "pos", "len", "fmt", "scale"]);
  return d;
}

// ---------------------------------------------------------------------------
// VAG diag tab (0.13.0): one module, up to 12 UDS DIDs / KWP block fields (GET /api/vdiag, settings "vdiag")
// ---------------------------------------------------------------------------
const VD_KINDS = [[0, "Off"], [1, "UDS DID"], [2, "KWP block"], [3, "OBD PID"]]; // 0.37.6: OBD PID = mode 01 asked of this value's module
function vdHex(n, width) { return Number(n || 0).toString(16).toUpperCase().padStart(width, "0"); }
// 0.34.0: a CAN id as the tab shows it: 3 hex digits for 11-bit, 8 for 29-bit
function vdIdHex(n) { return vdHex(n, Number(n || 0) > 0x7FF ? 8 : 3); }
function renderVdiag() {
  const v = settingsCache.vdiag;
  if (!v) return; // firmware before 0.13.0
  fillSimple(v, "vdiag");
  // the module list: the K-line tab's list (same VAG diagnostic addresses) plus "Other"
  const sel = $("vdModSel");
  sel.innerHTML = KL_MODULES.map(([a, n]) => `<option value="${a}">${n}</option>`).join("") + `<option value="custom">Other address...</option>`;
  const known = KL_MODULES.some(([a]) => a === v.addr);
  if (known) sel.value = String(v.addr);
  else sel.value = "custom";
  $("vdAddrBox").style.display = known ? "none" : "";
  // 0.37.0: 32 values, each with its own module (data-tx ... carry it through a save). The rows in use show, plus one
  // empty row to add another.
  const firstEmpty = v.values.findIndex(x => !x.kind);
  $("vdList").innerHTML = v.values.map((x, i) => `<div class="row-inline vd-row" data-i="${i}" data-tx="${x.tx || 0}" data-rx="${x.rx || 0}" data-txext="${x.txExt || 0}" data-rxext="${x.rxExt || 0}" data-addr="${x.addr || 0}" data-proto="${x.proto || 0}" style="${x.kind || i === firstEmpty ? "" : "display:none"}">
      <div class="form-group vd-modbox" style="flex-basis:100%"><span class="help">${vdModuleText(x)}</span></div>
      <div class="form-group narrow"><label>${i + 1}</label><select class="vd-kind">${VD_KINDS.map(([k, n]) => `<option value="${k}" ${x.kind === k ? "selected" : ""}>${n}</option>`).join("")}</select></div>
      <div class="form-group"><label>Name</label><input type="text" class="vd-name" maxlength="15" value="${esc(x.name)}"></div>
      <div class="form-group narrow"><label class="vd-idlabel">${vdIdLabel(x.kind)}</label><input type="text" class="vd-id" value="${vdIdText(x)}"></div>
      <div class="form-group narrow vd-kwp"><label>Field</label><select class="vd-field">${[1, 2, 3, 4].map(f => `<option value="${f}" ${x.field === f ? "selected" : ""}>${f}</option>`).join("")}</select></div>
      <div class="form-group narrow vd-uds"><label>Bit</label><input type="number" class="vd-bit" min="0" max="2040" value="${x.bit || 0}"></div>
      <div class="form-group narrow vd-uds"><label>Bits</label><input type="number" class="vd-bits" min="1" max="32" value="${x.bits || 8}"></div>
      <div class="form-group narrow vd-uds"><label>Signed</label><select class="vd-signed"><option value="0" ${x.signed ? "" : "selected"}>No</option><option value="1" ${x.signed ? "selected" : ""}>Yes</option></select></div>
      <div class="form-group narrow vd-uds"><label>Byte Order</label><select class="vd-lsb"><option value="0" ${x.lsb ? "" : "selected"}>High first</option><option value="1" ${x.lsb ? "selected" : ""}>Low first</option></select></div>
      <div class="form-group narrow vd-uds"><label>Scale</label><input type="number" class="vd-scale" step="any" value="${x.scale}"></div>
      <div class="form-group narrow vd-uds"><label>Offset</label><input type="number" class="vd-offset" step="any" value="${x.offset}"></div>
      <div class="form-group narrow"><label>Unit</label><input type="text" class="vd-unit" maxlength="7" value="${esc(x.unit)}"></div>
    </div>`).join("");
  document.querySelectorAll(".vd-row").forEach(vdRowBoxes);
}
// 0.37.0: which module a value comes from, as its row says it
function vdModuleText(x) {
  if (x.tx) return "Module " + vdIdHex(x.tx) + (x.rx ? " / " + vdIdHex(x.rx) : "") + (x.txExt ? "  ext " + vdHex(x.txExt, 2) : "");
  if (x.addr) return "Module at address " + vdHex(x.addr, 2) + (x.kind === 2 ? " (TP2.0)" : "");
  return "The module above";
}
// 0.37.6: the id box's label and text by kind: a KWP block number, an OBD PID or a UDS DID (both hex)
function vdIdLabel(kind) {
  if (kind === 2) return "Block";
  if (kind === 3) return "PID (hex)";
  return "DID (hex)";
}
function vdIdText(x) {
  if (x.kind === 2) return x.id;
  if (x.kind === 3) return vdHex(x.id, 2);
  return vdHex(x.id, 4);
}
function vdRowBoxes(row) { // only the boxes the value's kind uses
  const kind = Number(row.querySelector(".vd-kind").value);
  row.querySelectorAll(".vd-uds").forEach(e => { e.style.display = kind === 1 ? "" : "none"; });
  row.querySelectorAll(".vd-kwp").forEach(e => { e.style.display = kind === 2 ? "" : "none"; });
  row.querySelector(".vd-idlabel").textContent = vdIdLabel(kind);
}
function collectVdiag() {
  const d = collectSimple("vdiag", ["run", "addr", "protocol", "turbo", "groupSize", "session", "pollMs", "txId", "rxId", "tp20Module", "link", "txExt", "rxExt"]);
  d.values = [...document.querySelectorAll(".vd-row")].map(row => {
    const kind = Number(row.querySelector(".vd-kind").value);
    const idText = row.querySelector(".vd-id").value.trim();
    let id = 0;
    if (kind === 2) id = parseInt(idText, 10) || 0; // a block number (an OBD PID, kind 3, is hex like a DID)
    else id = parseInt(idText, 16) || 0;             // a DID, in hex
    return { kind, id, field: Number(row.querySelector(".vd-field").value), bit: Number(row.querySelector(".vd-bit").value) || 0,
      bits: Number(row.querySelector(".vd-bits").value) || 8, signed: Number(row.querySelector(".vd-signed").value),
      lsb: Number(row.querySelector(".vd-lsb").value),
      scale: Number(row.querySelector(".vd-scale").value) || 1, offset: Number(row.querySelector(".vd-offset").value) || 0,
      name: row.querySelector(".vd-name").value, unit: row.querySelector(".vd-unit").value,
      // 0.37.0: the value's own module, kept as it was
      tx: Number(row.dataset.tx) || 0, rx: Number(row.dataset.rx) || 0, txExt: Number(row.dataset.txext) || 0,
      rxExt: Number(row.dataset.rxext) || 0, addr: Number(row.dataset.addr) || 0, proto: Number(row.dataset.proto) || 0 };
  });
  return d;
}
async function pollVdiag() {
  const r = await api("/api/vdiag");
  const proto = ["", "UDS", "KWP2000 / TP2.0"][r.protocol] || "";
  $("vdState").textContent = titleCase(r.state + (r.detail ? " - " + r.detail : "") + (r.master ? "" : " (follower)"));
  const ident = [];
  if (proto) ident.push(proto + "  tx " + vdIdHex(r.txId) + " / rx " + vdIdHex(r.rxId) + (r.viaAdapter ? "  via the OBD adapter" : "") + (r.protocol === 1 && settingsCache.vdiag.turbo ? "  group " + r.groupSize + (r.turboGap ? ", 1 ms gap" : "") : ""));
  if (r.partNumber) ident.push("Part number  " + r.partNumber + (r.swVersion ? "  sw " + r.swVersion : ""));
  if (r.component) ident.push("Component    " + r.component);
  if (r.asamDataset) ident.push("ASAM dataset " + r.asamDataset + " " + r.asamVersion);
  ident.push(r.rate + " answers/s   ok " + r.ok + "   errors " + r.err);
  $("vdIdent").textContent = ident.join("\n");
  const cfg = settingsCache.vdiag.values;
  $("vdLive").textContent = r.values.map((x, i) => {
    const c = cfg[i];
    if (!c || !c.kind) return null;
    let where = "DID " + vdHex(c.id, 4);
    if (c.kind === 2) where = "block " + c.id + " field " + c.field;
    if (c.kind === 3) where = "PID " + vdHex(c.id, 2); // 0.37.6
    let shown = "--";
    if (x.valid) shown = x.value + " " + uShown(x.unit);
    if (!x.supported) shown = "refused (NRC " + vdHex(x.nrc, 2) + ")";
    else if (x.nrc === 0x22) shown += "  (NRC 22: not available now)";
    return String(i + 1).padStart(2) + " " + (c.name || "").padEnd(16) + where.padEnd(20) + shown.padEnd(22) + (x.raw ? "raw " + x.raw : "");
  }).filter(x => x).join("\n") || "No values set up yet - add them below.";
  const p = r.probe;
  if (p && p.text) $("vdProbeOut").textContent = "DID " + vdHex(p.id, 4) + ": " + p.text + (p.hex ? "\n" + p.hex : "");
}

// one switch per thing the car may have: its glass menu entry, its tab and its Dashboard tile, in tab order (the glass
// menu goes in the same order). Output / Illumination has no tab of its own - its settings are on Settings.
const menuItems = [["gauges", "Gauges"], ["nav", "Navigation"], ["can", "CAN Bus"], ["haldex", "OpenHaldex"], ["adc", "ADC"], ["ecu", "usb2ip"], ["kline", "K-line"],
  ["oil", "Oil"], ["dsg", "Gearbox"], ["ble", "Bluetooth OBD"], ["triggers", "Triggers"], ["chain", "Daisy Chain"], ["output", "Output / Illumination"],
  ["drag", "Drag Timer"]]; // 0.38.0
function menuOn(menu, key) { // a switch the firmware doesn't know yet counts as on (older firmware, the sim)
  if (!menu || menu[key] === undefined) return true;
  return !!menu[key];
}
function renderMenuToggles() {
  $("menuToggles").innerHTML = menuItems.map(([k, n]) => `<label class="toggle"><input type="checkbox" id="menu_${k}" ${menuOn(settingsCache.menu, k) ? "checked" : ""}><span class="toggle-slider"></span><span class="toggle-label">${n}</span></label>`).join("");
}
function collectMenu() { const m = {}; menuItems.forEach(([k]) => m[k] = $("menu_" + k).checked ? 1 : 0); return m; }
// each on-screen menu entry brings its tab and a Dashboard tile; off removes them. Gauges keeps its tab.
// (The OpenHaldex card shows only while an OpenHaldex is heard - pollStatus.)
const TILE_KEYS = ["can", "kline", "output", "dsg", "adc", "ble", "chain"];
let menuShown = {};
function applyMenu(menu) {
  menuShown = menu || {};
  document.querySelectorAll(".nav-tab[data-menu]").forEach(t => {
    const on = menuOn(menuShown, t.dataset.menu);
    t.style.display = on ? "" : "none";
    if (!on && t.classList.contains("active")) document.querySelector('.nav-tab[data-page="dashboard"]').click();
  });
  $("modeTilesCard").style.display = TILE_KEYS.some(k => menuShown[k]) ? "" : "none";
  if (statusCache) renderTiles(statusCache);
}
// every tile and badge says one of three things: On (working, green dot), Off (switched off, red dot),
// N/A (switched on but nothing heard / not connected, red dot). The detail goes on the tile's second line.
const STATE_TEXT = { on: "On", off: "Off", na: "N/A", fault: "Fault" }; // fault: the output's driver shut it down (flashing dot)
// a Dashboard tile: [title, tab, state ("on" / "off" / "na"), second line]
function tileData(k, s) {
  const sig = (key, d = 0) => s.signals[key] ? fixedText(Number(s.signals[key].v), d) : null; // 0.36.14: keeps the zeros
  if (k === "can") {
    const c = settingsCache.can;
    let relayPerSec = 0; // frames/s an OpenHaldex / adc2can relays over WiFi
    if (s.can.relay && s.can.relay.on) relayPerSec = s.can.relay.perSec;
    const wiredPerSec = s.can.up ? s.can.rxPerSec : 0; // frames/s on the gauge's own CAN wire
    let state = "na";                                 // switched on, but no frames heard
    if (!c.enabled) state = "off";
    else if (wiredPerSec > 0 || relayPerSec > 0) state = "on";
    let detail = c.kbps + " kbps \u2022 " + (CAN_PROFILES[c.profile] || "");
    if (wiredPerSec > 0) detail += " \u2022 " + wiredPerSec + " frames/s";
    else if (relayPerSec > 0) detail += " \u2022 WiFi relay " + relayPerSec + " frames/s";
    else if (c.enabled) detail += " \u2022 nothing heard";
    return ["CAN Bus", "can", state, titleCase(detail)];
  }
  if (k === "kline") {
    let state = "na";
    if (!settingsCache.kline.protocol) state = "off"; // protocol 0 = off
    else if (s.kline.up) state = "on";
    let detail = s.kline.state || "";
    if (s.kline.ecu) detail += " \u2022 " + s.kline.ecu;
    return ["K-line", "kline", state, titleCase(detail)];
  }
  if (k === "output") {
    let state = "off";
    if (s.io.outputFault) state = "fault";
    else if (s.outputs[0]) state = "on";
    const detail = "Illumination " + (s.io.dashLights ? "on" : "off") + (s.io.night ? " (dimmed)" : "");
    return ["Output / Illumination", "settings", state, titleCase(detail)];
  }
  if (k === "dsg") {
    const gear = sig("gear");
    let speed = sig("tcuSpeed");
    if (speed === null) speed = sig("speed");
    let state = "na";
    let detail = "No selector";
    if (s.dsg.selector !== "-") {
      state = "on";
      detail = s.dsg.selector;
      if (gear && gear !== "0") detail += " gear " + gear;
      if (speed !== null) detail += " \u2022 " + speed + " km/h";
    }
    return ["Gearbox", "dsg", state, titleCase(detail)];
  }
  if (k === "adc") {
    const sensors = Object.keys(s.signals).filter(isAdc).map(x => s.signals[x]);
    let state = "na";
    let detail = "Not Heard";
    if (sensors.length) {
      state = "on";
      detail = sensors.length + (sensors.length > 1 ? " Sensors" : " Sensor") + ": " + // the sensor names stay as typed
        sensors.slice(0, 2).map(x => x.n + " " + sigText(x, (v) => fmt(v, 1)) + " " + uShown(x.u)).join(" \u2022 ");
    } else if (s.adc && s.adc.heard) {
      // the adc2can is there, it just has no valid sensor values to send
      state = "on";
      detail = s.adc.no12v ? "No Valid Sensors (No 12 V)" : "No Valid Sensors";
    }
    return ["ADC", "adc", state, detail];
  }
  if (k === "ble") {
    let state = "na";
    if (!s.elm.enabled && !s.elm.connected) state = "off";
    else if (s.elm.connected) state = "on";
    let detail = elmStateText(s.elm.state);
    if (state === "off") detail = "Auto-connect off";
    return ["OBD Adapter", "ble", state, titleCase(detail)];
  }
  if (k === "chain") {
    let state = "off";
    let detail = "Standalone";
    if (s.chain.enabled) {
      state = "on";
      detail = "G" + s.chain.node + " " + s.chain.role;
    }
    return ["Daisy Chain", "chain", state, titleCase(detail)];
  }
  return null;
}
// an adc2can that is heard but sends no valid values: say so, and why when it told us (no 12 V = USB power only)
function adcNoValuesText(adc) {
  if (adc.no12v) {
    return "adc2can heard, no valid sensors: no 12 V (USB power only), so the sensor 5 V supply is off";
  }
  return "adc2can heard, but none of its sensors has a valid value";
}
function renderTiles(s) {
  if (!settingsCache) return;
  let html = "";
  for (let i = 0; i < TILE_KEYS.length; i++) {
    if (!menuShown[TILE_KEYS[i]]) continue; // not on this car's menu
    const tile = tileData(TILE_KEYS[i], s);
    if (!tile) continue;
    let dotClass = "off"; // red: off or nothing heard
    if (tile[2] === "on") dotClass = "on"; // green: working
    if (tile[2] === "fault") dotClass = "fault"; // flashing orange: the output has shut itself down
    html += `<div class="mode-tile" data-tab="${tile[1]}"><h3><span class="dot-on ${dotClass}"></span>${esc(tile[0])}</h3>` +
      `<div class="v">${STATE_TEXT[tile[2]]}</div><div class="s">${esc(tile[3])}</div></div>`;
  }
  if ($("modeTiles").innerHTML !== html) $("modeTiles").innerHTML = html;
}

// ---------------------------------------------------------------------------
// Haldex tab: OpenHaldex over CAN / WiFi (GET /api/haldex), commands over WiFi
// ---------------------------------------------------------------------------
const LL_PHASES = ["Idle", "Initial Sweep (all blocks on)", "BPK Adjust (torque ceiling)", "Sweeping Blocks", "Confirmation learn on final set", "Complete", "Cancelled", "Failed"];
const HDX_CMDS = { 1: "Mode", 2: "Generation", 3: "Start learn", 4: "Cancel learn", 5: "Clear learned table", 6: "Start long learn", 7: "Cancel long learn", 8: "Controller" };
function hdxCmd(cmd, arg = 0) { return action("haldexCmd", { cmd, arg }); }
async function pollHaldex() {
  const r = await api("/api/haldex"), o = r.ohx, s = statusCache, imp = settingsCache.units.quick == 1;
  const live = !!(o && r.heard); // WiFi state fresh: it has everything; otherwise what CAN (0x6B0) gives
  // link line
  let link = r.via ? "Linked over <b>" + esc(r.via) + "</b>" : "<b>Not linked</b>";
  if (o) link += " \u2022 " + esc(o.name) + " \u2022 firmware " + esc(o.fw) + " \u2022 " + esc(o.generationName) + " \u2022 " + o.rssi + " dBm" +
    (r.heard ? "" : " \u2022 last heard over WiFi " + Math.round(r.ageMs / 1000) + " s ago");
  else link += r.wireless ? " \u2022 no OpenHaldex heard over WiFi yet" : " \u2022 WiFi link off (CAN only)";
  $("hdxLink").innerHTML = link;
  // lock + values
  const sig = (k) => s && s.signals[k] ? Number(s.signals[k].v) : undefined;
  const tgt = live ? o.lockTarget : sig("hdxTarget"), act = live ? o.lockActual : sig("hdxActual");
  $("hdxTarget").textContent = tgt !== undefined ? fmt(tgt, 0) : "--";
  $("hdxActual").textContent = act !== undefined ? fmt(act, 0) : "--";
  $("hdxFill").style.width = (act || 0) + "%";
  const spd = live ? o.speed : sig("hdxSpeed"), thr = live ? o.throttle : sig("hdxPedal");
  const T = (c) => c === undefined ? undefined : imp ? c * 9 / 5 + 32 : c, tu = imp ? "\u00b0F" : "\u00b0C";
  const tiles = [
    ["Speed", spd === undefined ? undefined : imp ? spd * 0.621371 : spd, imp ? "mph" : "km/h", 0],
    ["Pedal", thr, "%", 0],
    ["RPM", live ? o.rpm : undefined, "rpm", 0],
    ["Boost", live && o.boost !== undefined ? (imp ? o.boost / 1000 * 14.5038 : o.boost / 1000) : undefined, imp ? "psi" : "bar", imp ? 1 : 2],
    ["Steering", live ? o.steering : undefined, "\u00b0", 0],
    ["Clutch temp", T(live ? o.clutchTemp : undefined), tu, 0],
    ["Haldex oil", T(live ? o.oilTemp : undefined), tu, 0],
    ["Module temp", T(live ? o.moduleTemp : undefined), tu, 0],
    ["Supply", live ? o.supply : undefined, "V", 1],
  ].filter(t => t[1] !== undefined || ["Speed", "Pedal", "RPM", "Boost"].includes(t[0]));
  const th = tiles.map(([n, v, u, d]) => `<div class="gauge"><div class="gauge-label">${n}</div><div class="gauge-value sig-live">${v === undefined || v === null ? "--" : fmt(v, d)}</div><div class="gauge-unit">${u}</div></div>`).join("");
  if ($("hdxTiles").innerHTML !== th) $("hdxTiles").innerHTML = th;
  // state chips (WiFi only - the CAN broadcast carries just the lock, mode, speed and pedal)
  let chips = "";
  if (o) {
    const chip = (on, text, cls = "on") => on ? `<span class="hdx-chip ${cls}">${text}</span>` : "";
    const force = [o.tcForce && "traction control off", o.hazard && "hazards", o.extButton && "external button"].filter(Boolean).join(", ");
    chips = chip(true, o.chassisCan ? "Chassis CAN OK" : "No chassis CAN", o.chassisCan ? "on" : "bad") + chip(true, o.haldexCan ? "Haldex CAN OK" : "No Haldex CAN", o.haldexCan ? "on" : "bad") +
      chip(o.standalone, "Standalone") + chip(o.controllerOff, "Controller disabled", "warn") + chip(o.busFailure, "Bus failure", "bad") +
      chip(o.override, "Force mode" + (force ? ": " + force : ""), "warn") + chip(o.brake, "Brake") + chip(o.handbrake, "Handbrake", "warn") +
      chip(o.tempProtection, "Temperature protection", "bad") + chip(o.couplingOpen, "Coupling open", "warn") + chip(o.speedLimit, "Speed limit", "warn") +
      chip(o.asrOff, "ASR off", "warn") + chip(o.steerScale, "Steering scaling") + chip(o.learned, "Learned table") + chip(o.fixHunting, "Fix hunting") +
      chip(o.canBroadcast, "CAN broadcast") + chip(o.analyzer, "Analyzer mode", "warn") +
      `<span class="hdx-chip">release under ${o.disableThrottle}% pedal, ${o.disengageUnder}-${o.disengageAbove} km/h</span>`;
  }
  $("hdxFlags").innerHTML = chips;
  $("hdxFlags").classList.toggle("stale", !!o && !r.heard);
  const slip = o && o.slip ? o.slip : [];
  $("hdxSlip").textContent = live && slip.some(x => x !== null) ? "Wheel slip  FL " + slip.map((x, i) => ["", "FR ", "RL ", "RR "][i] + (x === null ? "--" : x + "%")).join("  ") : "";
  // controls: only over WiFi
  const can = live && o.control;
  ["hdxGen", "hdxCtrl", "hdxLearnStart", "hdxLearnCancel", "hdxLearnClear", "hdxLongStart", "hdxLongCancel", "hdxLongAll"].forEach(id => $(id).disabled = !can);
  $("hdxCtlNote").textContent = !r.wireless ? "Turn on the WiFi link below to use these." : !o ? "Waiting for the OpenHaldex over WiFi (it needs firmware with \"Gauges over WiFi\")." :
    !r.heard ? "The OpenHaldex isn't heard over WiFi right now." : !o.control ? "\"Allow Gauges to Change Settings\" is off on the OpenHaldex." :
    o.keyRequired && !r.keySet ? "The OpenHaldex's WiFi has a password - enter it below or it will refuse these." : "";
  if (live) {
    const gen = $("hdxGen");
    if (document.activeElement !== gen && !(r.cmd.state === "sending" && r.cmd.cmd === 2)) {
      if (![...gen.options].some(x => Number(x.value) === o.generation)) gen.appendChild(new Option(o.generationName, o.generation));
      gen.value = o.generation;
    }
    if (document.activeElement !== $("hdxCtrl") && !(r.cmd.state === "sending" && r.cmd.cmd === 8)) $("hdxCtrl").value = o.controllerOff ? 0 : 1;
    $("hdxLearnFill").style.width = (o.learn.active ? o.learn.step : 0) + "%";
    $("hdxLearnState").textContent = o.learn.active ? "Learning " + o.learn.step + " / 100" : o.learned ? "Learned Table" : "Formula (Not Learned)";
    const ll = o.longLearn;
    $("hdxLongState").textContent = titleCase(ll.active ? LL_PHASES[ll.phase] + (ll.total ? " - sweep " + ll.sweep + " of ~" + ll.total : "") : ll.phase >= 5 ? "Last run: " + LL_PHASES[ll.phase] : "Idle");
  }
  const c = r.cmd;
  $("hdxCmdState").textContent = c.state === "none" ? "" : (HDX_CMDS[c.cmd] || "Command") + ": " + (c.state === "sending" ? "sending ..." : c.state === "done" ? c.text : c.text);
  $("hdxCmdState").style.color = c.state === "done" && !c.ok || c.state === "no answer" ? "var(--danger, #ff4d5e)" : "";
  // which OpenHaldex
  const chWarn = o && o.channel && r.myChannel && o.channel !== r.myChannel ? "  -  it is on WiFi channel " + o.channel + ", this gauge on " + r.myChannel + ": put both on the same one" : "";
  $("hdxFollow").textContent = !r.wireless ? "WiFi link off." : r.mac ? "Following " + (o ? o.name + " " : "") + "(" + r.mac + ")" + chWarn : "Not following an OpenHaldex yet - the first one heard is followed.";
  $("hdxForget").style.display = r.mac ? "" : "none";
  const oh = r.others.map(x => `<div class="found-row"><span>${esc(x.name || "(no name)")} <span class="help">${esc(x.mac)}  ${x.rssi} dBm</span></span><button class="btn-secondary" data-mac="${esc(x.mac)}">Follow</button></div>`).join("");
  if ($("hdxOthers").innerHTML !== oh) $("hdxOthers").innerHTML = oh;
}
// CAN frames relayed over WiFi by an OpenHaldex / adc2can (GET /api/status can.relay)
function renderCanRelay(r) {
  const el = $("canRelayInfo");
  if (!el || !r) return;
  const snd = r.senders.map(x => x.kind + " " + x.mac.slice(-5) + ": " + x.perSec + " frames/s, " + x.rssi + " dBm" +
    (x.airLost ? ", " + x.airLost + " packets lost in the air" : "") + (x.srcLost ? ", " + x.srcLost + " frames it could not send" : ""));
  el.textContent = !snd.length ? "No relay heard." : (r.on ? "Using " + r.perSec + " frames/s" + (r.ignored ? " (" + r.ignored + " skipped: heard on the wire or from another relay)" : "") : "Ignoring") +
    "  |  " + snd.join("  |  ") + (r.dropped ? "  |  " + r.dropped + " dropped on this gauge" : "");
}
// OBD-II polling on the wire (GET /api/status can.obd): only shown with the "OBD-II PIDs" profile
function renderCanObd(c) {
  const box = $("canObdBox");
  if (!box || !c.obd) return;
  const obdOn = c.profile === 1 || (settingsCache && settingsCache.obd.overCan === 1); // the OBD-II profile, or OBD-II next to it
  box.style.display = obdOn ? "" : "none";
  const o = c.obd;
  $("canObdRate").textContent = o.pidsPerSec;
  let info = "/s";
  if (!o.ecuFound) {
    info = "/s, looking for the ECU";
  } else {
    info = "/s from 0x" + o.ecuId.toString(16).toUpperCase() + ", " + o.answersPerSec + " answers/s, " + o.pidsPerRequest + " PID" + (o.pidsPerRequest === 1 ? "" : "s") + " each";
  }
  $("canObdInfo").textContent = info;
  // the OBD-II over CAN card
  if (!obdOn) { $("obdCanEcu").textContent = "Off"; $("obdCanRate").textContent = "--"; }
  else if (!o.ecuFound) { $("obdCanEcu").textContent = "Searching (0x7DF)"; $("obdCanRate").textContent = "0"; }
  else { $("obdCanEcu").textContent = "0x" + o.ecuId.toString(16).toUpperCase(); $("obdCanRate").textContent = o.pidsPerSec; }
  // every ECU that answered "0100" (bit n = answer id 0x7E8 + n)
  const heard = [];
  for (let n = 0; n < 8; n++) if ((o.answered || 0) & (1 << n)) heard.push("0x" + (0x7E8 + n).toString(16).toUpperCase() + (n === 0 ? " engine" : n === 1 ? " gearbox" : ""));
  $("obdCanHeard").textContent = heard.length ? "ECUs that answered OBD-II: " + heard.join(", ") : "";
}
async function pollCanRecent() {
  const r = await api("/api/can/recent");
  $("canRecent").textContent = r.frames.map(f => f.id.padStart(3, "0") + "  " + f.data + "  (" + f.age + " ms" + (f.air ? ", WiFi relay" : "") + ")").join("\n") || "no frames yet";
}
// ---------------------------------------------------------------------------
// K-line mode: the module browsed on the gauge (menu > K-Line) and live blocks on this page
// ---------------------------------------------------------------------------
function renderKlMode() {
  const a = settingsCache.kline.modeAddr, sel = $("klModeSel");
  const known = [...sel.options].some(o => Number(o.value) === a);
  sel.value = known ? String(a) : "custom";
  $("klModeCustomBox").style.display = known ? "none" : "";
}
async function pollKlBlock() {
  const r = await api("/api/kline/block");
  if (!$("klLive").checked) {
    return; // 0.17.5: switched off while this answer was on its way
  }
  const hex = (n) => n.toString(16).toUpperCase().padStart(2, "0");
  $("klLiveState").textContent = "Module 0x" + hex(r.addr) + ": " + titleCase(r.state || "starting") + (r.ecuId ? "  |  " + r.ecuId : "") +
    "  |  Reads " + r.reads + ", Errors " + r.errors;
  if (document.activeElement !== $("klGoto")) $("klGoto").value = r.group;
  // the label file: the ones that fit this module, the one in use selected ("none" when nothing fits)
  const sel = $("klLabelSel"), cands = r.labelCands || [], key = cands.join() + "|" + r.labelFile;
  $("klLabelRow").style.display = r.connected ? "" : "none";
  if (sel.dataset.key !== key && document.activeElement !== sel) {
    sel.dataset.key = key;
    sel.innerHTML = "";
    cands.forEach(c => sel.appendChild(new Option(c, c)));
    if (!cands.length) sel.appendChild(new Option("none fits this part number", ""));
    else sel.appendChild(new Option("automatic (best match)", ""));
    sel.value = r.labelFile || "";
  }
  if (r.connected) {
    $("klDetailsRow").style.display = "";
  } else {
    $("klDetailsRow").style.display = "none";
  }
  klLastBlock = r;
  renderKlBlock();
  klFindPrepare(r.labelFile); // 0.17.5: Find... uses the label file the gauge picked for this module
}

// 0.17.5: the block being read, as page lines instead of one block of text: a title line, then each value with its
// name on the left and value + unit on the right. "Show Formula and Spec" adds a small line under each value with
// the formula number, its a / b bytes and the label file's spec.
let klLastBlock = null; // the last /api/kline/block answer, so ticking "Show Formula and Spec" redraws straight away
function klAddLine(parent, className, text) { // adds a line (a <div>) with this class and text to parent
  const line = document.createElement("div");
  line.className = className;
  line.textContent = text; // textContent: label names are shown as text, never read as HTML
  parent.appendChild(line);
  return line;
}
function klHex(number) { // 5 -> "05"
  return number.toString(16).toUpperCase().padStart(2, "0");
}
function renderKlBlock() {
  const box = $("klLiveBlock");
  const r = klLastBlock;
  box.textContent = ""; // empties the box
  if (!r || !r.connected) {
    return;
  }
  // title line: "Block 3 · Basic Functions" on the left, how old the reading is on the right
  let title = "Block " + r.group;
  if (r.title) {
    title = title + " · " + r.title;
  }
  if (r.obd) { // 0.39.6: the engine didn't answer at 0x01, OBD-II at 0x33 did: pages of PIDs instead of blocks
    title = "OBD-II at 0x33 · page " + r.group + " of " + r.pages;
  }
  const head = klAddLine(box, "kl-block-head", title);
  if (r.ageMs && !r.searching) {
    klAddLine(head, "kl-age", r.ageMs + " ms ago");
  }
  if (r.searching) {
    klAddLine(box, "kl-note", "Empty, trying the next block");
    return;
  }
  // 0.17.4: a text block (ME7 81 = VIN, 82 = immobiliser id) comes as r.text instead of fields
  if (r.text) {
    klAddLine(box, "kl-text", r.text);
    return;
  }
  if (!r.fields.length) {
    klAddLine(box, "kl-note", "Reading...");
    return;
  }
  const showDetails = $("klDetails").checked && !r.obd; // OBD-II PIDs have no formula / a / b bytes
  for (let i = 0; i < r.fields.length; i++) {
    const field = r.fields[i];
    const row = klAddLine(box, "kl-field", "");
    let name = field.name;
    if (!name) {
      name = "Field " + (i + 1);
    }
    klAddLine(row, "kl-name", name);
    const value = klAddLine(row, "kl-value", String(field.value));
    if (field.unit) {
      const unit = document.createElement("small");
      unit.textContent = field.unit;
      value.appendChild(unit);
    }
    if (showDetails) {
      let detail = "Formula 0x" + klHex(field.formula) + " · a " + field.a + " · b " + field.b;
      if (field.spec) {
        detail = detail + " · " + field.spec;
      }
      klAddLine(row, "kl-detail", detail);
    }
  }
}

// 0.17.5: Find... - every value name in the module's label file (with its block's title and spec), so a value can be
// found without knowing its block number. Picking one goes to that block.
let klFindFile = "";    // the label file the list below was made from
let klFindEntries = []; // one per value: {group, field, name, title, words (name + title + spec in lower case)}
let klFindShown = [];   // the matches in the drop-down, in order
let klFindPick = -1;    // the match the arrow keys have highlighted, -1 = none
async function klFindPrepare(file) {
  if (!file && settingsCache) {
    file = klKnown[settingsCache.kline.modeAddr] || ""; // not connected yet: the file the gauge used last for this module
  }
  if (file === klFindFile) {
    return; // already made from this file
  }
  klFindFile = file;
  klFindEntries = []; // another module: its names, not the last one's
  if (!file) {
    return;
  }
  let text = "";
  try {
    text = await (await fetch("/api/lbl?file=" + encodeURIComponent(file))).text();
  } catch (e) {
    klFindFile = ""; // try again next time
    return;
  }
  // a label line: block TAB field TAB name TAB name (2nd part) TAB spec. Field 0 is the block's title.
  const titles = {};
  const lines = text.split("\n");
  for (let i = 0; i < lines.length; i++) {
    const parts = lines[i].split("\t");
    if (parts.length < 3) {
      continue;
    }
    const group = Number(parts[0]);
    const field = Number(parts[1]);
    const name = (parts[2] + " " + (parts[3] || "")).trim();
    const spec = parts[4] || "";
    if (field === 0) {
      titles[group] = name;
    } else if (name) {
      klFindEntries.push({ group: group, field: field, name: name, spec: spec });
    }
  }
  for (let i = 0; i < klFindEntries.length; i++) {
    const entry = klFindEntries[i];
    entry.title = titles[entry.group] || "";
    entry.words = (entry.name + " " + entry.title + " " + entry.spec).toLowerCase();
  }
  if (document.activeElement === $("klFind")) {
    renderKlFind();
  }
}
function renderKlFind() {
  const list = $("klFindList");
  const typed = $("klFind").value.trim().toLowerCase();
  list.textContent = "";
  klFindShown = [];
  klFindPick = -1;
  if (!typed) {
    list.style.display = "none";
    return;
  }
  list.style.display = "";
  if (!klFindEntries.length) {
    klAddLine(list, "kl-find-empty", "No value names for this module yet - tick Read Blocks Here so the gauge can find its label file.");
    return;
  }
  // every word typed has to be in the name, the block title or the spec ("boost act" finds "Boost Pressure Actual").
  // Matches in the name come first, then the ones only in the title / spec.
  const words = typed.split(/\s+/); // split at the spaces
  const inName = [];
  const elsewhere = [];
  for (let i = 0; i < klFindEntries.length; i++) {
    const entry = klFindEntries[i];
    let all = true;
    let allInName = true;
    for (let w = 0; w < words.length; w++) {
      if (entry.words.indexOf(words[w]) < 0) {
        all = false;
      }
      if (entry.name.toLowerCase().indexOf(words[w]) < 0) {
        allInName = false;
      }
    }
    if (allInName) {
      inName.push(entry);
    } else if (all) {
      elsewhere.push(entry);
    }
  }
  const matches = inName.concat(elsewhere);
  if (!matches.length) {
    klAddLine(list, "kl-find-empty", "Nothing called that in " + klFindFile);
    return;
  }
  const MAX_SHOWN = 40; // more than this and the list is too long to read: type more of the name
  for (let i = 0; i < matches.length && i < MAX_SHOWN; i++) {
    const entry = matches[i];
    const item = document.createElement("button");
    item.type = "button";
    item.className = "kl-find-item";
    item.dataset.index = String(i);
    const where = document.createElement("b");
    where.textContent = "Block " + entry.group;
    item.appendChild(where);
    item.appendChild(document.createTextNode(entry.name));
    let under = "Field " + entry.field;
    if (entry.title) {
      under = under + " · " + entry.title;
    }
    const sub = document.createElement("span");
    sub.textContent = under;
    item.appendChild(sub);
    list.appendChild(item);
    klFindShown.push(entry);
  }
  if (matches.length > MAX_SHOWN) {
    klAddLine(list, "kl-find-empty", (matches.length - MAX_SHOWN) + " more - type more of the name");
  }
}
function klFindGo(index) { // a match picked: go to its block (and start reading blocks here if that's off)
  const entry = klFindShown[index];
  if (!entry) {
    return;
  }
  $("klFind").value = "";
  $("klFind").blur();
  $("klFindList").style.display = "none";
  $("klGoto").value = entry.group;
  if (!$("klLive").checked) {
    $("klLive").checked = true;
  }
  action("klGoto", { group: entry.group });
}
function klFindHighlight(step) { // the arrow keys move the highlight up / down the list
  const items = $("klFindList").querySelectorAll(".kl-find-item");
  if (!items.length) {
    return;
  }
  klFindPick = klFindPick + step;
  if (klFindPick < 0) {
    klFindPick = items.length - 1;
  }
  if (klFindPick >= items.length) {
    klFindPick = 0;
  }
  for (let i = 0; i < items.length; i++) {
    items[i].classList.toggle("sel", i === klFindPick);
  }
  items[klFindPick].scrollIntoView({ block: "nearest" });
}
function klFindKey(e) { // Find... box: up / down, Enter picks, Escape closes the list
  if (e.key === "ArrowDown") {
    e.preventDefault();
    klFindHighlight(1);
  } else if (e.key === "ArrowUp") {
    e.preventDefault();
    klFindHighlight(-1);
  } else if (e.key === "Enter") {
    e.preventDefault();
    if (klFindPick >= 0) {
      klFindGo(klFindPick);
    } else {
      klFindGo(0); // nothing highlighted: the best match
    }
  } else if (e.key === "Escape") {
    $("klFindList").style.display = "none";
  }
}
function klFindClick(e) { // a tap / click on a match in the list
  const item = e.target.closest(".kl-find-item");
  if (item) {
    klFindGo(Number(item.dataset.index));
  }
}
function klFindHold(e) { // pressing on the list doesn't take the focus off the box (that would close the list first)
  e.preventDefault();
}
function klFindClose() {
  $("klFindList").style.display = "none";
}
function klFindOpen() {
  let file = "";
  if (klLastBlock && klLastBlock.connected) {
    file = klLastBlock.labelFile; // the file in use; otherwise the one the gauge used last for this module
  }
  klFindPrepare(file);
  renderKlFind();
}
function klLiveChanged() { // "Read Blocks Here" switched off: say so and clear the block (on: the next status poll reads it)
  if ($("klLive").checked) {
    return;
  }
  $("klLiveState").textContent = "Off - the session closes in a few seconds.";
  klLastBlock = null;
  renderKlBlock();
  $("klDetailsRow").style.display = "none";
  $("klLabelRow").style.display = "none";
}
function klDetailsChanged() { // "Show Formula and Spec": remembered in this browser only
  let remember = "0";
  if ($("klDetails").checked) {
    remember = "1";
  }
  try {
    localStorage.setItem("c2g.klDetails", remember);
  } catch (e) { /* private window: just not remembered */ }
  renderKlBlock();
}

// ---------------------------------------------------------------------------
// CAN: car identification + the DBC on the gauge (data/dbc/ from tools/dbc/dbc_tool.py pack)
// ---------------------------------------------------------------------------
let dbcIndex = null, dbcFps = null, dbcMatchKey = "", canIdsTick = 0;
const ownId = (id) => id >= 0x6A0 && id <= 0x6EF; // OpenHaldex + the gauge chain: not the car's
async function loadDbcLibrary() {
  if (dbcIndex) return;
  try {
    dbcIndex = (await (await fetch("/dbc/index.json")).json()).dbcs;
    dbcFps = await (await fetch("/dbc/fingerprints.json")).json();
  } catch (e) { $("dbcMatches").textContent = "The DBC library isn't on the gauge - upload the file system image (OTA tab)."; return; }
  const sel = $("dbcPick"), groups = {};
  dbcIndex.forEach(d => (groups[d.make] = groups[d.make] || []).push(d));
  sel.innerHTML = "";
  // 0.28.0: the gauge's own DBCs (aftermarket ECUs) first - they feed the gauge's values straight away
  const gaugeOnes = dbcIndex.filter(d => d.gauge);
  if (gaugeOnes.length) {
    const g = document.createElement("optgroup"); g.label = "GAUGE - ECU DBCS";
    gaugeOnes.forEach(d => g.appendChild(new Option(d.name + (d.desc ? " - " + d.desc : ""), d.name)));
    sel.appendChild(g);
  }
  Object.keys(groups).sort().forEach(make => {
    const others = groups[make].filter(d => !d.gauge);
    if (!others.length) return;
    const g = document.createElement("optgroup"); g.label = make.toUpperCase();
    others.forEach(d => g.appendChild(new Option(d.name + (d.desc ? " - " + d.desc : d.cars.length ? " - " + d.cars.slice(0, 2).join(", ") : "") + (d.en ? " (English)" : "") + (d.keys ? " - feeds the gauge" : ""), d.name)));
    sel.appendChild(g);
  });
}
// explained = share of the bus ids the DBC has (DLC agreeing); coverage = share of the DBC seen (tools/dbc/dbc_tool.py score)
function dbcScore(seen, fp) {
  const ids = new Map(fp.map(([id, dlc]) => [id, dlc]));
  let hit = 0;
  seen.forEach((dlc, id) => { if (ids.has(id) && ids.get(id) === dlc) hit++; });
  if (!seen.size || !ids.size) return [0, 0, 0];
  const ex = hit / seen.size, cov = hit / ids.size;
  return [ex * (0.8 + 0.2 * cov), ex, cov];
}
async function pollCanIds() {
  const r = await api("/api/can/ids");
  const hex = (id, ext) => "0x" + id.toString(16).toUpperCase().padStart(ext ? 8 : 3, "0");
  const known = r.ids.filter(x => x[4]).length;
  $("dbcNow").textContent = !r.enabled ? "CAN is off (above)." : r.dbc.name
    ? "On the gauge: " + r.dbc.name + " - " + r.dbc.messages + " messages, " + r.dbc.signals + " signals. " + known + " of the " + r.ids.length + " IDs on the bus are in it."
    : "No DBC on the gauge yet - use a match below or pick one.";
  if (!document.activeElement || !document.activeElement.matches(".dbc-base")) renderDbcSlots(r.dbc.slots); // not while typing a base
  $("canIds").textContent = r.ids.length ? r.ids.map(x => hex(x[0], x[5]).padEnd(11) + (x[4] || "-").padEnd(26) + " " + x[1] + "  " +
    String(x[2]).padStart(6) + " Hz   " + x[3]).join("\n") : (r.up ? "no frames yet" : "bus down");
  if (!dbcFps) return;
  const seen = new Map();
  r.ids.forEach(x => { if (!x[5] && !ownId(x[0])) seen.set(x[0], x[1]); });
  if (!seen.size) { $("dbcMatches").textContent = "Nothing from the car on the bus yet."; dbcMatchKey = ""; return; }
  const ranked = Object.entries(dbcFps).map(([n, fp]) => [n, ...dbcScore(seen, fp)]).filter(x => x[1] > 0).sort((a, b) => b[1] - a[1]).slice(0, 5);
  const key = ranked.map(x => x[0] + Math.round(x[1] * 100)).join() + "|" + r.dbc.name;
  if (key === dbcMatchKey) return; // same ranking: leave the buttons alone
  dbcMatchKey = key;
  if (!ranked.length) { $("dbcMatches").textContent = "None of the DBCs know these " + seen.size + " IDs."; return; }
  $("dbcMatches").innerHTML = ranked.map(([n, s, ex, cov]) => {
    const d = dbcIndex.find(x => x.name === n) || { cars: [] }, cars = d.desc || d.cars.slice(0, 4).join(", ");
    return '<div class="found-row"><span><b>' + esc(n) + "</b> " + Math.round(s * 100) + "% - explains " + Math.round(ex * 100) + "% of the bus, " +
      Math.round(cov * 100) + "% of the DBC" + (cars ? '<br><span style="color:var(--muted)">' + esc(cars) + "</span>" : "") +
      "</span>" + ((r.dbc.slots || []).some(s => s.loaded && s.name === n) ? "<span>on the gauge</span>" : '<button class="btn-secondary" data-dbc="' + esc(n) + '">Use</button>') + "</div>";
  }).join("") + '<div class="help">' + seen.size + " IDs heard.</div>";
}
async function postDbc(text) {
  const slot = Number($("dbcSlot").value) || 1; // 0.28.0: into the slot picked (1 car, 2 gearbox, 3 extra)
  const r = await fetch("/api/can/dbc?slot=" + slot, { method: "POST", headers: { "Content-Type": "text/plain" }, body: text });
  const j = await r.json();
  toast(j.error ? "DBC: " + j.error : j.name + " in slot " + slot + ": " + j.messages + " messages" + (j.keyed ? ", " + j.keyed + " feed the gauge" : ""));
  dbcMatchKey = "";
  pollCanIds();
}
// 0.28.0: the three DBC slots as the gauge has them - name, size, what feeds the gauge, why one isn't loaded
const DBC_SLOT_NAMES = ["Car / engine", "Gearbox", "Extra"];
function renderDbcSlots(slots) {
  const bases = (settingsCache && settingsCache.can && settingsCache.can.dbcBase) || [0, 0, 0];
  $("dbcSlots").innerHTML = (slots || []).map((s, i) => {
    let text = "Empty";
    if (s.loaded) {
      text = "<b>" + esc(s.name) + "</b> - " + s.messages + " messages" + (s.keyed ? ", " + s.keyed + " values feed the gauge" : "") +
        (s.bus ? ", " + (s.bus / 1000) + "k" : "");
    } else if (s.error) {
      text = '<span style="color:var(--danger)">Not loaded: ' + esc(s.error) + "</span>";
    }
    let base = "";
    if (s.loaded && s.reloc) { // an ECU whose base ID moves (EMU Black, MaxxECU): where its first frame really is
      const shown = bases[i] ? bases[i] : s.fileBase;
      base = '<span class="form-group narrow" style="display:inline-block;margin:0 8px"><label>Base ID (hex)</label><input type="text" class="dbc-base" data-slot="' + i + '" value="0x' + shown.toString(16).toUpperCase() + '"></span>';
    }
    const remove = s.loaded || s.error ? '<button class="btn-secondary" data-slot-remove="' + (i + 1) + '">Remove</button>' : "";
    return '<div class="found-row"><span>' + (i + 1) + ". " + DBC_SLOT_NAMES[i] + ": " + text + "</span>" + base + remove + "</div>";
  }).join("");
}
async function dbcSlotClicked(e) {
  const b = e.target.closest("[data-slot-remove]");
  if (!b) return;
  const slot = Number(b.dataset.slotRemove);
  if (!confirm("Remove the DBC in slot " + slot + " from the gauge?")) return;
  await action("canDbcClear", { slot: slot });
  dbcMatchKey = "";
  pollCanIds();
}
async function dbcSlotBaseChanged(e) {
  if (!e.target.matches(".dbc-base")) return;
  const slot = Number(e.target.dataset.slot), value = parseInt(e.target.value, 16);
  if (isNaN(value) || value < 0 || value > 0x7FF) { toast("Base ID: an 11-bit hex ID, e.g. 0x520"); return; }
  const bases = (settingsCache.can.dbcBase || [0, 0, 0]).slice();
  bases[slot] = value;
  await saveSettings({ can: { dbcBase: bases } }, "base ID saved - DBCs loaded again");
  dbcMatchKey = "";
  pollCanIds();
}
async function loadDbcToGauge(name) {
  toast("loading " + name + "...");
  const r = await fetch("/dbc/" + name + ".c2d");
  if (!r.ok) return toast(name + " isn't on the gauge");
  postDbc(await r.text());
}
// a .dbc file -> the gauge's compact .c2d (same as tools/dbc/dbc_tool.py c2d_text)
// units: tools/dbc/dbc_tool.py UNITS
const DBC_UNITS = {"Unit_PerCent": "%", "Unit_NewtoMeter": "Nm", "Unit_MeterPerSeconSquar": "m/s2", "Unit_Meter": "m", "Unit_KiloMeterPerHour": "km/h", "Unit_KilometerPerHour": "km/h", "Unit_DegreCelsi": "\u00b0C", "Unit_MinutInver": "rpm", "Unit_Secon": "s", "Unit_DegreOfArc": "\u00b0", "Unit_None": "", "Unit_Amper": "A", "Unit_Centimeter": "cm", "Unit_Volt": "V", "Unit_KiloMeter": "km", "Unit_Hours": "h", "Unit_Bar": "bar", "Unit_Watt": "W", "Unit_Month": "month", "Unit_Minut": "min", "Unit_Liter": "l", "Unit_DegreOfArcPerSecon": "\u00b0/s", "Unit_centiNewtoMeter": "cNm", "Unit_Ohm": "Ohm", "Unit_MilliMeter": "mm", "Unit_MilliLiter": "ml", "Unit_MilliBar": "mbar", "Unit_Year": "year", "Unit_MilliVolt": "mV", "Unit_MilliSecon": "ms", "Unit_MeterPerSecond": "m/s", "Unit_KiloGram": "kg", "Unit_Hertz": "Hz", "Unit_Day": "day", "Unit_WattHour": "Wh", "Unit_KiloWatt": "kW", "Unit_KiloNewto": "kN", "U/min": "rpm", "upm": "rpm", "1/min": "rpm", "Umin": "rpm", "deg/sek": "\u00b0/s", "Grad": "\u00b0", "\u00b0Grad": "\u00b0", "Grad/s": "\u00b0/s", "\u00b0Grad/s": "\u00b0/s", "C": "\u00b0C", "degC": "\u00b0C", "deg C": "\u00b0C", "Celsius": "\u00b0C", "\u00b0 C": "\u00b0C", "deg": "\u00b0", "Deg": "\u00b0"};
function dbcToC2d(text, name) {
  const clean = (s, n) => s.replace(/\t/g, " ").replace(/[^\x20-\x7e\u00b0]/g, "?").trim().slice(0, n);
  const unit = (u) => { u = u.trim(); return u in DBC_UNITS ? DBC_UNITS[u] : u.startsWith("Unit_") ? u.slice(5) : u; };
  const BO = /^BO_ (\d+) (\w+)\s*:\s*(\d+) (\w+)/, SG = /^ SG_ (\w+)\s*(\w*)\s*:\s*(\d+)\|(\d+)@([01])([+-]) \(([^,]+),([^)]+)\) \[([^|]*)\|([^\]]*)\] "([^"]*)"/;
  const out = ["C2D\t1\t" + clean(name, 39)];
  let cur = false;
  text.split(/\r?\n/).forEach(line => {
    let m = BO.exec(line);
    if (m) {
      const raw = Number(m[1]);
      cur = (raw & 0x1FFFFFFF) !== 0x1FFFFFFF && m[2] !== "VECTOR__INDEPENDENT_SIG_MSG";
      if (cur) out.push("M\t" + ((raw & 0x1FFFFFFF) + (raw >= 0x80000000 ? 0x80000000 : 0)) + "\t" + m[3] + "\t" + clean(m[2], 39));
      return;
    }
    if (!cur) return;
    const s = SG.exec(line);
    if (s) {
      const mm = /^m(\d+)/.exec(s[2]), mux = s[2] === "M" ? -2 : mm ? Number(mm[1]) : -1;
      out.push(["S", clean(s[1], 39), s[3], s[4], (s[5] === "1" ? 1 : 0) | (s[6] === "-" ? 2 : 0), s[7].trim(), s[8].trim(), clean(unit(s[11]), 9), mux].join("\t"));
    } else if (!line.trim() || !line.startsWith(" ")) cur = false;
  });
  return out.join("\n") + "\n";
}
// ---------------------------------------------------------------------------
// ADC tab: adc2can boards on the bus (src/can2gauge_ADC2CAN.cpp), 8 sensors each
// ---------------------------------------------------------------------------
const ADC_PRESETS = [ // linear presets: [label, scale (units / V), offset (units at 0 V), unit]
  ["Volts", 1, 0, "V"],
  ["Pressure 0-100 psi (0.5-4.5 V)", 25, -12.5, "psi"],
  ["Pressure 0-150 psi (0.5-4.5 V)", 37.5, -18.75, "psi"],
  ["Pressure 0-7 bar (0.5-4.5 V)", 1.75, -0.875, "bar"],
  ["Pressure 0-10 bar (0.5-4.5 V)", 2.5, -1.25, "bar"],
  ["MAP 3 bar abs (Bosch 0 261 230 119)", 0.6588, -0.0635, "bar"],
  ["Wideband AFR (0-5 V = 10-20)", 2, 10, "AFR"],
  ["Wideband lambda (0-5 V = 0.68-1.36)", 0.136, 0.68, "lambda"],
  ["EGT amplifier (0-5 V = 0-1250 C)", 250, 0, "C"],
];
let adcLast = null, adcCfgKey = [];
// 0.36.6: the adc2can sensors, for every list that offers them. The gauge says how many boards it takes (/api/adc
// boards), so more boards later need nothing here. A sensor is offered only once its board has been heard.
// has adc2can board `board` (0, 1 ...) been heard?
function adcBoardSeen(board) {
  if (!adcLast || !adcLast.boards || !adcLast.boards[board]) return false;
  return !!adcLast.boards[board].seen;
}
// should a list offer this signal key ("adc9")? Not an ADC sensor: yes. A sensor: only when its board has been heard.
function adcSensorFound(key) {
  if (!isAdc(key)) return true;
  return adcBoardSeen(Math.floor((Number(key.slice(3)) - 1) / 8));
}
// how every list names sensor `index` (0 = board 1 sensor 1): "Board 1 - Sensor 1 (V)", with the sensor's own name
// when it has one ("Board 1 - Sensor 1 - Oil press (bar)"). No unit yet = its raw reading, volts.
function adcSensorText(index) {
  let text = "Board " + (Math.floor(index / 8) + 1) + " - Sensor " + ((index % 8) + 1);
  let unit = "V";
  if (adcLast && adcLast.ch && adcLast.ch[index]) {
    const channel = adcLast.ch[index];
    if (channel.name && !/^ADC \d+$/.test(channel.name)) {
      text = text + " - " + channel.name;
    }
    if (channel.unit) {
      unit = channel.unit;
    }
  }
  return text + " (" + uShown(unit) + ")";
}
const ADC_MODES = ["Linear", "Table", "NTC"];
function renderAdc() {
  const a = settingsCache.adc || { baseId: 0x0AD2C000, ch: [] };
  setVal("adc.baseId", a.baseId);
  setVal("adc.wireless", a.wireless === undefined ? 1 : a.wireless);
  $("adcList").innerHTML = (a.ch || []).map((c, i) => `<div class="adc-row" id="adcRow${i}">
      <h3><span>Board ${Math.floor(i / 8) + 1} - Sensor ${i % 8 + 1}</span><span class="adc-live" id="adcLive${i}">--</span><span class="adc-chip" id="adcChip${i}">--</span></h3>
      <div class="row-inline"><div class="form-group wide"><label>Scaling</label><select class="ad-mode"><option value="0">On the adc2can (auto)</option><option value="1">On the gauge (override)</option></select></div>
        <div class="help ad-note" style="flex:1 1 300px;align-self:center"></div></div>
      <div class="ad-remote" data-nosave>
        <div class="row-inline">
          <div class="form-group narrow"><label>Mode</label><select class="r-mode">${ADC_MODES.map((m, k) => `<option value="${k}">${m}</option>`).join("")}</select></div>
          <div class="form-group r-lin"><label>Preset</label><select class="r-preset"><option value="">Custom</option>${ADC_PRESETS.map((p, k) => `<option value="${k}">${p[0]}</option>`).join("")}</select></div>
          <div class="form-group narrow r-lin"><label>Scale (per V)</label><input type="number" step="any" class="r-scale"></div>
          <div class="form-group narrow r-lin"><label>Offset</label><input type="number" step="any" class="r-offset"></div>
          <div class="form-group r-tbl"><label>Table (volts:value, ...)</label><input type="text" class="r-table" placeholder="0.5:0, 2.5:5, 4.5:10"></div>
          <div class="form-group narrow r-ntc"><label>R25 (ohm)</label><input type="number" step="any" class="r-r25"></div>
          <div class="form-group narrow r-ntc"><label>Beta</label><input type="number" step="any" class="r-beta"></div>
          <div class="form-group narrow r-ntc"><label>Pull-up (ohm)</label><input type="number" step="any" class="r-pu"></div>
        </div>
        <div class="row-inline">
          <div class="form-group"><label>Name</label><input type="text" class="r-name" maxlength="15"></div>
          <div class="form-group narrow"><label>Unit</label><input type="text" class="r-unit" maxlength="7"></div>
          <div class="form-group narrow"><label>Decimals</label><input type="number" class="r-dec" min="0" max="4"></div>
          <div class="form-group narrow"><label>Low warn</label><input type="number" step="any" class="r-lo" placeholder="none"></div>
          <div class="form-group narrow"><label>High warn</label><input type="number" step="any" class="r-hi" placeholder="none"></div>
          <div class="form-group narrow r-dial" title="The dial range the adc2can uses, and gauge pages picking this sensor take. Blank = Auto, worked out from the scaling"><label>Dial Min</label><input type="number" step="any" class="r-dmin" placeholder="Auto"></div>
          <div class="form-group narrow r-dial" title="The dial range the adc2can uses, and gauge pages picking this sensor take. Blank = Auto, worked out from the scaling"><label>Dial Max</label><input type="number" step="any" class="r-dmax" placeholder="Auto"></div>
          <div class="form-group narrow"><label>Smoothing</label><input type="number" class="r-sm" min="1" max="250"></div>
          <label class="toggle"><input type="checkbox" class="r-rail"><span class="toggle-slider"></span><span class="toggle-label">5 V rail on</span></label>
          <label class="toggle"><input type="checkbox" class="r-ratio"><span class="toggle-slider"></span><span class="toggle-label">Ratiometric</span></label>
        </div>
      </div>
      <div class="ad-local">
        <div class="row-inline">
          <div class="form-group ad-lin"><label>Preset</label><select class="ad-preset" data-nosave><option value="">Custom</option>${ADC_PRESETS.map((p, k) => `<option value="${k}">${p[0]}</option>`).join("")}</select></div>
          <div class="form-group narrow"><label>Scale (per V)</label><input type="number" step="any" class="ad-scale" value="${c.scale}"></div>
          <div class="form-group narrow"><label>Offset</label><input type="number" step="any" class="ad-offset" value="${c.offset}"></div>
          <div class="form-group"><label>Name</label><input type="text" class="ad-name" maxlength="15" value="${esc(c.name)}"></div>
          <div class="form-group narrow"><label>Unit</label><input type="text" class="ad-unit" maxlength="7" value="${esc(c.unit)}"></div>
          <div class="form-group narrow"><label>Decimals</label><input type="number" class="ad-dec" min="0" max="3" value="${c.decimals}"></div>
        </div>
      </div></div>`).join("");
  adcCfgKey = [];
  (a.ch || []).forEach((c, i) => {
    const row = $("adcRow" + i), mode = row.querySelector(".ad-mode");
    mode.value = String(c.mode);
    const show = () => { row.querySelector(".ad-local").style.display = mode.value === "1" ? "" : "none"; adcRowState(i); };
    mode.addEventListener("change", show); show();
    row.querySelector(".ad-preset").addEventListener("change", (e) => { // gauge override: a preset fills scale, offset, unit
      const p = ADC_PRESETS[Number(e.target.value)]; if (!p) return;
      row.querySelector(".ad-scale").value = p[1]; row.querySelector(".ad-offset").value = p[2]; row.querySelector(".ad-unit").value = p[3];
      row.querySelector(".ad-scale").dispatchEvent(new Event("change", { bubbles: true }));
    });
    // the adc2can's own settings: any edit goes to it over CAN (debounced), it saves and echoes back
    const rem = row.querySelector(".ad-remote");
    const modeShow = () => { const m = rem.querySelector(".r-mode").value; rem.querySelectorAll(".r-lin").forEach(e => e.style.display = m === "0" ? "" : "none");
      rem.querySelectorAll(".r-tbl").forEach(e => e.style.display = m === "1" ? "" : "none"); rem.querySelectorAll(".r-ntc").forEach(e => e.style.display = m === "2" ? "" : "none"); };
    rem.querySelector(".r-mode").addEventListener("change", modeShow); modeShow();
    rem.querySelector(".r-preset").addEventListener("change", (e) => {
      const p = ADC_PRESETS[Number(e.target.value)]; if (!p) return;
      rem.querySelector(".r-scale").value = p[1]; rem.querySelector(".r-offset").value = p[2]; rem.querySelector(".r-unit").value = p[3];
      adcSend(i);
    });
    let t = null;
    rem.addEventListener("change", (e) => { if (!e.target.matches(".r-preset")) { clearTimeout(t); adcSend(i); } });
    rem.addEventListener("input", (e) => { if (e.target.matches("input[type=text], input[type=number]")) { clearTimeout(t); t = setTimeout(() => adcSend(i), 900); } });
    rem._modeShow = modeShow;
  });
}
const numOrNull = (v) => (v === "" || isNaN(Number(v)) ? null : Number(v));
// 0.36.4: the Dial Min / Max boxes from the adc2can's settings: a number when set by hand, blank (with the Auto value
// as the hint) on Auto; hidden for an adc2can before 1.4, which has no dial range
function adcDialBoxes(rem, cfg) {
  const boxes = rem.querySelectorAll(".r-dial");
  const hasRange = cfg.dialLo !== undefined && cfg.dialHi !== undefined;
  for (let n = 0; n < boxes.length; n++) {
    if (hasRange) {
      boxes[n].style.display = "";
    } else {
      boxes[n].style.display = "none";
    }
  }
  if (!hasRange) return;
  const minBox = rem.querySelector(".r-dmin");
  const maxBox = rem.querySelector(".r-dmax");
  minBox.placeholder = "Auto (" + fmt(cfg.dialLo, 2) + ")";
  maxBox.placeholder = "Auto (" + fmt(cfg.dialHi, 2) + ")";
  minBox.value = "";
  maxBox.value = "";
  if (cfg.dialMinSet) minBox.value = cfg.dialLo;
  if (cfg.dialMaxSet) maxBox.value = cfg.dialHi;
}
async function adcSend(i) { // the form -> the adc2can's settings
  const rem = $("adcRow" + i).querySelector(".ad-remote"), q = (c) => rem.querySelector(c);
  const table = q(".r-table").value.split(",").map(x => x.split(":").map(Number)).filter(x => x.length === 2 && !isNaN(x[0]) && !isNaN(x[1])).slice(0, 8);
  const body = { board: i >> 3, ch: i & 7, mode: Number(q(".r-mode").value), decimals: Number(q(".r-dec").value) || 0, railOn: q(".r-rail").checked,
    ratiometric: q(".r-ratio").checked, smoothing: Number(q(".r-sm").value) || 1, scale: Number(q(".r-scale").value), offset: Number(q(".r-offset").value),
    lowWarn: numOrNull(q(".r-lo").value), highWarn: numOrNull(q(".r-hi").value), ntcR25: Number(q(".r-r25").value), ntcBeta: Number(q(".r-beta").value),
    ntcPullup: Number(q(".r-pu").value), table, name: q(".r-name").value, unit: q(".r-unit").value };
  // 0.36.4: the dial range, when this adc2can has one (1.4+): blank = Auto
  if (q(".r-dial").style.display !== "none") {
    body.dialMin = numOrNull(q(".r-dmin").value);
    body.dialMax = numOrNull(q(".r-dmax").value);
  }
  const r = await api("/api/adc/set", "POST", body);
  toast(r.error || r.status || "sent");
  adcCfgKey[i] = ""; // take the echo as it comes back
}
function adcRowState(i) { // auto: the adc2can's settings editor, or why it isn't there
  const row = $("adcRow" + i); if (!row) return;
  const auto = row.querySelector(".ad-mode").value === "0", c = adcLast && adcLast.ch[i], b = adcLast && adcLast.boards[i >> 3];
  const has = !!(c && c.cfg);
  row.querySelector(".ad-remote").style.display = auto && has ? "" : "none";
  row.querySelector(".ad-note").textContent = !auto ? "Scaled here from the connector voltage: volts x scale + offset." :
    !b || !b.seen ? "adc2can not heard." : !b.shares ? "This adc2can's firmware doesn't share its settings - update it to 1.1 to set it up from here. Its values still come across, scaled." :
    !has ? "Reading the adc2can's settings..." + (b.via === "wifi" ? " (over WiFi they take about 25 s to arrive in full)" : "") :
    b.via === "wifi" ? "The adc2can's own settings (firmware " + b.fw + "), heard over WiFi: shown here, but changes need CAN - or use the adc2can's own web UI." :
    "The adc2can's own settings (firmware " + b.fw + "): edits go to it over CAN and are saved there.";
}
function collectAdc() {
  return { baseId: getVal("adc.baseId"), wireless: Number($("adc.wireless").value), ch: [...document.querySelectorAll(".adc-row")].map(r => ({
    mode: Number(r.querySelector(".ad-mode").value), scale: Number(r.querySelector(".ad-scale").value), offset: Number(r.querySelector(".ad-offset").value),
    name: r.querySelector(".ad-name").value, unit: r.querySelector(".ad-unit").value, decimals: Number(r.querySelector(".ad-dec").value) })) };
}
// a dial range for an adc2can sensor: the one the adc2can itself uses (its Dial Min / Max, or Auto from its scaling -
// adc2can 1.4+ sends it), with the high warn as the redline. 0.36.4: the gauge no longer works one out itself; null for
// an adc2can before 1.4 (no range sent), and the page then takes the signal's own (0-5.5 V)
function adcRange(cfg) {
  if (cfg.dialLo === undefined || cfg.dialHi === undefined || !(cfg.dialHi > cfg.dialLo)) {
    return null;
  }
  let red = cfg.dialLo;
  if (cfg.highWarn !== undefined && cfg.highWarn > cfg.dialLo && cfg.highWarn < cfg.dialHi) {
    red = cfg.highWarn;
  }
  return { lo: cfg.dialLo, hi: cfg.dialHi, red: red, dec: Math.min(2, cfg.decimals || 0), name: cfg.name, unit: cfg.unit };
}
async function pollAdc() {
  const r = await api("/api/adc");
  adcLast = r;
  const boardsHost = $("adcBoards");
  if (boardsHost && boardsHost.children.length !== r.boards.length) { // 0.36.6: one line per board the gauge takes
    let lines = "";
    for (let n = 0; n < r.boards.length; n++) {
      lines = lines + `<div class="state-line">Board ${n + 1}: <b id="adcB${n}">--</b></div>`;
    }
    boardsHost.innerHTML = lines;
  }
  r.boards.forEach((b, i) => {
    const flags = [b.flags & 1 ? "12 V on a sensor pin" : "", b.flags & 2 ? "VBAT low" : "", b.flags & 4 ? "VBAT high" : "", b.flags & 0x18 ? "ADC chip failed" : "", b.flags & 0x40 ? "no 12 V (USB only)" : ""].filter(Boolean);
    $("adcB" + i).textContent = !b.seen ? "Not Heard" : titleCase((b.valid.toString(2).split("1").length - 1) + "/8 valid, sensors " + b.busV + " V, battery " + b.vbat + " V" +
      (b.fw ? ", v" + b.fw : "") + (b.via === "wifi" ? ", WiFi (" + b.rssi + " dBm)" : b.via === "both" ? ", CAN + WiFi" : b.via === "can" ? ", CAN" : "") +
      (flags.length ? " - " + flags.join(", ") : ""));
  });
  r.ch.forEach((c, i) => {
    const live = $("adcLive" + i), chip = $("adcChip" + i), b = r.boards[i >> 3];
    if (!live) return;
    const row = $("adcRow" + i), auto = row.querySelector(".ad-mode").value === "0";
    row.hidden = !b || !b.seen; // 0.36.6: a board that isn't there: its sensors aren't shown
    const dec = auto && c.cfg ? c.cfg.decimals : (settingsCache.adc.ch[i] || {}).decimals || 2;
    live.textContent = !b.seen ? "--" : (c.v !== undefined ? Number(c.v).toFixed(3) + " V" : "-- V") + "  ->  " + (c.value !== undefined ? fixedText(c.value, dec) + " " + uShown(c.unit) : "--");
    chip.textContent = !b.seen ? "Not Heard" : !c.railOn ? "Rail Off" : c.fault ? "Fault" : c.range ? "Out of Range" : c.value !== undefined ? "OK" : "No Value";
    // no value because the adc2can has no 12 V (status flag bit 6): its sensor 5 V supply is off
    if (b.seen && c.value === undefined && (b.flags & 0x40)) chip.textContent = "No 12 V";
    chip.className = "adc-chip" + (!b.seen || !c.railOn ? "" : c.fault || c.range ? " bad" : " ok");
    row.querySelector(".ad-name").placeholder = c.sentName || "ADC " + (i + 1);
    row.querySelector(".ad-unit").placeholder = c.sentUnit || "";
    adcRowState(i);
    // the adc2can's settings into the form - when they changed and nobody is typing in it
    const rem = row.querySelector(".ad-remote"), key = JSON.stringify(c.cfg || null);
    if (c.cfg && key !== adcCfgKey[i] && !rem.contains(document.activeElement)) {
      adcCfgKey[i] = key;
      const k = c.cfg, set = (sel, v) => { rem.querySelector(sel).value = v === undefined || v === null ? "" : v; };
      set(".r-mode", k.mode); set(".r-scale", k.scale); set(".r-offset", k.offset); set(".r-r25", k.ntcR25); set(".r-beta", k.ntcBeta); set(".r-pu", k.ntcPullup);
      set(".r-table", (k.table || []).map(p => p[0] + ":" + p[1]).join(", ")); set(".r-name", k.name); set(".r-unit", k.unit); set(".r-dec", k.decimals);
      set(".r-lo", k.lowWarn); set(".r-hi", k.highWarn); set(".r-sm", k.smoothing);
      rem.querySelector(".r-rail").checked = k.railOn; rem.querySelector(".r-ratio").checked = k.ratiometric;
      adcDialBoxes(rem, k);
      rem._modeShow();
    }
  });
}
// 0.17.3: the box under K-line > Status: the ME7 logger's values when it runs (their first line is the part number),
// otherwise the connected module's part number; hidden when there's neither
let klEcuText = "", me7Text = "";
let klLineState = ""; // 0.17.6: who has the K-line right now (status kline.state), for renderMe7State
function renderKlBox() {
  const text = me7Text || klEcuText;
  $("me7Vars").textContent = text;
  $("me7Vars").style.display = text ? "" : "none";
}
async function pollMe7() {
  const r = await api("/api/me7/vars");
  me7Text = (r.partNumber ? r.partNumber + " " + r.engine + "  " + r.logSpeed + " baud  " + r.sps + " sps\n" : "") +
    r.vars.map(v => v.name.padEnd(14) + (v.alias || "").padEnd(24) + String(v.value).padStart(10) + " " + v.unit).join("\n");
  renderKlBox();
  renderMe7State(r);
}
// 0.17.6: one line on the ME7 logger card saying what it's doing - before, nothing said that another K-line user
// (a gauge page with a K-line block) had the line, so the logger looked connected at 4/s and Start CSV Log did nothing
function renderMe7State(r) {
  let text = "";
  if (r.state === "logging") {
    $("me7Dot").className = "dot-on on"; // green
    text = "On - " + r.rate + "/s of " + r.sps + "/s";
    if (r.logSpeed) {
      text = text + " at " + r.logSpeed + " baud";
    }
    if (r.requests) {
      // 0.17.6: what sets the ceiling - the ECU answers one request every ~10 ms
      text = text + ", " + r.requests + " requests a sample";
    }
  } else {
    // idle / loading / connecting / defining / error: say who has the K-line meanwhile
    $("me7Dot").className = "dot-on off"; // red
    let lineState = klLineState;
    if (!lineState) {
      lineState = r.state;
    }
    text = "Off - " + titleCase(lineState);
  }
  $("me7State").textContent = text;
}

// ---------------------------------------------------------------------------
// settings forms
// ---------------------------------------------------------------------------
function fillSimple(obj, prefix) { Object.keys(obj).forEach(k => { if ($(prefix + "." + k)) setVal(prefix + "." + k, obj[k]); }); }
function collectSimple(prefix, keys) { const o = {}; keys.forEach(k => { const v = getVal(prefix + "." + k); if (v !== undefined && v !== "") o[k] = v; }); return o; }

// where each value comes from on the cars this gauge targets (shown in the Custom page slot pickers)
const sigSources = {
  rpm: "CAN Motor_1 / K-line / OBD", speed: "CAN Bremse_1 / OBD", coolant: "CAN Motor_2 / K-line / OBD", iat: "K-line engine block 4 / OBD",
  oiltemp: "CAN Kombi_2 / K-line cluster / OBD", map: "K-line block 115 / OBD", boost: "from boost abs", pedal: "CAN Motor_1",
  throttle: "K-line / OBD", load: "CAN Motor_1 / OBD", maf: "K-line / OBD", lambda: "ME7 logger / OBD", timing: "K-line / OBD",
  batt: "K-line engine block 4 / OBD", fuel: "CAN Kombi_1 / OBD", gear: "CAN gearbox", selector: "CAN gearbox",
  hdxTarget: "OpenHaldex", hdxActual: "OpenHaldex", hdxMode: "OpenHaldex", hdxSpeed: "OpenHaldex", hdxPedal: "OpenHaldex",
  ambient: "CAN Motor_3 / OBD", oilLevel: "K-line cluster (oil page)", oilPress: "CAN Kombi_1 (switch)",
  gboxTemp: "CAN MQB / MLB gearbox / Gearbox diagnostics", tcuSpeed: "Gearbox diagnostics", boardTemp: "this gauge", uptime: "this gauge",
  lights: "this gauge", output: "this gauge", outFault: "this gauge",
  // 0.26.0 gearbox
  shifting: "CAN MQB / MLB gearbox", lockup: "CAN MQB / MLB gearbox / Gearbox diagnostics", inputRpm: "CAN MQB Getriebe_13 / MLB Getriebe_03",
  convSlip: "engine rpm - input speed"
};
function slotOptions(sel, current) {
  sel.innerHTML = "";
  const live = statusCache ? statusCache.signals : {};
  sel.appendChild(new Option("Empty", "none", false, current === "none"));
  catalogue.signals.forEach(s => {
    if (s.key === "none") return;
    const src = sigSources[s.key] || (s.key.startsWith("me7_") ? "ME7 logger" : "");
    const o = new Option(s.name + (s.unit ? " (" + s.unit + ")" : "") + (src ? " - " + src : "") + (live[s.key] ? "  [live]" : ""), s.key, false, s.key === current);
    sel.appendChild(o);
  });
}

// ---------------------------------------------------------------------------
// ECU over WiFi (usb2ip, src/can2gauge_ECU.cpp): the link card on the CAN tab, and the ECU's channel lists for the
// page editor (GET /api/ecu/library, a group at a time). A page stores an ECU channel as "ecu:<kind>:<channel>".
// ---------------------------------------------------------------------------
let ecuLast = null;    // GET /api/ecu
const ecuLib = {};     // kind -> {id, rows, groups: [names], channels: [{c, k, n, u, d, lo, hi, g, v}]}
const ECU_KINDS = { 1: "Ignitron", 2: "MaxxECU", 3: "Link", 4: "Haltech", 5: "EMU PRO" };
// the ECU whose channels the page editor lists: the one usb2ip reads now, else the last one heard, else Ignitron
function ecuBrowseKind() {
  if (ecuLast && ecuLast.usb2ip && ECU_KINDS[ecuLast.usb2ip.kind]) return ecuLast.usb2ip.kind;
  if (ecuLast && ECU_KINDS[ecuLast.savedKind]) return ecuLast.savedKind;
  if (settingsCache && settingsCache.learnt && ECU_KINDS[settingsCache.learnt.ecuKind]) return settingsCache.learnt.ecuKind;
  return 1;
}
// fetch an ECU's channel list once, a group at a time
async function ecuLibraryLoad(kind) {
  if (ecuLib[kind]) return ecuLib[kind];
  try {
    const meta = await api("/api/ecu/library?kind=" + kind);
    const lib = { id: meta.id, rows: meta.rows, groups: meta.groups.map(g => g.name), channels: [] };
    for (let g = 0; g < meta.groups.length; g++) {
      if (!meta.groups[g].count) continue;
      for (let page = 0; page < 20; page++) {   // a page at a time: an EMU PRO group runs to 500+ channels
        const part = await api("/api/ecu/library?kind=" + kind + "&group=" + g + "&page=" + page);
        part.channels.forEach(ch => { ch.g = g; lib.channels.push(ch); });
        if (!part.more) break;
      }
    }
    ecuLib[kind] = lib;
  } catch (e) { return null; }
  return ecuLib[kind];
}
// the channel-list row of a page value "ecu:<kind>:<channel>" (null if it isn't one, or the list isn't loaded)
function ecuRowOf(value) {
  const m = /^ecu:(\d+):(\d+)$/.exec(value || "");
  if (!m || !ecuLib[m[1]]) return null;
  return ecuLib[m[1]].channels.find(ch => ch.c === Number(m[2])) || null;
}
// the picker for Source = ECU: the ECU (main value only), a Find box, and one list with the everyday values and every
// channel of that ECU, grouped. The second value and the slots use the main value's ECU.
function ecuPicker(host, value, second, mainHost, lbl, none) {
  const m = /^ecu:(\d+):(\d+)$/.exec(value || "");
  let kindBox = "";
  if (!second) {
    kindBox = `<div class="form-group" data-nosave><label>ECU</label><select class="sp-ekind">${Object.keys(ECU_KINDS).map(k => `<option value="${k}">${ECU_KINDS[k]}</option>`).join("")}</select></div>`;
  }
  host.innerHTML = kindBox +
    `<div class="form-group" data-nosave><label>Find</label><input type="text" class="sp-efind" placeholder="name or key"></div>` +
    `<div class="form-group"><label>${lbl || "Channel"}</label><select class="sp-sig"></select></div>`;
  if (!second) host.querySelector(".sp-ekind").value = String(m ? Number(m[1]) : ecuBrowseKind());
  const sel = host.querySelector(".sp-sig"), find = host.querySelector(".sp-efind");
  const kindOf = () => Number((mainHost || host).querySelector(".sp-ekind").value);
  let current = value || "";
  host._ecuFill = async () => { // (re)build the list for the ECU picked and the Find text
    const kind = kindOf(), want = find.value.trim().toLowerCase();
    const hit = (name, key) => !want || name.toLowerCase().includes(want) || key.toLowerCase().includes(want);
    let html = none;
    const everyday = catalogue.signals.filter(s => ECU_KEYS.includes(s.key) && hit(s.name, s.key));
    if (everyday.length) {
      html += `<optgroup label="Everyday values (any ECU)">` + everyday.map(s => `<option value="${s.key}">${esc(s.name)}${s.unit ? " (" + uShown(s.unit) + ")" : ""}</option>`).join("") + `</optgroup>`;
    }
    const lib = ecuLib[kind];
    if (!lib) {
      html += `<option value="" disabled>Loading the ${ECU_KINDS[kind]} channels...</option>`;
    } else {
      lib.groups.forEach((groupName, g) => {
        const rows = lib.channels.filter(ch => ch.g === g && hit(ch.n, ch.k));
        if (!rows.length) return;
        html += `<optgroup label="${esc(ECU_KINDS[kind] + ": " + groupName)}">` +
          rows.map(ch => `<option value="ecu:${kind}:${ch.c}">${esc(ch.n)}${ch.u ? " (" + esc(ch.u) + ")" : ""}${ch.v !== undefined ? "  [live]" : ""}</option>`).join("") + `</optgroup>`;
      });
    }
    sel.innerHTML = html;
    if (current && ![...sel.options].some(o => o.value === current)) { // keep the saved pick even when Find hides it
      const row = ecuRowOf(current);
      sel.appendChild(new Option(row ? row.n : current, current));
    }
    if (current) sel.value = current;
    if (!lib) { await ecuLibraryLoad(kind); if (ecuLib[kind]) host._ecuFill(); }
  };
  sel.addEventListener("change", () => { current = sel.value; });
  find.addEventListener("input", () => host._ecuFill());
  if (!second) {
    host.querySelector(".sp-ekind").addEventListener("change", () => { // another ECU: every picker of this page follows
      host.closest(".page-card").querySelectorAll(".pg-src").forEach(h => { if (h._ecuFill) h._ecuFill(); });
    });
  }
  host._ecuFill();
}

// ---------------------------------------------------------------------------
// 0.17.6: ME7 logger variables as gauge values. With the ME7 logger picked on the K-line tab, Source = K-line lists
// every variable in the .ecu (name, unit and scaling come from it) with a Find box, like the ECU over WiFi channels.
// A page's variables are logged as well as the .cfg's ("me7:<name>"). The .ecu and .cfg are read straight from the
// gauge (/download) and kept here.
// ---------------------------------------------------------------------------
let me7Ecu = { file: "", loading: null, failed: "", comm: {}, vars: [] }; // the .ecu read last: [Communication] +
// [Measurements]; failed = why the last read didn't work ("" = it did, or it's still reading)
let me7CfgNames = { file: "", names: [] };                    // the .cfg's [LogVariables] names

// the .ecu's own way of writing units, made plain the way the gauge does it ("°C" -> "C", "°KW" -> "deg")
function me7PlainUnit(unit) {
  if (unit.indexOf("°C") === 0) {
    return "C" + unit.slice(2);
  }
  if (unit.indexOf("°") === 0) {
    return "deg";
  }
  return unit.replace(/[^\x20-\x7E]/g, ""); // any other non-ASCII character left out
}
// one [Measurements] line split at its commas, but not at commas inside {braces}
function me7SplitLine(line) {
  const fields = [];
  let current = "";
  let inBraces = false;
  for (let i = 0; i < line.length; i++) {
    const letter = line[i];
    if (letter === "{") {
      inBraces = true;
    } else if (letter === "}") {
      inBraces = false;
    } else if (letter === "," && !inBraces) {
      fields.push(current.trim());
      current = "";
      continue;
    } else {
      current = current + letter;
    }
  }
  fields.push(current.trim());
  return fields;
}
// .ecu files are Latin-1 text (one byte per letter), not UTF-8: each byte is turned into its letter here, so it works
// in every browser (TextDecoder("windows-1252") isn't in all of them)
function me7Latin1(buffer) {
  const bytes = new Uint8Array(buffer);
  let text = "";
  for (let start = 0; start < bytes.length; start += 8192) {
    // a piece at a time: String.fromCharCode takes the bytes as separate arguments, and a whole file is too many
    text = text + String.fromCharCode.apply(null, bytes.subarray(start, start + 8192));
  }
  return text;
}
// reads the .ecu once per file name: LogSpeed / Connect and every variable. 0.17.6 fix: a read that fails isn't kept
// (the next call tries again - before, the list said "Reading..." for good and Find did nothing), it gives up after
// 20 s, and me7Ecu.failed says why.
async function me7EcuLoad(file) {
  if (!file || !file.toLowerCase().endsWith(".ecu")) {
    me7Ecu = { file: "", loading: null, failed: "", comm: {}, vars: [] };
    return me7Ecu;
  }
  if (me7Ecu.file === file && (me7Ecu.vars.length || me7Ecu.loading)) {
    if (me7Ecu.loading) {
      await me7Ecu.loading; // already on its way
    }
    return me7Ecu;
  }
  me7Ecu = { file: file, loading: null, failed: "", comm: {}, vars: [] };
  const target = me7Ecu;
  target.loading = (async () => {
    let text = "";
    const stopper = new AbortController(); // lets the 20 s timer below cancel the read
    const timer = setTimeout(() => stopper.abort(), 20000);
    try {
      const answer = await fetch("/download?path=" + encodeURIComponent(file), { signal: stopper.signal, cache: "no-store" });
      if (!answer.ok) {
        target.failed = "the gauge answered " + answer.status;
        return;
      }
      text = me7Latin1(await answer.arrayBuffer());
    } catch (e) {
      if (e.name === "AbortError") {
        target.failed = "no answer in 20 s";
      } else {
        target.failed = String(e.message || e);
      }
      return;
    } finally {
      clearTimeout(timer);
    }
    let section = "";
    const lines = text.split(/\r?\n/);
    for (let i = 0; i < lines.length; i++) {
      const line = lines[i].trim();
      if (!line || line[0] === ";") {
        continue;
      }
      if (line[0] === "[") {
        section = line.slice(1, line.indexOf("]")).toLowerCase();
        continue;
      }
      if (section === "communication") {
        const keyValue = /^(\w+)\s*=\s*([^;]*)/.exec(line); // "LogSpeed = 56000 ; comment"
        if (keyValue) {
          target.comm[keyValue[1].toLowerCase()] = keyValue[2].trim();
        }
      } else if (section === "measurements") {
        const f = me7SplitLine(line);
        if (f.length < 10) {
          continue;
        }
        target.vars.push({
          n: f[0],                  // the ME7 name ("nmot_w")
          a: f[1],                  // its alias ("EngineSpeed"), may be empty
          s: Number(f[3]) || 1,     // size in bytes
          u: me7PlainUnit(f[5]),    // unit
          sg: f[6] === "1",         // signed
          inv: f[7] === "1",        // inverse conversion: A / (raw - B)
          A: Number(f[8]),
          B: Number(f[9]),
          c: f[10] || "",           // the comment (German, from the map)
        });
      }
    }
    target.vars.sort((x, y) => x.n.localeCompare(y.n)); // A-Z by name
    if (!target.vars.length) {
      target.failed = "no [Measurements] variables in it";
    }
  })();
  await target.loading;
  target.loading = null;
  return target;
}
// the .cfg's [LogVariables] names, so the picker can list those first
async function me7CfgLoad(file) {
  if (me7CfgNames.file === file) {
    return me7CfgNames;
  }
  me7CfgNames = { file: file, names: [] };
  try {
    const answer = await fetch("/download?path=" + encodeURIComponent(file));
    if (answer.ok) {
      let section = "";
      (await answer.text()).split(/\r?\n/).forEach(function (raw) {
        const line = raw.split(";")[0].trim();
        if (!line) {
          return;
        }
        if (line[0] === "[") {
          section = line.toLowerCase();
        } else if (section === "[logvariables]") {
          me7CfgNames.names.push(line);
        }
      });
    }
  } catch (e) { /* no .cfg: just no "in your .cfg" group */ }
  return me7CfgNames;
}
function me7RowOf(value) { // "me7:nmot_w" -> its .ecu row, or undefined
  if (!value || !value.startsWith("me7:")) {
    return undefined;
  }
  const name = value.slice(4);
  return me7Ecu.vars.find(v => v.n === name);
}
// "EngineSpeed" -> "Engine Speed"; no alias -> the ME7 name
function me7Words(row) {
  if (!row.a) {
    return row.n;
  }
  return row.a.replace(/([a-z])([A-Z])/g, "$1 $2");
}
// a dial range for a variable: the ones people put on a gauge by name, else by unit, else what its bytes can hold
const ME7_RANGES = {
  nmot_w: [0, 8000], nmot: [0, 8000], tmot: [-40, 140], tans: [-40, 80], toel: [-40, 150],
  pvdkds_w: [0, 2500], ps_w: [0, 2500], pssol_w: [0, 2500], plsol_w: [0, 2500], pus_w: [0, 1100],
  rl_w: [0, 200], rl: [0, 200], rlsol_w: [0, 200], wped_w: [0, 100], wped: [0, 100], wdkba: [0, 100],
  zwout: [-10, 50], lamsoni_w: [0.6, 1.4], lamsbg_w: [0.6, 1.4], fr_w: [0.7, 1.3], ub: [8, 16],
  mshfm_w: [0, 300], ldtvm: [0, 100], vfzg: [0, 260],
};
const ME7_UNIT_RANGES = { rpm: [0, 8000], C: [-40, 150], mbar: [0, 2500], hPa: [0, 2500], V: [0, 16], deg: [-20, 60],
  "km/h": [0, 260], "g/s": [0, 300], ms: [0, 20], "-": [0, 2] };
function me7Range(row) {
  if (ME7_RANGES[row.n]) {
    return ME7_RANGES[row.n];
  }
  if (ME7_UNIT_RANGES[row.u]) {
    return ME7_UNIT_RANGES[row.u];
  }
  if (row.u.indexOf("%") === 0) {
    return [0, 100];
  }
  // what the raw bytes can hold, through the .ecu's conversion
  let rawLow = 0;
  let rawHigh = 255;
  if (row.s === 2) {
    rawHigh = 65535;
  }
  if (row.sg) {
    rawLow = -(rawHigh + 1) / 2;
    rawHigh = (rawHigh - 1) / 2;
  }
  let low = row.A * rawLow - row.B;
  let high = row.A * rawHigh - row.B;
  if (row.inv) {
    return [0, 2]; // A / (raw - B): no sensible range from the bytes
  }
  if (low > high) {
    const swap = low;
    low = high;
    high = swap;
  }
  return [Math.round(low), Math.round(high)];
}
// decimals by unit, the same rule as the gauge (userSigDecimals)
function me7Decimals(row) {
  if (["rpm", "mbar", "hPa", "C", "km/h"].includes(row.u)) {
    return 0;
  }
  if (row.u === "-") {
    return 2;
  }
  if (row.u.indexOf("%") === 0 || ["V", "deg", "g/s", "ms"].includes(row.u)) {
    return 1;
  }
  const step = Math.abs(row.A);
  if (row.inv) {
    return 2;
  }
  if (step >= 1) {
    return 0;
  }
  if (step >= 0.1) {
    return 1;
  }
  return 2;
}
function me7OptionText(row) { // "Engine Speed (rpm) - nmot_w"
  let text = me7Words(row);
  if (row.u && row.u !== "-") {
    text = text + " (" + row.u + ")";
  }
  if (row.a) {
    text = text + " - " + row.n;
  }
  return text;
}
// the picker for one page value when the ME7 logger is picked: Find + every .ecu variable (the .cfg's first)
function me7Picker(host, value, lbl, none) {
  host.innerHTML =
    `<div class="form-group" data-nosave><label>Find</label><input type="text" class="sp-mfind" placeholder="name, alias or unit"></div>` +
    `<div class="form-group"><label>${lbl || "Variable"}</label><select class="sp-sig sp-me7"></select></div>` +
    (lbl ? "" : `<div class="help sp-mhelp" style="flex-basis:100%"></div>`); // once per page, under the main value
  const sel = host.querySelector(".sp-me7");
  const find = host.querySelector(".sp-mfind");
  let current = value || "";
  host._me7Fill = async function () {
    const ecu = await me7EcuLoad(settingsCache.me7.ecuFile);
    const cfg = await me7CfgLoad(settingsCache.me7.cfgFile);
    const want = find.value.trim().toLowerCase();
    const words = want.split(/\s+/).filter(Boolean);
    function hit(row) { // every word typed in the name, alias, unit or comment
      const text = (row.n + " " + row.a + " " + me7Words(row) + " " + row.u + " " + row.c).toLowerCase();
      return words.every(w => text.includes(w));
    }
    let html = none;
    if (!ecu.vars.length) {
      // 0.17.6 fix: says why when the read failed (it said "Reading..." for good); typing in Find tries again
      let why = "No .ecu picked - K-line tab";
      if (settingsCache.me7.ecuFile && ecu.failed) {
        why = "Couldn't read " + settingsCache.me7.ecuFile + ": " + ecu.failed + " - type in Find to try again";
      } else if (settingsCache.me7.ecuFile) {
        why = "Reading " + settingsCache.me7.ecuFile + "...";
      }
      html += `<option value="" disabled>${esc(why)}</option>`;
    } else {
      // 0.20.0: only the logged variables (the ones ticked in the K-line / ME7 tab) - that's where the full .ecu
      // list and its checkboxes live now, so the gauge sources stay a short, relevant list
      const logged = ecu.vars.filter(row => cfg.names.includes(row.n) && hit(row));
      if (logged.length) {
        html += `<optgroup label="Logged ME7 variables">` + logged.map(row => `<option value="me7:${esc(row.n)}">${esc(me7OptionText(row))}</option>`).join("") + `</optgroup>`;
      } else {
        html += `<option value="" disabled>${cfg.names.length ? "No logged variable matches" : "No variables logged yet - tick some in the K-line / ME7 tab"}</option>`;
      }
    }
    sel.innerHTML = html;
    if (current && ![...sel.options].some(o => o.value === current)) { // keep the saved pick even when Find hides it
      let text = current;
      const row = me7RowOf(current);
      if (row) {
        text = me7OptionText(row);
      } else if (current.startsWith("kl:")) {
        text = "K-line block " + current.split(":")[2] + " field " + current.split(":")[3] + " (no value while the ME7 logger runs)";
      }
      sel.appendChild(new Option(text, current));
    }
    if (current) {
      sel.value = current;
    }
    const help = host.querySelector(".sp-mhelp");
    if (help) {
      help.textContent = "Variables from " + (ecu.file || "the .ecu") + " (" + ecu.vars.length + "). A page's variables are logged as well as the .cfg's: every 3 more cost about 10 ms a sample.";
    }
  };
  sel.addEventListener("change", () => { current = sel.value; });
  find.addEventListener("input", () => host._me7Fill());
  host._me7Fill();
}

// 0.17.6: Find for a plain K-line (KWP1281) page value: type part of a field's name, pick it and Block / Field are set
async function klFindForPicker(host, mainHost) {
  const find = host.querySelector(".sp-kfind");
  const matchSel = host.querySelector(".sp-kmatch");
  const typed = find.value.trim().toLowerCase();
  matchSel.innerHTML = "";
  if (!typed) {
    matchSel.parentElement.style.display = "none";
    return;
  }
  matchSel.parentElement.style.display = "";
  const names = await klLabelsFor(klAddrOf(mainHost || host));
  if (!names) {
    matchSel.appendChild(new Option("No names yet - read the module once (K-line mode or this page)", ""));
    return;
  }
  const words = typed.split(/\s+/);
  let count = 0;
  matchSel.appendChild(new Option("Pick a match...", ""));
  Object.keys(names).forEach(function (group) {
    const block = names[group];
    for (let field = 1; field <= 4; field++) {
      const name = block.f[field];
      if (!name || count >= 40) {
        continue;
      }
      const text = (name + " " + block.t).toLowerCase();
      if (words.every(w => text.includes(w))) {
        matchSel.appendChild(new Option("Block " + group + " / " + field + ": " + name, group + ":" + field));
        count++;
      }
    }
  });
  if (count === 0) {
    matchSel.innerHTML = "";
    matchSel.appendChild(new Option("Nothing called that in " + klKnown[klAddrOf(mainHost || host)], ""));
  }
}
function klFindPickForPicker(host, mainHost) { // a match picked: its block and field go into the boxes
  const matchSel = host.querySelector(".sp-kmatch");
  if (!matchSel.value) {
    return;
  }
  const parts = matchSel.value.split(":");
  host.querySelector(".sp-blk").value = parts[0];
  host.querySelector(".sp-fld").value = parts[1];
  host.querySelector(".sp-kfind").value = "";
  matchSel.parentElement.style.display = "none";
  klNames(host, mainHost);
  host.querySelector(".sp-fld").dispatchEvent(new Event("change", { bubbles: true })); // saves, like picking it by hand
}

// 0.17.6: the K-line tab's ECU address and Baud: with the ME7 logger they're the .ecu's Connect= and LogSpeed= lines
// (shown, not settings - the address / baud boxes stay saved for KWP1281 / KWP2000)
// 0.20.0: the ME7 logger's variable checkboxes. Tick a variable to log it, untick to drop it; the ticked set is the
// .cfg's [LogVariables]. Saving writes the .cfg and reconnects, and the logged variables then show in the gauge-page
// sources. It starts from the .cfg's own variables (ticked), then the user adds / removes.
let me7CfgSel = { file: null, names: [] }; // the ticked variable names, per .cfg file
let me7CfgSaveTimer = null;
async function renderMe7CfgPicker() {
  const box = $("me7CfgPick");
  if (!settingsCache || Number(settingsCache.kline.protocol) !== 3) { box.style.display = "none"; return; }
  const ecu = await me7EcuLoad(settingsCache.me7.ecuFile);
  const cfg = await me7CfgLoad(settingsCache.me7.cfgFile);
  if (!ecu.vars.length) { box.style.display = "none"; return; } // no .ecu read: nothing to tick
  // first time for this .cfg: start from its variables; after that keep the user's ticks
  if (me7CfgSel.file !== settingsCache.me7.cfgFile) {
    me7CfgSel = { file: settingsCache.me7.cfgFile, names: cfg.names.slice() };
  }
  box.style.display = "";
  const selected = me7CfgSel.names, full = selected.length >= 32;
  $("me7CfgCount").textContent = "(" + selected.length + " / 32)";
  const words = $("me7CfgFind").value.trim().toLowerCase().split(/\s+/).filter(Boolean);
  const rows = [];
  if (words.length) {
    // searching: filter the whole .ecu down to what matches (ticked or not), so the list narrows like any search
    for (let i = 0; i < ecu.vars.length && rows.length < 80; i++) {
      const row = ecu.vars[i];
      const text = (row.n + " " + row.a + " " + me7Words(row) + " " + row.u + " " + row.c).toLowerCase();
      if (words.every(w => text.includes(w))) { rows.push({ name: row.n, text: me7OptionText(row), on: selected.includes(row.n) }); }
    }
  } else {
    // not searching: just the logged set (ticked), nicely named from the .ecu if it has them
    selected.forEach(name => {
      const row = ecu.vars.find(v => v.n === name);
      rows.push({ name: name, text: row ? me7OptionText(row) : name, on: true });
    });
  }
  $("me7CfgList").innerHTML = rows.map(r =>
    `<label class="me7cfg-row"><input type="checkbox" data-name="${esc(r.name)}"${r.on ? " checked" : ""}${!r.on && full ? " disabled" : ""}> <span>${esc(r.text)}</span></label>`
  ).join("") || `<div class="help">${words.length ? "No variable matches that." : "Type above to find variables to log."}</div>`;
}
function me7CfgToggle(name, on) {
  const i = me7CfgSel.names.indexOf(name);
  if (on && i < 0 && me7CfgSel.names.length < 32) { me7CfgSel.names.push(name); }
  if (!on && i >= 0) { me7CfgSel.names.splice(i, 1); }
  renderMe7CfgPicker();
  clearTimeout(me7CfgSaveTimer);
  me7CfgSaveTimer = setTimeout(saveMe7Cfg, 800); // save + reconnect once the ticking settles
}
async function saveMe7Cfg() {
  const r = await api("/api/me7/cfg", "POST", { vars: me7CfgSel.names });
  me7CfgNames = { file: "", names: [] }; // the gauge-page picker re-reads the .cfg next time
  toast((r && r.status) || "ME7 variables saved");
}

async function renderKlFromEcu() {
  // 0.19.0: only the section for the chosen protocol is shown - the ME7 logger card with the ME7 logger, the K-line
  // mode card otherwise - so the page isn't cluttered with both.
  const isMe7 = Number($("kline.protocol").value) === 3;
  $("me7Card").style.display = isMe7 ? "" : "none";
  $("klModeCard").style.display = isMe7 ? "none" : "";
  renderMe7CfgPicker(); // 0.20.0: the ME7 logged-variable checkboxes (hides itself when not ME7)
  if (!isMe7) {
    $("klAddrBox").style.display = "";
    $("klEcuAddrBox").style.display = "none";
    $("klBaudBox").style.display = "";
    $("klEcuBaudBox").style.display = "none";
    $("klFastObdBox").style.display = "";
    return;
  }
  $("klAddrBox").style.display = "none";
  $("klEcuAddrBox").style.display = "";
  $("klBaudBox").style.display = "none";
  $("klEcuBaudBox").style.display = "";
  $("klFastObdBox").style.display = "none"; // Fast OBD is KWP2000's: the ME7 logger sets its own timing with the ECU
  const file = $("me7.ecuFile").value;
  $("klEcuAddr").value = "Reading the .ecu...";
  $("klEcuBaud").value = "Reading the .ecu...";
  const ecu = await me7EcuLoad(file);
  if (ecu.failed) {
    $("klEcuAddr").value = "Couldn't read the .ecu";
    $("klEcuBaud").value = ecu.failed;
    return;
  }
  // Connect = SLOW-0x11 (5-baud wake-up to 0x11) or FAST-0x10 (fast init to 0x10)
  const connect = /^(SLOW|FAST)-(0x[0-9a-f]+)/i.exec(ecu.comm.connect || "");
  if (connect) {
    let how = "slow init";
    if (connect[1].toUpperCase() === "FAST") {
      how = "fast init";
    }
    $("klEcuAddr").value = connect[2] + " (" + how + ")";
  } else {
    $("klEcuAddr").value = "0x01 (the .ecu has no Connect)"; // the logger's own fallback (me7Connect)
  }
  if (ecu.comm.logspeed) {
    $("klEcuBaud").value = ecu.comm.logspeed + " (LogSpeed)";
  } else {
    $("klEcuBaud").value = "10400 (the .ecu has no LogSpeed)";
  }
}
// the link card on the CAN tab (GET /api/ecu)
async function pollEcu() {
  let r;
  try { r = await api("/api/ecu"); } catch (e) { return; }
  ecuLast = r;
  const u = r.usb2ip;
  let link;
  if (!r.wireless) link = "<b>Off</b> - usb2ip isn't listened for";
  else if (!u) link = "<b>No usb2ip heard yet</b>";
  else {
    link = (r.heard ? "<b>Heard</b>" : "<b>Not heard</b> for " + Math.round(r.ageMs / 1000) + " s") + " • " + esc(u.kindName) +
      (u.ecu ? " (" + esc(u.ecu) + ")" : "") + (u.source ? " • " + esc(u.source) : "") + " • usb2ip " + esc(u.fw) + " • " + u.rssi + " dBm";
  }
  $("ecuLink").innerHTML = link;
  let chips = "";
  if (u) {
    const chip = (text, cls) => `<span class="hdx-chip ${cls || ""}">${esc(text)}</span>`;
    let lossPct = 0;
    if (u.packets + u.lost > 0) lossPct = Math.round(u.lost * 1000 / (u.packets + u.lost)) / 10;
    chips += chip(u.live ? "Reading the ECU" : "ECU not read", u.live ? "on" : "warn");
    if (u.fromPc) chips += chip("From the PC session (USB/IP)", "on");
    if (u.libraryMismatch) chips += chip("Channel lists differ (gauge " + u.ourLibrary + ", usb2ip " + u.theirLibrary + "): update can2gauge or usb2ip", "bad");
    else chips += chip(u.rowsReceiving + " of " + u.rowsTotal + " channels receiving", u.rowsReceiving ? "on" : "");
    chips += chip(u.packets + " packets, " + lossPct + " % lost in the air", lossPct > 5 ? "warn" : "");
    if (r.myChannel) chips += chip("Gauge on WiFi channel " + r.myChannel);
  }
  $("ecuChips").innerHTML = chips;
  $("ecuChips").classList.toggle("stale", !!u && !r.heard);
  const vals = (r.values || []).map(x => `<div class="gauge${x.used ? "" : " stale"}"><div class="gauge-label">${esc(x.name)}</div><div class="gauge-value sig-live">${esc(fmt(Number(x.value), Math.abs(x.value) < 10 ? 2 : 1))}</div><div class="gauge-unit">${esc(uShown(x.unit))}${x.used ? "" : " (other source)"}</div></div>`).join("");
  const valsHtml = vals || '<div class="help">Nothing yet.</div>';
  if ($("ecuValues").innerHTML !== valsHtml) $("ecuValues").innerHTML = valsHtml;
  let follow;
  if (!r.wireless) follow = "WiFi link off.";
  else if (r.mac) follow = "Following " + (u && u.source ? u.source + " " : "") + "(" + r.mac + ")";
  else follow = "Not following a usb2ip yet - the first one heard is followed.";
  $("ecuFollow").textContent = follow;
  $("ecuForget").style.display = r.mac ? "" : "none";
  const oh = r.others.map(x => `<div class="found-row"><span>${esc(x.name || "(no name yet)")} <span class="help">${esc(x.mac)}  ${x.rssi} dBm</span></span><button class="btn-secondary" data-mac="${esc(x.mac)}">Follow</button></div>`).join("");
  if ($("ecuOthers").innerHTML !== oh) $("ecuOthers").innerHTML = oh;
}

// ---------------------------------------------------------------------------
// gauge page sources (Gauges tab): CAN (the gauge's decode or a DBC signal), K-line (a measuring-block field), OBD (a
// PID) or this gauge. Values go to the gauge as "key" / "dbc:<id>:<signal>" / "kl:<addr>:<block>:<field>" / "pid:<n>".
// ---------------------------------------------------------------------------
const GAUGE_KEYS = ["boardTemp", "uptime", "lights", "output", "outFault", "tcuSpeed", "oilLevel", "me7_1", "me7_2", "me7_3", "me7_4", "me7_5", "me7_6", "me7_7", "me7_8"];
// 0.17.6: "This gauge" is now "Specifics": the gauge's own values and the OEM ones it decodes itself (DSG speed, oil
// temperature / level). ME7 1-8 left the list - a gauge picks any ME7 variable under Source = K-line now. GAUGE_KEYS
// above still keeps all of these out of the CAN list.
const SPECIFICS_KEYS = ["boardTemp", "uptime", "lights", "output", "outFault", "tcuSpeed", "oiltemp", "oilLevel"];
// 0.34.1: the title a Quad / Triple page gets: where its readings come from (it shows several, so one reading's name -
// "ADC8" - read wrongly). An ECU page uses the ECU's name when known (IGNITRON, HALTECH ...).
function sourceTitle(source, value) {
  if (source === "ecu") {
    const match = /^ecu:(\d+):/.exec(value || "");
    let kind = ecuBrowseKind();
    if (match) kind = Number(match[1]);
    if (ECU_KINDS[kind]) return ECU_KINDS[kind].toUpperCase();
    return "ECU";
  }
  if (source === "adc") return "ADC";
  if (source === "kline") {
    if ((value || "").startsWith("me7_")) return "ME7";
    return "K-LINE";
  }
  if (source === "obd") return "OBD";
  if (source === "diag") return "DIAGNOSTICS";
  if (source === "gauge") return "GAUGE";
  if (source === "car") return "CAR"; // 0.37.0
  return "CAN";
}
// 0.37.0: "Car" = what Scan My Car found (carscan.js): one list instead of knowing which of CAN / K-line / OBD /
// Diagnostics a value comes from
const SOURCES = [["car", "Car"], ["can", "CAN"],["kline", "K-line"], ["obd", "OBD"], ["adc", "ADC (adc2can)"], ["ecu", "ECU (usb2ip)"], ["diag", "Diagnostics (UDS / TP2.0)"], ["gauge", "Specifics"]];
// 0.37.0: the Source list a page offers. Basic: Car, the adc2can and usb2ip when one is heard, and Specifics (plus the
// page's own source if it's another one); Advanced: every source.
function pageSourceList(current) {
  if (showAdvanced) {
    return SOURCES;
  }
  const signals = (statusCache && statusCache.signals) || {};
  const adcHeard = Object.keys(signals).some(function (key) { return /^adc\d+$/.test(key); });
  const ecuHeard = !!(statusCache && statusCache.ecu && statusCache.ecu.heard);
  return SOURCES.filter(function (row) {
    const key = row[0];
    if (key === current || key === "car" || key === "gauge") return true;
    if (key === "adc") return adcHeard;
    if (key === "ecu") return ecuHeard;
    return false;
  });
}
// the everyday values usb2ip sends, as the gauge's own signals (src/can2gauge_ECU.cpp valueMap)
const ECU_KEYS = ["rpm", "speed", "coolant", "iat", "map", "boost", "pedal", "throttle", "load", "lambda", "lambdaTarget", "timing",
  "knockRetard", "injDuty", "batt", "oiltemp", "oilPressure", "fuelPressure", "egt", "ethanol", "gear"];
// [pid, name, gauge signal key, unit, gauge min, gauge max]. The 14 below are the fallback; init() replaces the list with
// the gauge's own table (every standard value up to PID 0x60, from /api/signals)
let OBD_PIDS = [[0x0C, "RPM", "rpm"], [0x0D, "Speed", "speed"], [0x05, "Coolant", "coolant"], [0x0B, "Intake pressure (MAP)", "map"], [0x0F, "Intake air", "iat"],
  [0x11, "Throttle valve", "throttle"], [0x04, "Engine load", "load"], [0x10, "Air mass (MAF)", "maf"], [0x0E, "Timing advance", "timing"], [0x42, "Battery", "batt"],
  [0x2F, "Fuel level", "fuel"], [0x5C, "Oil temperature", "oiltemp"], [0x44, "Lambda", "lambda"], [0x46, "Outside air", "ambient"]];
const KL_MODULES = [[1, "01 Engine"], [2, "02 Gearbox"], [3, "03 ABS"], [8, "08 Climate"], [0x15, "15 Airbag"], [0x16, "16 Steering wheel"], [0x17, "17 Instruments"],
  [0x19, "19 Gateway"], [0x22, "22 AWD (Haldex)"], [0x25, "25 Immobiliser"], [0x35, "35 Central locking"], [0x37, "37 Navigation"], [0x46, "46 Comfort"],
  [0x55, "55 Headlights"], [0x56, "56 Radio"], [0x65, "65 Tyre pressure"], [0x76, "76 Park assist"],
  // 0.34.0: MQB-era addresses the Diagnostics / Service tabs know the UDS ids of
  [0x09, "09 Central electrics"], [0x44, "44 Steering assist"], [0x53, "53 Parking brake"], [0x5F, "5F Infotainment"],
  [0x75, "75 Emergency call"], [0x8C, "8C Hybrid battery"], [0x51, "51 Electric drive"], [0xC6, "C6 Charger"]];
const isUser = (k) => /^user\d+$/.test(k), isAdc = (k) => /^adc\d+$/.test(k);
const canKeys = () => catalogue.signals.filter(s => s.key !== "none" && !isUser(s.key) && !isAdc(s.key) && !GAUGE_KEYS.includes(s.key));
let dbcMsgs = [], klKnown = {}, klLabels = {}, obdSupported = null; // obdSupported: the PIDs the car reported (Bluetooth OBD)
async function loadSourceData() {
  try { adcLast = await api("/api/adc"); } catch (e) { /* no adc2can */ }
  try { ecuLast = await api("/api/ecu"); } catch (e) { /* firmware without the ECU link */ }
  await ecuLibraryLoad(ecuBrowseKind());
  try { const e = await api("/api/elm"); window.lastElmStatus = e; obdSupported = e.supported && e.supported.length ? e.supported : null; } catch (e) { /* no adapter */ }
  // 0.28.0: every DBC slot's messages (car, gearbox, extra) in one list for the page editor
  dbcMsgs = [];
  for (let slot = 1; slot <= 3; slot++) {
    try { const r = await fetch("/api/can/dbc?slot=" + slot); if (r.ok) dbcMsgs = dbcMsgs.concat(parseC2d(await r.text())); } catch (e) { /* empty slot */ }
  }
  dbcMsgs.sort((a, b) => a.id - b.id);
  try {
    const r = await fetch("/lbl/known.txt");
    if (r.ok) (await r.text()).split("\n").forEach(l => { const f = l.split("\t"); if (f.length > 1 && f[1]) klKnown[parseInt(f[0], 16)] = f[1]; });
  } catch (e) { /* none yet */ }
}
function parseC2d(text) {
  const msgs = []; let cur = null;
  text.split("\n").forEach(l => {
    const f = l.split("\t");
    if (f[0] === "M") { cur = { id: Number(f[1]), name: f[3], sigs: [] }; msgs.push(cur); }
    else if (f[0] === "S" && cur && Number(f[8]) !== -2) cur.sigs.push({ name: f[1], unit: f[7] });
  });
  return msgs.sort((a, b) => a.id - b.id);
}
async function klLabelsFor(addr) { // the module's label file (as the gauge last picked it): {block: {t: title, f: [, name1..name4]}}
  const file = klKnown[addr];
  if (!file) return null;
  if (!klLabels[file]) {
    klLabels[file] = {};
    try {
      (await (await fetch("/api/lbl?file=" + encodeURIComponent(file))).text()).split("\n").forEach(l => {
        const f = l.split("\t"); if (f.length < 3) return;
        const g = Number(f[0]), k = Number(f[1]), n = (f[2] + (f[3] ? " " + f[3] : "")).trim();
        const b = klLabels[file][g] = klLabels[file][g] || { t: "", f: [] };
        if (k === 0) b.t = n; else b.f[k] = n;
      });
    } catch (e) { /* no names */ }
  }
  return klLabels[file];
}
const hex3 = (id) => "0x" + (id & 0x1FFFFFFF).toString(16).toUpperCase().padStart(id > 0x7FF ? 8 : 3, "0");
// draws the picker for one page value into host; second = the Dual / Haldex second value ("none" allowed)
function srcPicker(host, source, value, second, mainHost, label) {
  host.dataset.source = source;
  const none = second ? '<option value="none">None</option>' : "";
  const lbl = label || (second ? "Second value" : "");
  if (source === "car") { // 0.37.0: what Scan My Car found (carscan.js)
    carPicker(host, value, second, lbl, none);
    return;
  }
  if (source === "can") {
    const m = /^dbc:(\d+):(.*)$/.exec(value || "");
    // "Decoded by the gauge" = the gauge's own signals from the CAN profile; a DBC message = any signal of the loaded DBC
    let builtIn = "Decoded by the gauge (built-in signals)";
    let builtInHelp = "<b>Decoded by the gauge</b>: the signals the gauge reads itself with the CAN profile set on the CAN tab - no DBC needed. Or pick a message from the loaded DBC to use any of its signals.";
    if (!dbcMsgs.length) {
      builtIn = "Decoded by the gauge (no DBC loaded)";
      builtInHelp = "<b>Decoded by the gauge</b>: the signals the gauge reads itself with the CAN profile set on the CAN tab. Load a DBC on the CAN tab to pick any of its signals here instead.";
    }
    host.innerHTML = `<div class="form-group"><label>${lbl ? lbl + ": message" : "Message"}</label><select class="sp-msg"><option value="">${builtIn}</option>${dbcMsgs.map(x => `<option value="${x.id}">${hex3(x.id)} ${esc(x.name)}</option>`).join("")}</select></div>
      <div class="form-group"><label>${lbl || "Signal"}</label><select class="sp-sig"></select></div>` +
      (second || label ? "" : `<div class="help" style="flex-basis:100%">${builtInHelp}</div>`); // once per page, under the main value
    const msg = host.querySelector(".sp-msg"), sig = host.querySelector(".sp-sig");
    const fill = (cur) => {
      const d = dbcMsgs.find(x => String(x.id) === msg.value);
      sig.innerHTML = none + (d ? d.sigs.map(s => `<option value="dbc:${d.id}:${esc(s.name)}">${esc(s.name)}${s.unit ? " (" + esc(s.unit) + ")" : ""}</option>`).join("")
        : canKeys().map(s => `<option value="${s.key}">${esc(s.name)}${s.unit ? " (" + uShown(s.unit) + ")" : ""}${sigSources[s.key] ? " - " + sigSources[s.key] : ""}</option>`).join(""));
      if (cur && [...sig.options].some(o => o.value === cur)) sig.value = cur;
    };
    msg.value = m ? m[1] : "";
    if (m && msg.value !== m[1]) msg.appendChild(new Option(hex3(Number(m[1])) + " (not in the DBC on the gauge)", m[1], true, true));
    fill(value);
    msg.addEventListener("change", () => { fill(""); sig.dispatchEvent(new Event("change", { bubbles: true })); });
    if (m && sig.value !== value) sig.appendChild(new Option(m[2], value, true, true));
  } else if (source === "kline") {
    if (settingsCache && settingsCache.kline.protocol == 3) { // 0.17.6: the ME7 logger: its .ecu variables, not module / block
      host.dataset.me7 = "1";
      me7Picker(host, value, lbl, none);
      return;
    }
    host.dataset.me7 = "";
    const m = /^kl:(\d+):(\d+):(\d+)$/.exec(value || "") || [null, 1, 1, 1];
    host.innerHTML = (second ? "" : `<div class="form-group"><label>Module</label><select class="sp-mod">${KL_MODULES.map(([a, n]) => `<option value="${a}">${n}</option>`).join("")}<option value="custom">Other address...</option></select></div>
      <div class="form-group narrow sp-addrbox" style="display:none"><label>Address (hex)</label><input type="text" class="sp-addr"></div>`) +
      (second ? `<div class="form-group"><label>${lbl}</label><select class="sp-use"><option value="none">None</option><option value="kl">Another field (same module)</option></select></div>` : "") +
      // 0.17.6: Find - part of a field's name from the module's label file; picking a match sets Block and Field
      `<div class="form-group" data-nosave><label>Find</label><input type="text" class="sp-kfind" placeholder="part of a field's name"></div>
      <div class="form-group" data-nosave style="display:none"><label>Matches</label><select class="sp-kmatch"></select></div>` +
      `<div class="form-group narrow"><label>Block</label><input type="number" class="sp-blk" min="1" max="255" value="${m[2]}"></div>
      <div class="form-group"><label>Field</label><select class="sp-fld">${[1, 2, 3, 4].map(f => `<option value="${f}">${f}</option>`).join("")}</select></div>
      <div class="help sp-title" style="flex-basis:100%"></div>`;
    if (!second) {
      const mod = host.querySelector(".sp-mod"), known = KL_MODULES.some(([a]) => a === Number(m[1]));
      mod.value = known ? String(m[1]) : "custom";
      host.querySelector(".sp-addr").value = Number(m[1]).toString(16).toUpperCase().padStart(2, "0");
      host.querySelector(".sp-addrbox").style.display = known ? "none" : "";
      mod.addEventListener("change", () => { host.querySelector(".sp-addrbox").style.display = mod.value === "custom" ? "" : "none"; klNames(host); });
    } else host.querySelector(".sp-use").value = value && value.startsWith("kl:") ? "kl" : "none";
    host.querySelector(".sp-fld").value = String(m[3]);
    host.querySelector(".sp-blk").addEventListener("input", () => klNames(host, mainHost));
    host.querySelector(".sp-kfind").addEventListener("input", () => klFindForPicker(host, mainHost));
    host.querySelector(".sp-kmatch").addEventListener("change", () => klFindPickForPicker(host, mainHost));
    klNames(host, mainHost);
  } else if (source === "obd") {
    const pid = /^pid:(\d+)$/.exec(value || "");
    // the car's own PIDs first (once an adapter has read its list), then the rest marked as not supported
    const has = (p) => !obdSupported || obdSupported.includes(p);
    // 0.37.0: the PIDs the car doesn't report sit in their own group at the bottom
    const pidOption = ([p, n]) => `<option value="pid:${p}">${esc(n)} (PID ${p.toString(16).toUpperCase().padStart(2, "0")})</option>`;
    const reported = OBD_PIDS.filter(x => has(x[0])).map(pidOption).join("");
    const notReported = OBD_PIDS.filter(x => !has(x[0])).map(pidOption).join("");
    let notReportedGroup = "";
    if (notReported) notReportedGroup = `<optgroup label="Not reported by this car">${notReported}</optgroup>`;
    host.innerHTML = `<div class="form-group"><label>${lbl || "PID"}</label><select class="sp-pid">${none}${reported}${notReportedGroup}</select></div>`;
    const pidSel = host.querySelector(".sp-pid");
    if (pid) pidSel.value = value;
    if (pid && pidSel.value !== value) {
      pidSel.appendChild(new Option("PID " + Number(pid[1]).toString(16).toUpperCase().padStart(2, "0"), value, true, true)); // saved PID not in the list: kept
    } else if (!pid && !second) {
      // 0.25.3: nothing chosen yet: "Pick a PID" on top, not silently the first entry (Engine load, PID 04 - how the
      // Tiguan's RPM and BOOST pages both ended up reading engine load)
      pidSel.insertBefore(new Option("Pick a PID", "none"), pidSel.firstChild);
      pidSel.value = "none";
    }
  } else if (source === "adc") { // the adc2can sensors, by the names they have now
    // 0.36.6: only the boards heard (and the page's own sensor, so it never vanishes), every one named the same way
    let options = "";
    const sensors = catalogue.signals.filter(s => isAdc(s.key));
    for (let i = 0; i < sensors.length; i++) {
      if (adcSensorFound(sensors[i].key) || sensors[i].key === value) {
        options = options + `<option value="${sensors[i].key}">${esc(adcSensorText(i))}</option>`;
      }
    }
    if (options === "") {
      options = '<option value="none" disabled>No adc2can heard</option>';
    }
    host.innerHTML = `<div class="form-group"><label>${lbl || "Sensor"}</label><select class="sp-sig">${none}${options}</select></div>`;
    const sensorSel = host.querySelector(".sp-sig");
    if (isAdc(value || "")) sensorSel.value = value;
    if (!second && !isAdc(value || "")) {
      // 0.30.18: nothing chosen yet (the page was just moved to ADC): "Pick a Sensor" on top, not silently sensor 1 -
      // the page kept its old label and range (OIL TEMP) while reading sensor 1, until another sensor was picked
      sensorSel.insertBefore(new Option("Pick a Sensor", "none"), sensorSel.firstChild);
      sensorSel.value = "none";
    }
  } else if (source === "ecu") {
    ecuPicker(host, value, second, mainHost, lbl, none);
  } else if (source === "diag") { // the Diagnostics tab's values, "diag:<1-12>"
    const vals = (settingsCache.vdiag && settingsCache.vdiag.values) || [];
    const opts = vals.map((x, i) => {
      if (!x.kind) return "";
      const where = x.kind === 2 ? "block " + x.id + " field " + x.field : "DID " + vdHex(x.id, 4);
      return `<option value="diag:${i + 1}">${i + 1}: ${esc(x.name || "(no name)")} - ${where}${x.unit ? " (" + esc(x.unit) + ")" : ""}</option>`;
    }).join("");
    host.innerHTML = `<div class="form-group"><label>${lbl || "Value"}</label><select class="sp-sig">${none}${opts || '<option value="none">No values set up - Diagnostics tab</option>'}</select></div>`;
    if (value && value.startsWith("diag:")) host.querySelector(".sp-sig").value = value;
  } else { // "Specifics" (0.17.6, was "This gauge")
    host.innerHTML = `<div class="form-group"><label>${lbl || "Signal"}</label><select class="sp-sig">${none}${catalogue.signals.filter(s => SPECIFICS_KEYS.includes(s.key)).map(s => `<option value="${s.key}">${esc(s.name)}${s.unit ? " (" + uShown(s.unit) + ")" : ""}</option>`).join("")}</select></div>`;
    const sel = host.querySelector(".sp-sig");
    if (value && value.startsWith("me7_") && ![...sel.options].some(o => o.value === value)) {
      // a page saved with ME7 1-8 before 0.17.6 keeps working; picking again moves it on
      sel.appendChild(new Option("ME7 " + value.slice(4) + " (old - pick the variable under Source K-line)", value));
    }
    if (value) sel.value = value;
  }
}
// 0.25.3: when a page's Source changes, the same reading in the new source if there's an exact match (the gauge's
// "rpm" <-> OBD PID 0C), else "" (the picker then asks). Only exact matches: boost isn't MAP (absolute, in mbar)
function sameReadingIn(source, oldValue) {
  if (source === "car" && oldValue && oldValue !== "none") return oldValue; // 0.37.0: Car shows any reading
  if (source === "obd") {
    const match = OBD_PIDS.find(x => x[2] && x[2] === oldValue);
    if (match) return "pid:" + match[0];
  } else if (source === "can") {
    const pid = /^pid:(\d+)$/.exec(oldValue || "");
    const match = pid && OBD_PIDS.find(x => x[0] === Number(pid[1]));
    if (match && canKeys().some(s => s.key === match[2])) return match[2];
  }
  return "";
}
function klAddrOf(host) {
  const mod = host.querySelector(".sp-mod");
  return mod.value === "custom" ? parseInt(host.querySelector(".sp-addr").value, 16) || 1 : Number(mod.value);
}
async function klNames(host, mainHost) { // block title + field names from the module's label file
  const addr = klAddrOf(mainHost || host), names = await klLabelsFor(addr), g = Number(host.querySelector(".sp-blk").value);
  const b = names && names[g], fld = host.querySelector(".sp-fld");
  [...fld.options].forEach(o => { o.textContent = o.value + (b && b.f[o.value] ? "  " + b.f[o.value] : ""); });
  host.querySelector(".sp-title").textContent = !names ? "Field names appear once the gauge has talked to this module (K-line mode or this page)." :
    b ? "Block " + g + (b.t ? ": " + b.t : "") + "  (" + klKnown[addr] + ")" : "Block " + g + " isn't in " + klKnown[addr] + ".";
}
function readPicker(host, mainHost) {
  const s = host.dataset.source;
  if (s === "kline") {
    if (host.dataset.me7 === "1") {
      return host.querySelector(".sp-me7").value || "none"; // 0.17.6: an ME7 logger variable, "me7:<name>"
    }
    if (host.querySelector(".sp-use") && host.querySelector(".sp-use").value === "none") return "none";
    return "kl:" + klAddrOf(mainHost || host) + ":" + (Number(host.querySelector(".sp-blk").value) || 1) + ":" + host.querySelector(".sp-fld").value;
  }
  if (s === "obd") return host.querySelector(".sp-pid").value;
  if (s === "car") return carPickerValue(host); // 0.37.0: a diagnostics slot is given out here
  return host.querySelector(".sp-sig").value || "none";
}
function renderPages() {
  const list = $("pagesList"); list.innerHTML = "";
  settingsCache.pages.forEach((p, i) => {
    const d = document.createElement("div"); d.className = "list-item page-card" + (p.enabled ? "" : " disabled"); d.id = "page" + i;
    // 0.23.0: one line per gauge (number, label, what it shows) with its on / off switch; tap the line to open it.
    // Min / Max / Redline / Decimals / Sprite sit under More Options (the <div class="adv">).
    d.innerHTML = `<div class="pg-head">
        <button type="button" class="pg-open" data-index="${i}" aria-expanded="false"><span class="pg-num">${i + 1}</span><span class="pg-text"><span class="pg-name"></span><span class="pg-sum"></span></span><span class="pg-chev">&#9662;</span></button>
        <label class="toggle"><input type="checkbox" class="pg-en" aria-label="Show gauge ${i + 1}" ${p.enabled ? "checked" : ""}><span class="toggle-slider"></span></label>
      </div>
      <div class="pg-body">
      <div class="row-inline">
        <div class="form-group"><label>Style</label><select class="pg-style">${styleOptions(p.style)}</select></div>
        <div class="form-group"><label>Source</label><select class="pg-source">${pageSourceList(p.source || "can").map(([k, n]) => `<option value="${k}" ${(p.source || "can") === k ? "selected" : ""}>${n}</option>`).join("")}</select></div>
        <div class="form-group"><label>Label</label><input type="text" class="pg-label" value="${p.label}" maxlength="15"></div>
      </div>
      <div class="pg-src pg-src1"></div>
      <div class="pg-src pg-src2" style="margin-top:10px"></div>
      <div class="pg-slots" style="display:none;margin-top:10px">
        <div class="spec-title pg-slots-title"></div>
        <div class="pg-src pg-slotsrc" data-slot="0"></div>
        <div class="pg-src pg-slotsrc" data-slot="1" style="margin-top:10px"></div>
        <div class="pg-src pg-slotsrc" data-slot="2" style="margin-top:10px"></div>
      </div>
      <div class="adv" style="margin-top:14px">
      <div class="row-inline">
        <div class="form-group narrow"><label>Min</label><input type="number" class="pg-min" value="${p.min}" step="any"></div>
        <div class="form-group narrow"><label>Max</label><input type="number" class="pg-max" value="${p.max}" step="any"></div>
        <div class="form-group narrow"><label>Redline</label><input type="number" class="pg-red" value="${p.redline}" step="any"></div>
        <div class="form-group narrow"><label>Decimals</label><input type="number" class="pg-dec" value="${p.decimals}" min="0" max="2"></div>
        <div class="help pg-adcnote" style="flex-basis:100%" hidden>Min and Max come from the adc2can: this sensor's dial range there (Auto from its scaling, or Dial Min / Max). Change it on the ADC tab or the adc2can's own web UI.</div>
        <div class="form-group narrow"><label>Sprite</label><select class="pg-spriteon"><option value="0" ${p.spriteOn ? "" : "selected"}>Off</option><option value="1" ${p.spriteOn ? "selected" : ""}>On</option></select></div>
        <div class="form-group pg-spritebox" style="display:none"><label>Sprite pack</label><select class="pg-sprite"></select></div>
      </div>
      </div>
      <div class="row-inline" style="margin:14px 0 0">
        <button type="button" class="btn-secondary pg-move" data-index="${i}" data-move="-1" ${i === 0 ? "disabled" : ""}>Move Up</button>
        <button type="button" class="btn-secondary pg-move" data-index="${i}" data-move="1" ${i === settingsCache.pages.length - 1 ? "disabled" : ""}>Move Down</button>
      </div>
      </div>`;
    list.appendChild(d);
    const moveButtons = d.querySelectorAll(".pg-move"); // 0.23.1: change the swipe order
    for (let k = 0; k < moveButtons.length; k++) {
      moveButtons[k].addEventListener("click", gaugeMoveClicked);
    }
    if (i === openGauge) { // the gauge that was open before the redraw stays open, and so do its More Options
      d.classList.add("open");
      d.querySelector(".pg-open").setAttribute("aria-expanded", "true");
      if (isGaugeFoldOpen) d.querySelector(".adv").classList.add("open");
    }
    setupFolds(d);
    d.querySelector(".pg-open").addEventListener("click", gaugeLineClicked);
    d.addEventListener("change", gaugeCardChanged);
    d.addEventListener("input", gaugeCardChanged);
    const h1 = d.querySelector(".pg-src1"), h2 = d.querySelector(".pg-src2"), srcSel = d.querySelector(".pg-source");
    const styleOf = () => Number(d.querySelector(".pg-style").value);
    const usesSecond = () => [catalogue.styles.indexOf("Dual"), catalogue.styles.indexOf("Haldex")].includes(styleOf());
    const slotHosts = [...d.querySelectorAll(".pg-slotsrc")];
    const isQuad = () => styleOf() === catalogue.styles.indexOf("Quad"), usesSlots = () => isQuad() || isTriple() || styleOf() === catalogue.styles.indexOf("Custom");
    const isTriple = () => styleOf() === catalogue.styles.indexOf("Triple"); // 0.31.0: uses the first two slots
    const slotNames = () => isQuad() || isTriple() ? ["Value 2", "Value 3", "Value 4"] : ["Top", "Middle", "Bottom"];
    const drawSlots = (vals) => { // Quad values 2-4 / the Custom centre stack, from the page's source
      d.querySelector(".pg-slots").style.display = usesSlots() ? "" : "none";
      d.querySelector(".pg-slots-title").textContent = isQuad() ? "Values 2-4 (the first is the signal above)" : isTriple() ? "Values 2-3 (the first is the signal above, on top)" : "Centre stack (the signal above is the main arc gauge)";
      slotHosts.forEach((h, k) => srcPicker(h, srcSel.value, vals[k], true, h1, slotNames()[k]));
      slotHosts[2].style.display = isTriple() ? "none" : ""; // Triple has no fourth value
    };
    const drawSrc = (v1, v2, slots) => {
      srcPicker(h1, srcSel.value, v1, false);
      srcPicker(h2, srcSel.value, v2, true, h1);
      h2.style.display = usesSecond() ? "" : "none";
      drawSlots(slots);
    };
    drawSrc(p.signal, p.signal2, p.slots || ["none", "none", "none"]);
    // Sprite On: the page shows its sprite reacting to this page's reading, in place of the style above
    const spriteBox = () => { d.querySelector(".pg-spritebox").style.display = d.querySelector(".pg-spriteon").value === "1" ? "" : "none"; };
    spriteOptions(d.querySelector(".pg-sprite"), p.sprite || "", "First pack on the gauge");
    spriteBox();
    d.querySelector(".pg-spriteon").addEventListener("change", spriteBox);
    srcSel.addEventListener("change", () => drawSrc(sameReadingIn(srcSel.value, readPicker(h1)), "none", ["none", "none", "none"]));
    // 0.34.1: a Quad / Triple page is titled by its source (ADC, IGNITRON ...), not by its first reading's name
    const titleBySource = () => {
      if (isQuad() || isTriple()) d.querySelector(".pg-label").value = sourceTitle(srcSel.value, readPicker(h1));
    };
    d.querySelector(".pg-style").addEventListener("change", () => {
      h2.style.display = usesSecond() ? "" : "none";
      drawSlots(slotHosts.map(h => readPicker(h, h1)));
      titleBySource();
    });
    srcSel.addEventListener("change", titleBySource);
    h1.addEventListener("change", (e) => { // a new value: label (and range, for the gauge's own signals) to match
      const v = readPicker(h1), c = catalogue.signals.find(x => x.key === v), pid = OBD_PIDS.find(([n]) => "pid:" + n === v);
      const ai = isAdc(v) ? Number(v.slice(3)) - 1 : -1, acfg = ai >= 0 && adcLast && adcLast.ch[ai] && adcLast.ch[ai].cfg;
      const r = acfg ? adcRange(acfg) : null;
      if (r) { // an adc2can sensor: the dial range the adc2can uses (0.36.4)
        d.querySelector(".pg-min").value = r.lo; d.querySelector(".pg-max").value = r.hi; d.querySelector(".pg-red").value = r.red;
        d.querySelector(".pg-dec").value = r.dec; if (r.name) d.querySelector(".pg-label").value = r.name.toUpperCase().slice(0, 15);
        return;
      }
      const me7Row = me7RowOf(v);
      if (me7Row) { // 0.17.6: an ME7 logger variable: range, decimals and name from the .ecu
        const range = me7Range(me7Row);
        d.querySelector(".pg-min").value = range[0];
        d.querySelector(".pg-max").value = range[1];
        d.querySelector(".pg-dec").value = me7Decimals(me7Row);
        if (e.target.matches(".sp-me7")) {
          d.querySelector(".pg-label").value = me7Words(me7Row).toUpperCase().slice(0, 15).trim(); // the label holds 15
        }
        return;
      }
      const ecuRow = ecuRowOf(v);
      if (ecuRow) { // an ECU channel: its range, decimals and name from the ECU's channel list
        d.querySelector(".pg-min").value = ecuRow.lo;
        d.querySelector(".pg-max").value = ecuRow.hi;
        d.querySelector(".pg-dec").value = Math.min(2, ecuRow.d);
        if (e.target.matches(".sp-sig")) d.querySelector(".pg-label").value = ecuRow.n.toUpperCase().slice(0, 15);
        return;
      }
      const cc = c || (pid && catalogue.signals.find(x => x.key === pid[2]));
      if (cc) { // 0.27.0: the catalogue's range is in the stored unit; the page's is in the shown one
        d.querySelector(".pg-min").value = unitShowRange(cc.key, cc.min, cc.unit, cc.max - cc.min);
        d.querySelector(".pg-max").value = unitShowRange(cc.key, cc.max, cc.unit, cc.max - cc.min);
      }
      else if (pid && pid[4] !== undefined) { d.querySelector(".pg-min").value = pid[4]; d.querySelector(".pg-max").value = pid[5]; } // an OBD value with no gauge signal: the table's range
      const name = c ? c.name : pid ? pid[1] : v.startsWith("dbc:") ? v.split(":").slice(2).join(":") : "";
      if (name && e.target.matches(".sp-sig, .sp-pid")) d.querySelector(".pg-label").value = name.toUpperCase().slice(0, 15);
    });
    h1.addEventListener("change", titleBySource); // 0.34.1: runs after the one above, so a Quad / Triple keeps its source title
    h1.addEventListener("change", () => pageAdcRangeLock(d, readPicker(h1))); // 0.36.4
    d.querySelector(".pg-en").addEventListener("change", e => d.classList.toggle("disabled", !e.target.checked));
    d.querySelector(".pg-style").addEventListener("change", e => { // the oil page always shows the OEM oil temperature
      if (Number(e.target.value) !== oilStyleIndex()) return;
      const c = catalogue.signals.find(x => x.key === "oiltemp");
      srcSel.value = "can"; drawSrc("oiltemp", "none", ["none", "none", "none"]);
      const span = c.max - c.min; // 0.27.0: in the shown unit (C or F)
      d.querySelector(".pg-min").value = unitShowRange("oiltemp", c.min, c.unit, span); d.querySelector(".pg-max").value = unitShowRange("oiltemp", c.max, c.unit, span);
      d.querySelector(".pg-red").value = unitShowRange("oiltemp", 130, c.unit, span); d.querySelector(".pg-label").value = "OIL";
      toast("Oil (PQ) gauge - pick CAN or K-line on the Oil tab");
    });
    pageAdcRangeLock(d, readPicker(h1)); // 0.36.4
    updateGaugeSummary(d);
  });
}

// 0.36.4: a page showing an adc2can sensor scaled on the adc2can draws with the adc2can's dial range (the gauge does
// that itself - pageDrawnRange in GaugeUI.cpp), so its Min / Max boxes show that range, greyed out, with a note saying
// where to change it. Any other value (or an adc2can before 1.4, or a sensor scaled on this gauge): the boxes as usual.
function pageAdcRangeLock(card, value) {
  const minBox = card.querySelector(".pg-min");
  const maxBox = card.querySelector(".pg-max");
  const note = card.querySelector(".pg-adcnote");
  let config = null;
  if (isAdc(value)) {
    const sensor = Number(value.slice(3)) - 1;
    const scaledThere = !settingsCache.adc || !settingsCache.adc.ch[sensor] || settingsCache.adc.ch[sensor].mode === 0;
    if (scaledThere && adcLast && adcLast.ch && adcLast.ch[sensor] && adcLast.ch[sensor].cfg) {
      config = adcLast.ch[sensor].cfg;
    }
  }
  const followsAdc2can = config !== null && config.dialLo !== undefined && config.dialHi > config.dialLo;
  minBox.disabled = followsAdc2can;
  maxBox.disabled = followsAdc2can;
  note.hidden = !followsAdc2can;
  if (followsAdc2can) {
    minBox.value = fmt(unitShow(value, config.dialLo, config.unit).v, 2); // in the units on show, as the gauge draws it
    maxBox.value = fmt(unitShow(value, config.dialHi, config.unit).v, 2);
  }
}

// 0.23.0: a gauge's line on the Gauges tab: open it (and close the one that was open), or close it again
function gaugeLineClicked(event) {
  const index = Number(event.currentTarget.dataset.index);
  if (openGauge === index) {
    openGauge = -1;
  } else {
    openGauge = index;
  }
  isGaugeFoldOpen = false; // a newly opened gauge starts with its More Options shut
  const cards = document.querySelectorAll("#pagesList .page-card");
  for (let i = 0; i < cards.length; i++) {
    const isOpen = i === openGauge;
    cards[i].classList.toggle("open", isOpen);
    cards[i].querySelector(".pg-open").setAttribute("aria-expanded", String(isOpen));
    const fold = cards[i].querySelector(".adv");
    const foldButton = fold.previousElementSibling;
    fold.classList.remove("open");
    foldButton.classList.remove("open");
    foldButton.firstChild.textContent = "More Options ";
  }
}

// 0.23.1: Move Up / Move Down on an open gauge: swap it with the one above / below it (the swipe order on the glass),
// redraw the list with it still open, and save. Each gauge's settings move with it.
async function gaugeMoveClicked(event) {
  const from = Number(event.currentTarget.dataset.index);
  const to = from + Number(event.currentTarget.dataset.move);
  const pages = collectPages(); // what the list shows now, unsaved typing included
  if (to < 0 || to >= pages.length) return;
  const moving = pages[from];
  pages[from] = pages[to];
  pages[to] = moving;
  settingsCache.pages = pages;
  openGauge = to; // it stays open in its new place
  renderPages();
  await saveSettings({ pages: pages }, "gauges saved - screen rebuilt");
}

// anything changed on a gauge: its line follows (label, style, value, range)
function gaugeCardChanged(event) {
  updateGaugeSummary(event.currentTarget);
}

// a gauge's line: its label (or "Gauge n"), then "style · min to max · value" (the value last: a long name is cut short
// with "..." and the range still shows)
function updateGaugeSummary(card) {
  const number = card.querySelector(".pg-num").textContent;
  let name = card.querySelector(".pg-label").value.trim();
  if (name === "") name = "Gauge " + number;
  card.querySelector(".pg-name").textContent = name;
  const styleSelect = card.querySelector(".pg-style");
  let styleName = "";
  if (styleSelect.selectedIndex >= 0) styleName = styleSelect.options[styleSelect.selectedIndex].textContent;
  const range = card.querySelector(".pg-min").value + " to " + card.querySelector(".pg-max").value;
  card.querySelector(".pg-sum").textContent = styleName + " · " + range + " · " + gaugeValueText(card.querySelector(".pg-src1"));
}

// the value a gauge shows, as its picker names it ("Engine speed (rpm)", "Block 3, field 2", "No value picked")
function gaugeValueText(host) {
  const block = host.querySelector(".sp-blk");
  if (block && host.dataset.me7 !== "1") {
    return "Block " + block.value + ", field " + host.querySelector(".sp-fld").value;
  }
  const select = host.querySelector(".sp-me7, .sp-sig, .sp-pid, .sp-car");
  if (!select || select.selectedIndex < 0) return "No value picked";
  const option = select.options[select.selectedIndex];
  if (option.value === "" || option.value === "none") return "No value picked";
  return option.textContent.trim();
}
function collectPages() {
  return settingsCache.pages.map((p, i) => { const d = $("page" + i); return {
    enabled: d.querySelector(".pg-en").checked ? 1 : 0, style: Number(d.querySelector(".pg-style").value), source: d.querySelector(".pg-source").value,
    signal: readPicker(d.querySelector(".pg-src1")), signal2: d.querySelector(".pg-src2").style.display === "none" ? "none" : readPicker(d.querySelector(".pg-src2"), d.querySelector(".pg-src1")),
    label: d.querySelector(".pg-label").value, min: Number(d.querySelector(".pg-min").value),
    max: Number(d.querySelector(".pg-max").value), redline: Number(d.querySelector(".pg-red").value), decimals: Number(d.querySelector(".pg-dec").value),
    sprite: d.querySelector(".pg-sprite").value, spriteOn: Number(d.querySelector(".pg-spriteon").value),
    slots: [...d.querySelectorAll(".pg-slotsrc")].map(h => h.style.display === "none" ? "none" : readPicker(h, d.querySelector(".pg-src1"))) }; });
}

function renderIo() {
  $("io.outputMode").value = settingsCache.io.outputMode;
  $("io.nightSource").value = settingsCache.io.nightSource;
  $("io.dimInvert").value = settingsCache.io.dimInvert;
  setInterval(renderIoState, 1000);
}

// the live states on the Output / Illumination / Ignition cards (0.17.0: one "Name: value" line each with its dot,
// instead of a Status box beside the dropdowns)
function renderIoState() {
  if (!statusCache || !statusCache.io) return;
  const io = statusCache.io;
  let lightsText = "Off";
  if (io.dashLights) {
    lightsText = "On";
  }
  if (io.night) {
    lightsText = lightsText + " (Dimmed)";
  }
  $("lightsStatus").textContent = lightsText;
  setDot($("lightsDot"), io.dashLights);
  // why the output is off when it's been set On: the gauge is in standby (ignition off), which never leaves anything
  // switched on from the battery - or the driver chip has shut it down
  const pw = statusCache.power || {};
  let outNote = "";
  if (io.outputFault) {
    outNote = "Short circuit, overload or too hot: it retries by itself.";
  } else if (pw.state === "standby" || pw.state === "shutdown") {
    outNote = "Held off: ignition off (standby).";
  }
  $("outNote").textContent = outNote;
  if (statusCache.power) {
    let ignText = "Not Used (Switched Power)";
    $("pwrPart").style.display = settingsCache.power.ignSource ? "" : "none"; // the gauge's state only means something then
    if (settingsCache.power.ignSource) {
      ignText = "Off";
      if (pw.ignition) {
        ignText = "On";
      }
    }
    $("ignStatus").textContent = ignText;
    setDot($("ignDot"), !settingsCache.power.ignSource || pw.ignition);
    let gaugeText = pw.state; // "on", "standby", "switched power"
    if (pw.sleepInS) {
      if (pw.sleepInS >= 120) {
        gaugeText = gaugeText + " - sleeps in " + Math.round(pw.sleepInS / 60) + " min";
      } else {
        gaugeText = gaugeText + " - sleeps in " + pw.sleepInS + " s";
      }
    }
    $("pwrStatus").textContent = titleCase(gaugeText); // "Standby - Sleeps in 30 s"
    $("ignDetail").textContent = titleCase(pw.detail);
  }
}

// a status dot: green when on, red when off
function setDot(dot, isOn) {
  if (!dot) return;
  dot.classList.toggle("on", !!isOn);
  dot.classList.toggle("off", !isOn);
  dot.classList.remove("fault");
}

// the Output line on Settings: its dot and On / Off / Fault (a fault = the driver chip has shut the output down)
function showOutputState(channel, isOn, hasFault) {
  const dot = $("outdot" + channel);
  const text = $("outState" + channel);
  if (!dot || !text) return;
  if (hasFault) {
    dot.classList.remove("on", "off");
    dot.classList.add("fault"); // flashing orange (index.html)
    text.textContent = "Fault";
    return;
  }
  setDot(dot, isOn);
  if (isOn) {
    text.textContent = "On";
  } else {
    text.textContent = "Off";
  }
}

// ignition on permanent power (Output / Illumination > Ignition)
function ignBoxes() {
  const src = Number($("power.ignSource").value);
  $("ignCanBox").style.display = src === 1 ? "" : "none";
  document.querySelectorAll(".ign-custom").forEach(e => e.style.display = Number($("power.canMode").value) === 2 ? "" : "none");
  $("lightsIgnNote").style.display = src === 2 ? "" : "none";
  $("io.nightSource").disabled = src === 2;
}
function collectPower() {
  const p = collectSimple("power", ["ignSource", "canMode", "canBit", "canInvert", "offDelayS", "standbyMin"]);
  const id = parseInt($("ignCanId").value, 16);
  if (!isNaN(id)) p.canId = id > 0x7FF ? ((id & 0x1FFFFFFF) | 0x80000000) >>> 0 : id; // above 7FF: a 29-bit id (bit 31)
  p.shutdownSprite = $("power.shutdownSprite").value; // "" = no sprite (collectSimple would drop it)
  return { power: p };
}

// ---------------------------------------------------------------------------
// sprite packs (/sprites/<key>.c2gs, include/c2gs.h, docs/SPRITES.md): Sprite pages, the screensaver, the shutdown sprite
// ---------------------------------------------------------------------------
const SPRITE_ROLE_TEXT = { mood: "moods", event: "events", tap: "tap", saver: "screensaver", shutdown: "shutdown", sleep: "sleep", wake: "wake", rowLeft: "row wave", rowRight: "row wave" };
// a pack picker: noneText = what "" means (the first pack on the gauge / no sprite)
function spriteOptions(sel, current, noneText) {
  sel.innerHTML = "";
  sel.appendChild(new Option(noneText, ""));
  spritesCache.sprites.filter(s => !s.error).forEach(s => sel.appendChild(new Option((s.name || s.key) + " (" + s.key + ")", s.key)));
  if (current && ![...sel.options].some(o => o.value === current)) sel.appendChild(new Option(current + " (not on the gauge)", current));
  sel.value = current || "";
}
function spriteSelects() {
  spriteOptions($("saver.sprite"), settingsCache.saver.sprite, "First pack on the gauge");
  spriteOptions($("power.shutdownSprite"), settingsCache.power.shutdownSprite, "None - fade to black");
  document.querySelectorAll(".pg-sprite").forEach(sel => spriteOptions(sel, sel.value, "First pack on the gauge"));
}
async function loadSprites() {
  spritesCache = await api("/api/sprites");
  const list = $("spriteList"); list.innerHTML = "";
  if (!spritesCache.sprites.length)
    list.innerHTML = '<div class="help">No sprite packs on the gauge yet.</div>';
  spritesCache.sprites.forEach(s => {
    const roles = s.states ? [...new Set(s.states.map(x => SPRITE_ROLE_TEXT[x.role] || x.role))].join(", ") : "";
    const d = document.createElement("div"); d.className = "stack-row"; // 0.17.0: the text, then Delete underneath
    d.innerHTML = `<span><strong>${esc(s.name || s.key)}</strong> <span class="help">${esc(s.key)}.c2gs - ${s.error ? esc(s.error) :
      `${s.w} x ${s.h}${s.themed ? ", takes the theme colour" : s.alpha ? " with transparency" : ""}, ${s.clips} clips, ${s.frames} frames, ${Math.round(s.size / 1024)} kB - ${esc(roles)}`}</span></span><div><button class="btn-secondary">Delete</button></div>`;
    d.querySelector("button").addEventListener("click", async () => {
      if (!confirm("Delete the sprite pack " + s.key + "?")) return;
      await fetch("/api/files/delete", { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body: "path=" + encodeURIComponent("/sprites/" + s.key + ".c2gs") });
      await action("spritesChanged"); loadSprites();
    });
    list.appendChild(d);
  });
  spriteSelects();
}
async function uploadSprite() {
  const f = $("spriteFile").files[0];
  if (!f) return toast("choose a .c2gs file first");
  const key = f.name.replace(/\.c2gs$/i, "");
  if (!/\.c2gs$/i.test(f.name)) return toast("that isn't a .c2gs sprite pack - make one with tools/sprite_convert.py");
  if (key.length > 15) return toast("rename it to 15 characters or fewer (before .c2gs)");
  const head = new Uint8Array(await f.slice(0, 4).arrayBuffer());
  if (String.fromCharCode(...head) !== "C2GS") return toast(f.name + " is not a C2GS sprite pack");
  await uploadFile($("spriteFile"), "/api/files/upload?dir=" + encodeURIComponent("/sprites"), async () => { await action("spritesChanged"); loadSprites(); });
}

function renderOutputs() {
  const list = $("outList"); list.innerHTML = "";
  catalogue.outputs.forEach((name, i) => {
    // 0.17.1: "● Output: On" like the Illumination line (dot green / red / flashing orange on a fault), Off / On / Auto below
    const state = document.createElement("div"); state.className = "state-line";
    state.innerHTML = `<span class="dot-on" id="outdot${i}"></span>Output: <b id="outState${i}">--</b>`;
    list.appendChild(state);
    const d = document.createElement("div"); d.className = "out-row";
    d.innerHTML = ["Off", "On", "Auto"].map((l, s) => `<button class="out-btn ${settingsCache.io.outputs[i] == s ? "sel" : ""}" data-ch="${i}" data-state="${s}">${l}</button>`).join("");
    list.appendChild(d);
  });
  list.querySelectorAll(".out-btn").forEach(b => b.addEventListener("click", async () => {
    await action("output", { channel: Number(b.dataset.ch), state: Number(b.dataset.state) });
    settingsCache.io.outputs[Number(b.dataset.ch)] = Number(b.dataset.state);
    list.querySelectorAll(`.out-btn[data-ch="${b.dataset.ch}"]`).forEach(x => x.classList.toggle("sel", x === b));
  }));
}

// ---- Triggers: the "When" and threshold boxes follow the signal picked -------------------------------------------
// Two-state signals: "is On" / "is Off" instead of a number (the gauge stores it as >= 1 / <= 0).
// [what 1 means, what 0 means, the one a new trigger starts on (1 or 0): the warning state]
const TRIGGER_TWO_STATE = { lights: ["On", "Off", 1], output: ["On", "Off", 1], outFault: ["Faulty", "OK", 1], oilLevel: ["OK", "Low", 0], oilPress: ["OK", "Low", 0] };
// Signals with a few set values: the threshold is picked from a list. [value, label] pairs.
const TRIGGER_STEPS = {
  gear: [[1, "1"], [2, "2"], [3, "3"], [4, "4"], [5, "5"], [6, "6"], [7, "7"]],
  hdxMode: [[0, "Stock"], [1, "FWD"], [2, "50:50"], [3, "60:40"], [4, "75:25"], [5, "Expert"]],
};

// a signal's row in the signal list (/api/signals), or null
function triggerSignalInfo(key) {
  for (let i = 0; i < catalogue.signals.length; i++) {
    if (catalogue.signals[i].key === key) {
      return catalogue.signals[i];
    }
  }
  return null;
}

// the signal's usual range: the catalogue's, or for a gauge page's own value the range set on that page
function triggerRange(key) {
  const range = { min: 0, max: 100, unit: "" };
  const info = triggerSignalInfo(key);
  if (info) {
    range.min = info.min;
    range.max = info.max;
    range.unit = info.unit;
  }
  const userMatch = /^user(\d+)$/.exec(key);
  if (userMatch) {
    const number = Number(userMatch[1]);
    if (number <= 16 && number % 2 === 1) { // user1, 3, 5 ... = page 1, 2, 3 ... main value
      const page = settingsCache.pages[(number - 1) / 2];
      if (page) {
        range.min = page.min;
        range.max = page.max;
      }
    }
  }
  if (!(range.max > range.min)) {
    range.max = range.min + 1;
  }
  return range;
}

// how finely the threshold moves: 0.01 for lambda-sized ranges, 0.1 for up to 20, else whole numbers
function triggerStep(range) {
  const span = range.max - range.min;
  if (span <= 2) {
    return 0.01;
  }
  if (span <= 20) {
    return 0.1;
  }
  return 1;
}

function triggerRound(value, step) {
  return Math.round(value / step) * step;
}

// a sensible starting threshold for a signal: its redline, else three quarters up (above) / a quarter up (below)
function triggerDefaults(key, above) {
  if (TRIGGER_TWO_STATE[key]) {
    const warning = TRIGGER_TWO_STATE[key][2];
    return { above: warning, threshold: warning, hysteresis: 0 };
  }
  if (TRIGGER_STEPS[key]) {
    const steps = TRIGGER_STEPS[key];
    return { above: above, threshold: steps[Math.floor(steps.length * 0.6)][0], hysteresis: 0 };
  }
  const range = triggerRange(key);
  const step = triggerStep(range);
  const span = range.max - range.min;
  let threshold = range.min + span * 0.25;
  if (above) {
    threshold = range.min + span * 0.75;
    const info = triggerSignalInfo(key);
    if (info && info.redline > range.min && info.redline < range.max) {
      threshold = info.redline;
    }
  }
  const hysteresis = Math.max(step, triggerRound(span * 0.02, step));
  return { above: above, threshold: Number(triggerRound(threshold, step).toFixed(2)), hysteresis: Number(hysteresis.toFixed(2)) };
}

// the When / Threshold / Hysteresis boxes of one trigger, for its signal (t = the values to show)
function triggerConditionBoxes(row, key, t) {
  const box = row.querySelector(".t-cond");
  box.textContent = "";
  if (key === "none") {
    return; // no signal picked yet: nothing to compare
  }
  if (TRIGGER_TWO_STATE[key]) {
    const names = TRIGGER_TWO_STATE[key];
    let onSelected = "";
    let offSelected = "selected";
    if (t.above) {
      onSelected = "selected";
      offSelected = "";
    }
    box.innerHTML = `<div class="form-group narrow"><label>When</label><select class="t-state">` +
      `<option value="1" ${onSelected}>is ${names[0]}</option><option value="0" ${offSelected}>is ${names[1]}</option></select></div>`;
    return;
  }
  let aboveSelected = "selected";
  let belowSelected = "";
  if (!t.above) {
    aboveSelected = "";
    belowSelected = "selected";
  }
  const when = `<div class="form-group narrow"><label>When</label><select class="t-above">` +
    `<option value="1" ${aboveSelected}>Above</option><option value="0" ${belowSelected}>Below</option></select></div>`;
  if (TRIGGER_STEPS[key]) {
    let options = "";
    const steps = TRIGGER_STEPS[key];
    for (let i = 0; i < steps.length; i++) {
      let selected = "";
      if (Number(t.threshold) === steps[i][0]) {
        selected = "selected";
      }
      options = options + `<option value="${steps[i][0]}" ${selected}>${steps[i][1]}</option>`;
    }
    box.innerHTML = when + `<div class="form-group narrow"><label>Threshold</label><select class="t-thr">${options}</select></div>`;
    return;
  }
  const range = triggerRange(key);
  const step = triggerStep(range);
  let unit = "";
  if (range.unit) {
    unit = " (" + uShown(range.unit) + ")";
  }
  const hint = "usually " + fmt(range.min, 2) + " to " + fmt(range.max, 2);
  box.innerHTML = when +
    `<div class="form-group narrow"><label>Threshold${esc(unit)}</label><input type="number" class="t-thr" value="${t.threshold}" step="${step}" title="${hint}" placeholder="${hint}"></div>` +
    `<div class="form-group narrow"><label>Hysteresis</label><input type="number" class="t-hys" value="${t.hysteresis}" step="${step}" min="0"></div>`;
}

// a new signal picked: its own sensible threshold, before the change is saved (this runs before autoSave hears it)
function triggerSignalChanged(event) {
  const row = event.target.closest(".list-item");
  const key = event.target.value;
  let above = 1; // a trigger's first signal starts at "above"
  const aboveBox = row.querySelector(".t-above");
  if (aboveBox && row.dataset.signal !== "none") {
    above = Number(aboveBox.value); // changing from another signal keeps the direction already picked
  }
  row.dataset.signal = key;
  triggerConditionBoxes(row, key, triggerDefaults(key, above));
}

// ---- 0.35.0: the Triggers signal list, grouped by where the value comes from, offering only what's there ----------
// Page values (User 1-40) only when a page has one set up; ADC sensors only from an adc2can that's been heard (or a
// sensor named on the ADC tab); Haldex only once OpenHaldex has been heard; usb2ip's values only once it's been heard.
// ME7 1-8 never: nothing fills them since ME7 variables became page values. A trigger's own signal always stays listed.
const TRIGGER_GEARBOX_KEYS = ["selector", "tcuSpeed", "gboxTemp", "shifting", "lockup", "inputRpm", "convSlip"];
const TRIGGER_GAUGE_KEYS = ["boardTemp", "uptime", "lights", "output", "outFault"];
const TRIGGER_USB2IP_KEYS = ["knockRetard", "injDuty", "lambdaTarget"];
const PAGE_VALUE_SOURCES = { dbc: "CAN", kl: "K-line", pid: "OBD", ecu: "ECU", diag: "Diagnostics", me7: "ME7" };
let triggerHaldexHeard = false; // OpenHaldex heard since the Triggers tab was last opened (GET /api/haldex)

// the page value behind "userN": { page (1-8), text (its setting: "kl:1:2:3"), source ("K-line") }, or null when no
// page has that value set up. User 1-16 = page main / second values (two per page), 17-40 = Quad / Triple / Custom slots
function triggerPageValue(key) {
  const match = /^user(\d+)$/.exec(key);
  if (!match || !settingsCache || !settingsCache.pages) return null;
  const userIndex = Number(match[1]) - 1;
  let pageIndex = 0;
  let text = "";
  if (userIndex < 16) {
    pageIndex = Math.floor(userIndex / 2);
    const page = settingsCache.pages[pageIndex];
    if (!page) return null;
    if (userIndex % 2 === 0) {
      text = page.signal;
    } else {
      text = page.signal2;
    }
  } else {
    const slotIndex = userIndex - 16;
    pageIndex = Math.floor(slotIndex / 3);
    const slot = slotIndex % 3;
    const page = settingsCache.pages[pageIndex];
    if (!page || !page.slots) return null;
    const style = catalogue.styles[page.style];
    let slotsUsed = 0; // how many slots this page's style shows
    if (style === "Quad" || style === "Custom") {
      slotsUsed = 3;
    } else if (style === "Triple") {
      slotsUsed = 2;
    }
    if (slot >= slotsUsed) return null;
    text = page.slots[slot];
  }
  const prefix = /^([a-z0-9]+):/.exec(text || "");
  if (!prefix || !PAGE_VALUE_SOURCES[prefix[1]]) return null; // a gauge signal ("coolant") or nothing: the slot is free
  return { page: pageIndex + 1, text: text, source: PAGE_VALUE_SOURCES[prefix[1]] };
}

// which group a signal goes under in the Triggers list ("" = leave it out)
function triggerSignalGroup(signal) {
  const key = signal.key;
  if (key === "none") return "";
  if (/^me7_\d+$/.test(key)) return "";
  if (isUser(key)) {
    if (triggerPageValue(key)) return "Page values";
    return "";
  }
  if (isAdc(key)) {
    if (adcSensorFound(key)) return "ADC (adc2can)"; // 0.36.6: its board has been heard
    return "";
  }
  if (key.startsWith("hdx")) {
    if (triggerHaldexHeard) return "Haldex (OpenHaldex)";
    return "";
  }
  if (TRIGGER_USB2IP_KEYS.includes(key)) {
    if (statusCache && statusCache.ecu && statusCache.ecu.heard) return "ECU (usb2ip)";
    return "";
  }
  if (TRIGGER_GEARBOX_KEYS.includes(key)) return "Gearbox";
  if (TRIGGER_GAUGE_KEYS.includes(key)) return "This gauge";
  return "Engine and car";
}

// a page value with no name yet, said from its setting: "kl:1:2:3" -> "Block 2 field 3", "pid:12" -> the PID's name
function triggerPageValueName(text) {
  const parts = text.split(":");
  if (parts[0] === "kl") return "Block " + parts[2] + " field " + parts[3];
  if (parts[0] === "pid") {
    const pid = Number(parts[1]);
    for (let i = 0; i < OBD_PIDS.length; i++) {
      if (OBD_PIDS[i][0] === pid) return OBD_PIDS[i][1];
    }
    return "PID " + pid;
  }
  if (parts[0] === "ecu") return "Channel " + parts[2];
  if (parts[0] === "diag") return "Diag value " + parts[1];
  if (parts[0] === "dbc") return parts.slice(2).join(":"); // the DBC signal's name
  if (parts[0] === "me7") return parts[1]; // the ME7 variable's name
  return text;
}

// the name a signal shows in the Triggers list
function triggerSignalText(signal) {
  let text = signal.name;
  const pageValue = triggerPageValue(signal.key);
  if (pageValue) {
    let name = signal.name;
    if (/^User \d+$/.test(name)) {
      name = triggerPageValueName(pageValue.text); // not named yet: said from its setting
    }
    text = "Page " + pageValue.page + ": " + name + " (" + pageValue.source + ")";
  }
  if (isAdc(signal.key)) {
    return adcSensorText(Number(signal.key.slice(3)) - 1); // 0.36.6: "Board 1 - Sensor 1 - Oil press (bar)", as everywhere
  }
  if (signal.unit) {
    text = text + " (" + uShown(signal.unit) + ")";
  }
  return text;
}

// for sort(): which of two page values comes first - the lower page, then main / second value before the slots
function triggerPageOrder(first, second) {
  const pageDifference = triggerPageValue(first.key).page - triggerPageValue(second.key).page;
  if (pageDifference !== 0) return pageDifference;
  return Number(first.key.slice(4)) - Number(second.key.slice(4)); // "user5" -> 5
}

// fills one trigger's Signal box: "-" first, then the groups in this order
function triggerSigOptions(select, current) {
  const groupOrder = ["Engine and car", "Gearbox", "Page values", "ADC (adc2can)", "ECU (usb2ip)", "Haldex (OpenHaldex)", "This gauge", "Not found now"];
  const groups = {};
  for (let i = 0; i < groupOrder.length; i++) {
    groups[groupOrder[i]] = [];
  }
  for (let i = 0; i < catalogue.signals.length; i++) {
    const signal = catalogue.signals[i];
    let group = triggerSignalGroup(signal);
    if (group === "" && signal.key === current && current !== "none") {
      group = "Not found now"; // this trigger's own signal: kept so it doesn't vanish
    }
    if (group !== "") {
      groups[group].push(signal);
    }
  }
  // page values in page order (the catalogue has every page's main / second value first, then the slots)
  groups["Page values"].sort(triggerPageOrder);
  let html = '<option value="none">-</option>';
  for (let g = 0; g < groupOrder.length; g++) {
    const signals = groups[groupOrder[g]];
    if (signals.length === 0) continue;
    html = html + `<optgroup label="${groupOrder[g]}">`;
    for (let i = 0; i < signals.length; i++) {
      html = html + `<option value="${esc(signals[i].key)}">${esc(triggerSignalText(signals[i]))}</option>`;
    }
    html = html + "</optgroup>";
  }
  select.innerHTML = html;
  select.value = current;
  if (select.value !== current) {
    select.value = "none";
  }
}

// opening the Triggers tab: fresh names (a page edited since), which adc2can / OpenHaldex is there, then the list again
async function refreshTriggerSources() {
  try { catalogue = Object.assign(catalogue, await api("/api/signals")); } catch (e) { /* keep the old names */ }
  try { adcLast = await api("/api/adc"); } catch (e) { /* no adc2can */ }
  try { const haldex = await api("/api/haldex"); triggerHaldexHeard = !!haldex.heard; } catch (e) { triggerHaldexHeard = false; }
  if (!$("trigList").contains(document.activeElement)) renderTriggers();
}

function renderTriggers() {
  const list = $("trigList");
  list.innerHTML = "";
  for (let i = 0; i < settingsCache.triggers.length; i++) {
    const t = settingsCache.triggers[i];
    const row = document.createElement("div");
    row.className = "list-item";
    row.id = "trig" + i;
    let enabled = "";
    if (t.enabled) {
      enabled = "checked";
    }
    const actions = [[0, "Alert on screen"], [1, "Switch the output"], [2, "Alert + output"]];
    let actionOptions = "";
    for (let a = 0; a < actions.length; a++) {
      let selected = "";
      if (Number(t.action) === actions[a][0]) {
        selected = "selected";
      }
      actionOptions = actionOptions + `<option value="${actions[a][0]}" ${selected}>${actions[a][1]}</option>`;
    }
    let latchNo = "selected";
    let latchYes = "";
    if (t.latch) {
      latchNo = "";
      latchYes = "selected";
    }
    // 0.32.0: no reading (sensor unplugged / shorted / faulted, bus quiet): ignore it, or count it as the condition met
    let noReadingIgnore = "selected";
    let noReadingTrigger = "";
    if (t.noReading) {
      noReadingIgnore = "";
      noReadingTrigger = "selected";
    }
    row.innerHTML = `<h3><span><span class="dot-on" id="trigdot${i}"></span>Trigger ${i + 1}</span><label class="toggle"><input type="checkbox" class="t-en" ${enabled}><span class="toggle-slider"></span></label></h3>
      <div class="row-inline">
        <div class="form-group"><label>Label (shown on screen)</label><input type="text" class="t-label" value="${esc(t.label)}" maxlength="15"></div>
        <div class="form-group"><label>Signal</label><select class="t-sig"></select></div>
        <div class="t-cond row-inline" style="margin:0;flex:1 1 auto"></div>
        <div class="form-group narrow"><label>Hold ms</label><input type="number" class="t-hold" value="${t.holdMs}" min="0" max="60000"></div>
        <div class="form-group"><label>Action</label><select class="t-act">${actionOptions}</select></div>
        <div class="form-group narrow"><label>Latch</label><select class="t-latch"><option value="0" ${latchNo}>No</option><option value="1" ${latchYes}>Yes</option></select></div>
        <div class="form-group narrow"><label>No Reading</label><select class="t-noread" title="What the trigger does when its value has no reading: a sensor unplugged, shorted or faulted, or the bus gone quiet"><option value="0" ${noReadingIgnore}>Ignore</option><option value="1" ${noReadingTrigger}>Trigger</option></select></div>
      </div>`;
    list.appendChild(row);
    triggerSigOptions(row.querySelector(".t-sig"), t.signal); // "-" = not used yet; 0.35.0: grouped, only what's there
    row.dataset.signal = t.signal; // the signal before a change (triggerSignalChanged)
    triggerConditionBoxes(row, t.signal, t);
    row.querySelector(".t-sig").addEventListener("change", triggerSignalChanged);
  }
}

function collectTriggers() {
  const triggers = [];
  for (let i = 0; i < settingsCache.triggers.length; i++) {
    triggers.push(collectTrigger($("trig" + i)));
  }
  return triggers;
}

// one trigger's settings from its row
function collectTrigger(row) {
  const trigger = {
    enabled: 0, label: row.querySelector(".t-label").value, signal: row.querySelector(".t-sig").value,
    above: 1, threshold: 0, hysteresis: 0, holdMs: Number(row.querySelector(".t-hold").value),
    action: Number(row.querySelector(".t-act").value), output: 0, // the board has one output
    latch: Number(row.querySelector(".t-latch").value),
    noReading: Number(row.querySelector(".t-noread").value), // 0.32.0
  };
  if (row.querySelector(".t-en").checked) {
    trigger.enabled = 1;
  }
  const stateBox = row.querySelector(".t-state");
  if (stateBox) { // two-state: "is On" = at or above 1, "is Off" = at or below 0
    trigger.above = Number(stateBox.value);
    trigger.threshold = Number(stateBox.value);
    return trigger;
  }
  const aboveBox = row.querySelector(".t-above");
  if (!aboveBox) {
    return trigger; // no signal picked
  }
  trigger.above = Number(aboveBox.value);
  trigger.threshold = Number(row.querySelector(".t-thr").value);
  const hysteresisBox = row.querySelector(".t-hys");
  if (hysteresisBox) {
    trigger.hysteresis = Number(hysteresisBox.value);
  }
  return trigger;
}

async function saveSettings(partial, msg) {
  const r = await api("/api/settings", "POST", partial);
  toast(r.error || (r.status && r.status !== "ok" ? r.status : msg || "saved"));
  settingsCache = await api("/api/settings");
}

// Import Backup: read a Download Backup file, restore every setting (POST /api/settings takes the whole blob), reboot
async function importBackup(e) {
  const file = e.target.files[0];
  e.target.value = ""; // let the same file be picked again later
  if (!file) return;
  let settings;
  try {
    settings = JSON.parse(await file.text());
  } catch (err) {
    toast("Not a backup file");
    return;
  }
  if (!settings || typeof settings !== "object" || !settings.pages) {
    toast("Not a can2gauge backup");
    return;
  }
  if (!confirm("Restore all settings from this backup and reboot?")) return;
  const r = await api("/api/settings", "POST", settings);
  if (r.error) { toast(r.error); return; }
  toast("Backup restored - rebooting");
  await action("reboot");
}

// Settings apply as they change - no Save buttons. A select / checkbox / colour saves at once; typing saves 0.8 s after
// the last key (and on leaving the field). collect() returns the partial settings JSON for that card; after() runs once
// the gauge has taken it. Terminal / file inputs and anything marked data-nosave are not settings.
function autoSave(roots, collect, msg, after) {
  let timer = null, last = "";
  const fire = async () => {
    clearTimeout(timer); timer = null;
    const body = collect(), key = JSON.stringify(body);
    if (key === last) return; // typing saved it already; leaving the field adds nothing
    last = key;
    await saveSettings(body, msg);
    if (after) after();
  };
  const isSetting = (el) => el.matches("input, select, textarea") && el.type !== "file" && el.type !== "button" && !el.closest("[data-nosave]");
  (Array.isArray(roots) ? roots : [roots]).forEach((root) => {
    if (!root) return;
    root.addEventListener("change", (e) => { if (isSetting(e.target)) fire(); });
    root.addEventListener("input", (e) => {
      if (!isSetting(e.target) || e.target.tagName === "SELECT" || e.target.type === "checkbox") return;
      clearTimeout(timer); timer = setTimeout(fire, 800);
    });
  });
}

// ---------------------------------------------------------------------------
// themes (ids: 0-3 can2gauge palettes, 4 Custom, 5 Match car, 16+ manufacturers - include/can2gauge_themes.h)
// ---------------------------------------------------------------------------
const THEME_CUSTOM = 4, THEME_MATCH = 5, THEME_NONE = 255;
const confText = ["approximate - check against a photo", "known era colour", "sourced value"];
function themeById(id) { return themesCache.themes.find(t => t.id === id); }
function scaleRgb(hex, k) { const v = parseInt(hex.slice(1), 16); return `rgb(${Math.round((v >> 16 & 255) * k)},${Math.round((v >> 8 & 255) * k)},${Math.round((v & 255) * k)})`; }

// dropdown: can2gauge palettes, Match car / Custom, then one group per make with its eras
function renderThemeOptions() {
  const sel = $("display.theme"); sel.innerHTML = "";
  const groups = {};
  const group = (label) => groups[label] || (groups[label] = sel.appendChild(Object.assign(document.createElement("optgroup"), { label })));
  themesCache.themes.forEach(t => {
    let g, text;
    if (t.id < THEME_CUSTOM) { g = "can2gauge"; text = t.name.replace("can2gauge - ", ""); }
    else if (t.id === THEME_CUSTOM || t.id === THEME_MATCH) { g = "Automatic / custom"; text = t.name; }
    else { const p = t.name.split(" - "); g = p[0]; text = p[1] + "  -  " + t.era; }
    group(g).appendChild(new Option(text, t.id));
  });
  const lm = $("boot.make"); lm.innerHTML = "";
  lm.appendChild(new Option("Follow the theme / VIN", ""));
  themesCache.logos.forEach(l => {
    // every make logo ships with the web UI files, so only a missing one is marked
    lm.appendChild(new Option(l.name + (l.present ? "" : "  (file missing)"), l.key));
  });
  const missing = themesCache.logos.filter(l => !l.present).map(l => l.name);
  let logoText = "All " + themesCache.logos.length + " make logos are on the gauge.";
  if (missing.length === themesCache.logos.length) {
    logoText = "No make logo files on the gauge - upload the web UI files (they go in /logos/<make>.bin).";
  } else if (missing.length) {
    logoText = "Missing on the gauge: " + missing.join(", ") + " (upload the web UI files again to put them back).";
  }
  $("logoList").textContent = logoText;
}

// the colours the gauge would use for the selected entry (Match car -> matched theme or Custom)
function themeColours(id) {
  if (id === THEME_MATCH && themesCache.matched !== THEME_NONE && themeById(themesCache.matched)) id = themesCache.matched;
  if (id === THEME_CUSTOM || id === THEME_MATCH) return { dial: $("display.themeDial").value, needle: $("display.themeNeedle").value };
  const t = themeById(id) || themesCache.themes[0];
  return { dial: t.dial, needle: t.needle };
}

// mini needle page: track, ticks + numerals in the dial colour, needle + hub ring in the needle colour
function drawThemePreview(dial, needle) {
  const cv = $("themePreview"), g = cv.getContext("2d"), s = cv.width, c = s / 2, r = s / 2 - 4;
  const a0 = 0.75 * Math.PI, span = 1.5 * Math.PI, pt = (a, rr) => [c + Math.cos(a) * rr, c + Math.sin(a) * rr];
  g.clearRect(0, 0, s, s);
  g.fillStyle = "#000"; g.beginPath(); g.arc(c, c, s / 2, 0, 2 * Math.PI); g.fill();
  g.lineCap = "round";
  g.strokeStyle = scaleRgb(dial, 0.16); g.lineWidth = s * 0.03; g.beginPath(); g.arc(c, c, r * 0.95, a0, a0 + span); g.stroke();
  g.font = `${Math.round(s * 0.085)}px sans-serif`; g.textAlign = "center"; g.textBaseline = "middle";
  for (let i = 0; i <= 30; i++) {
    const major = i % 5 === 0, a = a0 + span * i / 30;
    g.strokeStyle = major ? dial : scaleRgb(dial, 0.55); g.lineWidth = major ? s * 0.02 : s * 0.008;
    g.beginPath(); g.moveTo(...pt(a, r * 0.88)); g.lineTo(...pt(a, r * (major ? 0.74 : 0.81))); g.stroke();
    if (major) { g.fillStyle = dial; g.fillText(String(i / 5), ...pt(a, r * 0.6)); }
  }
  g.strokeStyle = needle; g.lineWidth = s * 0.03;
  g.beginPath(); g.moveTo(c, c); g.lineTo(...pt(a0 + span * 0.62, r * 0.8)); g.stroke();
  g.fillStyle = scaleRgb(dial, 0.16); g.lineWidth = s * 0.016; g.beginPath(); g.arc(c, c, s * 0.05, 0, 2 * Math.PI); g.fill(); g.stroke();
}

function updateThemeInfo() {
  const id = Number($("display.theme").value), t = themeById(id), col = themeColours(id);
  drawThemePreview(col.dial, col.needle);
  const sw = `<span class="swatch" style="background:${col.dial}"></span>dial ${col.dial}&nbsp;&nbsp;<span class="swatch" style="background:${col.needle}"></span>needle ${col.needle}`;
  let html;
  if (id === THEME_MATCH) {
    html = themesCache.matched !== THEME_NONE
      ? `Matched from the VIN: <b>${esc(themesCache.matchedName)}</b>${themesCache.vin ? " (" + esc(themesCache.vin) + ")" : ""}`
      : "No VIN read yet - the Custom colours below are used until Bluetooth OBD reads one.";
  } else if (id === THEME_CUSTOM) html = "Your own dial and needle colours.";
  else if (id < THEME_CUSTOM) html = esc(t.name) + " - the original can2gauge palette.";
  else html = `<b>${esc(t.name)}</b><br>${esc(t.era)}<br><span class="conf">${confText[t.conf] || ""}</span>`;
  $("themeInfo").innerHTML = html + "<br>" + sw;
  $("themeCustom").style.display = (id === THEME_CUSTOM || (id === THEME_MATCH && themesCache.matched === THEME_NONE)) ? "" : "none";
}

async function loadThemes() {
  themesCache = await api("/api/themes");
  const cur = $("display.theme").value, mk = $("boot.make").value;
  renderThemeOptions();
  if (cur !== "") { $("display.theme").value = cur; $("boot.make").value = mk; }
}

// ---------------------------------------------------------------------------
// your own boot logo (Boot logo = "Your own image"): any browser-readable image -> 480x480 on black -> RGB565 ->
// "C2GL" (RLE) -> /logo.bin. Same format as tools/logo_convert.py and the GaugeUI loader (GaugeUI.cpp loadLogo); the
// make logos in /logos/<make>.bin ship with the web UI files.
// ---------------------------------------------------------------------------
let logoImg = null;
function logoCanvas(size) {
  const cv = document.createElement("canvas"); cv.width = cv.height = size;
  const g = cv.getContext("2d");
  g.fillStyle = "#000"; g.fillRect(0, 0, size, size);
  if (logoImg) {
    const box = size * Math.min(100, Math.max(40, Number($("logoUpScale").value) || 80)) / 100;
    const iw = logoImg.naturalWidth || box, ih = logoImg.naturalHeight || box, k = Math.min(box / iw, box / ih);
    g.imageSmoothingQuality = "high";
    g.drawImage(logoImg, (size - iw * k) / 2, (size - ih * k) / 2, iw * k, ih * k);
  }
  return cv;
}
function drawLogoPreview() {
  const cv = $("logoPreview"), g = cv.getContext("2d"), s = cv.width;
  g.clearRect(0, 0, s, s);
  g.save(); g.beginPath(); g.arc(s / 2, s / 2, s / 2, 0, 2 * Math.PI); g.clip();
  g.drawImage(logoCanvas(480), 0, 0, s, s); // the 480 render scaled down, clipped to the round glass
  g.restore();
}
function encodeC2GL(cv) {
  const w = cv.width, h = cv.height, d = cv.getContext("2d").getImageData(0, 0, w, h).data;
  const px = new Uint16Array(w * h);
  for (let i = 0, j = 0; i < px.length; i++, j += 4) px[i] = ((d[j] >> 3) << 11) | ((d[j + 1] >> 2) << 5) | (d[j + 2] >> 3);
  // packets: c < 0x80 -> c + 1 literal pixels follow, c >= 0x80 -> next pixel repeated (c & 0x7F) + 2 times
  const out = [];
  let i = 0;
  while (i < px.length) {
    let run = 1;
    while (i + run < px.length && px[i + run] === px[i] && run < 129) run++;
    if (run >= 2) { out.push(0x80 | (run - 2), px[i] & 255, px[i] >> 8); i += run; continue; }
    const start = i; let n = 0;
    while (i < px.length && n < 128 && !(i + 1 < px.length && px[i + 1] === px[i])) { i++; n++; }
    out.push(n - 1);
    for (let k = start; k < start + n; k++) out.push(px[k] & 255, px[k] >> 8);
  }
  const rle = out.length < px.length * 2;
  const len = rle ? out.length : px.length * 2;
  const buf = new Uint8Array(16 + len), dv = new DataView(buf.buffer);
  buf.set([0x43, 0x32, 0x47, 0x4C, 1, rle ? 1 : 0]); // "C2GL", version 1, flags
  dv.setUint16(6, w, true); dv.setUint16(8, h, true); dv.setUint32(12, len, true);
  if (rle) buf.set(out, 16); else buf.set(new Uint8Array(px.buffer), 16);
  return buf;
}
async function uploadLogo() {
  if (!logoImg) return toast("choose an image first");
  const bytes = encodeC2GL(logoCanvas(480));
  const fd = new FormData();
  fd.append("file", new Blob([bytes], { type: "application/octet-stream" }), "logo.bin");
  toast("uploading logo.bin (" + Math.round(bytes.length / 1024) + " kB)...");
  const r = await fetch("/api/files/upload?dir=" + encodeURIComponent("/"), { method: "POST", body: fd });
  const j = await r.json();
  toast(j.error || "uploaded - press Show it now to see it on the glass");
  ownLogoState(); loadFiles();
}
// whether /logo.bin is on the gauge (under the Convert & upload button)
async function ownLogoState() {
  const r = await api("/api/files").catch(() => ({ files: [] }));
  const f = (r.files || []).find(x => x.path === "/logo.bin");
  let text = "No logo of yours on the gauge yet: the boot screen shows the text version until one is uploaded.";
  if (f) {
    text = "Your logo is on the gauge (" + Math.round(f.size / 1024) + " kB). Uploading again replaces it.";
  }
  $("ownLogoState").textContent = text;
}

// ---------------------------------------------------------------------------
// files
// ---------------------------------------------------------------------------
// true for a file the gauge needs to run: the web UI itself (top-folder .html / .js / .css / .json), the make logos and
// the sprite packs (those have their own card on Settings). These are never listed here, so they can't be deleted
// (the firmware refuses it too - can2gauge_API.cpp isProtectedFile).
function isSystemFile(path) {
  if (path.startsWith("/logos/") || path.startsWith("/sprites/")) return true;
  const inTopFolder = path.lastIndexOf("/") === 0;
  if (inTopFolder && /\.(html|js|css|json|gz)$/i.test(path)) return true;
  return false;
}
async function deleteFile(path) {
  if (!confirm("Delete " + path + "?")) return;
  const r = await fetch("/api/files/delete", { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body: "path=" + encodeURIComponent(path) });
  const j = await r.json();
  if (j.error) toast(j.error);
  loadFiles();
}
async function loadFiles() {
  const r = await api("/api/files");
  const list = $("fileList"); list.innerHTML = "";
  const userFiles = r.files.filter(f => !isSystemFile(f.path));
  if (!userFiles.length) list.innerHTML = '<div class="help">No files of yours on the gauge yet.</div>';
  userFiles.forEach(f => {
    const d = document.createElement("div"); d.className = "file-row";
    d.innerHTML = `<span><a href="/download?path=${encodeURIComponent(f.path)}">${esc(f.path)}</a> <span class="help">${Math.round(f.size / 1024 * 10) / 10} kB</span></span><button class="btn-secondary">Delete</button>`;
    d.querySelector("button").addEventListener("click", () => deleteFile(f.path));
    list.appendChild(d);
  });
  $("fsUsage").textContent = "Flash used " + Math.round(r.used / 1024) + " / " + Math.round(r.total / 1024) + " kB";
  // ecu / cfg selectors
  const ecu = $("me7.ecuFile"), cfg = $("me7.cfgFile");
  ecu.innerHTML = ""; cfg.innerHTML = "";
  r.files.filter(f => f.path.toLowerCase().endsWith(".ecu")).forEach(f => ecu.appendChild(new Option(f.path, f.path, false, f.path === settingsCache.me7.ecuFile)));
  r.files.filter(f => f.path.toLowerCase().endsWith(".cfg")).forEach(f => cfg.appendChild(new Option(f.path, f.path, false, f.path === settingsCache.me7.cfgFile)));
  if (!ecu.options.length) ecu.appendChild(new Option("(upload a .ecu file)", settingsCache.me7.ecuFile));
  if (!cfg.options.length) cfg.appendChild(new Option("(upload a .cfg file)", settingsCache.me7.cfgFile));
  renderKlFromEcu(); // 0.17.6: the .ecu picked decides the baud shown
}
async function uploadFile(input, url, onDone) {
  const f = input.files[0];
  if (!f) return toast("choose a file first");
  const fd = new FormData(); fd.append("file", f);
  toast("uploading " + f.name + "...");
  const r = await fetch(url, { method: "POST", body: fd });
  const j = await r.json();
  toast(j.status || j.error || "done");
  if (onDone) onDone(j);
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------
async function init() {
  catalogue = await api("/api/signals");
  if (catalogue.obdPids && catalogue.obdPids.length) OBD_PIDS = catalogue.obdPids.map(p => [p[0], p[1], p[5], p[2], p[3], p[4]]);
  settingsCache = await api("/api/settings");
  await loadThemes(); // theme / logo options must exist before fillSimple sets them
  fillSimple(settingsCache.display, "display");
  fillSimple(settingsCache.units || {}, "units");
  fillSimple(settingsCache.boot || {}, "boot");
  fillSimple(settingsCache.saver || {}, "saver");
  unitRowsSetup(); // 0.27.0: a unit per quantity
  rotationSetup(); // 0.25.1: Screen Rotation card
  updateThemeInfo();
  $("display.theme").addEventListener("change", updateThemeInfo);
  ["display.themeDial", "display.themeNeedle"].forEach(id => $(id).addEventListener("input", updateThemeInfo));
  $("logoUpFile").addEventListener("change", e => {
    const f = e.target.files[0]; if (!f) return;
    const img = new Image();
    img.onload = () => { logoImg = img; drawLogoPreview(); $("logoPick").textContent = f.name; $("logoInfo").textContent = f.name + " - " + (img.naturalWidth || "?") + " x " + (img.naturalHeight || "?") + " px. Adjust the size until it sits inside the circle, then upload."; };
    img.onerror = () => toast("can't read that image");
    img.src = URL.createObjectURL(f);
  });
  $("logoUpScale").addEventListener("input", drawLogoPreview);
  $("logoPick").addEventListener("click", () => $("logoUpFile").click()); // a normal-height button opens the (hidden) file picker
  $("logoUpBtn").addEventListener("click", uploadLogo);
  drawLogoPreview();
  fillSimple(settingsCache.can, "can");
  fillSimple(settingsCache.obd || {}, "obd");
  fillSimple(settingsCache.kline, "kline");
  fillSimple(settingsCache.me7 || {}, "me7");
  $("me7.fast").value = settingsCache.me7.fast ? "2" : "0"; // 0.19.0: the saved Fast log choice (Test link is never saved)
  renderKlFromEcu(); // 0.17.6: KWP2000 baud, or the .ecu's LogSpeed with the ME7 logger
  if (!$("kline.baud").value) $("kline.baud").value = "10400"; // 0.17.3: a stored baud that isn't in the list shows as 10400
  fillSimple(settingsCache.wifi, "wifi");
  for (let i = 0; i < 16; i++) $("chain.nodeId").appendChild(new Option("G" + i, i));
  $("chainTable").addEventListener("change", chainPicked); // 0.29.0: a gauge's Role / Number picker changed
  fillSimple(settingsCache.elm, "elm");
  fillSimple(settingsCache.nav || {}, "nav"); // 0.30.0: nav link
  fillSimple(settingsCache.chain, "chain");
  renderDsg(); renderDsgBoxes(); renderMenuToggles(); renderOil(); renderKlMode(); renderAdc();
  $("brightVal").textContent = settingsCache.display.brightness; $("nightVal").textContent = settingsCache.display.nightBrightness;
  $("display.brightness").addEventListener("input", e => { $("brightVal").textContent = e.target.value; });
  $("display.nightBrightness").addEventListener("input", e => { $("nightVal").textContent = e.target.value; });
  await loadSourceData(); // DBC signals + module label names for the page editor
  spritesCache = await api("/api/sprites").catch(() => ({ sprites: [] }));
  renderPages(); renderIo(); renderOutputs(); renderTriggers();
  fillSimple(settingsCache.power, "power");
  $("ignCanId").value = (settingsCache.power.canId & 0x1FFFFFFF).toString(16).toUpperCase();
  ignBoxes();
  ["power.ignSource", "power.canMode"].forEach(id => $(id).addEventListener("change", ignBoxes));
  loadSprites();
  $("spriteUpBtn").addEventListener("click", uploadSprite);
  $("runnerBest").textContent = String(settingsCache.learnt.runnerBest || 0).padStart(5, "0");
  $("runnerReset").addEventListener("click", async () => {
    if (!confirm("Reset the Rev Runner high score?")) return;
    await action("runnerReset");
    settingsCache = await api("/api/settings");
    $("runnerBest").textContent = String(settingsCache.learnt.runnerBest || 0).padStart(5, "0");
  });
  // boot logo choice: the custom text box, the make picker and the "your own logo" card only show when they apply
  const bootText = () => {
    const choice = Number($("boot.logo").value);
    $("bootTextBox").style.display = choice === 3 ? "" : "none";
    $("logoMakeBox").style.display = choice === 5 ? "" : "none";
    $("logoList").style.display = choice === 5 ? "" : "none";
    $("ownLogoCard").style.display = choice === 4 ? "" : "none";
    if (choice === 4) ownLogoState();
  };
  $("boot.logo").addEventListener("change", bootText);
  bootText();
  // every setting applies as soon as it changes - there are no Save buttons
  const sec = (id) => $(id).closest("section");
  autoSave($("pagesList"), () => Object.assign({ pages: collectPages() }, carSettingsExtra()), "gauges saved - screen rebuilt"); // 0.37.0: + Car picks' slots
  autoSave([sec("io.outputMode"), sec("io.nightSource")], () => ({ io: { outputMode: Number($("io.outputMode").value),
    nightSource: Number($("io.nightSource").value), dimInvert: Number($("io.dimInvert").value) } }), "output + lights saved");
  autoSave($("trigList"), () => ({ triggers: collectTriggers() }), "triggers saved");
  autoSave($("ignCard"), collectPower, "ignition saved");
  autoSave($("drawCard"), () => ({ display: { renderMode: Number($("display.renderMode").value), simdBlend: Number($("display.simdBlend").value) } }),
    "drawing saved (the mode applies after a reboot, SIMD at once)");
  autoSave($("spritesCard"), () => ({ saver: { enabled: Number($("saver.enabled").value), delayS: Number($("saver.delayS").value),
    sprite: $("saver.sprite").value, react: Number($("saver.react").value) } }), "screensaver saved");
  autoSave(sec("can.enabled"), () => ({ can: collectSimple("can", ["enabled", "kbps", "profile", "termination", "relay", "maxxBaseId"]),
    haldex: collectSimple("haldex", ["broadcastId", "controlId"]) }), "CAN saved");
  autoSave($("canObdCard"), () => ({ obd: { overCan: Number($("obd.overCan").value), ecu: Number($("obd.ecu").value) } }), "OBD-II over CAN saved");
  const maxxBox = () => { const on = Number($("can.profile").value) === 4; $("maxxBox").style.display = $("maxxHelp").style.display = on ? "" : "none"; };
  $("can.profile").addEventListener("change", maxxBox);
  maxxBox();
  autoSave([sec("kline.protocol"), $("me7Card")], () => ({ kline: collectSimple("kline", ["protocol", "ecuAddr", "baud", "fastObd"]),
    me7: collectSimple("me7", ["ecuFile", "cfgFile", "sps", "logToFile"]) }), "K-line saved - reconnecting");
  $("kline.protocol").addEventListener("change", renderKlFromEcu);   // 0.17.6: ME7 logger <-> KWP2000 baud box
  $("me7.ecuFile").addEventListener("change", renderKlFromEcu); // another .ecu: its LogSpeed
  // 0.20.0: the ME7 logged-variable checkboxes - Find to add, tick/untick, a different .cfg re-reads its list
  $("me7.cfgFile").addEventListener("change", () => { me7CfgSel.file = null; renderMe7CfgPicker(); });
  $("me7CfgFind").addEventListener("input", renderMe7CfgPicker);
  $("me7CfgList").addEventListener("change", (e) => {
    if (e.target && e.target.matches('input[type="checkbox"]')) { me7CfgToggle(e.target.dataset.name, e.target.checked); }
  });
  // 0.19.0: Fast log (RAM routine). Tells the ME7 logger the mode; it installs on the next connect (the K-line save
  // above reconnects). 0 Normal, 1 Test link (B7 -> C2G!, one-off), 2 Fast. Normal / Fast are saved on the gauge.
  $("me7.fast").addEventListener("change", () => {
    const m = Number($("me7.fast").value);
    api("/api/me7/fast", "POST", { on: m !== 0, test: m === 1 });
    if (m !== 1) settingsCache.me7.fast = m === 2 ? 1 : 0; // keep the cached copy in step with what was saved
    const msg = ["Normal logging saved", "Test link armed - restart logging, the ECU should answer C2G!", "Fast logging saved - restart logging"][m];
    toast(msg || "Fast log set");
  });
  // K-line mode module: a pick from the list sets the address and saves; "Other" shows the hex field
  $("klModeSel").addEventListener("change", (e) => {
    const custom = e.target.value === "custom";
    $("klModeCustomBox").style.display = custom ? "" : "none";
    if (custom) return $("kline.modeAddr").focus();
    setVal("kline.modeAddr", Number(e.target.value));
    $("kline.modeAddr").dispatchEvent(new Event("change", { bubbles: true }));
  });
  autoSave($("klModeCard"), () => ({ kline: collectSimple("kline", ["modeAddr"]) }), "K-line mode module saved", async () => { renderKlMode(); });
  $("klPrev").addEventListener("click", () => action("klStep", { dir: -1 }));
  $("klNext").addEventListener("click", () => action("klStep", { dir: 1 }));
  $("klGoto").addEventListener("change", (e) => action("klGoto", { group: Number(e.target.value) }));
  $("klLabelSel").addEventListener("change", (e) => { e.target.dataset.key = ""; action("klLabel", { file: e.target.value }); });
  $("klLive").addEventListener("change", klLiveChanged);
  // 0.17.5: Find... and "Show Formula and Spec"
  $("klFind").addEventListener("input", renderKlFind);   // each key typed redraws the matches
  $("klFind").addEventListener("focus", klFindOpen);     // back in the box: the list again
  $("klFind").addEventListener("keydown", klFindKey);
  $("klFind").addEventListener("blur", klFindClose);
  $("klFindList").addEventListener("mousedown", klFindHold);
  $("klFindList").addEventListener("click", klFindClick);
  try {
    $("klDetails").checked = localStorage.getItem("c2g.klDetails") === "1"; // as last left in this browser
  } catch (e) { /* private window: off */ }
  $("klDetails").addEventListener("change", klDetailsChanged);
  // CAN: DBC picker / upload
  $("dbcMatches").addEventListener("click", (e) => { const b = e.target.closest("[data-dbc]"); if (b) loadDbcToGauge(b.dataset.dbc); });
  $("dbcLoad").addEventListener("click", () => { if ($("dbcPick").value) loadDbcToGauge($("dbcPick").value); });
  $("dbcSlots").addEventListener("click", dbcSlotClicked);   // 0.28.0: Remove on a slot
  $("dbcSlots").addEventListener("change", dbcSlotBaseChanged); // 0.28.0: a slot's base id (relocatable ECU)
  $("dbcUpload").addEventListener("click", async () => {
    const f = $("dbcFile").files[0];
    if (!f) return toast("choose a .dbc file first");
    const c2d = dbcToC2d(await f.text(), f.name.replace(/\.dbc$/i, ""));
    if (!c2d.includes("\nM\t")) return toast("no messages found in " + f.name);
    postDbc(c2d);
  });
  autoSave([sec("display.theme"), $("bootLogoCard")], () => { // 0.23.0: the boot logo has a card of its own
    const d = collectSimple("display", ["brightness", "nightBrightness", "theme", "themeDial", "themeNeedle", "gaugeFeel", "swipeAnimMs", "longPressMs"]); // the start page follows the glass (last page shown)
    const u = collectSimple("units", ["quick"].concat(UNIT_ROW_KEYS));
    const b = collectSimple("boot", ["logo", "text", "fadeMs"]);
    b.make = $("boot.make").value; // "" = follow the theme (collectSimple would drop it)
    return { display: d, units: u, boot: b };
  }, "display saved", async () => {
    await loadThemes(); updateThemeInfo();
    // 0.27.0: a unit change turned the gauges' Min / Max / Redline over on the gauge - show the new numbers
    fillSimple(settingsCache.display, "display"); fillSimple(settingsCache.units || {}, "units"); unitRowsShow(); renderPages();
  });
  // the access point: applies on leaving the field, after a warning - the phone loses the gauge while WiFi restarts
  ["wifi.apSsid", "wifi.apPassword", "wifi.hostname"].forEach((id) => $(id).addEventListener("change", () => {
    const w = collectSimple("wifi", ["apSsid", "hostname"]), cur = settingsCache.wifi;
    w.apPassword = $("wifi.apPassword").value; // "" is meaningful (= open AP): always send it
    if (w.apPassword.length > 0 && w.apPassword.length < 8) { toast("AP password: leave it blank for an open AP, or use 8+ characters"); return; }
    if (w.apSsid === cur.apSsid && w.apPassword === cur.apPassword && w.hostname === cur.hostname) return;
    if (!confirm("Apply now? The gauge's WiFi restarts - reconnect to \"" + w.apSsid + "\" afterwards.")) { fillSimple(cur, "wifi"); return; }
    saveSettings({ wifi: w }, "WiFi restarting");
  }));
  autoSave($("menuToggles"), () => ({ menu: collectMenu() }), "menu saved - screen menu rebuilt");
  $("menuToggles").addEventListener("change", () => applyMenu(collectMenu())); // tabs / tiles follow at once
  applyMenu(settingsCache.menu);
  $("modeTiles").addEventListener("click", (e) => { const t = e.target.closest("[data-tab]"); if (t) document.querySelector(`.nav-tab[data-page="${t.dataset.tab}"]`).click(); });
  // Haldex tab
  fillSimple(settingsCache.haldex, "haldex");
  const hdxKeyHint = () => { $("haldex.key").value = ""; $("haldex.key").placeholder = settingsCache.haldex.keySet ? "saved - type to change" : "none (open WiFi)"; };
  hdxKeyHint();
  autoSave(sec("haldex.wireless"), () => {
    const h = collectSimple("haldex", ["wireless", "carSignals"]);
    const k = $("haldex.key").value; if (k) h.key = k;
    return { haldex: h };
  }, "OpenHaldex link saved", hdxKeyHint);
  $("hdxGen").addEventListener("change", (e) => hdxCmd(2, Number(e.target.value)));
  $("hdxCtrl").addEventListener("change", (e) => hdxCmd(8, Number(e.target.value)));
  $("hdxLearnStart").addEventListener("click", () => hdxCmd(3));
  $("hdxLearnCancel").addEventListener("click", () => hdxCmd(4));
  $("hdxLearnClear").addEventListener("click", () => hdxCmd(5));
  $("hdxLongStart").addEventListener("click", () => hdxCmd(6, $("hdxLongAll").checked ? 1 : 0));
  $("hdxLongCancel").addEventListener("click", () => hdxCmd(7));
  $("hdxForget").addEventListener("click", () => { if (confirm("Forget this OpenHaldex? The next one heard is followed.")) action("haldexFollow", { mac: "" }); });
  $("hdxOthers").addEventListener("click", (e) => { const b = e.target.closest("[data-mac]"); if (b) action("haldexFollow", { mac: b.dataset.mac }); });
  // ECU over WiFi (usb2ip), CAN tab
  fillSimple(settingsCache.ecu || {}, "ecu");
  autoSave($("ecuCard"), () => ({ ecu: collectSimple("ecu", ["wireless", "carSignals"]) }), "ECU link saved");
  $("ecuForget").addEventListener("click", () => {
    if (confirm("Forget this usb2ip? The next one heard is followed.")) action("ecuFollow", { mac: "" });
  });
  $("ecuOthers").addEventListener("click", (e) => {
    const b = e.target.closest("[data-mac]");
    if (b) action("ecuFollow", { mac: b.dataset.mac });
  });
  autoSave(sec("dsg.source"), () => ({ dsg: collectDsg() }), "Gearbox saved");
  // VAG diag: the module picker sets the address field; each value row shows only the boxes its kind uses
  renderVdiag();
  $("vdModSel").addEventListener("change", (e) => {
    const custom = e.target.value === "custom";
    $("vdAddrBox").style.display = custom ? "" : "none";
    if (custom) return $("vdiag.addr").focus();
    setVal("vdiag.addr", Number(e.target.value));
    $("vdiag.addr").dispatchEvent(new Event("change", { bubbles: true }));
  });
  $("vdList").addEventListener("change", (e) => {
    const row = e.target.closest(".vd-row");
    if (row && e.target.matches(".vd-kind")) {
      vdRowBoxes(row);
      // 0.37.0: a value set up in the empty row: the next empty row shows, to add another
      const hidden = [...document.querySelectorAll(".vd-row")].find(r => r.style.display === "none");
      if (Number(e.target.value) && hidden) hidden.style.display = "";
    }
  });
  autoSave([$("vdModuleCard"), $("vdValuesCard")], () => ({ vdiag: collectVdiag() }), "Diagnostics saved - session restarting");
  $("vdProbeBtn").addEventListener("click", async () => {
    const did = parseInt($("vdProbeId").value, 16);
    if (isNaN(did) || did < 0 || did > 0xFFFF) return toast("type a DID in hex, e.g. F187");
    $("vdProbeOut").textContent = "asking...";
    await action("vdiagProbe", { did });
  });
  autoSave([$("adcCard"), $("adcList")], () => ({ adc: collectAdc() }), "ADC saved");
  autoSave(sec("oil.source"), () => ({ oil: collectOil() }), "oil source saved");
  $("oilAddPage").addEventListener("click", addOilPage);
  $("oilShowPage").addEventListener("click", () => action("oilShow"));
  const elmPassHint = () => { $("elm.wifiPass").value = ""; $("elm.wifiPass").placeholder = settingsCache.elm.wifiPassSet ? "saved - type to change" : "none (open network)"; };
  elmPassHint();
  elmBoxes();
  $("elm.transport").addEventListener("change", elmBoxes);
  const collectElm = () => {
    const e = collectSimple("elm", ["enabled", "pollObd", "fastObd", "transport", "wifiHost", "wifiPort", "extraSource", "klAddr", "klFast"]);
    e.wifiSsid = $("elm.wifiSsid").value.trim(); // "" is meaningful (= first OBD-looking network): always send it
    const p = $("elm.wifiPass").value; if (p) e.wifiPass = p;
    return { elm: e };
  };
  autoSave(sec("elm.enabled"), collectElm, "OBD adapter saved");
  // 0.37.7: the adapter's K-line has its own card on the K-line tab: On = Adapter Source 1 (VAG K-line blocks)
  const klAdapterShow = () => { $("klViaAdapter").value = String(Number(Number($("elm.extraSource").value) === 1)); }; // "1" / "0"
  klAdapterShow();
  $("elm.extraSource").addEventListener("change", klAdapterShow);
  $("klViaAdapter").addEventListener("change", () => {
    const on = Number($("klViaAdapter").value) === 1;
    if (on) $("elm.extraSource").value = "1";
    else if (Number($("elm.extraSource").value) === 1) $("elm.extraSource").value = "0";
    $("elm.klFast").dispatchEvent(new Event("change", { bubbles: true })); // the switch isn't a setting itself: save the card
  });
  autoSave($("klAdapterCard"), collectElm, "K-line through the adapter saved");
  $("elmRestart").addEventListener("click", async () => { toast("Restarting - Bluetooth starts first"); await action("reboot"); });
  // 0.30.0: nav link (Google Maps turns from the can2gauge Link phone app)
  autoSave($("navCard"), () => ({ nav: collectSimple("nav", ["mode", "popupM"]) }), "Navigation saved");
  $("navTest").addEventListener("click", () => action("navTest"));
  $("navRestart").addEventListener("click", async () => { toast("Restarting - Bluetooth starts first"); await action("reboot"); });
  // the WiFi fields (data-nosave) save when the field is left, not while typing: every save reconnects the adapter,
  // and a WiFi re-join moves the gauge's own WiFi to the adapter's channel
  ["elm.wifiSsid", "elm.wifiPass", "elm.wifiHost", "elm.wifiPort"].forEach(id =>
    $(id).addEventListener("change", () => saveSettings(collectElm(), "OBD adapter saved")));
  $("elmWifiScan").addEventListener("click", elmWifiScan);
  $("elmScan").addEventListener("click", () => elmAction("scan"));
  $("elmForget").addEventListener("click", () => { if (confirm("Forget the saved adapter?")) elmAction("forget"); });
  $("elmReid").addEventListener("click", () => elmAction("reidentify"));
  const sendCmd = () => { const c = $("elmCmd").value.trim(); if (c) { elmAction("cmd", { cmd: c }); $("elmCmd").value = ""; } };
  $("elmSend").addEventListener("click", sendCmd);
  $("elmCmd").addEventListener("keydown", e => { if (e.key === "Enter") sendCmd(); });
  document.querySelectorAll("[data-elmtest]").forEach(b => b.addEventListener("click", () => {
    // Clear DTCs is the one that changes the car: ask first
    if (b.dataset.elmtest === "clear" && !confirm("Clear the fault codes? Engine off, ignition on. This also resets the emissions readiness monitors.")) return;
    elmAction("test", { kind: b.dataset.elmtest, module: parseInt($("elmTestModule").value, 16) || 1, group: Number($("elmTestGroup").value) || 1 });
  }));
  autoSave(sec("chain.enabled"), () => {
    // 0.29.0: role and row number are not here any more - the Gauge Numbers table sets them, for any gauge
    const c = collectSimple("chain", ["enabled", "nodeId", "nodeName", "group", "transport", "canTx", "canBaseId", "followPage", "followNight", "failover"]);
    return { chain: c };
  }, "chain saved");
  $("backupBtn").addEventListener("click", () => { const a = document.createElement("a"); a.href = "data:application/json," + encodeURIComponent(JSON.stringify(settingsCache, null, 2)); a.download = "can2gauge_settings.json"; a.click(); });
  $("importBtn").addEventListener("click", () => $("importFile").click());
  $("importFile").addEventListener("change", importBackup);
  $("factoryBtn").addEventListener("click", () => { if (confirm("Restore factory defaults and reboot?")) action("factoryReset"); });
  $("fileUploadBtn").addEventListener("click", () => uploadFile($("fileUpload"), "/api/files/upload?dir=" + encodeURIComponent("/me7"), loadFiles)); // ME7 .ecu / .cfg files
  pollStatus();
  setInterval(pollStatus, setIntervalDuration);
}
document.addEventListener("DOMContentLoaded", init);
