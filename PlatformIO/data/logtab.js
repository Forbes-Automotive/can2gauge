// can2gauge - the Log tab (0.16.0): the data logger's channels and controls, its recording drawn by the shared log
// viewer (logview.js) and saved to the phone as CSV. The recording itself is in the gauge's PSRAM; this page copies it
// across in pieces (/api/log/data) and keeps its own copy while the tab is open.
// Uses app.js's helpers: $, api, action, toast, sigOptions, autoSave, settingsCache, catalogue.

const LOG_RATES = [1, 2, 5, 10, 20, 50];   // samples a second the gauge offers
const LOG_PIECES_PER_POLL = 12;            // most /api/log/data requests in one poll (keeps the page responsive)
const LOG_KEEP_MAX = 400000;               // samples this page keeps at most (older ones are dropped)

let logStatus = null;                      // last /api/log
let logSession = -1;                       // the recording this page's copy belongs to
let logNext = 0;                           // number of the next sample to fetch
let logCopy = { names: [], units: [], time: [], values: [] }; // this page's copy, in the viewer's format
let logFetching = false;                   // a poll is fetching pieces now
let logDrawnCount = -1;                    // samples the viewer last drew (redraw only when there are new ones)

// the channel pickers, rate and Auto Start, filled from the settings
function logTabRender() {
  const list = $("logChannels");
  list.textContent = "";
  for (let channel = 0; channel < 16; channel++) {
    const group = document.createElement("div");
    group.className = "form-group";
    const label = document.createElement("label");
    label.textContent = "Channel " + (channel + 1);
    const select = document.createElement("select");
    select.id = "logSig" + channel;
    let current = "none";
    if (settingsCache.log && settingsCache.log.signals) {
      current = settingsCache.log.signals[channel] || "none";
    }
    sigOptions(select, current, true);
    group.appendChild(label);
    group.appendChild(select);
    list.appendChild(group);
  }
  const rate = $("log.rateHz");
  rate.textContent = "";
  for (let i = 0; i < LOG_RATES.length; i++) {
    rate.appendChild(new Option(LOG_RATES[i] + " a second", LOG_RATES[i]));
  }
  if (settingsCache.log) {
    rate.value = settingsCache.log.rateHz;
    $("log.autoStart").value = settingsCache.log.autoStart;
    $("log.manualChannels").value = settingsCache.log.manualChannels || 0;
  }
  logChannelModeShow();
}

// Active Gauges hides the channel pickers (the logger uses the gauge pages); Chosen Below shows them
function logChannelModeShow() {
  const manual = $("log.manualChannels").value === "1";
  $("logChannelPick").style.display = manual ? "" : "none";
  $("logGaugeHelp").style.display = manual ? "none" : "";
}

// the settings JSON for the logger card
function logTabCollect() {
  const signals = [];
  for (let channel = 0; channel < 16; channel++) {
    signals.push($("logSig" + channel).value);
  }
  return { log: { signals: signals, rateHz: Number($("log.rateHz").value), autoStart: Number($("log.autoStart").value),
                  manualChannels: Number($("log.manualChannels").value) } };
}

// "1 h 40 min" / "12 min" / "45 s"
function logDurationText(seconds) {
  if (seconds >= 3600) {
    return Math.floor(seconds / 3600) + " h " + Math.floor((seconds % 3600) / 60) + " min";
  }
  if (seconds >= 60) {
    return Math.floor(seconds / 60) + " min";
  }
  return Math.floor(seconds) + " s";
}

// the "● Recording: On" line under the buttons (0.17.1: as few words as possible; how much it holds is on the memory
// line below). The dot is green while it records.
function logTabState(s) {
  let text = "Off";
  if (s.error) {
    text = titleCase("failed: " + s.error);
  } else if (s.running && s.paused) {
    text = "Paused (Ignition Off)";
  } else if (s.running) {
    text = "On";
  } else if (s.total > 0) {
    text = "Stopped";
  }
  $("logState").textContent = text;
  setDot($("logDot"), s.running && !s.paused);
}

// how many channels the next Start records: the values the active gauge pages show, or the ones picked below
function logPickedChannels() {
  if ($("log.manualChannels") && $("log.manualChannels").value === "0") {
    return logGaugeChannelCount();
  }
  let count = 0;
  for (let channel = 0; channel < 16; channel++) {
    const select = $("logSig" + channel);
    if (select && select.value && select.value !== "none") {
      count++;
    }
  }
  return count;
}

// how many channels the active gauge pages come to (for the room estimate before recording). Mirrors the firmware's
// gaugeChannels(): each enabled page's main value, the second value on a Dual page, the three centre values on a Quad /
// Custom page, in gauge order; a gauge signal shared by two pages is logged once, a page's own source is its own channel.
function logGaugeChannelCount() {
  if (!settingsCache || !settingsCache.pages || !catalogue) {
    return 0;
  }
  const dual = catalogue.styles.indexOf("Dual");
  const quad = catalogue.styles.indexOf("Quad");
  const triple = catalogue.styles.indexOf("Triple"); // 0.31.0
  const custom = catalogue.styles.indexOf("Custom");
  const seen = [];   // plain gauge-signal keys already counted
  let count = 0;
  settingsCache.pages.forEach(page => {
    if (!page.enabled) {
      return;
    }
    const shown = [page.signal];
    if (page.style === dual) {
      shown.push(page.signal2);
    }
    if (page.style === quad || page.style === triple || page.style === custom) {
      shown.push(...(page.slots || []));
    }
    shown.forEach(value => {
      if (count >= 16 || !value || value === "none") {
        return;
      }
      if (value.indexOf(":") >= 0) {   // a page's own source (me7:, pid:, dbc: ...): its own channel
        count++;
        return;
      }
      if (seen.indexOf(value) >= 0) {  // a gauge signal already being logged from an earlier page
        return;
      }
      seen.push(value);
      count++;
    });
  });
  return count;
}

// 0.17.0: the memory bar and the line under it - how much room there is, before Start as well as while recording
// (adc2can's Log tab does the same). One sample = 4 bytes of time + 4 bytes per value.
function logTabMemory(s) {
  let roomKb = s.memoryKb;
  if (roomKb === undefined) {
    roomKb = s.bufferKb; // older firmware: only known once recording
  }
  let fill = 0;
  let text = "";
  if (!roomKb) {
    text = "No free memory";
  } else if (s.capacity > 0 && s.rateHz > 0) {
    // a recording holds the memory: how full it is, and how long until the oldest is written over
    let held = s.total;
    if (held > s.capacity) {
      held = s.capacity;
    }
    fill = held / s.capacity;
    text = logDurationText(held / s.rateHz) + " / " + logDurationText(s.capacity / s.rateHz) + " used (" + s.channels.length +
      " values at " + s.rateHz + "/s, " + roomKb + " KB)";
    if (s.total >= s.capacity) {
      text = text + " - full, oldest overwritten";
    }
  } else {
    // nothing recorded: the room a Start would get with the channels and rate picked below
    const channels = logPickedChannels();
    const rate = Number($("log.rateHz").value) || 10;
    if (channels === 0) {
      const gauges = $("log.manualChannels").value === "0";
      text = "Empty (" + roomKb + " KB) - " + (gauges ? "switch a gauge page on" : "pick values below");
    } else {
      const samples = (roomKb * 1024) / (4 + 4 * channels);
      text = "Empty: room for " + logDurationText(samples / rate) + " (" + channels + " values at " + rate + "/s, " + roomKb + " KB)";
    }
  }
  $("logMemFill").style.width = (fill * 100).toFixed(1) + "%";
  $("logMemInfo").textContent = titleCase(text);
}

// the memory line again with the last status (a channel or the rate was just changed)
function logTabMemoryAgain() {
  if (logStatus) {
    logTabMemory(logStatus);
  }
}

// starts this page's copy again (a new recording on the gauge, or Clear)
function logCopyReset(s) {
  logSession = s.session;
  logNext = s.oldest;
  logCopy = { names: [], units: [], time: [], values: [] };
  for (let i = 0; i < s.channels.length; i++) {
    logCopy.names.push(s.channels[i].name);
    logCopy.units.push(s.channels[i].unit);
    logCopy.values.push([]);
  }
  logDrawnCount = -1;
}

// one piece of the recording (binary, can2gauge_Logger.h) added to this page's copy. Returns the samples it had.
function logAddPiece(buffer, s) {
  const view = new DataView(buffer);
  if (buffer.byteLength < 16) {
    return 0;
  }
  const session = view.getUint32(0, true); // little-endian, as the ESP32 stores numbers
  const first = view.getUint32(4, true);
  const count = view.getUint32(8, true);
  const recordBytes = view.getUint16(12, true);
  const channels = view.getUint16(14, true);
  if (session !== logSession || channels !== logCopy.names.length) {
    return 0; // a new recording started meanwhile: the next poll starts over
  }
  for (let i = 0; i < count; i++) {
    const at = 16 + i * recordBytes;
    if (at + recordBytes > buffer.byteLength) {
      break;
    }
    const timeMs = view.getUint32(at, true);
    logCopy.time.push((timeMs - s.startMs) / 1000); // seconds from the start
    for (let channel = 0; channel < channels; channel++) {
      const value = view.getFloat32(at + 4 + channel * 4, true);
      if (isNaN(value)) {
        logCopy.values[channel].push(null); // the value had gone stale: a gap in the graph
      } else {
        logCopy.values[channel].push(value);
      }
    }
  }
  logNext = first + count;
  // keep the page's copy a sensible size
  const extra = logCopy.time.length - LOG_KEEP_MAX;
  if (extra > 0) {
    logCopy.time.splice(0, extra);
    for (let channel = 0; channel < logCopy.values.length; channel++) {
      logCopy.values[channel].splice(0, extra);
    }
  }
  return count;
}

// fetches what's new on the gauge, at most maxPieces requests; true when it has caught up
async function logCatchUp(s, maxPieces) {
  for (let piece = 0; piece < maxPieces; piece++) {
    if (logNext < s.oldest) {
      logNext = s.oldest; // written over before we got to it
    }
    if (logNext >= s.total) {
      return true;
    }
    const answer = await fetch("/api/log/data?from=" + logNext + "&max=5000");
    const buffer = await answer.arrayBuffer();
    if (logAddPiece(buffer, s) === 0) {
      return true; // nothing came (a new recording, or no memory): try again next poll
    }
    $("logFetchState").textContent = "Copying from the gauge: " + logCopy.time.length + " samples";
  }
  return false;
}

// every second while the Log tab is open: the status, the new samples, the graphs
async function logTabPoll() {
  if (!$("log-page").classList.contains("active") || logFetching || !settingsCache) {
    return;
  }
  logFetching = true;
  try {
    const s = await api("/api/log");
    logStatus = s;
    logTabState(s);
    logTabMemory(s);
    if (s.session !== logSession) {
      // a new recording (Start, or Clear): throw the old graphs away so they're drawn again with the new channels.
      // (The viewer only redraws by itself when the NUMBER of channels changes, so swapping Channel 1 from Boost to
      // an ADC kept the old graphs until Clear.)
      logViewClear($("logGraphs"));
      logCopyReset(s);
    }
    const caughtUp = await logCatchUp(s, LOG_PIECES_PER_POLL);
    if (caughtUp) {
      $("logFetchState").textContent = "";
    }
    if (logCopy.time.length !== logDrawnCount) {
      logDrawnCount = logCopy.time.length;
      logViewUpdate($("logGraphs"), logCopy);
    }
  } catch (e) {
    // no connection this time: the next poll tries again
  }
  logFetching = false;
}

async function logTabStart() {
  if (logStatus && logStatus.total > 0 && !confirm("Start a new recording? The one on the gauge now is dropped.")) {
    return;
  }
  await action("logStart");
  logTabPoll();
}

async function logTabStop() {
  await action("logStop");
  logTabPoll();
}

async function logTabClear() {
  if (!confirm("Clear the recording and give its memory back to the gauge?")) {
    return;
  }
  await action("logClear");
  logViewClear($("logGraphs"));
  logTabPoll();
}

// the whole recording to the phone as a CSV file
async function logTabDownload() {
  if (!logStatus) {
    return;
  }
  toast("Copying the recording from the gauge...");
  let caughtUp = false;
  for (let tries = 0; tries < 200 && !caughtUp; tries++) {
    const s = await api("/api/log");
    if (s.session !== logSession) {
      logCopyReset(s);
    }
    caughtUp = await logCatchUp(s, LOG_PIECES_PER_POLL);
  }
  $("logFetchState").textContent = "";
  if (!logCopy.time.length) {
    toast("Nothing recorded yet");
    return;
  }
  logViewDownload(logViewCsvText(logCopy), logViewFileName("can2gauge", "csv"), "text/csv");
}

// the tab just opened: the graphs are drawn at the width they now have
function logTabShown() {
  logDrawnCount = -1;
  logTabPoll();
  dragRender(); // 0.38.0: the drag times, fresh each time the tab opens
  setTimeout(logViewResize, 50);
}

// ---------------------------------------------------------------------------
// 0.38.0: the drag timer's scoreboard (GET /api/drag; actions dragDelete / dragClear). Kind 0 = 0-100 km/h, 1 = 0-60 mph.
// ---------------------------------------------------------------------------
const DRAG_TITLES = ["0-100 km/h", "0-60 mph"];

// "4.87" from 4870 ms
function dragTime(ms) {
  return (Math.floor(ms / 10) / 100).toFixed(2);
}

async function dragRender() {
  const box = $("dragBoards");
  if (!box) {
    return;
  }
  let board = null;
  try {
    board = await api("/api/drag");
  } catch (e) {
    box.textContent = "The gauge didn't send its drag times (older firmware?)";
    return;
  }
  box.textContent = "";
  // the run the screen times now first
  const order = [board.now || 0, 1 - (board.now || 0)];
  order.forEach(function (kind) {
    const data = board.kinds[kind] || { runs: 0, times: [] };
    const title = document.createElement("label");
    let runsText = data.runs + " runs";
    if (data.runs === 1) {
      runsText = "1 run";
    }
    title.textContent = DRAG_TITLES[kind] + "  (" + runsText + ")";
    box.appendChild(title);
    if (!data.times.length) {
      const none = document.createElement("div");
      none.className = "help";
      none.textContent = "No times yet.";
      box.appendChild(none);
      return;
    }
    data.times.forEach(function (time, place) {
      const row = document.createElement("div");
      row.className = "row-inline";
      row.style.marginBottom = "6px";
      const text = document.createElement("div");
      text.className = "state-line";
      text.style.flex = "1 1 160px";
      text.innerHTML = "<b class=\"sig-live\">" + (place + 1) + ".  " + dragTime(time.ms) + " s</b>  run " + time.run;
      const remove = document.createElement("button");
      remove.className = "btn-secondary";
      remove.style.flex = "0 0 110px"; // a small button beside the time, not half the card
      remove.textContent = "Delete";
      remove.addEventListener("click", async function () {
        await action("dragDelete", { kind: kind, place: place });
        dragRender();
      });
      row.appendChild(text);
      row.appendChild(remove);
      box.appendChild(row);
    });
    const clearRow = document.createElement("div"); // its own line, so the next heading starts under it
    clearRow.className = "row-inline";
    const clear = document.createElement("button");
    clear.className = "btn-secondary";
    clear.textContent = "Clear All " + DRAG_TITLES[kind];
    clear.addEventListener("click", async function () {
      if (!confirm("Clear every " + DRAG_TITLES[kind] + " time?")) {
        return;
      }
      await action("dragClear", { kind: kind });
      dragRender();
    });
    clearRow.appendChild(clear);
    box.appendChild(clearRow);
  });
}

// sets the tab up once app.js has loaded the settings and the signal list (until then it looks again every 100 ms)
let logSetupTries = 0;
function logTabSetup() {
  if (!$("log-page")) {
    return;
  }
  if (!settingsCache || !catalogue) {
    logSetupTries++;
    if (logSetupTries < 100) {
      setTimeout(logTabSetup, 100);
    }
    return;
  }
  logTabRender();
  autoSave($("logSetupCard"), logTabCollect, "logger saved - applies at the next Start");
  $("logSetupCard").addEventListener("change", () => { logChannelModeShow(); logTabMemoryAgain(); }); // the pickers show/hide and the room follows the channels / rate picked
  $("logStartBtn").addEventListener("click", logTabStart);
  $("logStopBtn").addEventListener("click", logTabStop);
  $("logClearBtn").addEventListener("click", logTabClear);
  $("logCsvBtn").addEventListener("click", logTabDownload);
  document.querySelector('.nav-tab[data-page="log"]').addEventListener("click", logTabShown);
  setInterval(logTabPoll, 1000);
}
document.addEventListener("DOMContentLoaded", logTabSetup);
