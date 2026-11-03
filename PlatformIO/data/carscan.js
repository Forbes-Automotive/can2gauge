// can2gauge - Scan My Car (0.37.0). One button finds every value the car can show and turns them into the "Car" source
// on the Gauges tab, so nobody has to know about PIDs, DIDs, measuring blocks or CAN ids to get a value on a gauge.
// The scan, in order:
//   1. the way in: OBD on the gauge's own CAN wire (01 00 to the engine), else the OBD adapter (Bluetooth / WiFi)
//   2. the car's make from the VIN -> its group in the DID library (/vdid/<group>.json, from OBDb)
//   3. the OBD PIDs the engine reports (01 00, 01 20 ...)
//   4. the modules that answer UDS (the library's module list: engine, gearbox, AWD ... with their ids)
//   5. each module's DIDs from the library: only the ones it answers, with enough data, are kept
//   6. VW PQ-era cars (no UDS modules, gauge's CAN): the modules that open a TP2.0 channel, then their measuring blocks
//   7. no engine found on the CAN (a Mk4 and older): the K-line modules, then their measuring blocks
// The gauge does the asking (POST /api/carscan, src/can2gauge_CarScan.cpp); this file decides what to ask and keeps
// what answered in /car/values.json on the gauge. Picking a value on a gauge page gives it an OBD PID, a K-line block
// field, or one of the 32 diagnostics value slots (with its own module), which the gauge then reads.
// Uses app.js's helpers: $, api, toast, esc, uShown, settingsCache, statusCache, catalogue, OBD_PIDS, SPECIFICS_KEYS,
// showAdvanced; and vdid.js's vdidGuessGroup / vdidLoadIndex.

const CAR_FILE = "/car/values.json";       // the last scan's result, on the gauge
const CAR_OBD_LISTS = ["0100", "0120", "0140", "0160", "0180", "01A0", "01C0"]; // the PID list requests (01 00 = PIDs 01-20 ...)
const CAR_TP20_ADDRESSES = [0x01, 0x02, 0x03, 0x08, 0x09, 0x15, 0x16, 0x17, 0x19, 0x22, 0x44, 0x46, 0x55, 0x5F]; // PQ modules worth a look
const CAR_KLINE_ADDRESSES = [0x01, 0x02, 0x03, 0x17, 0x08, 0x15, 0x22, 0x56]; // K-line modules worth a look (each costs a few seconds)
const CAR_JOB_MAX = 250;                   // requests in one gauge job (it takes up to 255)
// 0.37.2: what the scan says when there's no way in at all (the board can't tell a loose K-line wire from an idle one:
// its own 510R pull-up holds K at battery voltage either way, so the K-line check is one wake-up of the engine)
const CAR_NO_WAY_IN = "Connect an ECU: nothing answered on the gauge's CAN wire or K-line, and no OBD adapter is " +
  "connected. Wire the gauge's CAN or K-line to the car (or pair an OBD adapter on the Bluetooth OBD tab), turn the " +
  "ignition on, then scan again.";
const CAR_SLOTS = 32;                      // the gauge's diagnostics value slots (VDIAG_VALUES)

// 0.38.0: Simos 18.1 engines (EA888 gen 3 on MQB: Golf R / GTI, S3, TT, Leon Cupra ...). The DIDs, their lengths and
// their scaling come from the RokketGauge (RokketUds, contributed by Kile at Rokketek), read on cars. OBDb's VW list
// merges every VW engine, so some of these DIDs carry another engine's meaning there (0x1040 is the turbo speed on a
// Simos 18, "DPF differential pressure" in OBDb's list; 0x13A0 is the DI injection time, not a coolant temperature).
// A VW engine that answers like a Simos 18 gets these instead of OBDb's for the same DIDs.
// Each row: DID, its data bytes, name (15 characters at most: the gauge page's value name), first bit, bits,
// flags (1 = signed), scale, offset, unit
const CAR_SIMOS18 = [
  [0xF40C, 2, "Engine speed", 0, 16, 0, 0.25, 0, "rpm"],
  [0x39C0, 2, "MAP", 0, 16, 0, 0.1, 0, "kPa"],
  [0x39C1, 2, "MAP target", 0, 16, 0, 0.1, 0, "kPa"],
  [0x202A, 2, "Charge pressure", 0, 16, 0, 0.1, 0, "kPa"],     // before the throttle (PUT)
  [0x2029, 2, "Charge target", 0, 16, 0, 0.1, 0, "kPa"],
  [0x13CA, 2, "Ambient press", 0, 16, 0, 1 / 120.6, 0, "kPa"],
  [0xF405, 1, "Coolant temp", 0, 8, 0, 1, -40, "C"],
  [0x202F, 2, "Oil temp", 0, 16, 0, 0.1, -273.15, "C"],
  [0x1001, 1, "Intake air temp", 0, 8, 0, 0.75, -48, "C"],
  [0x10C0, 2, "AFR", 0, 16, 0, 14.7 / 1024, 0, "AFR"],         // lambda x 14.7
  [0xF444, 2, "Lambda target", 0, 16, 0, 1 / 32768, 0, ""],
  [0xF452, 1, "Ethanol", 0, 8, 0, 1 / 2.55, 0, "%"],
  [0xF406, 1, "Fuel trim short", 0, 8, 0, 0.78125, -100, "%"],
  [0xF407, 1, "Fuel trim long", 0, 8, 0, 0.78125, -100, "%"],   // wanders like a short trim: don't trigger on one sample
  [0x2004, 2, "Ignition timing", 0, 16, 1, 0.01, 0, "deg"],
  [0x200A, 2, "Knock cyl 1", 0, 16, 1, 0.01, 0, "deg"],          // timing pulled: reads negative
  [0x200B, 2, "Knock cyl 2", 0, 16, 1, 0.01, 0, "deg"],
  [0x200C, 2, "Knock cyl 3", 0, 16, 1, 0.01, 0, "deg"],
  [0x200D, 2, "Knock cyl 4", 0, 16, 1, 0.01, 0, "deg"],
  [0x2904, 2, "Misfires", 0, 16, 0, 1, 0, ""],
  [0x2033, 2, "Speed", 0, 16, 0, 0.01, 0, "km/h"],              // check against GPS: can depend on the car's coding
  [0x210F, 2, "Gear", 8, 8, 0, 1, 1, ""],                        // second byte 0-6 = gear 1-7 (neutral 0xFF reads 256)
  [0x20BA, 2, "Throttle plate", 0, 16, 1, 0.1, 0, "%"],
  [0x1070, 2, "Accel pedal", 0, 16, 0, 1 / 10.24, 0, "%"],
  [0x14A6, 1, "Battery", 0, 8, 0, 0.1015625, 0, "V"],
  [0x39A2, 2, "Wastegate", 0, 16, 0, -0.01, 100, "%"],          // the ECU sends it the other way up: 100 - x
  [0x1040, 2, "Turbo speed", 0, 16, 0, 1 / 163.84, 0, "krpm"],
  [0x201A, 2, "Exhaust cam", 0, 16, 1, 0.1, -24, "deg"],
  [0x2019, 2, "Exhaust cam tgt", 0, 16, 1, 0.1, -24, "deg"],
  [0x201E, 2, "Intake cam", 0, 16, 1, -0.1, 30, "deg"],          // 30 - x
  [0x201D, 2, "Intake cam tgt", 0, 16, 1, -0.1, 30, "deg"],
  [0x3D6C, 1, "Valve lift", 0, 8, 0, 1, 0, ""],                  // 0 = high lift, else low lift
  [0x13A0, 2, "Injection DI", 0, 16, 0, 0.004, 0, "ms"],
  [0x13AC, 2, "Injection MPI", 0, 16, 0, 0.004, 0, "ms"],
  [0x2027, 2, "Rail pressure", 0, 16, 0, 0.1, 0, "bar"],
  [0x293B, 2, "Rail press tgt", 0, 16, 0, 0.1, 0, "bar"],
  [0x2025, 2, "Low fuel press", 0, 16, 0, 0.001, 0, "bar"],
  [0x2932, 2, "Low fuel tgt", 0, 16, 0, 0.001, 0, "bar"],
  [0x2028, 2, "Fuel pump duty", 0, 16, 0, 0.01, 0, "%"],
  [0x4380, 2, "Torque request", 0, 16, 1, 0.1, 0, "Nm"],
  [0x437C, 2, "Torque actual", 0, 16, 1, 0.1, 0, "Nm"],
  [0x203F, 2, "Torque limit", 0, 16, 0, 0.1, 0, "Nm"],
  [0x209C, 2, "Torque loss", 0, 16, 0, 0.1, 0, "Nm"],
  [0x29D5, 2, "Torque gearbox", 0, 16, 0, 0.1, 0, "Nm"],        // the gearbox's torque intervention
  [0x1021, 2, "Torque ESP", 0, 16, 0, 0.1, 0, "Nm"],            // ESP / ASR torque request
  [0x1044, 2, "Turbo tq limit", 0, 16, 0, 0.1, 0, "Nm"],        // torque limit for turbo overspeed
];
const CAR_SIMOS18_MATCH = 36;              // of the 46, this many must answer with their own length (and MAP, AFR and
                                           // turbo speed always) before a VW engine counts as a Simos 18

let carCatalog = null;     // the last scan: { date, vin, make, group, link, modules: [...], values: [...] }
let carScanning = false;   // a scan is running
let carStopAsked = false;  // the Stop button was pressed
let carVdiagChanged = false; // a Car pick filled a diagnostics slot: the next Gauges save sends the slots too

// ---------------------------------------------------------------------------
// the catalogue on the gauge
// ---------------------------------------------------------------------------
async function carLoadCatalog() {
  try {
    const response = await fetch(CAR_FILE, { cache: "no-store" });
    if (response.ok) {
      carCatalog = await response.json();
    }
  } catch (e) {
    carCatalog = null; // never scanned
  }
  return carCatalog;
}

async function carSaveCatalog() {
  const form = new FormData();
  form.append("file", new Blob([JSON.stringify(carCatalog)], { type: "application/json" }), "values.json");
  const response = await fetch("/api/files/upload?dir=" + encodeURIComponent("/car"), { method: "POST", body: form });
  if (!response.ok) {
    throw new Error("couldn't save the scan on the gauge");
  }
}

// ---------------------------------------------------------------------------
// the gauge's scan jobs
// ---------------------------------------------------------------------------
function carSleep(ms) {
  return new Promise(function (resolve) { setTimeout(resolve, ms); });
}

// runs one job on the gauge and waits for it; onProgress(asked, total, state) while it runs. Returns the result with
// its items, or throws when the gauge won't start it.
async function carJob(body, onProgress) {
  const started = await api("/api/carscan", "POST", body);
  if (started && started.error) {
    throw new Error(started.error);
  }
  while (true) {
    await carSleep(400);
    if (carStopAsked) {
      await api("/api/carscan", "POST", { job: "stop" });
    }
    const progress = await api("/api/carscan?brief=1");
    if (onProgress) {
      onProgress(progress.asked || 0, progress.total || 0, progress.state || "");
    }
    if (!progress.busy) {
      break;
    }
  }
  return await api("/api/carscan");
}

// UDS / OBD requests in jobs of up to CAR_JOB_MAX; returns the answers in the same order as the requests
async function carAsk(link, requests, onProgress) {
  const answers = [];
  for (let start = 0; start < requests.length && !carStopAsked; start += CAR_JOB_MAX) {
    const part = requests.slice(start, start + CAR_JOB_MAX);
    const result = await carJob({ job: "uds", via: link, items: part }, function (asked, total, state) {
      if (onProgress) {
        onProgress(start + asked, requests.length, state);
      }
    });
    for (let i = 0; i < part.length; i++) {
      answers.push((result.items || [])[i] || { s: 3 });
    }
  }
  return answers;
}

// the bytes of an answer ("62F1873035..." -> [0x62, 0xF1, ...])
function carBytes(hexText) {
  const bytes = [];
  for (let i = 0; i + 1 < (hexText || "").length; i += 2) {
    bytes.push(parseInt(hexText.substr(i, 2), 16));
  }
  return bytes;
}

// printable text out of answer bytes (a part number, a name)
function carText(bytes) {
  let text = "";
  for (let i = 0; i < bytes.length; i++) {
    if (bytes[i] >= 0x20 && bytes[i] < 0x7F) {
      text = text + String.fromCharCode(bytes[i]);
    }
  }
  return text.trim();
}

// a value out of a DID's data, the way the gauge reads it (can2gauge_VagDiag.cpp udsValue): bit 0 = the top bit of
// the first data byte, read most significant first; flags 1 = signed, 2 = bytes least significant first
function carDecode(data, bit, bits, flags, scale, offset) {
  const firstByte = Math.floor(bit / 8);
  const lastBit = bit + bits - 1;
  if (Math.floor(lastBit / 8) >= data.length) {
    return null; // the DID is shorter than the value needs
  }
  const byteCount = Math.floor(lastBit / 8) - firstByte + 1;
  const bytes = [];
  for (let i = 0; i < byteCount; i++) {
    if ((flags & 2) && bits > 8) {
      bytes.push(data[firstByte + byteCount - 1 - i]);
    } else {
      bytes.push(data[firstByte + i]);
    }
  }
  let raw = 0;
  for (let b = bit; b <= lastBit; b++) {
    const byteIndex = Math.floor(b / 8) - firstByte;
    raw = raw * 2;
    if (bytes[byteIndex] & (0x80 >> (b % 8))) {
      raw = raw + 1;
    }
  }
  if ((flags & 1) && raw >= Math.pow(2, bits - 1)) {
    raw = raw - Math.pow(2, bits); // two's complement
  }
  return raw * scale + offset;
}

// "text" with a space in front, or "" when there is no text (for joining names)
function carWithSpace(text) {
  if (!text) {
    return "";
  }
  return " " + text;
}

// 1 for a true-ish value, 0 otherwise (the settings store switches as 0 / 1)
function carOneIf(condition) {
  if (condition) {
    return 1;
  }
  return 0;
}

// a module's id as text: 7E0, 17FC0076
function carIdText(id) {
  if (id > 0x7FF) {
    return id.toString(16).toUpperCase().padStart(8, "0");
  }
  return id.toString(16).toUpperCase().padStart(3, "0");
}

// a label file's names: { block: { t: title, f: [ , name1 .. name4 ] } } (as app.js klLabelsFor reads them)
async function carLabels(file) {
  const names = {};
  if (!file) {
    return names;
  }
  try {
    const text = await (await fetch("/api/lbl?file=" + encodeURIComponent(file))).text();
    text.split("\n").forEach(function (line) {
      const parts = line.split("\t");
      if (parts.length < 3) {
        return;
      }
      const block = Number(parts[0]);
      const field = Number(parts[1]);
      const name = (parts[2] + carWithSpace(parts[3])).trim();
      if (!names[block]) {
        names[block] = { t: "", f: [] };
      }
      if (field === 0) {
        names[block].t = name;
      } else {
        names[block].f[field] = name;
      }
    });
  } catch (e) {
    // no names: "Block 3 field 2" instead
  }
  return names;
}

// the measuring block fields that came back, as Car values ("kwp" over TP2.0, "kl" over the K-line)
async function carBlockValues(kind, moduleIndex, result) {
  const names = await carLabels(result.label);
  const values = [];
  (result.items || []).forEach(function (item) {
    if (item.s !== 1 || !item.f) {
      return;
    }
    item.f.forEach(function (field, f) {
      const unit = field[4] || "";
      const label = names[item.n] && names[item.n].f[f + 1];
      let name = "Block " + item.n + " field " + (f + 1);
      if (label) {
        name = label;
      }
      values.push({ k: kind, m: moduleIndex, id: item.n, field: f + 1, name: name, unit: unit, sample: field[3], conf: carOneIf(label) });
    });
  });
  return values;
}

// 0.37.2: is there an engine on the gauge's K-line? The K-line already talking says so; otherwise one wake-up of the
// engine (address 01, a few seconds) instead of every module. Remembered for the rest of the scan.
let carKlineAnswer = null; // null = not checked yet in this scan, then true / false
async function carKlineEngine(status, step) {
  if (carKlineAnswer !== null) {
    return carKlineAnswer;
  }
  if (status.kline && status.kline.up) {
    step("The K-line is talking to " + (status.kline.ecu || "the engine"));
    carKlineAnswer = true;
    return true;
  }
  // 0.37.6: already known to be empty - First Boot's check found no K-line, or the K-line gave up looking - so no
  // second wake-up
  const firstBootSaidNo = typeof fbFound !== "undefined" && fbFound && fbFound.kline && fbFound.kline.text && !fbFound.kline.ok;
  if (firstBootSaidNo || (status.kline && /not found/.test(status.kline.state || ""))) {
    step("No K-line (already checked)");
    carKlineAnswer = false;
    return false;
  }
  step("Checking the K-line (waking the engine, a few seconds)...");
  carKlineAnswer = false;
  try {
    const result = await carJob({ job: "klineModules", addrs: [0x01] });
    carKlineAnswer = !!(result.items && result.items[0] && result.items[0].s === 1);
  } catch (e) {
    step("K-line: " + e.message);
  }
  if (carKlineAnswer) {
    step("The engine answers on the K-line");
  } else {
    step("Nothing answers on the K-line");
  }
  return carKlineAnswer;
}

// 0.37.6: the OBD ECUs to ask (each answers on its own id): 11-bit 7E0-7E7, and 29-bit 18 DA xx F1 for the usual
// engine / gearbox / other addresses
const CAR_OBD_11 = [0x7E0, 0x7E1, 0x7E2, 0x7E3, 0x7E4, 0x7E5, 0x7E6, 0x7E7];
const CAR_OBD_29 = [0x10, 0x11, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x28];
// 0.37.6: BMW's usual module addresses (extended addressing on 6F1) and what they are
const CAR_BMW_NAMES = { 0x12: "Engine (DME)", 0x13: "Engine 2 (DME)", 0x18: "Gearbox (EGS)", 0x29: "Brakes (DSC)",
  0x30: "Steering (EPS)", 0x40: "Body (BDC / FEM)", 0x60: "Instruments (KOMBI)", 0x63: "Head unit", 0x78: "Climate (IHKA)" };

// an OBD ECU's name: the engine and gearbox by their usual ids, the rest by id
function carObdEcuName(tx) {
  if (tx === 0x7E0 || tx === 0x18DA10F1) return "Engine";
  if (tx === 0x7E1 || tx === 0x18DA18F1) return "Gearbox";
  return "OBD ECU " + carIdText(tx);
}

// 0.37.6: the Bluetooth OBD adapter, connected and talking to the car: pairs the likely one a scan finds when none is
// paired, or looks again for the paired one; then waits until it has found the car. null = no adapter / no car.
async function carAdapterUp(step, progress) {
  let elm = null;
  try { elm = await api("/api/elm"); } catch (e) { return null; }
  if (elm.connected && elm.state === "polling") return elm;
  if (elm.transport === 1) {
    // a WiFi adapter joins its own network: it's paired on the Bluetooth OBD tab (or First Boot), not here
    if (!elm.connected) return null;
  } else if (!elm.mac) {
    step("Looking for a Bluetooth OBD adapter...");
    progress(0.02, "Looking for an OBD adapter");
    await api("/api/elm/action", "POST", { action: "scan" });
    for (let i = 0; i < 15; i++) {
      await carSleep(1000);
      elm = await api("/api/elm");
      if (!elm.scanning && i >= 5) break;
    }
    const likely = (elm.found || []).filter(function (f) { return f.likely; })
      .sort(function (a, b) { return b.rssi - a.rssi; })[0];
    if (!likely) {
      step("No Bluetooth OBD adapter found (is it plugged in, with the ignition on?)");
      return null;
    }
    step("Pairing " + (likely.name || likely.mac) + "...");
    await api("/api/elm/action", "POST", { action: "pair", mac: likely.mac, type: likely.type });
  } else if (!elm.connected) {
    await api("/api/elm/action", "POST", { action: "scan" }); // the paired adapter: look again (it may have stopped looking)
  }
  step("Connecting to the adapter and finding the car (up to a minute)...");
  for (let i = 0; i < 60 && !carStopAsked; i++) {
    await carSleep(1000);
    progress(0.02 + 0.03 * i / 60, "Adapter: " + (elm.state || "connecting"));
    elm = await api("/api/elm");
    if (elm.connected && elm.state === "polling") {
      step("The adapter is connected (" + (elm.protocol || "OBD") + ")");
      return elm;
    }
  }
  step("The adapter didn't find the car (" + (elm.state || "not connected") + ")");
  return null;
}

// 0.37.6: the car's make, from the VIN the gauge has (asking for it over the CAN when it hasn't one yet)
async function carMakeOf(link, found) {
  let vehicle = null;
  try { vehicle = await api("/api/vehicle"); } catch (e) { return; }
  const hasVin = function (v) { return v && ((v.car && v.car.vin) || (v.detected && v.detected.vin)); };
  if (!hasVin(vehicle) && link === "can") {
    await api("/api/action", "POST", { action: "vehicleDetect" }); // a one-off VIN read on the gauge's CAN
    for (let i = 0; i < 6 && !hasVin(vehicle); i++) {
      await carSleep(1000);
      vehicle = await api("/api/vehicle");
    }
  }
  found.vin = (vehicle.car && vehicle.car.vin) || (vehicle.detected && vehicle.detected.vin) || "";
  found.make = (vehicle.car && vehicle.car.confirmed && vehicle.car.make) || (vehicle.detected && vehicle.detected.make) || "";
  found.makeKey = (vehicle.car && vehicle.car.confirmed && vehicle.car.makeKey) || (vehicle.detected && vehicle.detected.makeKey) || "";
}

// 0.38.0: OBD-II on the gauge's K-line, any make: one wake-up of the OBD address 0x33 and the PID lists (the gauge's
// klineObd job; it also reads the VIN for Your Car). For a car whose engine isn't a VW one at address 01, or a VW that
// only answers OBD. Remembered for the rest of the scan: { supported: [PIDs], protocol: "ISO 9141-2 (...)" } or false.
let carKlineObdAnswer = null; // null = not asked yet in this scan
async function carKlineObd(step) {
  if (carKlineObdAnswer !== null) {
    return carKlineObdAnswer;
  }
  carKlineObdAnswer = false;
  step("Checking the K-line for OBD-II (any make, a few seconds)...");
  let result = null;
  try {
    result = await carJob({ job: "klineObd", items: CAR_OBD_LISTS.map(function (request) { return { req: request }; }) });
  } catch (e) {
    step("K-line OBD-II: " + e.message);
    return false;
  }
  const supported = [];
  (result.items || []).forEach(function (answer, listIndex) {
    const bytes = carBytes(answer.a);
    if (answer.s !== 1 || bytes.length < 6 || bytes[0] !== 0x41) {
      return;
    }
    for (let bit = 0; bit < 32; bit++) {
      if (bytes[2 + Math.floor(bit / 8)] & (0x80 >> (bit % 8))) {
        supported.push(listIndex * 32 + bit + 1);
      }
    }
  });
  if (supported.length) {
    carKlineObdAnswer = { supported: supported, protocol: result.moduleId || "" };
    step("OBD-II answers on the K-line (" + (result.moduleId || "K-line") + ")");
  } else {
    step("No OBD-II on the K-line");
  }
  return carKlineObdAnswer;
}

// ---------------------------------------------------------------------------
// the scan
// ---------------------------------------------------------------------------
// step(text) writes a line; progress(fraction 0..1, text) moves the bar
async function carScan(step, progress) {
  carStopAsked = false;
  const found = { date: new Date().toISOString().slice(0, 10), vin: "", make: "", group: "", link: "", modules: [], values: [] };

  // 1. the way in: OBD on the gauge's CAN (11-bit, then 29-bit), else the OBD adapter (paired and connected here if
  // need be), else the K-line
  step("Finding the way in...");
  const status = await api("/api/status");
  let elm = null;
  let link = "";
  let car29 = false; // the car answers OBD on 29-bit ids
  if (status.can && status.can.up) {
    const answers = await carAsk("can", [{ tx: 0x7E0, req: "0100" }, { tx: 0x18DA10F1, req: "0100" }]);
    if (answers[0].s === 1 || answers[1].s === 1) {
      link = "can";
      car29 = answers[0].s !== 1;
      let idsText = "";
      if (car29) {
        idsText = " (29-bit)";
      }
      step("OBD answers on the gauge's CAN wire" + idsText);
    }
  }
  if (!link) {
    elm = await carAdapterUp(step, progress);
    if (elm) {
      link = "adapter";
      car29 = /29/.test(elm.protocol || "");
    }
  }
  found.link = link;
  carKlineAnswer = null;
  carKlineObdAnswer = null; // 0.38.0
  if (!link) {
    // no CAN / OBD way in: an older car on the K-line? One quick check, not a scan of every module
    let klineThere = await carKlineEngine(status, step);
    if (!klineThere) {
      klineThere = !!(await carKlineObd(step)); // 0.38.0: any make's OBD-II on the K-line
    }
    if (!klineThere) {
      const canTalking = status.can && status.can.up && status.can.rxPerSec > 0;
      if (canTalking) {
        // the car's CAN is busy, but nothing answers questions (a CAN-broadcast car, an aftermarket ECU)
        step("The car's CAN is talking, but nothing answers OBD or diagnostics, so there's nothing to scan. Its values " +
          "are under Live Now in the Car list (load the car's DBC on the CAN tab for more).");
        progress(0, "Nothing to scan");
      } else {
        step(CAR_NO_WAY_IN);
        progress(0, "Connect an ECU");
      }
      return null; // nothing to scan: the last scan (if any) stays as it was
    }
  }

  // 2. the car: its make from the VIN. 0.37.6: a make's DID library only once the make is known - never assume VW;
  // without it the scan is plain ISO (OBD from every ECU)
  await carMakeOf(link, found);
  await vdidLoadIndex();
  found.group = "";
  if (found.makeKey && typeof VDID_GROUP_OF_MAKE !== "undefined" && VDID_GROUP_OF_MAKE[found.makeKey]) {
    found.group = VDID_GROUP_OF_MAKE[found.makeKey];
  } else if (found.make && vdidIndex) {
    // the make's name against the library's makes ("smart", "Lotus" ...); no match = no library (not VW)
    const make = found.make.toLowerCase();
    vdidIndex.libs.forEach(function (lib) {
      lib.makes.forEach(function (name) {
        if (!found.group && make.indexOf(name.toLowerCase()) === 0) found.group = lib.key;
      });
    });
  }
  let library = null;
  if (found.group) {
    try { library = await (await fetch("/vdid/" + found.group + ".json")).json(); } catch (e) { library = null; }
  }
  let vinText = "";
  if (found.vin) {
    vinText = " (" + found.vin + ")";
  }
  if (library) {
    step("Car: " + found.make + vinText + " - " + library.name);
  } else {
    step("Car: " + (found.make || "make not known yet") + vinText + " - standard OBD (ISO) only");
  }

  // 3. OBD from every ECU (0.37.6): each one that answers 01 00 lists its own PIDs - the engine's go to the gauge's
  // OBD polling, the others' (the gearbox's ...) are asked of that ECU by the diagnostics values
  if (link && !carStopAsked) {
    progress(0.05, "OBD ECUs");
    let ecus = CAR_OBD_11;
    if (car29) {
      ecus = CAR_OBD_29.map(function (address) { return 0x18DA00F1 + address * 256; });
    }
    const firstLists = await carAsk(link, ecus.map(function (tx) { return { tx: tx, req: "0100" }; }));
    const answering = ecus.filter(function (tx, i) { return firstLists[i].s === 1; });
    let pidCount = 0;
    for (let e = 0; e < answering.length && !carStopAsked; e++) {
      const tx = answering[e];
      const lists = await carAsk(link, CAR_OBD_LISTS.map(function (request) { return { tx: tx, req: request }; }));
      const supported = [];
      lists.forEach(function (answer, listIndex) {
        const bytes = carBytes(answer.a);
        if (answer.s !== 1 || bytes.length < 6 || bytes[0] !== 0x41) {
          return;
        }
        for (let bit = 0; bit < 32; bit++) {
          if (bytes[2 + Math.floor(bit / 8)] & (0x80 >> (bit % 8))) {
            supported.push(listIndex * 32 + bit + 1);
          }
        }
      });
      const isEngine = e === 0 && (tx === 0x7E0 || tx === 0x18DA10F1);
      if (isEngine && supported.length === 0 && elm && elm.supported) {
        elm.supported.forEach(function (pid) { supported.push(pid); }); // the adapter's own list
      }
      const moduleIndex = found.modules.length;
      found.modules.push({ name: carObdEcuName(tx) + " (OBD)", proto: "obd", tx: tx, rx: 0 });
      let shown = 0;
      OBD_PIDS.forEach(function (row) {
        if (!supported.includes(row[0])) {
          return;
        }
        if (isEngine) {
          found.values.push({ k: "obd", m: moduleIndex, id: row[0], name: row[1], unit: row[3] || "", conf: 1 });
        } else {
          found.values.push({ k: "obd2", m: moduleIndex, id: row[0], name: row[1], unit: row[3] || "", conf: 1 });
        }
        shown++;
      });
      pidCount += shown;
      step(carObdEcuName(tx) + " (" + carIdText(tx) + "): " + supported.length + " OBD PIDs, " + shown + " the gauge can show");
    }
    if (!answering.length) {
      step("No ECU answers OBD mode 01");
    }
  }

  // 4. the UDS modules
  const udsModules = [];
  if (link && library && !carStopAsked) {
    progress(0.1, "Modules");
    const requests = [];
    const libraryMods = [];
    library.mods.forEach(function (mod, index) {
      if (link === "adapter" && !mod[0]) {
        return;
      }
      const tx = parseInt(mod[0], 16);
      const rx = parseInt(mod[1], 16);
      const ext = mod[2] || 0;
      libraryMods.push(index);
      let rxExt = 0;
      if (ext) {
        rxExt = 0xF1; // extended addressing (BMW): the module answers with the tester's address in front
      }
      requests.push({ tx: tx, rx: rx, txExt: ext, rxExt: rxExt, req: "22F187" });
    });
    const answers = await carAsk(link, requests, function (asked, total) { progress(0.1 + 0.1 * asked / Math.max(total, 1), "Modules"); });
    answers.forEach(function (answer, i) {
      if (answer.s === 3) {
        return; // nobody there
      }
      const mod = library.mods[libraryMods[i]];
      const request = requests[i];
      let part = "";
      if (answer.s === 1) {
        part = carText(carBytes(answer.a).slice(3));
      }
      let name = mod[4];
      if (found.group === "bmw" && CAR_BMW_NAMES[request.txExt]) {
        name = CAR_BMW_NAMES[request.txExt]; // 0.37.6: BMW's modules by what they are, not "Module 6F1 ext 12"
      }
      udsModules.push({ lib: libraryMods[i], name: name, proto: "uds", addr: mod[3] || 0, tx: request.tx, rx: request.rx,
        txExt: request.txExt, rxExt: request.rxExt, ident: part });
    });
    let moduleList = "";
    if (udsModules.length) {
      moduleList = ": " + udsModules.map(function (m) { return m.name; }).join(", ");
    }
    step(udsModules.length + " module(s) answer UDS" + moduleList);
  }

  // 5. each module's DIDs from the library
  for (let u = 0; u < udsModules.length && !carStopAsked; u++) {
    const module = udsModules[u];
    const rows = library.dids.filter(function (row) { return row[0] === module.lib; });
    if (!rows.length) {
      continue;
    }
    const moduleIndex = found.modules.length;
    found.modules.push({ name: module.name, proto: "uds", addr: module.addr, tx: module.tx, rx: module.rx, txExt: module.txExt,
      rxExt: module.rxExt, ident: module.ident });
    // the extended session first (some values need it), then every DID once
    const requests = [{ tx: module.tx, rx: module.rx, txExt: module.txExt, rxExt: module.rxExt, req: "1003" }];
    rows.forEach(function (row) {
      requests.push({ tx: module.tx, rx: module.rx, txExt: module.txExt, rxExt: module.rxExt, req: "22" + row[1] });
    });
    // 0.38.0: a VW engine is asked the Simos 18 DIDs OBDb's list hasn't got as well, to see if it is one
    const isVwEngine = found.group === "vag" && module.tx === 0x7E0;
    const simosExtra = []; // the extra DIDs asked, in request order (after the library's rows)
    if (isVwEngine) {
      CAR_SIMOS18.forEach(function (simos) {
        const hex = simos[0].toString(16).toUpperCase().padStart(4, "0");
        const inLibrary = rows.some(function (row) { return row[1].toUpperCase() === hex; });
        if (!inLibrary) {
          simosExtra.push(simos[0]);
          requests.push({ tx: module.tx, rx: module.rx, txExt: module.txExt, rxExt: module.rxExt, req: "22" + hex });
        }
      });
    }
    const start = 0.2 + 0.5 * u / udsModules.length;
    const answers = await carAsk(link, requests, function (asked, total) {
      progress(start + 0.5 / udsModules.length * asked / Math.max(total, 1), module.name + ": DID " + asked + " of " + total);
    });
    // 0.38.0: every DID that answered, by its number: its data and how many bytes the module sent
    const answerOf = {};
    for (let a = 1; a < answers.length; a++) {
      const answer = answers[a];
      const bytes = carBytes(answer.a);
      const asked = parseInt(requests[a].req.substring(2), 16);
      if (answer.s === 1 && bytes[0] === 0x62 && ((bytes[1] << 8) | bytes[2]) === asked) {
        answerOf[asked] = { data: bytes.slice(3), length: (answer.len || bytes.length) - 3 };
      }
    }
    // a Simos 18: most of its DIDs answer with exactly their own length, and MAP, AFR and turbo speed always do
    let isSimos = false;
    if (isVwEngine) {
      let matched = 0;
      CAR_SIMOS18.forEach(function (simos) {
        if (answerOf[simos[0]] && answerOf[simos[0]].length === simos[1]) {
          matched++;
        }
      });
      const keyDids = [0x39C0, 0x10C0, 0x1040];
      const keysThere = keyDids.every(function (did) { return answerOf[did] && answerOf[did].length === 2; });
      isSimos = matched >= CAR_SIMOS18_MATCH && keysThere;
    }
    let kept = 0;
    if (isSimos) {
      found.modules[moduleIndex].simos = 1; // a pick from it turns Turbo on (carSlotFor)
      found.modules[moduleIndex].name = module.name + " (Simos 18)";
      CAR_SIMOS18.forEach(function (simos) {
        const got = answerOf[simos[0]];
        if (!got || got.length !== simos[1]) {
          return; // this one didn't answer (or not as a Simos 18 does)
        }
        found.values.push({ k: "uds", m: moduleIndex, id: simos[0], bit: simos[3], bits: simos[4], flags: simos[5],
          scale: simos[6], offset: simos[7], name: simos[2], unit: simos[8], conf: 1,
          sample: carDecode(got.data, simos[3], simos[4], simos[5], simos[6], simos[7]) });
        kept++;
      });
      step(module.name + ": a Simos 18 - " + kept + " values from the Simos 18 list");
    }
    rows.forEach(function (row, r) {
      const answer = answers[r + 1];
      const bytes = carBytes(answer.a);
      const did = parseInt(row[1], 16);
      if (answer.s !== 1 || bytes[0] !== 0x62 || ((bytes[1] << 8) | bytes[2]) !== did) {
        return; // refused or not there
      }
      if (isSimos && CAR_SIMOS18.some(function (simos) { return simos[0] === did; })) {
        return; // 0.38.0: the Simos 18 list has this DID already, with its Simos 18 meaning
      }
      const data = bytes.slice(3);
      const dataLength = (answer.len || bytes.length) - 3;
      const seen = {};
      row[2].forEach(function (signal) {
        const where = signal[1] + ":" + signal[2];
        if (seen[where] || signal[1] + signal[2] > dataLength * 8) {
          return; // a second name for the same bits, or more bits than the module sent
        }
        seen[where] = true;
        found.values.push({ k: "uds", m: moduleIndex, id: did, bit: signal[1], bits: signal[2], flags: signal[3],
          scale: signal[4], offset: signal[5], name: signal[0], unit: signal[6] || "", conf: carOneIf(signal[7]),
          sample: carDecode(data, signal[1], signal[2], signal[3], signal[4], signal[5]) });
        kept++;
      });
    });
    step(module.name + ": " + kept + " values answered");
  }

  // 6. VW PQ era: TP2.0 modules and their measuring blocks (the gauge's own CAN only)
  const vwGroup = found.group === "vag";
  const engineFound = udsModules.some(function (m) { return m.addr === 1 || m.tx === 0x7E0; });
  if (link === "can" && vwGroup && !udsModules.some(function (m) { return m.addr && m.addr !== 1; }) && !carStopAsked) {
    progress(0.72, "TP2.0 modules");
    const result = await carJob({ job: "tp20Modules", addrs: CAR_TP20_ADDRESSES });
    const tp20Found = (result.items || []).filter(function (item) { return item.s === 1; });
    step(tp20Found.length + " module(s) answer TP2.0");
    for (let t = 0; t < tp20Found.length && !carStopAsked; t++) {
      const item = tp20Found[t];
      let label = "";
      try {
        const files = await api("/api/lbl/find?addr=" + item.n + "&ident=" + encodeURIComponent(item.id || ""));
        label = (files.files || [])[0] || "";
      } catch (e) { label = ""; }
      const blocks = [];
      for (let b = 1; b <= 255; b++) {
        blocks.push(b);
      }
      const start = 0.72 + 0.15 * t / tp20Found.length;
      const blockResult = await carJob({ job: "tp20Blocks", addr: item.n, blocks: blocks }, function (asked, total) {
        progress(start + 0.15 / tp20Found.length * asked / Math.max(total, 1), "Module " + item.n.toString(16).toUpperCase().padStart(2, "0") + ": block " + asked);
      });
      blockResult.label = label;
      const moduleIndex = found.modules.length;
      found.modules.push({ name: carModuleName(item.n), proto: "tp20", addr: item.n, ident: item.id || "", label: label });
      const values = await carBlockValues("kwp", moduleIndex, blockResult);
      values.forEach(function (value) { found.values.push(value); });
      step(carModuleName(item.n) + " (TP2.0): " + values.length + " values");
    }
  }

  // 7. nothing on the CAN for the engine (a Mk4 and older): the K-line
  const canEngine = engineFound || found.modules.some(function (m) { return m.proto === "tp20" && m.addr === 1; });
  // 0.37.6: also with the make not known yet - a Mk4 often has no VIN until its K-line has talked
  if (!canEngine && (vwGroup || !found.group) && !carStopAsked && await carKlineEngine(status, step)) { // 0.37.2: only with an engine there
    progress(0.87, "K-line modules");
    step("Waking the K-line modules (a few seconds each)...");
    let klineResult = null;
    try {
      klineResult = await carJob({ job: "klineModules", addrs: CAR_KLINE_ADDRESSES }, function (asked, total, state) {
        progress(0.87 + 0.04 * asked / Math.max(total, 1), state);
      });
    } catch (e) {
      step("K-line: " + e.message);
    }
    const klineFound = ((klineResult && klineResult.items) || []).map(function (item, i) {
      return Object.assign({ n: CAR_KLINE_ADDRESSES[i] }, item);
    }).filter(function (item) { return item.s === 1; });
    step(klineFound.length + " module(s) answer on the K-line");
    for (let k = 0; k < klineFound.length && !carStopAsked; k++) {
      const item = klineFound[k];
      let lastBlock = 64;
      if (item.n === 1) {
        lastBlock = 255; // the engine has the most blocks
      }
      const blocks = [];
      for (let b = 1; b <= lastBlock; b++) {
        blocks.push(b);
      }
      const start = 0.91 + 0.08 * k / klineFound.length;
      const blockResult = await carJob({ job: "klineBlocks", addr: item.n, blocks: blocks }, function (asked, total) {
        progress(start + 0.08 / klineFound.length * asked / Math.max(total, 1), carModuleName(item.n) + ": block " + asked);
      });
      const moduleIndex = found.modules.length;
      found.modules.push({ name: carModuleName(item.n), proto: "kline", addr: item.n, ident: item.id || blockResult.moduleId || "",
        label: blockResult.label || item.lbl || "" });
      const values = await carBlockValues("kl", moduleIndex, blockResult);
      values.forEach(function (value) { found.values.push(value); });
      step(carModuleName(item.n) + " (K-line): " + values.length + " values");
    }
  }

  // 7b. 0.38.0: no engine found on the CAN, the dongle or as a VW on the K-line (any other make, or OBD only): OBD-II
  // on the K-line. Its PIDs are read by the K-line tab's OBD mode, switched on at the end.
  const klineVwFound = found.modules.some(function (m) { return m.proto === "kline"; });
  const obdFound = found.values.some(function (v) { return v.k === "obd"; });
  if (!canEngine && !klineVwFound && !obdFound && !carStopAsked) {
    progress(0.95, "K-line OBD-II");
    const klineObd = await carKlineObd(step);
    if (klineObd) {
      const moduleIndex = found.modules.length;
      found.modules.push({ name: "Engine (OBD-II, K-line)", proto: "obd", tx: 0, rx: 0, ident: klineObd.protocol });
      let shown = 0;
      OBD_PIDS.forEach(function (row) {
        if (klineObd.supported.includes(row[0])) {
          found.values.push({ k: "obd", m: moduleIndex, id: row[0], name: row[1], unit: row[3] || "", conf: 1 });
          shown++;
        }
      });
      found.klineObd = 1;
      step("Engine (OBD-II on the K-line): " + klineObd.supported.length + " OBD PIDs, " + shown + " the gauge can show");
    }
  }

  if (carStopAsked) {
    progress(1, "Stopped");
  } else {
    progress(1, "Done");
  }
  carCatalog = found;
  await carSaveCatalog();
  // the diagnostics values go the way the scan found
  if (settingsCache && settingsCache.vdiag && link) {
    settingsCache.vdiag.link = carOneIf(link === "adapter"); // 0 = the gauge's CAN, 1 = the OBD adapter
    if (!settingsCache.vdiag.run) {
      settingsCache.vdiag.run = 1; // When used
    }
    const changes = { vdiag: { link: settingsCache.vdiag.link, run: settingsCache.vdiag.run } };
    const anyPid = found.values.some(function (v) { return v.k === "obd"; });
    if (link === "can" && anyPid) {
      changes.obd = { overCan: 1 }; // the PIDs are asked on the gauge's CAN
    }
    await api("/api/settings", "POST", changes);
  }
  // 0.38.0: the engine's PIDs come over the K-line: the K-line tab's OBD-II mode (KWP2000 / ISO 9141-2 to 0x33).
  // Only from Off (or already OBD): a VW's KWP1281 / ME7 setting is left alone.
  if (found.klineObd && settingsCache && settingsCache.kline) {
    const klineProtocol = Number(settingsCache.kline.protocol) || 0;
    if (klineProtocol === 0 || klineProtocol === 2) {
      settingsCache.kline.protocol = 2; // KL_KWP2000: OBD-II mode 01 PIDs
      settingsCache.kline.ecuAddr = 0;  // 0 = the OBD address 0x33
      await api("/api/settings", "POST", { kline: { protocol: 2, ecuAddr: 0 } });
    } else {
      step("The K-line tab is set to a VW protocol: switch it to KWP2000 (OBD-II) for these values");
    }
  }
  return found;
}

// a VAG address's name, from the K-line tab's list (app.js KL_MODULES)
function carModuleName(address) {
  const row = (typeof KL_MODULES !== "undefined") && KL_MODULES.find(function (m) { return m[0] === address; });
  if (row) {
    return row[1];
  }
  return "Module " + address.toString(16).toUpperCase().padStart(2, "0");
}

// ---------------------------------------------------------------------------
// the Gauges tab's "Your Car" card
// ---------------------------------------------------------------------------
function carSummary() {
  if (!carCatalog) {
    return "Not scanned yet. Scan My Car finds every value your car can show (ignition on; a few seconds to a few minutes).";
  }
  const moduleNames = carCatalog.modules.map(function (m) { return m.name; });
  let modulesWord = " modules: ";
  if (moduleNames.length === 1) {
    modulesWord = " module: ";
  }
  return carCatalog.values.length + " values from " + moduleNames.length + modulesWord + moduleNames.join(", ") +
    ". Scanned " + carCatalog.date + carWithSpace(carCatalog.make && "on the " + carCatalog.make) + ".";
}

function carBuildCard() {
  const page = $("carscan-page"); // 0.37.5: its own tab (was the top of the Gauges tab)
  if (!page || $("carCard")) {
    return;
  }
  const card = document.createElement("section");
  card.className = "card";
  card.id = "carCard";
  card.innerHTML =
    '<h2>Your Car\'s Values</h2>' +
    '<div class="help" id="carSummary"></div>' +
    '<div class="row-inline" data-nosave="1">' +
    '<button class="btn" id="carScanBtn" style="flex:0 1 200px">Scan My Car</button>' +
    '<button class="btn-secondary" id="carStopBtn" style="flex:0 1 120px" disabled>Stop</button>' +
    '</div>' +
    '<div class="car-progress" id="carProgress" style="display:none"><div class="car-bar"><span id="carBar"></span></div><div class="help" id="carProgressText"></div></div>' +
    '<div class="mono" id="carLog" style="display:none"></div>' +
    '<div id="carValues"></div>' + // 0.37.7: every value found, by module
    '<div class="help">Ignition on. The gauge asks the car through its CAN wire or the OBD adapter (and the K-line on older VWs), ' +
    'and keeps only what answers. Then pick Source = Car on a gauge (Gauges tab). The gauge\'s screen shows the scan as it goes.</div>';
  page.insertBefore(card, page.firstChild);
  $("carSummary").textContent = carSummary();
  carShowValues();
  $("carScanBtn").addEventListener("click", function () {
    carRunScan($("carLog"), $("carProgress"), $("carBar"), $("carProgressText"), null, null, true); // + on the screen
  });
  $("carStopBtn").addEventListener("click", function () { carStopAsked = true; $("carStopBtn").disabled = true; });
}

// 0.37.5: a progress ring on the gauge's screen (POST /api/firstboot {"screen"}), for First Boot and the Scan My Car
// tab. Repeats are skipped, and only one update is on its way at a time (the newest one goes next). hideMs: the last
// word, shown that long before the ring fades; failed: a red ring.
let screenSent = "";    // the last update sent
let screenBusy = false; // an update is on its way
let screenNext = null;  // the newest one waiting behind it
let screenSetupEnded = false; // 0.37.7: First Boot was applied / skipped: its SETTING UP updates stop (a late one would stick)
async function screenProgress(title, detail, percent, hideMs, failed) {
  if (title === "SETTING UP" && screenSetupEnded) return;
  const update = { title: title, detail: String(detail).substring(0, 40), pct: Math.round(percent) };
  if (hideMs) update.hideMs = hideMs;
  if (failed) update.failed = true;
  const key = JSON.stringify(update);
  if (key === screenSent) return;
  if (screenBusy) {
    screenNext = update;
    return;
  }
  screenBusy = true;
  screenSent = key;
  try { await api("/api/firstboot", "POST", { screen: update }); } catch (e) { /* older firmware */ }
  screenBusy = false;
  if (screenNext && screenNext.title === "SETTING UP" && screenSetupEnded) screenNext = null; // the setup ended meanwhile
  if (screenNext) {
    const next = screenNext;
    screenNext = null;
    screenProgress(next.title, next.detail, next.pct, next.hideMs, next.failed);
  }
}

// runs the scan with a log and a progress bar (the Scan My Car tab, or First Boot's); onDone after it's saved;
// onProgress(fraction, text) as it goes (0.37.3: First Boot shows it on the gauge's screen); onScreen = 0.37.5: the
// scan shows on the screen by itself (SCANNING, then SCANNED and the ring fades) - the Scan My Car tab
async function carRunScan(logBox, progressBox, bar, progressText, onDone, onProgress, onScreen) {
  if (carScanning) {
    return;
  }
  carScanning = true;
  if ($("carScanBtn")) $("carScanBtn").disabled = true;
  if ($("carStopBtn")) $("carStopBtn").disabled = false;
  logBox.style.display = "";
  logBox.textContent = "";
  progressBox.style.display = "";
  const step = function (text) { logBox.textContent = logBox.textContent + text + "\n"; };
  const progress = function (fraction, text) {
    bar.style.width = Math.round(Math.min(1, Math.max(0, fraction)) * 100) + "%";
    progressText.textContent = text;
    if (onProgress) onProgress(fraction, text);
    if (onScreen) screenProgress("SCANNING", text || "scanning the car", 100 * fraction);
  };
  if (onScreen) screenProgress("SCANNING", "finding the way in", 0);
  try {
    const found = await carScan(step, progress);
    if (found) {
      step("Found " + found.values.length + " values. Pick Source = Car on a gauge.");
      toast("scan done: " + found.values.length + " values");
      if ($("carSummary")) $("carSummary").textContent = carSummary();
      carShowValues();
      carRefreshPickers(); // 0.37.11: the Gauges tab's Car lists get the new values straight away
      if (onScreen) screenProgress("SCANNED", found.values.length + " values found", 100, 3000);
      if (onDone) onDone(found);
    } else {
      toast("connect an ECU first"); // 0.37.2: no way in to the car
      if (onScreen) screenProgress("NO WAY IN", "connect an ECU", 0, 4000, true);
    }
  } catch (e) {
    step("Stopped: " + e.message);
    if (onScreen) screenProgress("SCAN STOPPED", e.message, 0, 4000, true);
  }
  carScanning = false;
  if ($("carScanBtn")) $("carScanBtn").disabled = false;
  if ($("carStopBtn")) $("carStopBtn").disabled = true;
}

// ---------------------------------------------------------------------------
// the "Car" source on a gauge page (app.js srcPicker / readPicker call these)
// ---------------------------------------------------------------------------

// a Car value as the page stores it: "pid:12", "kl:1:3:2", or "cv:<index>" (a diagnostics slot, given at save time)
function carOptionValue(index) {
  const value = carCatalog.values[index];
  if (value.k === "obd") {
    return "pid:" + value.id;
  }
  if (value.k === "kl") {
    return "kl:" + carCatalog.modules[value.m].addr + ":" + value.id + ":" + value.field;
  }
  return "cv:" + index;
}

// the gauge's own values that are live now and come from the car (the CAN profile's, the OBD adapter's ...)
function carLiveKeys() {
  const signals = (statusCache && statusCache.signals) || {};
  return catalogue.signals.filter(function (s) {
    if (!signals[s.key]) return false;
    if (typeof SPECIFICS_KEYS !== "undefined" && SPECIFICS_KEYS.includes(s.key)) return false; // the gauge's own
    if (/^(adc|ecu|user|me7_)/.test(s.key)) return false; // other sources
    return true;
  });
}

// the diagnostics slot that holds this value already, -1 = none
function carSlotOf(value) {
  const slots = (settingsCache.vdiag && settingsCache.vdiag.values) || [];
  const module = carCatalog.modules[value.m] || {};
  for (let i = 0; i < slots.length; i++) {
    const slot = slots[i];
    if (!slot.kind) continue;
    if (value.k === "uds" && slot.kind === 1 && slot.id === value.id && slot.bit === value.bit && slot.bits === value.bits &&
        (slot.tx || 0) === (module.tx || 0)) {
      return i;
    }
    if (value.k === "kwp" && slot.kind === 2 && slot.id === value.id && slot.field === value.field && (slot.addr || 0) === (module.addr || 0)) {
      return i;
    }
    if (value.k === "obd2" && slot.kind === 3 && slot.id === value.id && (slot.tx || 0) === (module.tx || 0)) {
      return i; // 0.37.6: an OBD PID from another ECU (the gearbox's ...)
    }
  }
  return -1;
}

// a value into a free diagnostics slot (or the one it's in already); returns "diag:<n>", or "none" if all 32 are used
function carSlotFor(value) {
  let index = carSlotOf(value);
  if (index >= 0) {
    return "diag:" + (index + 1);
  }
  const slots = settingsCache.vdiag.values;
  for (let i = 0; i < slots.length && index < 0; i++) {
    if (!slots[i].kind) index = i;
  }
  if (index < 0) {
    toast("all " + CAR_SLOTS + " diagnostics values are in use - free one on the Diagnostics tab");
    return "none";
  }
  const module = carCatalog.modules[value.m] || {};
  const slot = slots[index];
  slot.kind = 1; // UDS DID
  slot.proto = 1;
  if (value.k === "kwp") {
    slot.kind = 2; // KWP block field over TP2.0
    slot.proto = 2;
  }
  if (value.k === "obd2") {
    slot.kind = 3; // 0.37.6: an OBD PID asked of this module (the gearbox's own PIDs ...)
  }
  slot.id = value.id;
  slot.field = value.field || 1;
  slot.bit = value.bit || 0;
  slot.bits = value.bits || 8;
  slot.signed = carOneIf(value.flags & 1);
  slot.lsb = carOneIf(value.flags & 2);
  slot.scale = 1;
  if (value.scale !== undefined) {
    slot.scale = value.scale;
  }
  slot.offset = value.offset || 0;
  slot.name = String(value.name).substring(0, 15);
  slot.unit = String(value.unit || "").substring(0, 7);
  slot.tx = module.tx || 0;
  slot.rx = module.rx || 0;
  slot.txExt = module.txExt || 0;
  slot.rxExt = module.rxExt || 0;
  slot.addr = module.addr || 0;
  if (module.simos) {
    // 0.38.0: a Simos 18 answers 8 DIDs in one request: Turbo (requests back to back, no gap between the answer's
    // frames) in groups of 8, instead of one DID every 50 ms
    settingsCache.vdiag.turbo = 1;
    settingsCache.vdiag.groupSize = 8;
  }
  carVdiagChanged = true;
  return "diag:" + (index + 1);
}

// "cv:<index>" -> the value the page stores (other values pass through)
function carResolve(text) {
  const match = /^cv:(\d+)$/.exec(text || "");
  if (!match || !carCatalog || !carCatalog.values[Number(match[1])]) {
    return text;
  }
  return carSlotFor(carCatalog.values[Number(match[1])]);
}

// the settings a Car pick changed, for the Gauges tab's save (then cleared)
function carSettingsExtra() {
  if (!carVdiagChanged || !settingsCache.vdiag) {
    return {};
  }
  carVdiagChanged = false;
  return { vdiag: { values: settingsCache.vdiag.values, link: settingsCache.vdiag.link, run: settingsCache.vdiag.run || 1,
    turbo: settingsCache.vdiag.turbo || 0, groupSize: settingsCache.vdiag.groupSize || 4 } }; // 0.38.0: + Turbo (a Simos 18 pick)
}

// the option that shows a page's saved value: its Car value, a live key, or the saved value kept as it is
function carMatchOption(value) {
  if (!value || value === "none") return "";
  const diag = /^diag:(\d+)$/.exec(value);
  if (diag && carCatalog && settingsCache.vdiag) {
    const slot = settingsCache.vdiag.values[Number(diag[1]) - 1];
    for (let i = 0; slot && i < carCatalog.values.length; i++) {
      const candidate = carCatalog.values[i];
      if (candidate.k !== "uds" && candidate.k !== "kwp" && candidate.k !== "obd2") continue;
      if (carSlotOf(candidate) === Number(diag[1]) - 1) return "cv:" + i;
    }
  }
  return value;
}

// 0.37.11: name and unit only - the reading at scan time ("- now 22") was clutter
function carValueLabel(value) {
  let text = value.name;
  if (value.unit) text = text + " (" + uShown(value.unit) + ")";
  if (!value.conf) text = text + " - untested";
  return text;
}

// 0.37.7: one module's values, as indexes into carCatalog.values: tested ones first, then A to Z
function carModuleRows(moduleIndex) {
  const rows = [];
  carCatalog.values.forEach(function (v, i) { if (v.m === moduleIndex) rows.push(i); });
  rows.sort(function (a, b) {
    const va = carCatalog.values[a];
    const vb = carCatalog.values[b];
    if (va.conf !== vb.conf) return vb.conf - va.conf;
    return String(va.name).localeCompare(String(vb.name));
  });
  return rows;
}

// 0.37.7: the Scan My Car tab's list: every value the scan found, one fold per module
function carShowValues() {
  const box = $("carValues");
  if (!box) return;
  if (!carCatalog || !carCatalog.values.length) {
    box.innerHTML = "";
    return;
  }
  box.innerHTML = carCatalog.modules.map(function (module, moduleIndex) {
    const rows = carModuleRows(moduleIndex);
    if (!rows.length) return "";
    let title = module.name;
    if (module.proto === "kline") title = title + " (K-line)";
    const lines = rows.map(function (i) { return esc(carValueLabel(carCatalog.values[i])); }).join("\n");
    return '<details class="car-module"><summary>' + esc(title) + " - " + rows.length + " values</summary>" +
      '<div class="mono">' + lines + "</div></details>";
  }).join("");
}

// draws the Car picker into host (as app.js srcPicker does for the other sources)
function carPicker(host, value, second, label, noneOption) {
  host.dataset.source = "car";
  host.carDrawnWith = { second: second, label: label, noneOption: noneOption }; // 0.37.11: for carRefreshPickers()
  const groups = [];
  // 0.37.7: the scanned modules first (each with its values), the gauge's own live values last
  if (carCatalog) {
    carCatalog.modules.forEach(function (module, moduleIndex) {
      const rows = carModuleRows(moduleIndex);
      if (!rows.length) return;
      let title = module.name;
      if (module.proto === "kline") title = title + " (K-line)";
      groups.push('<optgroup label="' + esc(title) + '">' + rows.map(function (i) {
        return '<option value="' + carOptionValue(i) + '">' + esc(carValueLabel(carCatalog.values[i])) + "</option>";
      }).join("") + "</optgroup>");
    });
  }
  const live = carLiveKeys();
  if (live.length) {
    groups.push('<optgroup label="Live now (decoded by the gauge)">' + live.map(function (s) {
      let unitText = "";
      if (s.unit) unitText = " (" + uShown(s.unit) + ")";
      return '<option value="' + s.key + '">' + esc(s.name) + unitText + "</option>";
    }).join("") + "</optgroup>");
  }
  let empty = "";
  if (!groups.length) {
    empty = '<option value="none">Scan My Car first (its own tab)</option>';
  }
  let pickFirst = "";
  let findBox = ""; // the Find box: on the main value only (a second value or a slot has the same list)
  if (!second) {
    pickFirst = '<option value="none">Pick a Value</option>';
    findBox = '<div class="form-group" data-nosave><label>Find</label><input type="text" class="sp-carfind" placeholder="oil, boost, clutch ..."></div>';
  }
  host.innerHTML = findBox +
    '<div class="form-group"><label>' + (label || "Value") + '</label><select class="sp-car">' + (noneOption || "") +
    pickFirst + groups.join("") + empty + "</select></div>";
  const select = host.querySelector(".sp-car");
  const shown = carMatchOption(value);
  if (shown) {
    select.value = shown;
    if (select.value !== shown) {
      select.appendChild(new Option(carSavedName(shown), shown, true, true)); // a saved value the scan didn't list: kept
    }
  } else {
    select.value = "none";
  }
  if (!host.querySelector(".sp-carfind")) {
    return;
  }
  host.querySelector(".sp-carfind").addEventListener("input", function (event) {
    const words = event.target.value.toLowerCase().split(/\s+/).filter(function (w) { return w; });
    [...select.options].forEach(function (option) {
      const text = option.textContent.toLowerCase();
      const hit = words.every(function (w) { return text.indexOf(w) >= 0; });
      option.hidden = !hit && option.value !== "none";
    });
  });
}

// 0.37.11: a scan has finished: every Car picker already on the Gauges tab is drawn again with the new list (before,
// a First Boot scan left them showing only "Live now" until the page was reloaded). The picked value stays: matched by
// its value, or by its name when it was picked from the old list (those values number the old list's entries).
function carRefreshPickers() {
  document.querySelectorAll('[data-source="car"]').forEach(function (host) {
    const select = host.querySelector(".sp-car");
    if (!select || !host.carDrawnWith) return;
    const oldValue = select.value;
    let oldText = "";
    if (select.selectedIndex >= 0) oldText = select.options[select.selectedIndex].textContent;
    let keepValue = oldValue;
    if (oldValue.indexOf("cv:") === 0) keepValue = "none"; // an entry of the old list: found again by name below
    carPicker(host, keepValue, host.carDrawnWith.second, host.carDrawnWith.label, host.carDrawnWith.noneOption);
    const newSelect = host.querySelector(".sp-car");
    if (oldValue.indexOf("cv:") === 0) {
      [...newSelect.options].forEach(function (option) {
        if (option.textContent === oldText) newSelect.value = option.value;
      });
    }
  });
}

// a saved value the scan doesn't list, named as well as it can be
function carSavedName(value) {
  const diag = /^diag:(\d+)$/.exec(value);
  if (diag && settingsCache.vdiag) {
    const slot = settingsCache.vdiag.values[Number(diag[1]) - 1];
    if (slot && slot.kind) return "Diagnostics " + diag[1] + ": " + (slot.name || "(no name)");
  }
  const pid = /^pid:(\d+)$/.exec(value);
  if (pid) return "OBD PID " + Number(pid[1]).toString(16).toUpperCase().padStart(2, "0");
  const kl = /^kl:(\d+):(\d+):(\d+)$/.exec(value);
  if (kl) return "K-line module " + Number(kl[1]).toString(16).toUpperCase() + ", block " + kl[2] + " field " + kl[3];
  const signal = catalogue.signals.find(function (s) { return s.key === value; });
  if (signal) return signal.name;
  return value;
}

// the picker's value as the page stores it (a diagnostics slot is given out here)
function carPickerValue(host) {
  const select = host.querySelector(".sp-car");
  if (!select) return "none";
  return carResolve(select.value || "none");
}

// ---------------------------------------------------------------------------
// First Boot (firstboot.js): the values worth a gauge, found by name in the scan
// ---------------------------------------------------------------------------
const CAR_SUGGEST = [
  ["oiltemp", /oil temp/i, "OIL TEMP"],
  ["boost", /(boost|charge air|charge pressure|intake manifold pressure)/i, "BOOST"],
  ["iat", /(intake air temp|charge air temp)/i, "INTAKE"],
  ["egt", /exhaust gas temp/i, "EGT"],
  ["gboxTemp", /(gearbox|transmission|clutch).*(temp)/i, "GEARBOX"],
  ["lambda", /lambda/i, "LAMBDA"],
  ["gear", /^(current |engaged |actual )?gear( engaged| actual)?$/i, "GEAR"], // 0.37.13: First Boot's starter pack
  ["throttle", /(accelerator pedal|pedal position)/i, "THROTTLE"],         // 0.37.13: a module's pedal when OBD has none
];

// [key, "cv:<index>" or "pid:.." / "kl:..", label] for each kind of value the scan has (tested ones first)
function carSuggestions() {
  const out = [];
  if (!carCatalog) return out;
  CAR_SUGGEST.forEach(function (row) {
    let best = -1;
    carCatalog.values.forEach(function (value, i) {
      if (!row[1].test(value.name)) return;
      if (best < 0 || (value.conf && !carCatalog.values[best].conf)) best = i;
    });
    if (best >= 0) out.push([row[0], carOptionValue(best), row[2], carCatalog.values[best]]);
  });
  return out;
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------
function carAddStyle() {
  const style = document.createElement("style");
  style.textContent =
    ".car-progress { margin: 10px 0; }" +
    ".car-bar { height: 8px; border-radius: 4px; background: var(--panel-2); overflow: hidden; }" +
    ".car-bar span { display: block; height: 100%; width: 0; background: var(--accent); transition: width .3s; }" +
    ".car-module { margin-top: 8px; } .car-module summary { cursor: pointer; color: var(--accent); }";
  document.head.appendChild(style);
}

async function carSetup() {
  carAddStyle();
  carBuildCard();
  await carLoadCatalog();
  if ($("carSummary")) $("carSummary").textContent = carSummary();
  carShowValues();
}
document.addEventListener("DOMContentLoaded", carSetup);
