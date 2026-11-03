// can2gauge - Your Car (0.35.0): which car the gauge is in. The gauge reads the VIN where it can (the OBD adapter, the
// CAN broadcast, OBD over its CAN, the K-line's ME7 block 081, src/can2gauge_Vehicle.cpp) and works out the make, the
// VW-group model and the year; the driver confirms it here (or picks the car by hand) and chooses a paint colour for
// the picture. Used twice: First Boot's step 4 (firstboot.js calls carMountEditor) and Settings > Device > Your Car.
// The pictures are data/cars/*.svg (one per VW-group model generation, plus one per body type for every other car),
// put straight into the page so the paint colour (the --paint CSS value) tints them.
// Uses app.js's helpers: $, api, action, toast, esc.

// the makes the VIN table knows (src/can2gauge_Vehicle.cpp's make keys): the VW group first, then A to Z.
// 0.36.1: every make the VIN table can name, so a VIN from any car lands on its make here.
const CAR_MAKES = [["vw", "Volkswagen"], ["audi", "Audi"], ["seat", "SEAT"], ["cupra", "Cupra"], ["skoda", "Skoda"],
  ["porsche", "Porsche"], ["bentley", "Bentley"], ["lamborghini", "Lamborghini"], ["bugatti", "Bugatti"],
  ["acura", "Acura"], ["alfa", "Alfa Romeo"], ["aston", "Aston Martin"], ["bmw", "BMW"], ["buick", "Buick"], ["byd", "BYD"],
  ["cadillac", "Cadillac"], ["chevrolet", "Chevrolet"], ["chrysler", "Chrysler"], ["citroen", "Citroen"], ["dacia", "Dacia"],
  ["daewoo", "Daewoo"], ["daihatsu", "Daihatsu"], ["dodge", "Dodge"], ["ds", "DS"], ["ferrari", "Ferrari"], ["fiat", "Fiat"],
  ["ford", "Ford"], ["geely", "Geely"], ["genesis", "Genesis"], ["gmc", "GMC"], ["greatwall", "Great Wall"], ["honda", "Honda"],
  ["hyundai", "Hyundai"], ["infiniti", "Infiniti"], ["isuzu", "Isuzu"], ["jaguar", "Jaguar"], ["jeep", "Jeep"], ["kia", "Kia"],
  ["lada", "Lada"], ["lancia", "Lancia"], ["landrover", "Land Rover"], ["lexus", "Lexus"], ["lincoln", "Lincoln"],
  ["lotus", "Lotus"], ["maserati", "Maserati"], ["mazda", "Mazda"], ["mclaren", "McLaren"], ["mercedes", "Mercedes-Benz"],
  ["mg", "MG"], ["mini", "MINI"], ["mitsubishi", "Mitsubishi"], ["nissan", "Nissan"], ["opel", "Opel / Vauxhall"],
  ["peugeot", "Peugeot"], ["polestar", "Polestar"], ["pontiac", "Pontiac"], ["ram", "Ram"], ["renault", "Renault"],
  ["rollsroyce", "Rolls-Royce"], ["rover", "Rover"], ["saab", "Saab"], ["saturn", "Saturn"], ["smart", "smart"],
  ["ssangyong", "SsangYong"], ["subaru", "Subaru"], ["suzuki", "Suzuki"], ["tesla", "Tesla"], ["toyota", "Toyota"],
  ["volvo", "Volvo"], ["other", "Other"]];
// body types (the fallback pictures, data/cars/body-<type>.svg)
const CAR_BODIES = [["hatch", "Hatchback"], ["saloon", "Saloon"], ["estate", "Estate"], ["suv", "SUV"], ["coupe", "Coupe"],
  ["convertible", "Convertible"], ["mpv", "MPV"], ["van", "Van"], ["pickup", "Pickup"]];
// paint colours to tap (the picker next to them takes any other)
const CAR_PAINTS = [["#f2f2f2", "White"], ["#151515", "Black"], ["#9aa3ab", "Silver"], ["#5b6168", "Grey"],
  ["#c8102e", "Red"], ["#1f3a93", "Blue"], ["#2a7fd4", "Light blue"], ["#1d5e3a", "Green"], ["#e8b100", "Yellow"],
  ["#e06a12", "Orange"], ["#5a2a82", "Purple"], ["#7a5a3a", "Brown"]];

let carIndex = null;      // /cars/index.json: the model pictures
const carSvgCache = {};   // file -> the SVG's text
let carLast = null;       // the last /api/vehicle

async function carLoadIndex() {
  if (carIndex) {
    return carIndex;
  }
  try {
    const response = await fetch("/cars/index.json");
    carIndex = await response.json();
  } catch (e) {
    carIndex = { bodies: [], models: [] }; // no pictures on this gauge: the form still works
  }
  return carIndex;
}

// the model entry for a VW-group model code ("AD"), or null
function carModelEntry(code) {
  if (!carIndex || !code) {
    return null;
  }
  for (let i = 0; i < carIndex.models.length; i++) {
    if (carIndex.models[i].code === code) {
      return carIndex.models[i];
    }
  }
  return null;
}

// the picture for a car: its model's own drawing, else its body type's
function carPictureFile(choice) {
  const entry = carModelEntry(choice.modelCode);
  if (entry && entry.file) {
    return entry.file;
  }
  let body = choice.body;
  if (!body && entry) {
    body = entry.body;
  }
  return "body-" + (body || "hatch") + ".svg";
}

function carPaintHex(paint) {
  return "#" + Number(paint || 0x8a96a3).toString(16).padStart(6, "0");
}

// draws the car into box (an element), tinted in its paint
async function carDrawPicture(box, choice) {
  await carLoadIndex();
  const file = carPictureFile(choice);
  if (carSvgCache[file] === undefined) {
    try {
      const response = await fetch("/cars/" + file);
      carSvgCache[file] = response.ok ? await response.text() : "";
    } catch (e) {
      carSvgCache[file] = "";
    }
  }
  box.innerHTML = carSvgCache[file];
  box.style.setProperty("--paint", carPaintHex(choice.paint));
}

// the car the form should start from: the confirmed one, else what the VIN said
function carStartingChoice(vehicle) {
  const car = vehicle.car || {};
  const seen = vehicle.detected || {};
  if (car.confirmed) {
    return { vin: car.vin, makeKey: car.makeKey, make: car.make, modelCode: car.modelCode, model: car.model,
      year: car.year, body: car.body, paint: car.paint };
  }
  const entry = carModelEntry(seen.modelCode);
  let body = "";
  if (entry) {
    body = entry.body;
  } else if (seen.makeKey === "bmw" && /^X/.test(seen.model || "")) {
    body = "suv"; // 0.37.8: a BMW X model from the VIN (X5 ...) gets the SUV picture
  }
  return { vin: seen.vin, makeKey: seen.makeKey || "", make: seen.make || "", modelCode: seen.modelCode || "",
    model: seen.model || "", year: seen.year || 0, body: body, paint: 0x8a96a3 };
}

// the editor: picture, VIN line, make / model / year / body / paint, This Is My Car. prefix keeps two copies apart.
function carEditorHtml(prefix) {
  let makes = "";
  for (let i = 0; i < CAR_MAKES.length; i++) {
    makes = makes + '<option value="' + CAR_MAKES[i][0] + '">' + CAR_MAKES[i][1] + "</option>";
  }
  let bodies = "";
  for (let i = 0; i < CAR_BODIES.length; i++) {
    bodies = bodies + '<option value="' + CAR_BODIES[i][0] + '">' + CAR_BODIES[i][1] + "</option>";
  }
  let swatches = "";
  for (let i = 0; i < CAR_PAINTS.length; i++) {
    swatches = swatches + '<button type="button" class="car-swatch" title="' + CAR_PAINTS[i][1] + '" data-paint="' +
      CAR_PAINTS[i][0] + '" style="background:' + CAR_PAINTS[i][0] + '"></button>';
  }
  return '<div class="car-pic" id="' + prefix + 'Pic"></div>' +
    '<div class="mono" id="' + prefix + 'Vin"></div>' +
    '<div class="row-inline" data-nosave="1">' +
    '<div class="form-group"><label>Make</label><select id="' + prefix + 'Make">' + makes + "</select></div>" +
    '<div class="form-group"><label>Model</label><select id="' + prefix + 'Model"></select></div>' +
    "</div>" +
    '<div class="row-inline" data-nosave="1">' +
    '<div class="form-group" id="' + prefix + 'ModelTextBox"><label>Model (type it)</label><input type="text" id="' + prefix + 'ModelText" maxlength="31"></div>' +
    '<div class="form-group narrow"><label>Year</label><input type="number" id="' + prefix + 'Year" min="1980" max="2035"></div>' +
    '<div class="form-group"><label>Body</label><select id="' + prefix + 'Body">' + bodies + "</select></div>" +
    "</div>" +
    '<div class="form-group" data-nosave="1"><label>Paint</label><div class="car-swatches">' + swatches +
    '<input type="color" id="' + prefix + 'Paint" title="Any colour"></div></div>' +
    '<div class="row-inline" data-nosave="1"><button class="btn primary" id="' + prefix + 'Save">This Is My Car</button>' +
    '<button class="btn-secondary" id="' + prefix + 'Detect">Detect Again</button></div>' +
    '<div class="help" id="' + prefix + 'State"></div>';
}

// the model list for the make picked: the VW group's model generations with a picture, then "Other"
function carFillModels(prefix, makeKey, modelCode) {
  const select = $(prefix + "Model");
  select.textContent = "";
  const models = (carIndex && carIndex.models) || [];
  let any = false;
  for (let i = 0; i < models.length; i++) {
    if (models[i].make === makeKey) {
      select.appendChild(new Option(models[i].name + " (" + models[i].code + ")", models[i].code));
      any = true;
    }
  }
  select.appendChild(new Option(any ? "Other (type it)" : "Type it below", ""));
  select.value = modelCode || "";
  if (select.value !== (modelCode || "")) {
    select.value = ""; // a code with no picture: typed
  }
  $(prefix + "ModelTextBox").style.display = select.value ? "none" : "";
}

// the make's name for a make key ("vw" -> "Volkswagen"), "" if the list doesn't have it
function carMakeName(makeKey) {
  for (let i = 0; i < CAR_MAKES.length; i++) {
    if (CAR_MAKES[i][0] === makeKey) {
      return CAR_MAKES[i][1];
    }
  }
  return "";
}

// what the form says now
function carFromForm(prefix, vin) {
  const makeKey = $(prefix + "Make").value;
  const make = carMakeName(makeKey);
  const modelCode = $(prefix + "Model").value;
  let model = $(prefix + "ModelText").value.trim();
  const entry = carModelEntry(modelCode);
  if (entry) {
    model = entry.name;
  }
  return { vin: vin || "", makeKey: makeKey, make: make, modelCode: modelCode, model: model,
    year: Number($(prefix + "Year").value) || 0, body: $(prefix + "Body").value,
    paint: parseInt($(prefix + "Paint").value.substring(1), 16) || 0x8a96a3 };
}

// redraws the picture from the form
function carFormChanged(prefix, vin) {
  carDrawPicture($(prefix + "Pic"), carFromForm(prefix, vin));
}

// sets an editor up in container (First Boot or Settings) and fills it from /api/vehicle
async function carMountEditor(container, prefix) {
  container.innerHTML = carEditorHtml(prefix);
  await carLoadIndex();
  let vin = "";
  $(prefix + "Make").addEventListener("change", function () {
    carFillModels(prefix, $(prefix + "Make").value, "");
    carFormChanged(prefix, vin);
  });
  $(prefix + "Model").addEventListener("change", function () {
    const entry = carModelEntry($(prefix + "Model").value);
    $(prefix + "ModelTextBox").style.display = $(prefix + "Model").value ? "none" : "";
    if (entry) {
      $(prefix + "Body").value = entry.body;
      if (!Number($(prefix + "Year").value)) {
        $(prefix + "Year").value = entry.year;
      }
    }
    carFormChanged(prefix, vin);
  });
  $(prefix + "Body").addEventListener("change", function () { carFormChanged(prefix, vin); });
  $(prefix + "Paint").addEventListener("input", function () { carFormChanged(prefix, vin); });
  container.querySelectorAll(".car-swatch").forEach(function (swatch) {
    swatch.addEventListener("click", function () {
      $(prefix + "Paint").value = swatch.dataset.paint;
      carFormChanged(prefix, vin);
    });
  });
  $(prefix + "Save").addEventListener("click", async function () {
    const choice = carFromForm(prefix, vin);
    if (choice.makeKey === "other" && !choice.model) {
      toast("type the car's make and model in the Model box");
      return;
    }
    await action("vehicleConfirm", choice);
    $(prefix + "State").textContent = "Saved: " + [choice.make, choice.model, choice.year || ""].filter(Boolean).join(" ") + ".";
    carRefreshAll();
  });
  $(prefix + "Detect").addEventListener("click", async function () {
    $(prefix + "State").textContent = "Reading the VIN...";
    await action("vehicleDetect");
    carWatchDetect(prefix);
  });
  const vehicle = await carFetch();
  vin = carFillEditor(prefix, vehicle);
}

// fills an editor from /api/vehicle; returns the VIN it shows
function carFillEditor(prefix, vehicle) {
  if (!vehicle || !$(prefix + "Make")) {
    return "";
  }
  const choice = carStartingChoice(vehicle);
  const seen = vehicle.detected || {};
  let vinLine = "No VIN read yet: pick the car below.";
  if (seen.vin) {
    vinLine = "VIN " + seen.vin + "  (from the " + seen.source + ")";
    if (!seen.make) {
      vinLine = vinLine + "\nThe make isn't in the gauge's list: pick it below.";
    }
  }
  if (vehicle.car && vehicle.car.confirmed === 2) {
    // 0.36.2: set by itself from the first OBD scan's VIN
    vinLine = vinLine + "\nMy Car (set from the VIN): " + [vehicle.car.label, vehicle.car.year || ""].filter(Boolean).join(" ") +
      ". Change anything that's wrong and press This Is My Car.";
  } else if (vehicle.car && vehicle.car.confirmed) {
    vinLine = vinLine + "\nConfirmed: " + [vehicle.car.make, vehicle.car.model, vehicle.car.year || ""].filter(Boolean).join(" ");
  }
  $(prefix + "Vin").textContent = vinLine;
  let makeKey = choice.makeKey;
  if (!makeKey && choice.make) {
    makeKey = "other";
  }
  // 0.36.1: a make key this list doesn't have yet (a newer gauge firmware) still shows, under the VIN's own name
  if (makeKey && !carMakeName(makeKey)) {
    CAR_MAKES.splice(CAR_MAKES.length - 1, 0, [makeKey, choice.make || makeKey]);
    $(prefix + "Make").insertBefore(new Option(choice.make || makeKey, makeKey), $(prefix + "Make").lastChild);
  }
  $(prefix + "Make").value = makeKey || "vw";
  carFillModels(prefix, $(prefix + "Make").value, choice.modelCode);
  $(prefix + "ModelText").value = carModelEntry(choice.modelCode) ? "" : (choice.model || "");
  $(prefix + "Year").value = choice.year || "";
  $(prefix + "Body").value = choice.body || "hatch";
  $(prefix + "Paint").value = carPaintHex(choice.paint);
  carDrawPicture($(prefix + "Pic"), carFromForm(prefix, choice.vin));
  return choice.vin || "";
}

async function carFetch() {
  try {
    carLast = await api("/api/vehicle");
  } catch (e) {
    carLast = null;
  }
  carNameBootOption(carLast);
  carCheckBootLogo(carLast);
  return carLast;
}

// 0.36.2: the boot logo list's "My Car" entry says which car it is ("My Car (SEAT Ibiza IV)")
function carNameBootOption(vehicle) {
  const select = $("boot.logo");
  if (!select) {
    return;
  }
  for (let i = 0; i < select.options.length; i++) {
    if (select.options[i].value === "7") {
      let text = "My Car";
      if (vehicle && vehicle.car && vehicle.car.label) {
        text = "My Car (" + vehicle.car.label + ")";
      }
      select.options[i].textContent = text;
    }
  }
}

// 0.36.2: My Car's boot picture (boot logo 7). The gauge can't draw the SVG pictures itself, so the browser draws the
// car in its paint on black, packs it the same way as the make logos (encodeC2GL in app.js) and uploads it as
// /logos/car.bin. The gauge deletes that file whenever My Car changes (set from a VIN, This Is My Car, Forget), so a
// missing file means "make a new one": that happens here, whenever the web UI is open.
let carLogoBusy = false; // a picture is being made / uploaded
async function carCheckBootLogo(vehicle) {
  if (!vehicle || !vehicle.car || !vehicle.car.confirmed || vehicle.carLogo || carLogoBusy) {
    return;
  }
  if (typeof encodeC2GL !== "function") {
    return; // app.js isn't loaded (shouldn't happen)
  }
  carLogoBusy = true;
  try {
    await carMakeBootLogo(vehicle.car);
    vehicle.carLogo = true;
  } catch (e) {
    console.log("My Car boot picture:", e);
  }
  carLogoBusy = false;
}

async function carMakeBootLogo(car) {
  await carLoadIndex();
  const file = carPictureFile(car);
  const response = await fetch("/cars/" + file);
  let svg = await response.text();
  // the picture's paint: the SVG's --paint value written in as the colour itself (a CSS value doesn't reach an image)
  svg = svg.replace(/var\(--paint[^)]*\)/g, carPaintHex(car.paint));
  const picture = new Image();
  await new Promise(function (done, failed) {
    picture.onload = done;
    picture.onerror = failed;
    picture.src = "data:image/svg+xml;charset=utf-8," + encodeURIComponent(svg);
  });
  // 400 x 240 (the drawings' own size): its corners stay inside the 480 px round screen; the gauge centres it
  const canvas = document.createElement("canvas");
  canvas.width = 400;
  canvas.height = 240;
  const drawing = canvas.getContext("2d");
  drawing.fillStyle = "#000";
  drawing.fillRect(0, 0, canvas.width, canvas.height);
  drawing.drawImage(picture, 0, 0, canvas.width, canvas.height);
  const bytes = encodeC2GL(canvas);
  const form = new FormData();
  form.append("file", new Blob([bytes], { type: "application/octet-stream" }), "car.bin");
  const upload = await fetch("/api/files/upload?dir=" + encodeURIComponent("/logos"), { method: "POST", body: form });
  if (!upload.ok) {
    throw new Error("upload failed (" + upload.status + ")");
  }
}

// after Detect Again: watch for the CAN read to finish (the adapter and K-line report on their own)
async function carWatchDetect(prefix) {
  for (let i = 0; i < 20; i++) {
    await new Promise(function (done) { setTimeout(done, 500); });
    const vehicle = await carFetch();
    if (vehicle && !vehicle.canReading) {
      break;
    }
  }
  carFillEditor(prefix, carLast);
  const seen = (carLast && carLast.detected) || {};
  $(prefix + "State").textContent = seen.vin ? "VIN read from the " + seen.source + "." :
    "No VIN on the gauge's CAN. The OBD adapter and the K-line read it when they connect.";
}

// both editors show the latest
async function carRefreshAll() {
  const vehicle = await carFetch();
  if ($("carSetMake")) {
    carFillEditor("carSet", vehicle);
  }
}

// the editor's look (both copies: First Boot and Settings)
function carAddStyle() {
  if ($("carStyle")) {
    return;
  }
  const style = document.createElement("style");
  style.id = "carStyle";
  style.textContent = ".car-pic{max-width:420px;margin:0 auto 8px}.car-pic svg{width:100%;height:auto;display:block}" +
    ".car-swatches{display:flex;flex-wrap:wrap;gap:8px;align-items:center}" +
    ".car-swatch{width:30px;height:30px;min-width:0;padding:0;border-radius:50%;border:2px solid var(--border,#2a3340);cursor:pointer}" +
    ".car-swatches input[type=color]{width:40px;height:32px;padding:0;border:none;background:none}";
  document.head.appendChild(style);
}

// Settings > Device: the Your Car card, put in front of the Device card
let carSetupTries = 0;
function carSetup() {
  carAddStyle();
  if (!document.querySelector('#settings-page .card[data-sec="device"]')) {
    return;
  }
  if (typeof settingsCache === "undefined" || !settingsCache) {
    carSetupTries++;
    if (carSetupTries < 100) {
      setTimeout(carSetup, 100);
    }
    return;
  }
  const device = document.querySelector('#settings-page .card[data-sec="device"]');
  const card = document.createElement("section");
  card.className = "card";
  card.dataset.sec = "device";
  card.id = "carCard";
  if (device.classList.contains("sec-hidden")) {
    card.classList.add("sec-hidden");
  }
  card.innerHTML = '<h2>Your Car</h2><div class="help" style="margin-top:0">The car this gauge is in. The gauge reads the ' +
    "VIN where it can and fills this in; check it, pick the paint, and press This Is My Car. It decides Match Car " +
    '(the theme and boot logo) and the DID Library\'s car group.</div><div id="carSetBox"></div>';
  device.parentElement.insertBefore(card, device);
  carMountEditor($("carSetBox"), "carSet");
}
document.addEventListener("DOMContentLoaded", carSetup);
