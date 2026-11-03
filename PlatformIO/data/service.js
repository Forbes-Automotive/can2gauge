// can2gauge - the Service tab (0.16.0): fault codes and throttle body adaptation over the gauge's own K-line / CAN
// (src/can2gauge_Service.cpp). A button starts a job on the gauge; this page asks /api/service for news while the tab
// is open (a basic setting stops by itself a few seconds after the tab closes). 0.34.0: the faults through the OBD
// adapter too, and Scan Modules (every module's part number, name and fault count).
// Uses app.js's helpers: $, api, action, toast, esc, KL_MODULES, settingsCache.

const SVC_JOB_READ = 1;   // serviceJob_t: read faults
const SVC_JOB_CLEAR = 2;  // clear faults
const SVC_JOB_BASIC = 3;  // basic setting
const SVC_JOB_SCAN = 4;   // 0.34.0: scan modules
let svcLast = null;       // last /api/service
let svcWatching = false;  // a job runs: ask often

const SVC_OBD_ADDRESS = 0x33; // 0.38.0: OBD-II, any make (the K-line's OBD address: fault codes by mode 03 / 07 / 04)
const SVC_FAULT_STORED = 0x08;  // 0.38.0: the status the gauge gives an OBD-II code that is stored ...
const SVC_FAULT_PENDING = 0x04; // ... and one that is pending (the UDS status bits for confirmed / pending)
let svcDtcText = null;          // 0.38.0: the fault code descriptions (/dtc/sae.json), once loaded
let svcDtcLoading = false;      // ... being fetched now

// a module picker: the usual VAG addresses, then "Other address..." which shows the hex box. 0.38.0: withObd puts
// "OBD-II (any make)" first, for fault codes from any car on the K-line
function svcModuleOptions(select, withObd) {
  select.textContent = "";
  if (withObd) {
    select.appendChild(new Option("OBD-II (any make, K-line)", SVC_OBD_ADDRESS));
  }
  for (let i = 0; i < KL_MODULES.length; i++) {
    select.appendChild(new Option(KL_MODULES[i][1], KL_MODULES[i][0]));
  }
  select.appendChild(new Option("Other address...", "custom"));
}

// the module address picked (the list, or the hex box under "Other")
function svcAddress(selectId, boxId) {
  if ($(selectId).value === "custom") {
    return parseInt($(boxId).value, 16) || 1;
  }
  return Number($(selectId).value);
}

function svcModulePicked(event) {
  const boxId = event.target.dataset.box;
  if (event.target.value === "custom") {
    $(boxId).parentElement.style.display = "";
  } else {
    $(boxId).parentElement.style.display = "none";
  }
}

// the throttle body presets: which group the basic setting runs
function svcPresetPicked() {
  const preset = $("svcTbPreset").value;
  if (preset === "custom") {
    $("svcTbGroupBox").style.display = "";
  } else {
    $("svcTbGroupBox").style.display = "none";
    $("svcTbGroup").value = preset;
  }
}

async function svcStart(job) {
  let body = { job: job, via: Number($("svcVia").value), addr: svcAddress("svcModule", "svcAddr"), group: 0 };
  if (job === SVC_JOB_SCAN) {
    body = { job: job, via: Number($("svcScanVia").value), addr: 0, group: 0 };
  }
  if (job === SVC_JOB_CLEAR) {
    if (!confirm("Clear this module's fault codes? Engine off, ignition on. An engine clear also resets the emissions readiness monitors.")) {
      return;
    }
  }
  if (job === SVC_JOB_BASIC) {
    const group = Number($("svcTbGroup").value);
    if (!(group >= 1 && group <= 255)) {
      toast("type a group number, 1 to 255");
      return;
    }
    if (!confirm("Start the throttle body adaptation (basic setting group " + String(group).padStart(3, "0") + ")?\n\n" +
      "Ignition on, engine off, foot off the pedal, nothing else switched on. The throttle moves by itself for a few seconds. Keep this page open until it finishes.")) {
      return;
    }
    body = { job: job, via: 0, addr: svcAddress("svcTbModule", "svcTbAddr"), group: group };
  }
  const answer = await action("serviceStart", body);
  if (answer.status === "started") {
    svcWatching = true;
    svcPoll();
  }
}

// the buttons, one job each
function svcReadClicked() {
  svcStart(SVC_JOB_READ);
}

function svcClearClicked() {
  svcStart(SVC_JOB_CLEAR);
}

function svcBasicClicked() {
  svcStart(SVC_JOB_BASIC);
}

function svcScanClicked() {
  svcStart(SVC_JOB_SCAN);
}

// a CAN id as text: 3 hex digits for 11-bit, 8 for 29-bit
function svcIdText(id) {
  if (id > 0x7FF) {
    return id.toString(16).toUpperCase().padStart(8, "0");
  }
  return id.toString(16).toUpperCase().padStart(3, "0");
}

// tapping a module the scan found: read its faults the same way it was scanned
function svcModuleTapped(address) {
  const via = $("svcScanVia").value;
  $("svcVia").value = via;
  let known = false;
  for (let i = 0; i < KL_MODULES.length; i++) {
    if (KL_MODULES[i][0] === address) {
      known = true;
    }
  }
  if (known) {
    $("svcModule").value = String(address);
    $("svcAddr").parentElement.style.display = "none";
  } else {
    $("svcModule").value = "custom";
    $("svcAddr").value = address.toString(16).toUpperCase().padStart(2, "0");
    $("svcAddr").parentElement.style.display = "";
  }
  svcStart(SVC_JOB_READ);
}

// the scan's list: one row per module that answered
function svcRenderScan(s) {
  const box = $("svcModules");
  if (s.job !== SVC_JOB_SCAN) {
    return; // keep the last scan's list while another job runs
  }
  let progress = "";
  if (s.busy && s.scanTotal) {
    progress = s.scanDone + " of " + s.scanTotal + " asked";
  }
  $("svcScanState").textContent = (s.state || "") + (progress ? "  |  " + progress : "");
  box.textContent = "";
  const modules = s.modules || [];
  for (let i = 0; i < modules.length; i++) {
    const module = modules[i];
    const row = document.createElement("div");
    row.className = "row-inline";
    const text = document.createElement("div");
    text.className = "mono";
    text.style.flex = "1 1 220px";
    let title = module.name;
    if (module.addr) {
      title = module.addr.toString(16).toUpperCase().padStart(2, "0") + " " + module.name;
    }
    let faults = "faults: ?";
    if (module.faults >= 0) {
      faults = "faults: " + module.faults;
    }
    text.textContent = title + "\n" + module.part + (module.component ? "  " + module.component : "") +
      "\n" + svcIdText(module.txId) + " / " + svcIdText(module.rxId) + "   " + faults;
    row.appendChild(text);
    if (module.addr) {
      const button = document.createElement("button");
      button.className = "btn-secondary";
      button.textContent = "Read Faults";
      button.style.flex = "0 0 120px";
      button.addEventListener("click", function () { svcModuleTapped(module.addr); });
      row.appendChild(button);
    }
    box.appendChild(row);
  }
}

async function svcStop() {
  await action("serviceStop");
  svcPoll();
}

// the fault list
// 0.38.0: the fault code descriptions (OBDex, CC0: the generic SAE J2012 codes), fetched the first time a fault list
// shows; the list is drawn again once they're in
async function svcLoadDtcText() {
  if (svcDtcText || svcDtcLoading) {
    return;
  }
  svcDtcLoading = true;
  try {
    const answer = await fetch("/dtc/sae.json");
    if (answer.ok) {
      svcDtcText = await answer.json();
      if (svcLast) {
        svcRenderFaults(svcLast);
      }
    }
  } catch (e) {
    svcDtcText = {}; // not on this gauge's filesystem: codes show without descriptions
  }
  svcDtcLoading = false;
}

// 0.38.0: a code's description: "P0300" or a UDS "P0300-00" (the code and its failure type); a VAG fault number
// ("16684") or a maker's own code (P1xxx ...) has none
function svcDtcDescription(code) {
  const match = /^([PCBU][0-9A-F]{4})/.exec(code || "");
  if (!match || !svcDtcText) {
    return "";
  }
  return svcDtcText[match[1]] || "";
}

function svcRenderFaults(s) {
  const box = $("svcFaults");
  if (s.job !== SVC_JOB_READ && s.job !== SVC_JOB_CLEAR) {
    box.textContent = "";
    return;
  }
  let text = "";
  if (s.protocol) {
    text = text + s.protocol;
    if (s.moduleId) {
      text = text + "  |  " + s.moduleId;
    }
    text = text + "\n";
  }
  if (s.done && s.faults.length === 0 && s.ok) {
    text = text + "No faults stored.";
  }
  if (s.faults.length) {
    svcLoadDtcText();
  }
  const isObd = /^OBD-II/.test(s.protocol || ""); // 0.38.0: an OBD-II read: stored / pending instead of the status byte
  for (let i = 0; i < s.faults.length; i++) {
    const fault = s.faults[i];
    let statusText = "status " + fault.status.toString(16).toUpperCase().padStart(2, "0");
    if (isObd && fault.status === SVC_FAULT_PENDING) {
      statusText = "pending";
    } else if (isObd && fault.status === SVC_FAULT_STORED) {
      statusText = "stored";
    }
    text = text + fault.code + "   " + statusText + "\n";
    const description = svcDtcDescription(fault.code);
    if (description) {
      text = text + "    " + description + "\n";
    }
  }
  box.textContent = text;
}

// the basic setting's fields, like the measuring blocks on the K-line tab
function svcRenderBasic(s) {
  const grid = $("svcTbFields");
  if (s.job !== SVC_JOB_BASIC) {
    grid.textContent = "";
    $("svcTbInfo").textContent = "";
    return;
  }
  grid.textContent = "";
  const fields = s.fields || [];
  for (let i = 0; i < fields.length; i++) {
    const field = fields[i];
    const box = document.createElement("div");
    box.className = "gauge";
    const label = document.createElement("div");
    label.className = "gauge-label";
    label.textContent = field.name || "Field " + (i + 1);
    const value = document.createElement("div");
    value.className = "gauge-value";
    value.textContent = fmt(field.value, 1);
    const unit = document.createElement("div");
    unit.className = "gauge-unit";
    unit.textContent = uShown(field.unit) + "  (" + field.formula.toString(16).toUpperCase() + " " + field.a + " " + field.b + ")";
    box.appendChild(label);
    box.appendChild(value);
    box.appendChild(unit);
    grid.appendChild(box);
  }
  let info = "";
  if (s.title) {
    info = s.title + "  |  ";
  }
  if (s.protocol) {
    info = info + s.protocol + " " + (s.moduleId || "") + "  |  ";
  }
  info = info + (s.seconds || 0) + " s, " + (s.reads || 0) + " answers";
  $("svcTbInfo").textContent = info;
}

// asks the gauge for news (often while a job runs, once when the tab opens)
async function svcPoll() {
  if (!$("service-page").classList.contains("active")) {
    return;
  }
  let s;
  try {
    s = await api("/api/service");
  } catch (e) {
    return;
  }
  svcLast = s;
  let stateText = s.state || "No job run since the gauge started.";
  if (s.job === SVC_JOB_BASIC) {
    $("svcTbState").textContent = stateText;
    $("svcState").textContent = "";
  } else if (s.job === SVC_JOB_SCAN) {
    $("svcState").textContent = "";
    $("svcTbState").textContent = "";
  } else {
    $("svcState").textContent = stateText;
    $("svcTbState").textContent = "";
  }
  svcRenderFaults(s);
  svcRenderBasic(s);
  svcRenderScan(s);
  svcWatching = s.busy;
  $("svcTbStop").disabled = !(s.busy && s.job === SVC_JOB_BASIC);
}

function svcTick() {
  if (svcWatching) {
    svcPoll();
  }
}

// sets the tab up once app.js has loaded the settings (until then it looks again every 100 ms)
let svcSetupTries = 0;
function svcSetup() {
  if (!$("service-page")) {
    return;
  }
  if (!settingsCache) {
    svcSetupTries++;
    if (svcSetupTries < 100) {
      setTimeout(svcSetup, 100);
    }
    return;
  }
  svcModuleOptions($("svcModule"), true);   // 0.38.0: + OBD-II (any make)
  svcModuleOptions($("svcTbModule"), false); // basic settings: VW modules only
  $("svcModule").value = "1";               // the engine stays the first pick
  $("svcModule").addEventListener("change", svcModulePicked);
  $("svcTbModule").addEventListener("change", svcModulePicked);
  $("svcTbPreset").addEventListener("change", svcPresetPicked);
  svcPresetPicked();
  $("svcRead").addEventListener("click", svcReadClicked);
  $("svcClear").addEventListener("click", svcClearClicked);
  $("svcTbStart").addEventListener("click", svcBasicClicked);
  $("svcTbStop").addEventListener("click", svcStop);
  $("svcScan").addEventListener("click", svcScanClicked);
  document.querySelector('.nav-tab[data-page="service"]').addEventListener("click", svcPoll);
  setInterval(svcTick, 700);
  setInterval(svcPoll, 4000); // now and then even when idle, so a finished job's result shows
}
document.addEventListener("DOMContentLoaded", svcSetup);
