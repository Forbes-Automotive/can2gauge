// can2gauge - the Diagnostics tab's DID library (0.34.0). The measuring values OBDb (github.com/OBDb, CC BY-SA 4.0) lists
// for each car group, built into /vdid/<group>.json by tools/vdid/obdb_to_c2g.py. Pick a group and a module, search,
// and Add puts the value in the first free slot of the Diagnostics tab with its DID, bit position, scaling and unit filled in
// (0.37.0: with its own module, so values from several modules run together).
// Uses app.js's helpers: $, esc, toast, settingsCache, renderVdiag. Builds its own card on the Diagnostics tab.

const VDID_MAX_SHOWN = 80;  // rows listed at once (narrow the search for more)
let vdidIndex = null;       // /vdid/index.json: the groups
let vdidLibrary = null;     // the group loaded now
let vdidLoading = false;

// one CAN id as text: 3 hex digits for 11-bit, 8 for 29-bit
function vdidIdText(id) {
  if (id > 0x7FF) {
    return id.toString(16).toUpperCase().padStart(8, "0");
  }
  return id.toString(16).toUpperCase().padStart(3, "0");
}

// the card, put in front of the tab's Values card
function vdidBuildCard() {
  const card = document.createElement("section");
  card.className = "card";
  card.id = "vdidCard";
  card.innerHTML =
    '<h2>DID Library</h2>' +
    '<div class="row-inline" data-nosave="1">' +
    '<div class="form-group"><label>Car Group</label><select id="vdidLib"></select></div>' +
    '<div class="form-group"><label>Module</label><select id="vdidMod"></select></div>' +
    '</div>' +
    '<div class="row-inline" data-nosave="1">' +
    '<div class="form-group"><label>Search</label><input type="text" id="vdidSearch" placeholder="boost, oil temp, clutch, 2BE4 ..."></div>' +
    '<label class="toggle"><input type="checkbox" id="vdidConfirmed" checked><span class="toggle-slider"></span><span class="toggle-label">Confirmed Only</span></label>' +
    '</div>' +
    '<div class="mono" id="vdidInfo"></div>' +
    '<div id="vdidList"></div>' +
    '<div class="help">From OBDb, an open database of what cars answer (github.com/OBDb, CC BY-SA 4.0). ' +
    '"Confirmed" values have been checked on at least one real car; the rest are still being tried and may read wrong. ' +
    'Search looks in the module picked first, then in every module; added values show on the Gauges tab under Source = Diagnostics. ' +
    'Engine and gearbox lists are big because they cover every engine in the group: a value your engine doesn\'t have ' +
    'is refused (NRC 31) and the tab stops asking for it.</div>';
  const values = $("vdValuesCard");
  values.parentElement.insertBefore(card, values);
}

// the VIN's make key (from the adapter, can2gauge_ELM.cpp's WMI table) -> the DID library's car group
const VDID_GROUP_OF_MAKE = {
  vw: "vag", audi: "vag", seat: "vag", cupra: "vag", skoda: "vag", porsche: "vag",
  bmw: "bmw", mini: "bmw", mercedes: "mercedes", ford: "ford", jaguar: "jlr", landrover: "jlr",
  fiat: "stellantis", alfa: "stellantis", peugeot: "stellantis", citroen: "stellantis", opel: "stellantis",
  vauxhall: "stellantis", dodge: "stellantis", jeep: "stellantis", chrysler: "stellantis",
  renault: "renault", dacia: "renault", volvo: "volvo", honda: "honda", nissan: "nissan", infiniti: "nissan",
  mazda: "mazda", subaru: "subaru", mitsubishi: "mitsubishi", hyundai: "hyundai", kia: "hyundai",
  chevrolet: "gm", lexus: "toyota", toyota: "toyota",
  // 0.36.1: the other makes the VIN table names, under the group that builds them
  bentley: "vag", lamborghini: "vag", bugatti: "vag", rollsroyce: "bmw", smart: "mercedes", lincoln: "ford",
  ds: "stellantis", lancia: "stellantis", maserati: "stellantis", ram: "stellantis", polestar: "volvo",
  acura: "honda", genesis: "hyundai", buick: "gm", cadillac: "gm", gmc: "gm", pontiac: "gm", saturn: "gm"
};

// the group for the car the adapter saw (VIN), else the VW group
function vdidGuessGroup() {
  let make = "";
  if (window.lastElmStatus && window.lastElmStatus.make) {
    make = String(window.lastElmStatus.make).toLowerCase();
  }
  // 0.35.0: the car the driver confirmed (Your Car) comes first
  if (typeof carLast !== "undefined" && carLast && carLast.car && carLast.car.confirmed &&
      VDID_GROUP_OF_MAKE[carLast.car.makeKey]) {
    return VDID_GROUP_OF_MAKE[carLast.car.makeKey];
  }
  // the make key first ("opel" for "Opel / Vauxhall", which the name alone doesn't match)
  if (window.lastElmStatus && VDID_GROUP_OF_MAKE[window.lastElmStatus.makeKey]) {
    return VDID_GROUP_OF_MAKE[window.lastElmStatus.makeKey];
  }
  // then the make's name against the groups' OBDb makes (smart, Lotus ...)
  if (vdidIndex && make) {
    for (let i = 0; i < vdidIndex.libs.length; i++) {
      const lib = vdidIndex.libs[i];
      for (let j = 0; j < lib.makes.length; j++) {
        if (make.indexOf(lib.makes[j].toLowerCase()) === 0) {
          return lib.key;
        }
      }
    }
  }
  return "vag";
}

async function vdidLoadIndex() {
  if (vdidIndex) {
    return true;
  }
  try {
    const response = await fetch("/vdid/index.json");
    vdidIndex = await response.json();
  } catch (e) {
    $("vdidInfo").textContent = "The DID library isn't on this gauge (flash the filesystem image).";
    return false;
  }
  const select = $("vdidLib");
  select.textContent = "";
  for (let i = 0; i < vdidIndex.libs.length; i++) {
    const lib = vdidIndex.libs[i];
    select.appendChild(new Option(lib.name + " - " + lib.values + " values", lib.key));
  }
  select.value = vdidGuessGroup();
  return true;
}

async function vdidLoadLibrary(key) {
  if (vdidLibrary && vdidLibrary.key === key) {
    return;
  }
  vdidLoading = true;
  $("vdidInfo").textContent = "loading...";
  try {
    const response = await fetch("/vdid/" + key + ".json");
    vdidLibrary = await response.json();
  } catch (e) {
    vdidLibrary = null;
    $("vdidInfo").textContent = "couldn't load that group";
  }
  vdidLoading = false;
  vdidFillModules();
}

// the module list of the group, with how many values each has; the tab's own module picked when it is there
function vdidFillModules() {
  const select = $("vdidMod");
  select.textContent = "";
  if (!vdidLibrary) {
    return;
  }
  const counts = [];
  for (let i = 0; i < vdidLibrary.mods.length; i++) {
    counts.push(0);
  }
  for (let i = 0; i < vdidLibrary.dids.length; i++) {
    counts[vdidLibrary.dids[i][0]] += vdidLibrary.dids[i][2].length;
  }
  select.appendChild(new Option("Every module", "all"));
  let pick = "all";
  const tab = settingsCache.vdiag || {};
  for (let i = 0; i < vdidLibrary.mods.length; i++) {
    const mod = vdidLibrary.mods[i];
    let label = mod[4] + " (" + mod[0] + ")";
    if (mod[3]) {
      label = mod[4] + " - address " + mod[3].toString(16).toUpperCase().padStart(2, "0") + " (" + mod[0] + ")";
    }
    select.appendChild(new Option(label + " - " + counts[i], String(i)));
    if (tab.txId && parseInt(mod[0], 16) === tab.txId) {
      pick = String(i);
    } else if (!tab.txId && mod[3] && mod[3] === tab.addr && pick === "all") {
      pick = String(i);
    }
  }
  select.value = pick;
  vdidShow();
}

// the search box's words, lower case; "0x2BE4" counts as the DID "2be4"
function vdidSearchWords() {
  const words = $("vdidSearch").value.trim().toLowerCase().split(/\s+/);
  const out = [];
  for (let i = 0; i < words.length; i++) {
    let word = words[i];
    if (word.indexOf("0x") === 0 && word.length > 2) {
      word = word.substring(2);
    }
    if (word) {
      out.push(word);
    }
  }
  return out;
}

// does one value match every search word? (its name, DID, unit and module name are searched)
function vdidMatches(row, signal, words) {
  const mod = vdidLibrary.mods[row[0]];
  const text = (signal[0] + " " + row[1] + " " + signal[6] + " " + mod[4]).toLowerCase();
  for (let w = 0; w < words.length; w++) {
    if (text.indexOf(words[w]) < 0) {
      return false;
    }
  }
  return true;
}

// counts (and with list given, lists) the matching values. modulePick "all" = every module.
// Returns [found, shown, unconfirmed matches left out by Confirmed Only].
function vdidSearch(modulePick, words, confirmedOnly, list) {
  let found = 0;
  let shown = 0;
  let hidden = 0;
  for (let i = 0; i < vdidLibrary.dids.length; i++) {
    const row = vdidLibrary.dids[i];
    if (modulePick !== "all" && String(row[0]) !== modulePick) {
      continue;
    }
    for (let j = 0; j < row[2].length; j++) {
      const signal = row[2][j];
      if (!vdidMatches(row, signal, words)) {
        continue;
      }
      if (confirmedOnly && !signal[7]) {
        hidden++;
        continue;
      }
      found++;
      if (!list || shown >= VDID_MAX_SHOWN) {
        continue;
      }
      shown++;
      list.appendChild(vdidRow(row, j));
    }
  }
  return [found, shown, hidden];
}

// the values matching the search, as rows with an Add button. 0.36.8: nothing on the picked module = every module
// is searched instead, and matches left out by Confirmed Only are counted, so a search never just comes back empty.
function vdidShow() {
  const list = $("vdidList");
  list.textContent = "";
  if (!vdidLibrary) {
    return;
  }
  const words = vdidSearchWords();
  const confirmedOnly = $("vdidConfirmed").checked;
  let modulePick = $("vdidMod").value;
  let note = "";
  let result = vdidSearch(modulePick, words, confirmedOnly, list);
  if (result[0] === 0 && result[2] === 0 && words.length > 0 && modulePick !== "all") {
    const others = vdidSearch("all", words, confirmedOnly, null);
    if (others[0] > 0 || others[2] > 0) {
      note = " in other modules";
      modulePick = "all";
      result = vdidSearch("all", words, confirmedOnly, list);
    }
  }
  let info = result[0] + " values" + note;
  if (result[0] > result[1]) {
    info = info + ", first " + result[1] + " shown";
  }
  if (result[2] > 0 && words.length > 0) {
    info = info + ", " + result[2] + " unconfirmed hidden";
  }
  $("vdidInfo").textContent = info;
}

function vdidRow(row, signalIndex) {
  const signal = row[2][signalIndex];
  const mod = vdidLibrary.mods[row[0]];
  const line = document.createElement("div");
  line.className = "row-inline vdid-row";
  const text = document.createElement("div");
  text.className = "mono";
  text.style.flex = "1 1 200px";
  let where = mod[4] + "  DID " + row[1] + "  bit " + signal[1] + " x" + signal[2];
  if (signal[8]) {
    where = where + "  (" + signal[8] + ")";
  }
  if (!signal[7]) {
    where = where + "  unconfirmed";
  }
  text.textContent = signal[0] + (signal[6] ? " [" + signal[6] + "]" : "") + "\n" + where;
  const button = document.createElement("button");
  button.className = "btn-secondary";
  button.textContent = "Add";
  button.style.flex = "0 0 80px";
  button.addEventListener("click", function () { vdidAdd(row, signalIndex); });
  line.appendChild(text);
  line.appendChild(button);
  return line;
}

// 0.37.0: Add puts the value in the first free slot with its own module (each value can come from a different module
// now), so the tab's other values keep working and nothing has to move
function vdidAdd(row, signalIndex) {
  const tab = settingsCache.vdiag;
  if (!tab) {
    return;
  }
  const signal = row[2][signalIndex];
  const mod = vdidLibrary.mods[row[0]];
  let slot = -1;
  for (let i = 0; i < tab.values.length && slot < 0; i++) {
    if (!tab.values[i].kind) {
      slot = i;
    }
  }
  if (slot < 0) {
    toast("all " + tab.values.length + " values are in use - set one to Off first");
    return;
  }
  const value = tab.values[slot];
  value.kind = 1;              // UDS DID
  value.id = parseInt(row[1], 16);
  value.name = signal[0].substring(0, 15);
  value.bit = signal[1];       // first bit of the value in the DID's data (0 = the top bit of the byte after the DID)
  value.bits = signal[2];      // how many bits long
  value.signed = 0;
  if (signal[3] & 1) {
    value.signed = 1;
  }
  value.lsb = 0;
  if (signal[3] & 2) {
    value.lsb = 1;             // bytes least significant first
  }
  value.scale = signal[4];
  value.offset = signal[5];
  value.unit = (signal[6] || "").substring(0, 7);
  // the value's own module
  value.tx = parseInt(mod[0], 16);
  value.rx = parseInt(mod[1], 16);
  value.txExt = mod[2];
  value.rxExt = 0;
  if (mod[2]) {
    value.rxExt = 0xF1;        // extended addressing: the module answers with the tester's address (F1) in front
  }
  value.addr = mod[3] || 0;
  value.proto = 1;             // UDS (OBDb lists UDS values only)
  renderVdiag();
  // save it the way an edit on the tab would (app.js's autoSave listens for a change on the Values card)
  const firstRow = document.querySelector(".vd-row");
  if (firstRow) {
    firstRow.dispatchEvent(new Event("change", { bubbles: true }));
  }
  toast("added as value " + (slot + 1) + ": " + value.name + " (" + mod[4] + ")");
}

// sets the card up once app.js has loaded the settings and drawn the Diagnostics tab
let vdidSetupTries = 0;
function vdidSetup() {
  if (!$("vdValuesCard")) {
    return;
  }
  if (!settingsCache || !settingsCache.vdiag) {
    vdidSetupTries++;
    if (vdidSetupTries < 100) {
      setTimeout(vdidSetup, 100);
    }
    return;
  }
  vdidBuildCard();
  $("vdidLib").addEventListener("change", function () { vdidLoadLibrary($("vdidLib").value); });
  $("vdidMod").addEventListener("change", vdidShow);
  $("vdidConfirmed").addEventListener("change", vdidShow);
  let searchTimer = null;
  $("vdidSearch").addEventListener("input", function () {
    clearTimeout(searchTimer);
    searchTimer = setTimeout(vdidShow, 250); // a phone types slowly: wait for a pause
  });
  // the library loads the first time the Diagnostics tab is opened (it's ~240 KB for the VW group)
  document.querySelector('.nav-tab[data-page="vdiag"]').addEventListener("click", vdidTabOpened);
  if ($("vdiag-page").classList.contains("active")) {
    vdidTabOpened();
  }
}

async function vdidTabOpened() {
  if (vdidLibrary || vdidLoading) {
    return;
  }
  if (await vdidLoadIndex()) {
    vdidLoadLibrary($("vdidLib").value);
  }
}
document.addEventListener("DOMContentLoaded", vdidSetup);
