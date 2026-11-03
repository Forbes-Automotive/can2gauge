// can2gauge web UI - First Boot Setup (src/can2gauge_FirstBoot.cpp keeps the state, GET / POST /api/firstboot).
// A hidden tab: it opens by itself when the driver said Yes on the glass (or the gauge was never set up), and from
// Settings > Device > First Boot Setup. Three steps:
//   1. Check   every source, using the endpoints the other tabs already use, with a tick or cross per row
//   2. Setup   a table of suggested settings from what was found (CAN profile, OBD adapter, K-line, power, dimming,
//              boot logo, sprites, menu ...), each one changeable
//   3. Gauges  the gauges to show, suggested from the live values seen, ticked / unticked and put in order
//   4. Car     0.35.0: the car the VIN says it is (car.js), confirmed or picked by hand, with its paint colour
// Apply sends it all in one POST /api/settings (the same partial JSON the tabs send), then marks setup done.
// Uses app.js's helpers: $, api, action, toast, esc, uShown, catalogue, settingsCache, statusCache.

// ---------------------------------------------------------------------------
// what the checks found (filled by fbRunChecks, read by the suggestions)
// ---------------------------------------------------------------------------
let fbFound = null;          // the results, see fbEmptyResults()
let fbRunning = false;       // a check is running (the Check again button waits)
let fbCandidates = [];       // step 3: the pages on offer, in the driver's order: {key, picked, page, why, live, group}
let fbTriggerCandidates = []; // 0.38.4: step 3's suggested triggers: {trigger, why, picked}
const FB_WATCH_MS = 8000;    // how long the passive checks listen (CAN, OpenHaldex, adc2can, usb2ip, other gauges)
const FB_KLINE_MS = 16000;   // the K-line: a 5-baud wake-up and the module's answer take a few seconds
const FB_CHECKS_MS = 20000;  // 0.38.0: about how long all the checks take (the screen's ring creeps on over it)
const FB_OBD_WIFI = /OBD|ELM|V-?LINK|VGATE|ICAR|KONNWEI|VEEPEAK|WIFI_OBD/i; // WiFi names of OBD adapters (as the Bluetooth OBD tab)
// 0.23.1: the gauge's own board temperature and uptime - not values "seen" from the car. 0.38.0: + its dash-light input,
// output and output fault: always a reading (an open dash-light wire reads off), so they're no sign of the car either
const FB_ALWAYS_THERE = ["boardTemp", "uptime", "lights", "output", "outFault"];

// CAN ids that give the car away (11-bit, the gauge's own 0x6A0-0x6EF left out)
const FB_PQ_IDS = [0x280, 0x288, 0x380, 0x1A0, 0x320, 0x420, 0x440, 0x570, 0x572]; // Motor_1/2/3, Bremse_1, Kombi_1/2, Getriebe_1, BSG_Last, ZAS_1
const FB_MQB_IDS = [0x0A8, 0x0FD, 0x0AD, 0x3C0, 0x107, 0x30B, 0x0A7, 0x6B2];       // Motor_12, ESP_21, Getriebe_11, Klemmen_Status_01 ...

// gauge styles (include/can2gauge_defs.h gaugeStyle_t)
const FB_STYLE = { arc: 0, needle: 1, bar: 2, numeric: 3, dual: 4, haldex: 5, quad: 6, gear: 7, oil: 8, custom: 9, graph: 10, triple: 11 }; // numeric = "Single", oil = "Oil (PQ)" in the list
// a sensible red line and decimals per value (the catalogue gives the range)
const FB_REDLINE = { rpm: 6800, coolant: 110, oiltemp: 130, boost: 1.6, map: 2200, iat: 60, batt: 15, gboxTemp: 120 };
const FB_DECIMALS = { boost: 2, lambda: 2, batt: 1, timing: 1 };

function fbEmptyResults() {
  return {
    can: { ok: false, text: "", ids: 0, rate: 0, profile: -1, profileWhy: "", kombi2: false, car: "", dbcs: [] },
    relay: { ok: false, text: "" },
    obdCan: { ok: false, text: "", answered: [] }, // answered: the ECUs' answer ids (0x7E8 engine, 0x7E9 gearbox ...)
    haldex: { ok: false, text: "" },
    adc: { ok: false, text: "", boards: [] },
    ecu: { ok: false, text: "" },
    kline: { ok: false, text: "", ecuId: "" },
    klineObd: { ok: false, text: "", supported: [], protocol: "", skip: false }, // 0.38.6: OBD-II on the K-line at 33
    ble: { ok: false, text: "", adapters: [], connected: false },
    obdWifi: { ok: false, text: "", networks: [] },
    chain: { ok: false, text: "", nodes: 0 },
    vin: { make: "", makeKey: "", model: "", year: "" },
    live: {},          // every signal key seen valid during the checks: key -> {n: name, u: unit}
    lightsSeen: [],    // the illumination input's states seen (true / false)
  };
}

// ---------------------------------------------------------------------------
// the tab itself: built here, so index.html only needs the <script> line
// ---------------------------------------------------------------------------
function fbBuildTab() {
  const nav = document.querySelector(".nav-tabs");
  const tab = document.createElement("button");
  tab.className = "nav-tab";
  tab.dataset.page = "firstboot";
  tab.id = "fbTab";
  tab.textContent = "First Boot";
  tab.style.display = "none"; // hidden until the setup is opened
  nav.insertBefore(tab, nav.firstChild);
  // the same switching as app.js's tabs (app.js wired the ones that existed when it loaded)
  tab.addEventListener("click", function () {
    document.querySelectorAll(".nav-tab").forEach(function (t) { t.classList.remove("active"); });
    document.querySelectorAll(".page").forEach(function (p) { p.classList.remove("active"); });
    tab.classList.add("active");
    $("firstboot-page").classList.add("active");
  });

  const page = document.createElement("div");
  page.className = "page";
  page.id = "firstboot-page";
  page.innerHTML = `
    <style>
      .fb-table { width: 100%; border-collapse: collapse; font-size: 13px; table-layout: fixed; } /* fixed: the columns keep to the card's width */
      .fb-table td:first-child, .fb-table th:first-child { width: 38%; }
      .fb-table th { text-align: left; color: var(--muted); font-weight: 600; font-size: 11px; text-transform: uppercase; letter-spacing: .6px; padding: 6px 8px; border-bottom: 1px solid var(--line); }
      .fb-table td { padding: 8px; border-bottom: 1px solid var(--line); vertical-align: top; }
      .fb-table td.fb-mark { width: 28px; font-size: 18px; text-align: center; }
      .fb-table td select { width: 100%; max-width: 100%; min-height: 36px; }
      .fb-table td { overflow-wrap: anywhere; } /* a long word breaks instead of running past the edge */
      .fb-menu-title { font-size: 13px; margin-bottom: 10px; }
      .fb-ok { color: var(--ok); } .fb-no { color: var(--muted); } .fb-busy { color: var(--accent); }
      .fb-why { color: var(--muted); font-size: 12px; margin-top: 2px; }
      .fb-steps { display: flex; gap: 8px; margin-bottom: 14px; flex-wrap: wrap; }
      .fb-step { padding: 4px 12px; border-radius: 999px; border: 1px solid var(--line); color: var(--muted); font-size: 12px; }
      .fb-step.on { color: var(--accent); border-color: var(--accent); }
      .fb-page { display: flex; align-items: center; gap: 10px; padding: 8px 10px; border: 1px solid var(--line); border-radius: 10px; margin-bottom: 8px; background: var(--panel-2); }
      .fb-push { display: flex; align-items: center; gap: 8px; margin-bottom: 4px; cursor: pointer; }
      .fb-page.off { opacity: .5; }
      .fb-page .fb-name { flex: 1 1 auto; min-width: 0; }
      .fb-page .fb-name b { display: block; font-size: 14px; }
      .fb-page button { padding: 4px 10px; min-width: 36px; }
      .fb-live { display: inline-block; font-size: 11px; padding: 0 7px; border-radius: 999px; border: 1px solid var(--ok); color: var(--ok); margin-left: 6px; }
      .fb-chips { display: flex; flex-wrap: wrap; gap: 6px; margin-top: 8px; }
    </style>
    <section class="card">
      <h2>First Boot Setup</h2>
      <div class="fb-steps"><span class="fb-step" id="fbStep1">1. Check what is connected</span><span class="fb-step" id="fbStep2">2. Suggested setup</span><span class="fb-step" id="fbStep3">3. Gauges</span><span class="fb-step" id="fbStep4">4. Your car</span></div>
      <div class="help" style="margin-top:0">The gauge looks for everything it can read from, then suggests a setup to match. Nothing changes until you press <b>Apply</b> at the bottom, and everything can be changed later on its own tab.</div>
    </section>
    <section class="card">
      <h2>1. What is connected</h2>
      <div class="help" style="margin-top:0" id="fbCheckHelp">Turn the ignition on (engine running is even better) so the car's modules are awake.</div>
      <table class="fb-table" id="fbCheckTable"></table>
      <div class="fb-chips" id="fbLive"></div>
      <div class="row-inline" style="margin-top:12px"><button class="btn-secondary" id="fbCheckAgain">Check Again</button></div>
    </section>
    <section class="card" id="fbSetupCard" style="display:none">
      <h2>2. Suggested setup</h2>
      <table class="fb-table" id="fbSetupTable"></table>
    </section>
    <section class="card" id="fbPagesCard" style="display:none">
      <h2>3. Gauges</h2>
      <div class="help">A starter set from what was found: only gauges this car can show, from every source the gauge found (the engine, the gearbox, adc2can, usb2ip, OpenHaldex, the car's other modules), 8 ticked. Untick or reorder any. The car is scanned as part of the checks and the best values are added here. Scan Again if the car has changed.</div>
      <div class="row-inline" data-nosave="1"><button class="btn-secondary" id="fbCarScan" style="flex:0 1 200px">Scan Again</button></div>
      <div class="car-progress" id="fbCarProgress" style="display:none"><div class="car-bar"><span id="fbCarBar"></span></div><div class="help" id="fbCarProgressText"></div></div>
      <div class="mono" id="fbCarLog" style="display:none"></div>
      <div id="fbPages"></div>
      <div class="fb-menu-title" style="margin-top:14px">Triggers</div>
      <div class="help">Alerts on the screen when a value goes past its limit: high revs, boost, coolant, battery, and the adc2can sensors' own warning limits. Only for values this car has; untick any. Change them later on the Triggers tab.</div>
      <div id="fbTriggers"></div>
    </section>
    <section class="card" id="fbCarCard" style="display:none">
      <h2>4. Your car</h2>
      <div class="help" style="margin-top:0">The car the VIN says this is (read from the OBD adapter, the CAN or the K-line). Check it, change anything that's wrong, pick the paint and press <b>This Is My Car</b>. No VIN? Pick the car by hand.</div>
      <div id="fbCarBox"></div>
    </section>
    <section class="card" id="fbApplyCard" style="display:none">
      <h2>Finish</h2>
      <div id="fbPushBox" style="display:none" data-nosave="1">
        <label class="fb-push"><input type="checkbox" id="fbPush" checked> <b>Set Up the Other Gauges the Same</b></label>
        <div class="help" id="fbPushWhy"></div>
      </div>
      <div class="row-inline">
        <button class="btn primary" id="fbApply">Apply</button>
        <button class="btn-secondary" id="fbSkip">Skip - Keep Current Settings</button>
      </div>
      <div class="help" id="fbApplyInfo"></div>
    </section>`;
  document.querySelector(".container").appendChild(page);

  $("fbCheckAgain").addEventListener("click", fbRunChecks);
  $("fbApply").addEventListener("click", fbApply);
  $("fbSkip").addEventListener("click", fbSkip);

  // Settings > Device: the button that runs it again (0.23.0: beside Download Backup, so it shows in Basic too -
  // Factory Reset is under More Options now)
  const backup = $("backupBtn");
  if (backup) {
    const again = document.createElement("button");
    again.className = "btn-secondary";
    again.id = "fbRunAgain";
    again.textContent = "First Boot Setup";
    backup.parentNode.insertBefore(again, backup.nextSibling);
    again.addEventListener("click", fbRunAgain);
  }
}

// waits until someone can actually see the page: a phone tab in the background reports "hidden", and a frame that
// isn't shown (the simulator's Web UI tab before it's picked) is 0 pixels wide
async function fbWaitUntilSeen() {
  while (document.visibilityState !== "visible" || window.innerWidth === 0) {
    await fbSleep(500);
  }
}

// 0.37.3: the setup's progress on the gauge's screen (a "SETTING UP" ring, held until Apply or Skip). 0.37.5: sent by
// carscan.js screenProgress, shared with the Scan My Car tab.
function fbScreen(detail, percent) {
  screenProgress("SETTING UP", detail, percent);
}

// opens the tab, then (once it's on screen) takes the login card off the glass and starts the checks
async function fbOpen() {
  $("fbTab").style.display = "";
  $("fbTab").click();
  document.documentElement.classList.remove("fb-pending"); // 0.38.0: the cover index.html put up comes off
  await fbWaitUntilSeen();
  try { await api("/api/firstboot", "POST", { opened: true }); } catch (e) { /* older firmware */ }
  screenSent = ""; // the screen starts again: send the next update even if it matches the last
  screenSetupEnded = false; // 0.37.7: a new setup: its updates go to the screen again
  fbRunChecks();
}

// Settings > Device > First Boot Setup
async function fbRunAgain() {
  if (!confirm("Run the First Boot Setup again?\n\nThe gauge checks what is connected and suggests a setup. Nothing changes until you press Apply.")) return;
  try { await api("/api/firstboot", "POST", { state: "waiting" }); } catch (e) { /* older firmware */ }
  fbOpen();
}

// ---------------------------------------------------------------------------
// step 1: the checks
// ---------------------------------------------------------------------------
const FB_ROWS = [ // [results key, row title]
  ["can", "CAN bus (wired)"],
  ["obdCan", "OBD-II over CAN"],
  ["relay", "CAN over WiFi"],
  ["haldex", "OpenHaldex"],
  ["adc", "adc2can sensors"],
  ["ecu", "usb2ip (Ignitron / MaxxECU)"],
  ["kline", "K-line (VAG engine 01)"],
  ["klineObd", "K-line OBD-II (any make, 33)"],
  ["ble", "Bluetooth OBD adapter"],
  ["obdWifi", "WiFi OBD adapter"],
  ["chain", "Other gauges (daisy chain)"],
];

// draws the check table: busy rows show a spinner-ish dot, finished rows a tick or a cross
function fbDrawChecks(busy) {
  let html = "<tr><th></th><th>Source</th><th>Found</th></tr>";
  FB_ROWS.forEach(function (row) {
    const key = row[0];
    const result = fbFound[key];
    let mark = "";
    let cls = "";
    if (busy[key]) {
      mark = "&#9679;"; // dot while checking
      cls = "fb-busy";
    } else if (result.skip) {
      mark = "&#8211;"; // 0.37.7: a dash - not needed, neither found nor missing
      cls = "fb-busy";
    } else if (result.ok) {
      mark = "&#10004;"; // tick
      cls = "fb-ok";
    } else {
      mark = "&#10008;"; // cross
      cls = "fb-no";
    }
    let text = result.text;
    if (busy[key] && !text) text = "checking...";
    html += `<tr><td class="fb-mark ${cls}">${mark}</td><td>${esc(row[1])}</td><td>${esc(text)}</td></tr>`;
  });
  $("fbCheckTable").innerHTML = html;
  const keys = Object.keys(fbFound.live);
  if (keys.length) {
    $("fbLive").innerHTML = '<span class="help" style="margin:0">Live values seen:</span>' +
      keys.map(function (k) { return `<span class="hdx-chip on">${esc(fbFound.live[k].n)}</span>`; }).join("");
  } else {
    $("fbLive").innerHTML = "";
  }
}

const fbSleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms)); // wait without blocking the page

// a GET that gives null instead of throwing (older firmware / the sim may lack an endpoint)
async function fbGet(path) {
  try { return await api(path); } catch (e) { return null; }
}

// the passive checks: listen for FB_WATCH_MS while the car talks
async function fbWatch(busy) {
  const start = Date.now();
  let lastCan = null;
  while (Date.now() - start < FB_WATCH_MS) {
    const status = await fbGet("/api/status");
    if (status) {
      statusCache = status;
      // every live value, and the illumination input's state
      Object.keys(status.signals || {}).forEach(function (k) {
        if (FB_ALWAYS_THERE.includes(k)) return; // the gauge's own readings: always there, so not "found"
        fbFound.live[k] = status.signals[k];
      });
      if (status.io && !fbFound.lightsSeen.includes(!!status.io.dashLights)) fbFound.lightsSeen.push(!!status.io.dashLights);
      // CAN over WiFi: an OpenHaldex / adc2can relaying the car's frames
      const relay = status.can && status.can.relay;
      if (relay && relay.senders && relay.senders.length) {
        fbFound.relay.ok = true;
        fbFound.relay.text = relay.senders.map(function (s) { return s.kind + " (" + s.perSec + " frames/s)"; }).join(", ");
      }
    }
    lastCan = (await fbGet("/api/can/ids")) || lastCan;
    const haldex = await fbGet("/api/haldex");
    if (haldex && haldex.via) {
      fbFound.haldex.ok = true;
      let name = "";
      if (haldex.ohx && haldex.ohx.name) name = haldex.ohx.name + " ";
      fbFound.haldex.text = name + "heard on " + haldex.via;
    }
    const adc = await fbGet("/api/adc");
    if (adc && adc.boards) {
      const boards = [];
      adc.boards.forEach(function (b, i) { if (b.seen) boards.push("board " + (i + 1) + " on " + (b.via || "CAN")); });
      if (boards.length) {
        fbFound.adc.ok = true;
        fbFound.adc.text = boards.join(", ");
        fbFound.adc.boards = adc.boards;
        fbFound.adc.ch = adc.ch;
      }
    }
    const ecu = await fbGet("/api/ecu");
    if (ecu && ecu.heard && ecu.usb2ip) {
      fbFound.ecu.ok = true;
      fbFound.ecu.text = ecu.usb2ip.kindName + (ecu.usb2ip.ecu ? " (" + ecu.usb2ip.ecu + ")" : "") + (ecu.usb2ip.live ? ", reading the ECU" : ", ECU not answering");
    } else if (ecu && !ecu.wireless) {
      fbFound.ecu.text = "listening is off (CAN tab)";
    }
    const chain = await fbGet("/api/chain");
    if (chain && !chain.enabled) {
      fbFound.chain.text = "the daisy chain is off (Daisy Chain tab)"; // 0.25.2: an off chain can't hear anyone
    } else if (chain && chain.nodes) {
      const others = chain.nodes.filter(function (n) { return n.node !== chain.node; });
      if (others.length) {
        fbFound.chain.ok = true;
        fbFound.chain.nodes = others.length;
        fbFound.chain.text = others.map(function (n) { return n.name || ("G" + n.node); }).join(", ");
      }
    }
    fbDrawChecks(busy);
    await fbSleep(700);
  }
  await fbJudgeCan(lastCan, statusCache);
  ["can", "relay", "haldex", "adc", "ecu", "chain"].forEach(function (k) { busy[k] = false; });
  ["relay", "haldex", "adc", "ecu", "chain"].forEach(function (k) { if (!fbFound[k].text) fbFound[k].text = "nothing heard"; });
  fbDrawChecks(busy);
}

// what the CAN ids say about the car
async function fbJudgeCan(ids, status) {
  const can = fbFound.can;
  if (!ids || !ids.enabled) {
    can.text = "CAN is switched off (CAN tab)";
    return;
  }
  const own = (id) => id >= 0x6A0 && id <= 0x6EF; // OpenHaldex + the gauge chain: not the car's
  const seen = {};
  let carIds = 0;
  (ids.ids || []).forEach(function (row) {
    const id = row[0];
    const extended = row.length > 5;
    if (!extended && !own(id)) {
      seen[id] = row[1];
      carIds++;
    }
  });
  can.ids = carIds;
  can.rate = ids.rate || 0;
  if (!carIds) {
    if (status && status.can && status.can.err > 0) can.text = "no frames, bus errors at " + ids.kbps + " kbit/s - check the wiring or the bit rate";
    else can.text = "no car frames heard at " + ids.kbps + " kbit/s";
    if (status && status.can && status.can.obd && status.can.obd.ecuFound) { // OBD-II answers (profile already OBD-II)
      can.ok = true;
      can.profile = 1;
      can.profileWhy = "the engine answers OBD-II requests";
      can.text = "engine answers OBD-II";
    }
    return;
  }
  can.ok = true;
  let pq = 0;
  let mqb = 0;
  FB_PQ_IDS.forEach(function (id) { if (seen[id] !== undefined) pq++; });
  FB_MQB_IDS.forEach(function (id) { if (seen[id] !== undefined) mqb++; });
  const maxxBase = (settingsCache && settingsCache.can && settingsCache.can.maxxBaseId) || 0x520;
  const maxx = seen[maxxBase] !== undefined && seen[maxxBase + 1] !== undefined;
  can.kombi2 = seen[0x420] !== undefined;
  if (maxx) {
    can.profile = 4;
    can.profileWhy = "MaxxECU frames from 0x" + maxxBase.toString(16).toUpperCase();
  } else if (pq >= 2 && pq >= mqb) {
    can.profile = 0;
    can.profileWhy = pq + " VAG PQ frames (Motor_1, Bremse_1 ...)";
  } else if (mqb >= 2) {
    can.profile = 3;
    can.profileWhy = mqb + " VAG MQB frames (ESP_21, Klemmen_Status_01 ...)";
  } else {
    can.profile = 2;
    can.profileWhy = "frames heard, but not a car the gauge decodes by itself (a DBC on the CAN tab can)";
  }
  can.text = carIds + " ids, " + Math.round(can.rate) + " frames/s at " + ids.kbps + " kbit/s";
  // the closest DBC from the library (as the CAN tab does)
  if (typeof dbcScore === "function" && typeof loadDbcLibrary === "function") {
    await fbGuessCar(seen);
  }
}

// the CAN tab's DBC fingerprints: the best match names the car
async function fbGuessCar(seen) {
  try {
    if (!dbcFps) await loadDbcLibrary();
    if (!dbcFps) return;
    const seenMap = new Map(Object.keys(seen).map(function (k) { return [Number(k), seen[k]]; }));
    const ranked = [];
    Object.keys(dbcFps).forEach(function (name) {
      const score = dbcScore(seenMap, dbcFps[name])[0]; // [score, share of the bus it explains, share of the DBC seen]
      if (score > 0) ranked.push({ name: name, score: score });
    });
    ranked.sort(function (a, b) { return b.score - a.score; });
    // the best three, leaving out the ones under half as good as the best (a few shared ids, not the car)
    const close = ranked.filter(function (m) { return m.score >= ranked[0].score / 2; });
    fbFound.can.dbcs = close.slice(0, 3).map(function (m) {
      const info = (dbcIndex && dbcIndex.dbcs || []).find(function (d) { return d.name === m.name; });
      let label = m.name;
      if (info && info.cars && info.cars.length) label = info.cars.slice(0, 3).join(", ") + " (" + m.name + ")";
      return { name: m.name, score: m.score, label: label };
    });
    if (fbFound.can.dbcs.length) {
      fbFound.can.car = fbFound.can.dbcs[0].label;
      fbFound.can.text += " - looks like " + fbFound.can.car;
    }
  } catch (e) { /* no DBC library */ }
}

// an OBD-II ECU by its answer id (ISO 15765-4: 0x7E8 the engine, 0x7E9 the gearbox, the rest numbered)
function fbObdEcuName(id) {
  const hex = "0x" + id.toString(16).toUpperCase();
  if (id === 0x7E8) return "engine (" + hex + ")";
  if (id === 0x7E9) return "gearbox (" + hex + ")";
  return "ECU " + hex;
}

// OBD-II over the gauge's own CAN: the gauge asks the engine (0x7DF "0100") for a few seconds, whatever the profile,
// and the status says whether it answered (0x7E8). Nothing is saved.
async function fbCheckObdCan(busy) {
  let status = null;
  for (let i = 0; i < 4 && !status; i++) {
    status = await fbGet("/api/status");
    if (!status) await fbSleep(500);
  }
  const can = status && status.can;
  if (!can || !can.up) {
    fbFound.obdCan.text = "CAN is not up";
  } else if (can.listenOnly) {
    fbFound.obdCan.text = "CAN is listen-only (ignition off)";
  } else {
    try { await api("/api/firstboot", "POST", { obdProbe: true }); } catch (e) { /* older firmware: only the OBD-II profile asks */ }
    const start = Date.now();
    while (Date.now() - start < 8000) {
      await fbSleep(700);
      const now = await fbGet("/api/status");
      if (now && now.can && now.can.obd && now.can.obd.ecuFound) {
        fbFound.obdCan.ok = true;
        fbFound.obdCan.answered = [];
        for (let n = 0; n < 8; n++) {
          if ((now.can.obd.answered || 0) & (1 << n)) fbFound.obdCan.answered.push(0x7E8 + n);
        }
        if (!fbFound.obdCan.answered.length) fbFound.obdCan.answered.push(now.can.obd.ecuId); // older firmware
        fbFound.obdCan.text = fbFound.obdCan.answered.map(fbObdEcuName).join(", ") + " answer";
        if (fbFound.obdCan.answered.length === 1) fbFound.obdCan.text += "s";
        // keep listening a moment longer: other ECUs answer the same request a little later
        await fbSleep(1500);
        const later = await fbGet("/api/status");
        if (later && later.can && later.can.obd) {
          for (let n = 0; n < 8; n++) {
            const id = 0x7E8 + n;
            if ((later.can.obd.answered || 0) & (1 << n) && !fbFound.obdCan.answered.includes(id)) fbFound.obdCan.answered.push(id);
          }
          fbFound.obdCan.text = fbFound.obdCan.answered.map(fbObdEcuName).join(", ") + (fbFound.obdCan.answered.length === 1 ? " answers" : " answer");
        }
        break;
      }
    }
    if (!fbFound.obdCan.ok) fbFound.obdCan.text = "no answer to OBD-II requests";
  }
  await fbReadVinCan(); // 0.35.0: the VIN over the CAN (22 F190 / 09 02 to the engine), once
  busy.obdCan = false;
  fbDrawChecks(busy);
}

// 0.35.0: asks the gauge to read the VIN over its CAN, and waits for it (a few seconds at most)
async function fbReadVinCan() {
  try {
    await api("/api/action", "POST", { action: "vehicleDetect" });
  } catch (e) {
    return; // older firmware
  }
  for (let i = 0; i < 10; i++) {
    await fbSleep(500);
    const vehicle = await fbGet("/api/vehicle");
    if (!vehicle || !vehicle.canReading) {
      return;
    }
  }
}

// 0.35.0: on an ME7 the engine's measuring block 081 is the VIN and the immobiliser ID (a text block): show it for a
// moment on the K-line session First Boot opened (the gauge picks the VIN out of it), then go back to block 1
async function fbReadVinKline() {
  try {
    await api("/api/action", "POST", { action: "klGoto", group: 81 });
  } catch (e) {
    return;
  }
  for (let i = 0; i < 8; i++) {
    await fbSleep(750);
    const vehicle = await fbGet("/api/vehicle");
    if (vehicle && vehicle.detected && vehicle.detected.vin) {
      break;
    }
  }
  try {
    await api("/api/action", "POST", { action: "klGoto", group: 1 });
  } catch (e) { /* the session may have closed */ }
}

// the K-line: every GET of the measuring block holds the line for 3 s and wakes the module (5-baud init)
async function fbCheckKline(busy) {
  const start = Date.now();
  let block = null;
  while (Date.now() - start < FB_KLINE_MS) {
    block = await fbGet("/api/kline/block");
    if (block && block.connected) break;
    if (block && block.state) {
      fbFound.kline.text = block.state;
      fbDrawChecks(busy);
    }
    await fbSleep(1000);
  }
  if (block && block.connected) {
    fbFound.kline.ok = true;
    fbFound.kline.ecuId = block.ecuId || "";
    fbFound.kline.text = "module 0x" + (block.addr || 1).toString(16).toUpperCase().padStart(2, "0") + " answered: " + (block.ecuId || "(no id)");
    if ((block.addr || 1) === 1) {
      await fbReadVinKline(); // 0.35.0: the engine: try its block 081 for the VIN
    }
  } else {
    fbFound.kline.text = "no answer from module 01";
  }
  busy.kline = false;
  fbDrawChecks(busy);
  // 0.38.6: no VW engine at 01: OBD-II on the K-line at 33, any make - the same check Scan My Car makes (one wake-up
  // of 33 and its PID lists). Before, First Boot never asked 33, so an older car with only K-line OBD-II found nothing.
  if (fbFound.kline.ok) {
    fbFound.klineObd.skip = true;
    fbFound.klineObd.text = "not needed: the engine answered at 01";
  } else {
    fbFound.klineObd.text = "asking OBD-II at 33 (a few seconds)...";
    fbDrawChecks(busy);
    carKlineObdAnswer = null; // ask again (Check Again), not the last answer
    const obd = await carKlineObd(function () {});
    if (obd) {
      fbFound.klineObd.ok = true;
      fbFound.klineObd.supported = obd.supported;
      fbFound.klineObd.protocol = obd.protocol;
      let protocolText = "";
      if (obd.protocol) protocolText = " (" + obd.protocol + ")";
      fbFound.klineObd.text = "OBD-II answered at 33" + protocolText + ": " + obd.supported.length + " PIDs";
    } else {
      fbFound.klineObd.text = "no OBD-II answer at 33";
    }
  }
  busy.klineObd = false;
  fbDrawChecks(busy);
}

// Bluetooth OBD: a 5 s scan (works with the adapter switched off), then what the adapter told us if one is connected
async function fbCheckBle(busy) {
  const before = await fbGet("/api/elm");
  if (before && before.connected) { // already paired and talking: no scan (it would drop the link)
    fbFound.ble.ok = true;
    fbFound.ble.connected = true;
    fbFound.ble.text = "connected: " + (before.adapter || before.mac);
    fbTakeVin(before);
  } else {
    // 0.37.7: an adapter that's found is paired straight away, and the check waits until it has found the car, so the
    // car's VIN, make and OBD are known for the rest of the setup (Scan My Car's carAdapterUp, in carscan.js)
    const show = function (text) {
      fbFound.ble.text = text;
      fbDrawChecks(busy);
    };
    const elm = await carAdapterUp(show, function () {});
    const after = await fbGet("/api/elm");
    fbFound.ble.adapters = ((after && after.found) || []).filter(function (d) { return d.likely; });
    if (elm) {
      fbFound.ble.ok = true;
      fbFound.ble.connected = true;
      fbFound.ble.text = "paired and connected: " + (elm.adapter || elm.mac);
      fbTakeVin(elm);
    } else if (after && after.mac) {
      fbFound.ble.ok = true; // the adapter is there; the car didn't answer it
      fbFound.ble.text = "paired " + (after.adapter || after.mac) + " - no answer from the car (ignition on?)";
    } else {
      fbFound.ble.text = "no OBD adapter in range";
    }
  }
  busy.ble = false;
  fbDrawChecks(busy);
}

// the VIN from a connected adapter: make / model / year for the theme and boot logo
function fbTakeVin(elm) {
  if (!elm || !elm.vin) return;
  fbFound.vin = { make: elm.make || "", makeKey: elm.makeKey || "", model: elm.model || "", year: elm.year || "" };
  fbFound.ble.text += " - " + [elm.make, elm.model, elm.year].filter(Boolean).join(" ");
}

// WiFi OBD adapters: the shared WiFi scan (it answers {scanning:true} until done)
async function fbCheckObdWifi(busy) {
  if (fbFound.ble.ok) {
    fbFound.obdWifi.skip = true; // 0.37.7: neither a tick nor a cross
    fbFound.obdWifi.text = "not needed (a Bluetooth adapter is there)";
    busy.obdWifi = false;
    fbDrawChecks(busy);
    return;
  }
  let scan = null;
  for (let i = 0; i < 15; i++) {
    scan = await fbGet("/api/wifi/scan");
    if (scan && scan.networks) break;
    await fbSleep(1000);
  }
  const adapters = (scan && scan.networks || []).filter(function (n) { return FB_OBD_WIFI.test(n.ssid); });
  fbFound.obdWifi.networks = adapters;
  if (adapters.length) {
    fbFound.obdWifi.ok = true;
    fbFound.obdWifi.text = adapters.map(function (n) { return n.ssid + " (" + n.rssi + " dBm)"; }).join(", ");
  } else {
    fbFound.obdWifi.text = "no OBD adapter network in range";
  }
  busy.obdWifi = false;
  fbDrawChecks(busy);
}

// runs every check (the passive ones, the K-line and the Bluetooth scan side by side, then the WiFi scan)
async function fbRunChecks() {
  if (fbRunning) return;
  fbRunning = true;
  fbScreen("checking what's connected", 10);
  // 0.38.0: the screen's ring creeps from 10 % to 38 % through the checks, instead of sitting at 10 % and jumping to 40 %
  const checksStartMs = Date.now();
  const creep = setInterval(function () {
    const share = Math.min(1, (Date.now() - checksStartMs) / FB_CHECKS_MS);
    fbScreen("checking what's connected", 10 + 28 * share);
  }, 1000);
  $("fbCheckAgain").disabled = true;
  $("fbStep1").classList.add("on");
  fbFound = fbEmptyResults();
  const busy = {};
  FB_ROWS.forEach(function (row) { busy[row[0]] = true; });
  fbDrawChecks(busy);
  await Promise.all([fbWatch(busy), fbCheckObdCan(busy), fbCheckKline(busy), fbCheckBle(busy)]); // side by side, wait for all four
  await fbCheckObdWifi(busy); // last: the WiFi scan can pause the gauge's own WiFi for a moment
  clearInterval(creep);
  fbRunning = false;
  fbScreen("choose the setup on your phone", 40);
  $("fbCheckAgain").disabled = false;
  fbDrawSetup();
  // 0.37.14: the gauge list waits for the car scan (it runs by itself below), so it's drawn once, with the car's values
  if (fbWillScan()) {
    $("fbPages").innerHTML = '<div class="help">Scanning the car for its values - the gauges appear here when it\'s done.</div>';
  } else {
    fbMakeCandidates();
    fbDrawPages();
  }
  $("fbSetupCard").style.display = "";
  $("fbPagesCard").style.display = "";
  $("fbApplyCard").style.display = "";
  $("fbStep2").classList.add("on");
  $("fbStep3").classList.add("on");
  // 0.35.0: step 4, the car (car.js), from whatever VIN the checks read
  if (typeof carMountEditor === "function") {
    $("fbCarCard").style.display = "";
    $("fbStep4").classList.add("on");
    carMountEditor($("fbCarBox"), "fbCar");
  }
  if (fbWillScan()) {
    await fbAutoScan(); // 0.37.14: the car's own values into the starter set, then the list (fbScanDone)
    if (!fbCandidates.length || !$("fbPages").querySelector(".fb-page")) {
      fbMakeCandidates(); // the scan didn't run after all (or stopped): the list from what the checks found
      fbDrawPages();
    }
  }
}

// ---------------------------------------------------------------------------
// step 2: the suggested settings table
// ---------------------------------------------------------------------------
// one row: a setting, a dropdown (options [value, text]) with the suggestion picked, and why
function fbRow(id, title, options, picked, why) {
  const opts = options.map(function (o) {
    return `<option value="${esc(o[0])}" ${String(o[0]) === String(picked) ? "selected" : ""}>${esc(o[1])}</option>`;
  }).join("");
  return `<tr><td>${esc(title)}</td><td><select id="${id}">${opts}</select><div class="fb-why">${esc(why)}</div></td></tr>`;
}

// 0.38.7: other gauges heard on the chain: the choice to give them this setup too (ticked)
function fbShowPushChoice() {
  const box = $("fbPushBox");
  if (!box) return;
  if (!fbFound.chain.ok) {
    box.style.display = "none";
    return;
  }
  box.style.display = "";
  $("fbPushWhy").textContent = "The " + fbFound.chain.nodes + " other gauge(s) heard (" + fbFound.chain.text + ") get the " +
    "same gauges, triggers and car settings, so it doesn't matter which one your phone is on. Each starts on its own " +
    "page by its place in the row, and keeps its own WiFi, row number, name, CAN termination, OBD dongle, K-line and " +
    "dash-light wire. They need the same firmware as this one (Daisy Chain > Update All Gauges).";
}

function fbDrawSetup() {
  fbShowPushChoice();
  const f = fbFound;
  const s = settingsCache;
  let html = "<tr><th>Setting</th><th>Suggested</th></tr>";

  // CAN profile
  let profile = s.can.profile;
  let profileWhy = "kept: nothing on the CAN wires said otherwise";
  if (f.can.profile >= 0) { profile = f.can.profile; profileWhy = f.can.profileWhy; }
  else if (f.haldex.ok && !f.can.ok) { profile = 2; profileWhy = "OpenHaldex only: no car frames heard"; }
  if (f.obdCan.ok && (!f.can.ok || f.can.profile === 2)) { profile = 1; profileWhy = "no car frames the gauge decodes, but the engine answers OBD-II"; }
  let obdCanPick = s.obd.overCan ? 1 : 0;
  let obdCanWhy = "the engine didn't answer OBD-II requests on this CAN";
  if (f.obdCan.ok) { obdCanPick = 1; obdCanWhy = "the engine answers: its OBD-II values (intake, timing, MAF ...) as well as the car's frames"; }
  let obdRow = fbRow("fb.obdCan", "OBD-II over CAN", [[0, "Off"], [1, "On"]], obdCanPick, obdCanWhy);
  // which ECU it reads (0 = the engine, 1..8 = answer id 0x7E8..0x7EF)
  const ecuOptions = [[0, "Engine"]];
  f.obdCan.answered.forEach(function (id) { ecuOptions.push([id - 0x7E8 + 1, fbObdEcuName(id).replace(/^./, function (c) { return c.toUpperCase(); })]); });
  let ecuWhy = "the engine unless you pick another";
  if (f.obdCan.answered.length > 1) ecuWhy = f.obdCan.answered.length + " ECUs answered: pick the one to read";
  obdRow += fbRow("fb.obdEcu", "OBD-II ECU", ecuOptions, s.obd.ecu || 0, ecuWhy);
  // the car's DBC, so its every signal can go on a gauge (Gauges > Source CAN)
  const dbcOptions = [["", "None"]];
  f.can.dbcs.forEach(function (d) { dbcOptions.push([d.name, d.name + " (" + Math.round(d.score * 100) + " %)"]); });
  let dbcPick = "";
  let dbcWhy = "no DBC matches what is on the bus";
  if (f.can.dbcs.length) { dbcPick = f.can.dbcs[0].name; dbcWhy = "the closest match to the IDs on the bus: any of its signals can go on a gauge"; }
  obdRow += fbRow("fb.dbc", "Car DBC", dbcOptions, dbcPick, dbcWhy);
  html += fbRow("fb.canProfile", "CAN profile", [[0, "VAG PQ"], [3, "VAG MQB"], [5, "VAG MLB"], [1, "OBD-II"], [2, "OpenHaldex only"], [4, "MaxxECU"]], profile, profileWhy);

  html += obdRow; // (the OBD-II profile asks anyway)

  // CAN termination: can't be seen from here
  html += fbRow("fb.canTerm", "CAN termination", [[0, "Off"], [1, "On"]], s.can.termination,
    "can't be checked: on only when this gauge is the last device at the end of the CAN wires (OpenHaldex harness: usually off)");

  // OBD adapter
  const bleOptions = [["off", "Not used"]];
  let blePick = "off";
  let bleWhy = "no adapter found";
  if (f.ble.connected) { bleOptions.push(["keep", "Keep paired"]); blePick = "keep"; bleWhy = "already connected"; }
  f.ble.adapters.forEach(function (d) { bleOptions.push(["ble:" + d.mac + ":" + d.type, d.name]); });
  f.obdWifi.networks.forEach(function (n) { bleOptions.push(["wifi:" + n.ssid, n.ssid + " (WiFi)"]); });
  if (blePick === "off" && f.ble.adapters.length) { blePick = bleOptions[1][0]; bleWhy = "the strongest Bluetooth OBD adapter found"; }
  else if (blePick === "off" && f.obdWifi.networks.length) { blePick = bleOptions[1][0]; bleWhy = "an OBD adapter's WiFi was found"; }
  if (f.can.ok && f.can.profile !== 1 && blePick !== "keep" && blePick !== "off") bleWhy += " (CAN already gives the engine values, so it is optional)";
  html += fbRow("fb.obd", "OBD adapter", bleOptions, blePick, bleWhy);

  // K-line: whatever answered gets its protocol (the K-line mode and the K-line page sources use KWP1281 too)
  let kline = s.kline.protocol;
  let klineWhy = "kept";
  if (f.kline.ok) {
    if (kline === 0) kline = 1;
    klineWhy = "the engine answered (" + (f.kline.ecuId || "VAG module 01") + "): KWP1281 measuring blocks";
  } else if (f.klineObd.ok) {
    kline = 2;
    klineWhy = "OBD-II answered on the K-line at 33: its PIDs (KWP2000 / ISO 9141-2)";
  } else {
    kline = 0;
    klineWhy = "nothing answered on the K-line";
  }
  html += fbRow("fb.kline", "K-line logging", [[0, "Off"], [1, "KWP1281"], [2, "KWP2000 OBD-II"], [3, "ME7 logger"], [4, "KWP2000 VAG"]], kline, klineWhy);

  // the cluster's oil temperature: the PQ Kombi_2 frame on CAN, else the cluster over the K-line
  let oil = s.oil.source;
  let oilWhy = "kept";
  if (f.can.kombi2) { oil = 0; oilWhy = "the cluster's Kombi_2 (0x420) is on the bus"; }
  else if (f.kline.ok) { oil = 1; oilWhy = "no Kombi_2 on CAN: the cluster (17) over the K-line, while the oil page shows"; }
  html += fbRow("fb.oil", "Oil temp from", [[0, "CAN"], [1, "K-line"]], oil, oilWhy);

  // the daisy chain: on when other gauges were heard
  let chain = s.chain.enabled ? 1 : 0;
  let chainWhy = "no other gauges heard";
  if (chain) chainWhy = "none heard yet - leaving it on finds gauges added later";
  if (f.chain.ok) { chain = 1; chainWhy = f.chain.nodes + " other gauge(s) heard: " + f.chain.text; }
  html += fbRow("fb.chain", "Daisy chain", [[0, "Off"], [1, "On"]], chain, chainWhy);

  // OpenHaldex / usb2ip listening
  html += fbRow("fb.haldex", "OpenHaldex", [[1, "Listen"], [0, "Off"]], s.haldex.wireless,
    f.haldex.ok ? f.haldex.text : "none heard - leaving it on costs nothing");
  if (s.ecu) {
    html += fbRow("fb.ecu", "usb2ip", [[1, "On"], [2, "Gauges only"], [0, "Off"]],
      s.ecu.wireless ? (s.ecu.carSignals ? 1 : 2) : 0, f.ecu.ok ? f.ecu.text : "none heard - leaving it on costs nothing");
  }

  // power + illumination input (the same wire can be either)
  const lightsBoth = f.lightsSeen.length > 1;
  let power = "switched";
  if (s.power.ignSource === 1) power = "can";
  if (s.power.ignSource === 2) power = "wire";
  html += fbRow("fb.power", "Power", [["switched", "Switched 12 V"], ["can", "Permanent + CAN"], ["wire", "Permanent + pin 3"]], power,
    f.can.ok ? "CAN can tell the gauge when the ignition is on, if it is on permanent power" : "can't be checked: pick how it is wired");
  let night = s.io.nightSource ? 1 : 0;
  let nightWhy = "the illumination input (J_OUT pin 3) dims the glass when the dash lights are on";
  if (lightsBoth) nightWhy = "the input changed during the check: it is wired to something";
  html += fbRow("fb.night", "Dimmed by", [[1, "Dash lights"], [0, "Nothing"]], night, nightWhy);
  html += fbRow("fb.dimInvert", "Lights wire", [[0, "12 V = on"], [1, "0 V = on"]], s.io.dimInvert,
    "right now the input reads " + (statusCache && statusCache.io && statusCache.io.dashLights ? "ON" : "OFF") + " - switch the headlights to check");

  // look
  html += fbRow("fb.units", "Units", [[0, "Metric"], [1, "Imperial"]], s.units.quick, "kept");
  // colours: keep the theme set now, or Match car (the make's colours, picked from the VIN)
  let theme = s.display.theme;
  let themeWhy = "kept";
  const themeOptions = [];
  if (s.display.theme !== 5) {
    themeOptions.push([s.display.theme, "Keep current"]);
  }
  themeOptions.push([5, "Match car"]);
  if (f.vin.makeKey) { theme = 5; themeWhy = "the VIN says " + f.vin.make; }
  html += fbRow("fb.theme", "Colours", themeOptions, theme, themeWhy);
  let logo = s.boot.logo ? "on" : "off";
  let logoWhy = "the logo shown while the gauge starts";
  const logoOptions = [["on", "On"], ["off", "Off"]];
  if (f.vin.makeKey) { logoOptions.push(["make", f.vin.make + " logo"]); logo = "make"; logoWhy = "the VIN says " + f.vin.make; }
  html += fbRow("fb.bootLogo", "Boot logo", logoOptions, logo, logoWhy);
  const anySprite = (s.pages || []).some(function (p) { return p.enabled && p.spriteOn; });
  html += fbRow("fb.sprites", "Sprites", [[0, "Off"], [1, "On"]], anySprite ? 1 : 0, "each gauge shows its sprite, reacting to the reading");
  html += fbRow("fb.saver", "Screensaver", [[0, "Off"], [1, "On"]], s.saver.enabled, "the sprite screensaver after the car has been still " + s.saver.delayS + " s");

  // menu: the long-press items that have something behind them
  html += `<tr><td colspan="2"><div class="fb-menu-title">Menu and Tabs</div><div class="menu-toggles" id="fbMenu" style="margin-bottom:0"></div><div class="fb-why">the ones with nothing found are suggested off (Settings &gt; Device &gt; Menu and Tabs brings them back)</div></td></tr>`;
  $("fbSetupTable").innerHTML = html;

  const canFound = f.can.ok || f.relay.ok || f.obdCan.ok; // anything at all from a CAN bus
  const menuWanted = {
    gauges: true,
    can: canFound,
    haldex: f.haldex.ok,
    adc: f.adc.ok,
    ecu: f.ecu.ok,
    kline: f.kline.ok || f.klineObd.ok,
    oil: f.kline.ok || canFound,     // the cluster's oil sender comes over the K-line or CAN
    dsg: canFound,                   // the gearbox is read over CAN
    ble: f.ble.ok || f.obdWifi.ok,
    triggers: true,
    chain: f.chain.ok,
    output: menuOn(s.menu, "output"), // the gauge's own output: kept as it is
    nav: menuOn(s.menu, "nav"),       // the phone's turn-by-turn: nothing on the car to find it by, so kept as it is
    drag: true,                      // 0.38.0: the drag timer, on for everyone (it was left out: Apply hid it)
  };
  $("fbMenu").innerHTML = menuItems.map(function (item) {
    return `<label class="toggle"><input type="checkbox" id="fbMenu_${item[0]}" ${menuWanted[item[0]] ? "checked" : ""}><span class="toggle-slider"></span><span class="toggle-label">${esc(item[1])}</span></label>`;
  }).join("");
}

// ---------------------------------------------------------------------------
// step 3: the gauges
// ---------------------------------------------------------------------------
// the catalogue entry of a signal key
function fbSignal(key) {
  return catalogue.signals.find(function (s) { return s.key === key; });
}

// one page as /api/settings takes it
function fbPage(style, source, signal, label, slots) {
  const info = fbSignal(signal) || { min: 0, max: 100, unit: "" };
  // 0.27.0: a page's Min / Max / Redline are in the shown unit (app.js unitShowRange); the catalogue's are stored units
  const span = info.max - info.min;
  let redline = 0;
  if (FB_REDLINE[signal]) {
    redline = unitShowRange(signal, FB_REDLINE[signal], info.unit, span);
  }
  return {
    enabled: 1, style: style, source: source, signal: signal, signal2: "none", label: label,
    min: unitShowRange(signal, info.min, info.unit, span), max: unitShowRange(signal, info.max, info.unit, span),
    redline: redline, decimals: FB_DECIMALS[signal] || 0,
    spriteOn: 0, slots: slots || ["none", "none", "none"],
  };
}

// 0.37.0: a page showing a value Scan My Car found (Source = Car); the range from the gauge's own signal of the same
// kind when it has one
function fbCarPage(pageValue, label, key, value) {
  const page = fbPage(FB_STYLE.arc, "car", key, label);
  page.signal = pageValue; // "cv:<n>" (a diagnostics slot, given at Apply), "pid:<n>" or "kl:<a>:<b>:<f>"
  const info = fbSignal(key);
  const valueUnit = String((value && value.unit) || "").toLowerCase();
  const sameUnit = info && String(info.unit || "").toLowerCase() === valueUnit;
  if (!info || (valueUnit && !sameUnit)) {
    // 0.38.4: no gauge value of this kind, or one in another unit (a MAP in kPa, the catalogue's boost in bar): the
    // range from the value's own unit
    const range = carRangeFor(value || {});
    page.min = range.min;
    page.max = range.max;
    page.redline = 0;
    page.decimals = range.decimals;
  }
  return page;
}

// 0.37.13: the values a found route always carries, so their gauges are offered before anything is seen live.
// OBD-II: the PIDs nearly every engine has; the K-line engine: its measuring blocks 1, 3 and 4 (KLine.cpp groupMap);
// usb2ip: its everyday values (app.js ECU_KEYS)
const FB_OBD_KEYS = ["rpm", "speed", "coolant", "iat", "load", "throttle"];
const FB_KLINE_KEYS = ["rpm", "coolant", "throttle", "timing", "batt", "iat"];

// 0.37.13: every value key this car can give, and how: "live" (seen during the checks) or "route" (a found way in
// always carries it: OBD-II, the K-line engine, usb2ip, or a PID Scan My Car found)
function fbAvailableKeys(found) {
  const f = found;
  const keys = {};
  const addKeys = function (list, how) {
    list.forEach(function (key) { if (!keys[key]) keys[key] = how; });
  };
  addKeys(Object.keys(f.live), "live");
  if (f.obdCan.ok || f.ble.connected || f.klineObd.ok) addKeys(FB_OBD_KEYS, "route"); // 0.38.6: + OBD-II on the K-line
  if (f.kline.ok) addKeys(FB_KLINE_KEYS, "route");
  if (f.ecu.ok && typeof ECU_KEYS !== "undefined") addKeys(ECU_KEYS, "route");
  // the PIDs the scan found that feed one of the gauge's own values (oil temperature, fuel level ...)
  if (carCatalog) {
    carCatalog.values.forEach(function (value) {
      if (value.k !== "obd") return;
      const row = OBD_PIDS.find(function (p) { return p[0] === value.id; });
      if (row && row[2]) addKeys([row[2]], "route");
    });
  }
  return keys;
}

// First Boot's starter pack, from what its checks found
function fbMakeCandidates() {
  fbCandidates = fbBuildCandidates(fbFound);
  fbTriggerCandidates = fbBuildTriggers(fbFound); // 0.38.4: and the triggers to go with them
}

// 0.37.13: the starter pack - only gauges this car can actually show, the likely favourites first. OpenHaldex only
// when one was heard; a value with no way in isn't offered at all. Scan My Car's values fill in what the routes don't
// carry. 0.38.4: from any found results (First Boot's checks, or fbFoundNow for Scan My Car's Suggested Gauges), every
// source the gauge found offered - the engine, the gearbox (DSG / ZF), adc2can, usb2ip, OpenHaldex and each other
// module the scan found - and the 8 ticks spread over them (fbLimitList)
function fbBuildCandidates(found) {
  const f = found;
  const has = fbAvailableKeys(found);
  let source = "can"; // the gauge's own values, from whichever route feeds them
  if (!f.can.ok && !f.relay.ok && f.ecu.ok) source = "ecu";
  const scanned = {}; // key -> [key, page value, label, scan value] from Scan My Car
  carSuggestions().forEach(function (row) { scanned[row[0]] = row; });
  if (!has.boost && has.map) has.boost = has.map; // the MAP sensor gives boost too
  const list = [];
  // group: where it comes from ("engine", "gearbox", "adc", "ecu", "haldex", "mod<n>"), for spreading the ticks
  const add = function (key, page, why, seenLive, group) {
    list.push({ key: key, page: page, why: why, live: seenLive, picked: true, group: group });
  };
  // one value's gauge: from the gauge's own value when a route carries it, else from Scan My Car, else not offered
  const offer = function (key, style, label, why, group, pageSource) {
    if (list.some(function (c) { return c.key === key; })) return; // offered already
    if (has[key]) {
      add(key, fbPage(style, pageSource || source, key, label), why, has[key] === "live", group);
      return;
    }
    if (scanned[key]) {
      const row = scanned[key];
      const page = fbCarPage(row[1], row[2], key, row[3]);
      page.style = style;
      const module = (carCatalog && carCatalog.modules[row[3].m]) || {};
      add(key, page, row[3].name + " (" + (module.name || "Scan My Car") + ")", false, group);
    }
  };

  // the engine's everyday gauges
  offer("rpm", FB_STYLE.needle, "RPM", "engine speed on a needle", "engine");
  offer("speed", FB_STYLE.bar, "SPEED", "road speed", "engine");
  offer("oiltemp", FB_STYLE.arc, "OIL TEMP", "oil temperature", "engine");
  offer("oilPressure", FB_STYLE.arc, "OIL PRESS", "oil pressure", "engine");
  offer("coolant", FB_STYLE.arc, "COOLANT", "coolant temperature", "engine");
  offer("boost", FB_STYLE.arc, "BOOST", "boost pressure", "engine");
  offer("batt", FB_STYLE.numeric, "BATTERY", "battery voltage", "engine");
  // OpenHaldex
  if (f.haldex.ok) {
    add("haldex", fbPage(FB_STYLE.haldex, "can", "hdxActual", "HALDEX"), "OpenHaldex lock, target and mode", true, "haldex");
  }
  // the gearbox (DSG / ZF): the gear page when the gauge has the selector or gear itself, else the gear the scan found;
  // its temperature and the clutch's
  if (has.gear || has.selector) {
    add("gear", fbPage(FB_STYLE.gear, "can", "gear", "GEAR"), "selector and gear (DSG / auto)", has.gear === "live" || has.selector === "live", "gearbox");
  } else {
    offer("gear", FB_STYLE.numeric, "GEAR", "current gear", "gearbox");
  }
  offer("gboxTemp", FB_STYLE.arc, "GEARBOX", "gearbox temperature", "gearbox");
  offer("clutchTemp", FB_STYLE.arc, "CLUTCH", "clutch temperature", "gearbox");
  // usb2ip: what an aftermarket ECU has that a car's own doesn't
  if (f.ecu.ok) {
    offer("knockRetard", FB_STYLE.bar, "KNOCK", "knock retard (usb2ip)", "ecu", "ecu");
    offer("injDuty", FB_STYLE.bar, "INJ DUTY", "injector duty (usb2ip)", "ecu", "ecu");
    offer("ethanol", FB_STYLE.numeric, "ETHANOL", "ethanol content (usb2ip)", "ecu", "ecu");
    offer("lambdaTarget", FB_STYLE.arc, "LAMBDA TGT", "lambda target (usb2ip)", "ecu", "ecu");
  }
  // the adc2can sensors that sent a value, by their names, with the dial range and warning the adc2can has for them
  if (f.adc.ok && f.adc.ch) {
    f.adc.ch.forEach(function (ch, i) {
      if (ch.v === undefined && ch.value === undefined) return; // nothing wired to it
      const key = "adc" + (i + 1);
      const name = (ch.name || ("ADC " + (i + 1))).toUpperCase().slice(0, 15);
      const page = fbPage(FB_STYLE.arc, "adc", key, name);
      const cfg = ch.cfg || {};
      if (cfg.dialLo !== undefined && cfg.dialHi !== undefined && cfg.dialHi > cfg.dialLo) {
        page.min = cfg.dialLo;
        page.max = cfg.dialHi;
      }
      if (cfg.highWarn !== undefined) page.redline = cfg.highWarn;
      add(key, page, "adc2can " + (i < 8 ? "board 1" : "board 2") + " sensor " + (i % 8 + 1) + (ch.unit ? " (" + uShown(ch.unit) + ")" : ""), true, "adc");
    });
  }
  // more from the engine
  offer("egt", FB_STYLE.arc, "EGT", "exhaust gas temperature", "engine");
  offer("iat", FB_STYLE.arc, "INTAKE", "intake air temperature", "engine");
  offer("lambda", FB_STYLE.arc, "LAMBDA", "air / fuel ratio", "engine");
  offer("throttle", FB_STYLE.bar, "THROTTLE", "throttle (the pedal where the car reports it)", "engine");
  offer("fuelPressure", FB_STYLE.arc, "FUEL PRESS", "fuel pressure", "engine");
  offer("turbo", FB_STYLE.arc, "TURBO", "turbo speed", "engine");
  offer("knock", FB_STYLE.bar, "KNOCK", "knock", "engine");
  offer("fuelLitres", FB_STYLE.bar, "FUEL", "fuel in the tank (litres)", "engine");
  offer("ambient", FB_STYLE.numeric, "OUTSIDE", "outside temperature", "engine");
  if (f.can.kombi2 || f.kline.ok) {
    add("oil", fbPage(FB_STYLE.oil, "can", "oiltemp", "OIL"), "the cluster's oil temp (and level on the K-line)", false, "engine");
  }
  // four values on one page, from the ones this car has
  const quadKeys = ["coolant", "rpm", "boost", "iat", "oiltemp", "throttle", "speed", "batt"].filter(function (k) { return has[k]; });
  if (quadKeys.length >= 4) {
    const quadNames = quadKeys.slice(0, 4).join(", ");
    add("engine", fbPage(FB_STYLE.quad, source, quadKeys[0], "ENGINE", quadKeys.slice(1, 4)), "four values: " + quadNames, has[quadKeys[0]] === "live", "engine");
  }
  // the rest of the scan's picks: one from each other module it found (the AWD, the brakes ...)
  Object.keys(scanned).forEach(function (key) {
    if (list.some(function (c) { return c.key === key; })) return; // offered above
    const row = scanned[key];
    const page = fbCarPage(row[1], row[2], key, row[3]);
    if (/\bgear\b/i.test(row[3].name) && !row[3].unit) page.style = FB_STYLE.numeric;
    const module = (carCatalog && carCatalog.modules[row[3].m]) || {};
    add(key, page, row[3].name + " (" + (module.name || "Scan My Car") + ")", false, "mod" + row[3].m);
  });
  // nothing found at all (on the bench, ignition off): the everyday gauges, unticked, so Apply still has something
  if (!list.length) {
    add("rpm", fbPage(FB_STYLE.needle, source, "rpm", "RPM"), "nothing found yet - check again with the ignition on", false, "engine");
    add("coolant", fbPage(FB_STYLE.arc, source, "coolant", "COOLANT"), "nothing found yet", false, "engine");
    add("boost", fbPage(FB_STYLE.arc, source, "boost", "BOOST"), "nothing found yet", false, "engine");
    list.forEach(function (c) { c.picked = false; });
  }
  fbLimitList(list);
  return list;
}

// no more than 8 ticked (the gauge has 8 page slots). 0.38.4: spread over the sources - first the first gauge of each
// one found (the engine, the gearbox, adc2can, usb2ip, OpenHaldex), then the rest in the list's order
function fbLimitList(list) {
  const wanted = list.filter(function (c) { return c.picked; });
  list.forEach(function (c) { c.picked = false; });
  let count = 0;
  const groupsTicked = {};
  wanted.forEach(function (c) {
    if (count >= 8 || groupsTicked[c.group] || /^mod/.test(c.group || "")) return;
    groupsTicked[c.group] = true;
    c.picked = true;
    count++;
  });
  wanted.forEach(function (c) {
    if (count >= 8 || c.picked) return;
    c.picked = true;
    count++;
  });
}

// 0.38.4: the triggers a starter set suggests, for the values this car has: [value key, label, 1 = above / 0 = below,
// threshold, hysteresis (both in the value's own unit: C, rpm, bar, V), hold ms]. Each an alert on the screen.
const FB_TRIGGERS = [
  ["coolant", "COOLANT HOT", 1, 110, 5, 2000],
  ["rpm", "RPM HIGH", 1, 6800, 300, 0],
  ["boost", "BOOST HIGH", 1, 1.6, 0.1, 500],
  ["batt", "BATTERY LOW", 0, 11.8, 0.4, 10000], // 10 s: starting the engine dips it for a moment
  ["oiltemp", "OIL HOT", 1, 130, 5, 2000],
  ["gboxTemp", "GEARBOX HOT", 1, 120, 5, 2000],
  ["egt", "EGT HIGH", 1, 950, 25, 1000],
  ["batt", "BATTERY HIGH", 1, 15.2, 0.3, 5000],  // a charging fault
];

// one trigger as /api/settings takes it
function fbTrigger(key, label, above, threshold, hysteresis, holdMs) {
  return { enabled: 1, signal: key, above: above, threshold: threshold, hysteresis: hysteresis, action: 0, output: 0,
    latch: 0, noReading: 0, holdMs: holdMs, label: label.substring(0, 15) };
}

// 0.38.4: the suggested triggers: {trigger, why, picked}, for the values found (high revs, boost, coolant, battery ...)
// and the adc2can sensors' own warning limits; all ticked (8 at most: the gauge has 8)
function fbBuildTriggers(found) {
  const has = fbAvailableKeys(found);
  if (!has.boost && has.map) has.boost = has.map;
  const list = [];
  const describe = function (key, above, threshold, unit) {
    const shown = unitShow(key, threshold, unit);
    let word = "below";
    if (above) word = "above";
    return word + " " + Math.round(shown.v * 100) / 100 + " " + uShown(shown.u);
  };
  FB_TRIGGERS.forEach(function (row) {
    if (!has[row[0]]) return; // this car hasn't got the value
    const info = fbSignal(row[0]) || { unit: "" };
    list.push({ trigger: fbTrigger(row[0], row[1], row[2], row[3], row[4], row[5]), why: describe(row[0], row[2], row[3], info.unit), picked: true });
  });
  if (found.adc.ok && found.adc.ch) {
    found.adc.ch.forEach(function (ch, i) {
      const cfg = ch.cfg || {};
      if ((ch.v === undefined && ch.value === undefined) || (cfg.highWarn === undefined && cfg.lowWarn === undefined)) return;
      const key = "adc" + (i + 1);
      const name = (ch.name || ("ADC " + (i + 1))).toUpperCase();
      if (cfg.highWarn !== undefined) {
        const band = Math.abs(cfg.highWarn) * 0.03; // 3 %: re-arms once a little back under
        list.push({ trigger: fbTrigger(key, name.substring(0, 10) + " HIGH", 1, cfg.highWarn, band, 1000),
          why: "adc2can's own warning: " + describe(key, 1, cfg.highWarn, ch.unit || ""), picked: true });
      }
      if (cfg.lowWarn !== undefined) {
        const band = Math.abs(cfg.lowWarn) * 0.03;
        list.push({ trigger: fbTrigger(key, name.substring(0, 11) + " LOW", 0, cfg.lowWarn, band, 1000),
          why: "adc2can's own warning: " + describe(key, 0, cfg.lowWarn, ch.unit || ""), picked: true });
      }
    });
  }
  list.forEach(function (c, i) { if (i >= 8) c.picked = false; });
  return list;
}

// the trigger list, ticks only, into box
function fbDrawTriggerList(box, candidates) {
  if (!box) return;
  if (!candidates.length) {
    box.innerHTML = '<div class="help">None to suggest yet: they come with the values found (coolant, rpm, boost, battery, adc2can sensors).</div>';
    return;
  }
  box.innerHTML = candidates.map(function (c, i) {
    return `<div class="fb-page ${c.picked ? "" : "off"}">
      <input type="checkbox" data-fbtrig="${i}" ${c.picked ? "checked" : ""}>
      <div class="fb-name"><b>${esc(c.trigger.label)}</b><span class="fb-why">alert - ${esc(c.why)}</span></div>
    </div>`;
  }).join("");
}

// a tick on a trigger list (8 at most)
function fbTriggerListClick(event, candidates, redraw) {
  const pick = event.target.closest("[data-fbtrig]");
  if (!pick) return;
  const picked = candidates.filter(function (c) { return c.picked; }).length;
  if (pick.checked && picked >= 8) {
    pick.checked = false;
    toast("8 triggers at most - untick one first");
    return;
  }
  candidates[Number(pick.dataset.fbtrig)].picked = pick.checked;
  redraw();
}

// the 8 trigger slots from a list on offer: the ticked ones, then the unticked switched off, then empty ones
function fbTriggersFromList(candidates) {
  const triggers = [];
  candidates.filter(function (c) { return c.picked; }).concat(candidates.filter(function (c) { return !c.picked; })).slice(0, 8).forEach(function (c) {
    const trigger = Object.assign({}, c.trigger);
    trigger.enabled = c.picked ? 1 : 0;
    triggers.push(trigger);
  });
  while (triggers.length < 8) {
    triggers.push({ enabled: 0, signal: "none", above: 1, threshold: 0, hysteresis: 0, action: 0, output: 0, latch: 0, noReading: 0, holdMs: 0, label: "" });
  }
  return triggers;
}

// a list of gauges on offer, ticks and arrows, into box (First Boot's step 3, Scan My Car's Suggested Gauges)
function fbDrawPageList(box, candidates) {
  if (!box) return;
  const styles = catalogue.styles || [];
  box.innerHTML = candidates.map(function (c, i) {
    const style = styles[c.page.style] || "";
    return `<div class="fb-page ${c.picked ? "" : "off"}">
      <input type="checkbox" data-fbpick="${i}" ${c.picked ? "checked" : ""}>
      <div class="fb-name"><b>${esc(c.page.label)}${c.live ? '<span class="fb-live">live</span>' : ""}</b><span class="fb-why">${esc(style)} - ${esc(c.why)}</span></div>
      <button class="btn-secondary" data-fbmove="${i}" data-dir="-1" ${i === 0 ? "disabled" : ""}>&#9650;</button>
      <button class="btn-secondary" data-fbmove="${i}" data-dir="1" ${i === candidates.length - 1 ? "disabled" : ""}>&#9660;</button>
    </div>`;
  }).join("");
}

function fbDrawPages() {
  fbDrawPageList($("fbPages"), fbCandidates);
  fbDrawTriggerList($("fbTriggers"), fbTriggerCandidates); // 0.38.4
}

// ticks and arrows on a page list (one listener for the whole list); redraw draws the list again
function fbPageListClick(event, candidates, redraw) {
  const pick = event.target.closest("[data-fbpick]");
  if (pick) {
    const candidate = candidates[Number(pick.dataset.fbpick)];
    const picked = candidates.filter(function (c) { return c.picked; }).length;
    if (pick.checked && picked >= 8) {
      pick.checked = false;
      toast("8 gauges at most - untick one first");
      return;
    }
    candidate.picked = pick.checked;
    redraw();
    return;
  }
  const move = event.target.closest("[data-fbmove]");
  if (move) {
    const from = Number(move.dataset.fbmove);
    const to = from + Number(move.dataset.dir);
    if (to < 0 || to >= candidates.length) return;
    const moving = candidates[from];
    candidates[from] = candidates[to];
    candidates[to] = moving;
    redraw();
  }
}

function fbPagesClick(event) {
  fbPageListClick(event, fbCandidates, fbDrawPages);
}

// the 8 page slots from a list on offer: the ticked pages in order, then the unticked ones switched off (one tick away
// on the Gauges tab). A Car value from the scan takes its diagnostics slot here (carscan.js carResolve).
function fbPagesFromCandidates(candidates, spritesOn) {
  const picked = candidates.filter(function (c) { return c.picked; });
  const pages = [];
  picked.concat(candidates.filter(function (c) { return !c.picked; })).slice(0, 8).forEach(function (c) {
    const page = Object.assign({}, c.page);
    page.enabled = c.picked ? 1 : 0;
    page.spriteOn = spritesOn && c.picked ? 1 : 0;
    pages.push(page);
  });
  while (pages.length < 8) pages.push({ enabled: 0 });
  pages.forEach(function (page) {
    if (page.signal) page.signal = carResolve(page.signal);
    if (page.slots) page.slots = page.slots.map(carResolve);
  });
  return pages;
}

// 0.38.4: what the gauge has right now, for Scan My Car's Suggested Gauges (the same list First Boot makes): one look
// at the live values, OpenHaldex, the adc2can boards and usb2ip, plus the way in the last scan found
async function fbFoundNow() {
  const found = fbEmptyResults();
  const status = await fbGet("/api/status");
  if (status) {
    statusCache = status;
    Object.keys(status.signals || {}).forEach(function (k) {
      if (!FB_ALWAYS_THERE.includes(k)) found.live[k] = status.signals[k];
    });
    found.can.ok = !!(status.can && status.can.up && status.can.rxPerSec > 0);
  }
  const haldex = await fbGet("/api/haldex");
  if (haldex && haldex.via) found.haldex.ok = true;
  const adc = await fbGet("/api/adc");
  if (adc && adc.boards && adc.boards.some(function (b) { return b.seen; })) {
    found.adc.ok = true;
    found.adc.boards = adc.boards;
    found.adc.ch = adc.ch;
  }
  const ecu = await fbGet("/api/ecu");
  if (ecu && ecu.heard && ecu.usb2ip) found.ecu.ok = true;
  if (carCatalog) {
    found.obdCan.ok = carCatalog.link === "can" && carCatalog.values.some(function (v) { return v.k === "obd"; });
    found.ble.connected = carCatalog.link === "adapter";
    found.kline.ok = carCatalog.modules.some(function (m) { return m.proto === "kline" && m.addr === 1; });
    found.klineObd.ok = !!carCatalog.klineObd; // the scan found OBD-II on the K-line
  }
  return found;
}

// ---------------------------------------------------------------------------
// apply / skip
// ---------------------------------------------------------------------------
function fbValue(id) {
  const element = $(id);
  if (!element) return null;
  return element.value;
}

async function fbApply() {
  const picked = fbCandidates.filter(function (c) { return c.picked; });
  if (!picked.length) return toast("tick at least one gauge");
  const spritesOn = Number(fbValue("fb.sprites")) === 1;
  const pages = fbPagesFromCandidates(fbCandidates, spritesOn); // the 8 page slots, Car values in their slots
  const triggers = fbTriggersFromList(fbTriggerCandidates);       // 0.38.4: the 8 trigger slots, the ticked ones on

  const body = {
    pages: pages,
    triggers: triggers,
    can: { profile: Number(fbValue("fb.canProfile")), termination: Number(fbValue("fb.canTerm")) },
    obd: { overCan: Number(fbValue("fb.obdCan")), ecu: Number(fbValue("fb.obdEcu")) },
    oil: { source: Number(fbValue("fb.oil")) },
    chain: { enabled: Number(fbValue("fb.chain")) },
    kline: { protocol: Number(fbValue("fb.kline")) },
    haldex: { wireless: Number(fbValue("fb.haldex")) },
    display: {
      startPage: 0,
    },
    units: { quick: Number(fbValue("fb.units")) },
    io: {
      nightSource: Number(fbValue("fb.night")),
      dimInvert: Number(fbValue("fb.dimInvert")),
    },
    saver: { enabled: Number(fbValue("fb.saver")) },
    boot: {},
    power: {},
    menu: {},
  };
  Object.assign(body, carSettingsExtra()); // 0.37.0: the slots the Car values took
  if (body.kline.protocol === 2 && fbFound.klineObd.ok) {
    body.kline.ecuAddr = 0; // 0.38.6: 0 = the OBD-II address 33, where it answered (no wake-up of 01 first)
  }
  const ecuPick = fbValue("fb.ecu");
  if (ecuPick !== null) body.ecu = { wireless: Number(ecuPick) > 0 ? 1 : 0, carSignals: Number(ecuPick) === 1 ? 1 : 0 };
  // colours
  const theme = Number(fbValue("fb.theme"));
  if (theme !== settingsCache.display.theme) body.display.theme = theme;
  // boot logo: off, as it was, or the make's logo (choice 5)
  const logo = fbValue("fb.bootLogo");
  if (logo === "off") body.boot.logo = 0;
  else if (logo === "make") { body.boot.logo = 5; body.boot.make = fbFound.vin.makeKey; }
  else if (!settingsCache.boot.logo) body.boot.logo = 6; // was off: back to the can2gauge logo
  // power: ignition source; the illumination wire can't dim the glass when it carries the ignition
  const power = fbValue("fb.power");
  if (power === "switched") body.power.ignSource = 0;
  if (power === "can") {
    body.power.ignSource = 1;
    if (Number(fbValue("fb.canProfile")) === 3) body.power.canMode = 1; // MQB: Klemmen_Status_01
    else if (Number(fbValue("fb.canProfile")) === 0) body.power.canMode = 0; // PQ: BSG_Last / ZAS_1
    else body.power.canMode = 3; // any frame on the bus
  }
  if (power === "wire") { body.power.ignSource = 2; body.io.nightSource = 0; }
  menuItems.forEach(function (item) { body.menu[item[0]] = $("fbMenu_" + item[0]).checked ? 1 : 0; });
  // OBD adapter
  const obd = fbValue("fb.obd");
  if (obd === "off") body.elm = { enabled: 0 };
  else if (obd && obd.startsWith("wifi:")) body.elm = { enabled: 1, pollObd: 1, transport: 1, wifiSsid: obd.slice(5) };
  else if (obd && obd.startsWith("ble:")) body.elm = { enabled: 1, pollObd: 1, transport: 0 };

  $("fbApply").disabled = true;
  $("fbApplyInfo").textContent = "Applying...";
  fbScreen("saving the setup", 85); // 0.38.0: in steps to SET UP (was 95 straight away)
  try {
    const reply = await api("/api/settings", "POST", body);
    if (reply.error) throw new Error(reply.error);
    fbScreen("settings saved", 90);
    if (obd && obd.startsWith("ble:")) { // pair the Bluetooth adapter picked: "ble:<mac>:<type>"
      fbScreen("pairing the adapter", 92);
      const parts = obd.split(":");
      const mac = parts.slice(1, 7).join(":");
      await api("/api/elm/action", "POST", { action: "pair", mac: mac, type: Number(parts[7] || 0) });
    }
    const dbc = fbValue("fb.dbc");
    if (dbc) { // the car's DBC onto the gauge (as CAN > Car / DBC > Load to Gauge)
      $("fbApplyInfo").textContent = "Loading the " + dbc + " DBC...";
      fbScreen("loading the car's DBC", 95);
      const r = await fetch("/dbc/" + dbc + ".c2d");
      if (r.ok) await postDbc(await r.text());
    }
    // 0.38.7: the other gauges in the chain set up the same (after the DBC load, so the DBC travels with it)
    if (fbFound.chain.ok && $("fbPush") && $("fbPush").checked) {
      await fbPushToOthers(body);
    }
    screenSetupEnded = true; // 0.37.7: no setup update after this one (a late one would stick on the screen)
    await api("/api/firstboot", "POST", { state: "done", applied: true }); // 0.37.3: the screen says SET UP
    $("fbApplyInfo").textContent = "Done - the gauge has the new setup. Reloading the page...";
    toast("First Boot Setup applied");
    setTimeout(function () { location.reload(); }, 1500);
  } catch (e) {
    $("fbApply").disabled = false;
    $("fbApplyInfo").textContent = "Couldn't apply: " + e.message;
  }
}

// 0.38.7: what the other gauges get: the gauges, triggers and car settings. Each keeps its own WiFi, row number and
// name, CAN termination, OBD dongle, K-line and dash-light wire; the theme, units, screensaver, ignition and boot logo
// reach them as shared settings already. Their start page they work out themselves (by their place in the row).
function fbPushBody(body) {
  const push = { pages: body.pages, triggers: body.triggers, obd: body.obd, haldex: body.haldex, menu: body.menu };
  if (body.can) push.can = { profile: body.can.profile };
  if (body.ecu) push.ecu = body.ecu;
  if (body.vdiag) push.vdiag = body.vdiag;
  if (body.oil && Number(body.oil.source) === 0) push.oil = body.oil; // the cluster's oil on the CAN reaches every gauge
  return push;
}

// 0.38.7: sends the setup to the other gauges (this gauge passes it on over ESP-NOW, one gauge at a time) and follows
// the chain's progress until it's done (2 minutes at most)
async function fbPushToOthers(body) {
  $("fbApplyInfo").textContent = "Setting up the other gauges...";
  fbScreen("setting up the other gauges", 97);
  let reply = null;
  try {
    reply = await api("/api/chain/setup", "POST", fbPushBody(body));
  } catch (e) {
    reply = { error: e.message };
  }
  if (!reply || reply.error) {
    $("fbApplyInfo").textContent = "This gauge is set up; the others couldn't be: " + ((reply && reply.error) || "no answer");
    await fbSleep(5000);
    return;
  }
  let seenSending = false;
  for (let i = 0; i < 120; i++) {
    await fbSleep(1000);
    const chain = await fbGet("/api/chain");
    if (!chain || !chain.update) continue;
    const update = chain.update;
    if (update.text) $("fbApplyInfo").textContent = update.text;
    if (update.sending) {
      seenSending = true;
      continue;
    }
    if (seenSending || i >= 3) {
      const lines = (update.results || []).map(function (r) { return "G" + r.node + " " + r.text; }).join(", ");
      let resultText = "";
      if (lines) resultText = ": " + lines;
      $("fbApplyInfo").textContent = (update.text || "Done") + resultText;
      await fbSleep(4000);
      return;
    }
  }
}

async function fbSkip() {
  if (!confirm("Skip the setup and keep the current settings?\n\nIt can be run again from Settings > Device > First Boot Setup.")) return;
  screenSetupEnded = true; // 0.37.7: no setup update after Skip
  try { await api("/api/firstboot", "POST", { state: "done" }); } catch (e) { /* older firmware */ }
  $("fbTab").style.display = "none";
  document.querySelector('.nav-tab[data-page="dashboard"]').click();
  toast("setup skipped");
}

// ---------------------------------------------------------------------------
// Scan My Car inside First Boot (step 3)
// ---------------------------------------------------------------------------
// the scan is done: the starter set again, now with the scan's values
function fbScanDone() {
  fbMakeCandidates();
  fbDrawPages();
  fbScreen("pick the gauges on your phone", 80);
}

// the scan's progress on the gauge's screen: it fills 40 % to 80 % of the ring
function fbScanProgress(fraction, text) {
  fbScreen(text || "scanning the car", 40 + 40 * fraction);
}

// 0.37.0: Scan My Car, then the suggestions again (the button, and 0.37.14 straight after the checks)
async function fbScanCar() {
  await carRunScan($("fbCarLog"), $("fbCarProgress"), $("fbCarBar"), $("fbCarProgressText"), fbScanDone, fbScanProgress);
}

// 0.37.14: after the checks, the scan runs by itself when there's a way in to the car (OBD on the CAN, a connected
// dongle or the K-line), so the starter set has the car's own values (oil temperature, gear ...) without a second
// press. Not again when the last scan was of this same car (Check Again would mean minutes more): the button rescans.
// is there a way in to the car for the scan?
function fbWillScan() {
  const f = fbFound;
  return f.obdCan.ok || f.ble.connected || f.kline.ok || f.klineObd.ok;
}

async function fbAutoScan() {
  if (!fbWillScan()) {
    return; // no way in: nothing to scan
  }
  if (carScanning) {
    return;
  }
  if (carCatalog && carCatalog.values && carCatalog.values.length) {
    const vehicle = await fbGet("/api/vehicle");
    let vin = "";
    if (vehicle && vehicle.car && vehicle.car.vin) vin = vehicle.car.vin;
    else if (vehicle && vehicle.detected && vehicle.detected.vin) vin = vehicle.detected.vin;
    if (vin && carCatalog.vin === vin) {
      fbScanDone(); // this car was scanned already: the starter set from that scan
      return;
    }
  }
  await fbScanCar();
}

// ---------------------------------------------------------------------------
// start: build the tab, then open it if the gauge isn't set up yet
// ---------------------------------------------------------------------------
async function fbStart() {
  fbBuildTab();
  $("fbPages").addEventListener("change", fbPagesClick);
  $("fbTriggers").addEventListener("change", function (event) { fbTriggerListClick(event, fbTriggerCandidates, fbDrawPages); });
  $("fbCarScan").addEventListener("click", fbScanCar);
  $("fbPages").addEventListener("click", function (event) { if (event.target.closest("[data-fbmove]")) fbPagesClick(event); });
  // 0.38.0: the answer index.html asked for first thing (window.fbEarlyState), so First Boot doesn't wait behind every
  // request app.js makes while it loads
  let state = null;
  if (window.fbEarlyState) {
    state = await window.fbEarlyState;
  }
  // app.js loads the settings and the signal catalogue first (the checks need them; the rest of the main page goes on
  // loading underneath)
  for (let i = 0; i < 100 && (!settingsCache || !catalogue); i++) await fbSleep(100);
  if (!state) {
    state = await fbGet("/api/firstboot"); // no early answer: ask now, as before
  }
  if (state && state.state !== "done") {
    fbOpen();
  } else {
    document.documentElement.classList.remove("fb-pending");
  }
}
document.addEventListener("DOMContentLoaded", fbStart);
