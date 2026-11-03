// logview.js - Forbes Automotive shared log viewer (graphs + CSV export), 2026
// Master copy: _SharedTheme/logview.js (+ logview.css). Each product copies both into its data/ folder, with uPlot
// (uPlot.iife.min.js + uPlot.min.css, MIT licence, github.com/leeoniya/uPlot) which draws the graphs.
// See _SharedTheme/README_LOGVIEW.md for how a product uses it.
//
// A log handed to the viewer is a plain object:
//   { names: ["RPM", "Boost"], units: ["rpm", "bar"], time: [0, 0.1, 0.2 ...] (seconds from the start),
//     values: [[800, 820, ...], [0.1, 0.12, ...]] }   one array per channel, null = no value at that moment (a gap)
// The viewer draws one small graph per channel, stacked, all on the same time line: the cursor and the zoom move
// together on every graph, and each graph's heading shows its value at the cursor and its min / average / max for
// the stretch on screen.

const LOGVIEW_CHART_HEIGHT = 120; // px, each channel's graph
const logViewStates = {};         // one per viewer on the page, by its host element's id
let logViewSyncing = false;       // true while one graph's zoom is copied to the others (stops it bouncing back)

// a colour from the page's CSS (style.css :root), or the fallback when the page doesn't set it
function logViewColour(name, fallback) {
  const value = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  if (value) {
    return value;
  }
  return fallback;
}

// seconds -> "1:05" / "1:02:05" (and tenths when zoomed right in)
function logViewTimeText(seconds, withTenths) {
  if (seconds === null || seconds === undefined || isNaN(seconds)) {
    return "--";
  }
  let rest = Math.max(0, seconds);
  const hours = Math.floor(rest / 3600);
  rest = rest - hours * 3600;
  const minutes = Math.floor(rest / 60);
  rest = rest - minutes * 60;
  let secondsText = String(Math.floor(rest)).padStart(2, "0");
  if (withTenths) {
    secondsText = rest.toFixed(1).padStart(4, "0");
  }
  if (hours > 0) {
    return hours + ":" + String(minutes).padStart(2, "0") + ":" + secondsText;
  }
  return minutes + ":" + secondsText;
}

// a value with sensible decimals: 4520, 98.6, 1.23
function logViewValueText(value) {
  if (value === null || value === undefined || isNaN(value)) {
    return "--";
  }
  if (Number.isInteger(value)) {
    return String(value); // a whole number: 0, 50, 5000
  }
  const size = Math.abs(value);
  if (size >= 100) {
    return value.toFixed(0);
  }
  if (size >= 10) {
    return value.toFixed(1);
  }
  return value.toFixed(2);
}

// how units are shown ("C" -> "°C", as on the gauge)
function logViewUnitText(unit) {
  if (unit === "C") {
    return "°C";
  }
  if (unit === "F") {
    return "°F";
  }
  if (unit === "deg") {
    return "°";
  }
  return unit || "";
}

// the time axis labels uPlot asks for (splits = the times it put ticks at)
function logViewAxisTimes(chart, splits) {
  const range = chart.scales.x.max - chart.scales.x.min;
  const labels = [];
  for (let i = 0; i < splits.length; i++) {
    labels.push(logViewTimeText(splits[i], range < 20));
  }
  return labels;
}

// the value axis labels
function logViewAxisValues(chart, splits) {
  const labels = [];
  for (let i = 0; i < splits.length; i++) {
    labels.push(logViewValueText(splits[i]));
  }
  return labels;
}

// min / average / max of one channel over what's on screen, into its heading
function logViewStats(state, channel) {
  const chart = state.charts[channel];
  const times = state.log.time;
  const values = state.log.values[channel];
  const from = chart.scales.x.min;
  const to = chart.scales.x.max;
  let low = null;
  let high = null;
  let sum = 0;
  let count = 0;
  for (let i = 0; i < times.length; i++) {
    if (times[i] < from || times[i] > to) {
      continue;
    }
    const value = values[i];
    if (value === null || isNaN(value)) {
      continue;
    }
    if (low === null || value < low) {
      low = value;
    }
    if (high === null || value > high) {
      high = value;
    }
    sum = sum + value;
    count++;
  }
  let text = "no data on screen";
  if (count > 0) {
    text = "min " + logViewValueText(low) + "  avg " + logViewValueText(sum / count) + "  max " + logViewValueText(high);
  }
  state.heads[channel].stats.textContent = text;
}

// uPlot calls this when the cursor moves on a graph (the others follow it): that graph's value at the cursor
function logViewOnCursor(chart) {
  const state = logViewStates[chart.logViewHost];
  if (!state) {
    return;
  }
  const channel = chart.logViewChannel;
  const index = chart.cursor.idx;
  let valueText = "";
  if (index !== null && index !== undefined) {
    valueText = logViewValueText(state.log.values[channel][index]) + " " + logViewUnitText(state.log.units[channel]);
    if (channel === 0) {
      state.cursorTime.textContent = "Cursor " + logViewTimeText(state.log.time[index], true);
    }
  }
  state.heads[channel].value.textContent = valueText;
}

// uPlot calls this when a graph's zoom changes: copy it to the others and refresh the min / avg / max
function logViewOnScale(chart, scaleKey) {
  if (scaleKey !== "x") {
    return;
  }
  const state = logViewStates[chart.logViewHost];
  if (!state) {
    return;
  }
  if (!logViewSyncing) {
    logViewSyncing = true;
    for (let i = 0; i < state.charts.length; i++) {
      if (state.charts[i] !== chart) {
        state.charts[i].setScale("x", { min: chart.scales.x.min, max: chart.scales.x.max });
      }
    }
    logViewSyncing = false;
    // following the live end: only while the right-hand edge is at the newest sample
    const newest = state.log.time[state.log.time.length - 1];
    state.follow = newest === undefined || chart.scales.x.max >= newest - 0.001;
    state.zoomed = chart.scales.x.min > state.log.time[0] + 0.001 || !state.follow;
  }
  logViewStats(state, chart.logViewChannel);
}

// the uPlot settings for one channel's graph
function logViewOptions(hostId, channel, width) {
  const textColour = logViewColour("--muted", "#7d8ea0");
  const gridColour = logViewColour("--line", "#24303b");
  const lineColour = logViewColour("--accent", "#24d1c4");
  return {
    width: width,
    height: LOGVIEW_CHART_HEIGHT,
    legend: { show: false }, // each graph's own heading shows the name and the value
    cursor: {
      sync: { key: "logview-" + hostId }, // the cursor moves on every graph together
      drag: { x: true, y: false },        // drag across a graph to zoom in (mouse)
      points: { size: 8 },
    },
    scales: { x: { time: false } },     // the time line is seconds, not a calendar
    axes: [
      { stroke: textColour, grid: { stroke: gridColour, width: 1 }, ticks: { stroke: gridColour }, values: logViewAxisTimes },
      { stroke: textColour, grid: { stroke: gridColour, width: 1 }, ticks: { stroke: gridColour }, values: logViewAxisValues, size: 48 },
    ],
    series: [
      {},
      { stroke: lineColour, width: 2, spanGaps: false }, // a gap where the value went stale
    ],
    hooks: { setCursor: [logViewOnCursor], setScale: [logViewOnScale] },
  };
}

// the controls row above the graphs: zoom, pan, all, and the time at the cursor
function logViewControls(state) {
  const row = document.createElement("div");
  row.className = "lv-controls";
  const buttons = [["Zoom In", logViewZoomIn], ["Zoom Out", logViewZoomOut], ["◀", logViewPanBack], ["▶", logViewPanOn], ["All", logViewShowAll]];
  for (let i = 0; i < buttons.length; i++) {
    const button = document.createElement("button");
    button.className = "btn-secondary";
    button.textContent = buttons[i][0];
    button.dataset.logviewHost = state.hostId;
    button.addEventListener("click", buttons[i][1]);
    row.appendChild(button);
  }
  state.cursorTime = document.createElement("span");
  state.cursorTime.className = "lv-cursor";
  row.appendChild(state.cursorTime);
  return row;
}

// draws a log into host (an empty element with an id), replacing what was there
function logViewDraw(host, log) {
  logViewClear(host);
  const state = { hostId: host.id, log: log, charts: [], heads: [], follow: true, zoomed: false, cursorTime: null };
  logViewStates[host.id] = state;
  if (!log.time.length) {
    const empty = document.createElement("div");
    empty.className = "help";
    empty.textContent = "Nothing recorded yet.";
    host.appendChild(empty);
    return;
  }
  host.appendChild(logViewControls(state));
  const width = Math.max(200, host.clientWidth);
  for (let channel = 0; channel < log.names.length; channel++) {
    const box = document.createElement("div");
    box.className = "lv-chart";
    const head = document.createElement("div");
    head.className = "lv-head";
    const name = document.createElement("span");
    name.className = "lv-name";
    name.textContent = log.names[channel];
    if (log.units[channel]) {
      name.textContent = log.names[channel] + " (" + logViewUnitText(log.units[channel]) + ")";
    }
    const value = document.createElement("span");
    value.className = "lv-value";
    const stats = document.createElement("div");
    stats.className = "lv-stats";
    head.appendChild(name);
    head.appendChild(value);
    box.appendChild(head);
    box.appendChild(stats);
    host.appendChild(box);
    state.heads.push({ value: value, stats: stats });
    const chart = new uPlot(logViewOptions(host.id, channel, width), [log.time, log.values[channel]], box);
    chart.logViewHost = host.id;     // which viewer this graph belongs to (read back in the hooks)
    chart.logViewChannel = channel;  // which channel it shows
    state.charts.push(chart);
    logViewStats(state, channel);
  }
}

// new data for a viewer already drawn (live recording): keeps the zoom, and keeps following the newest samples
// when the right-hand edge was at the end
function logViewUpdate(host, log) {
  const state = logViewStates[host.id];
  if (!state || state.charts.length !== log.names.length || !state.charts.length) {
    logViewDraw(host, log); // first draw, or the channels changed
    return;
  }
  const oldMin = state.charts[0].scales.x.min;
  const oldMax = state.charts[0].scales.x.max;
  state.log = log;
  const newest = log.time[log.time.length - 1];
  for (let i = 0; i < state.charts.length; i++) {
    state.charts[i].setData([log.time, log.values[i]], !state.zoomed); // not zoomed: fit the whole log
  }
  if (state.zoomed) {
    let min = oldMin;
    let max = oldMax;
    if (state.follow) {
      max = newest; // same width, moved to the newest samples
      min = newest - (oldMax - oldMin);
    }
    state.charts[0].setScale("x", { min: min, max: max }); // the others follow (logViewOnScale)
  }
}

// removes a viewer's graphs
function logViewClear(host) {
  const state = logViewStates[host.id];
  if (state) {
    for (let i = 0; i < state.charts.length; i++) {
      state.charts[i].destroy();
    }
    delete logViewStates[host.id];
  }
  host.textContent = "";
}

// shows the stretch from min to max seconds on every graph of a viewer
function logViewSetRange(state, min, max) {
  const first = state.log.time[0];
  const last = state.log.time[state.log.time.length - 1];
  const width = max - min;
  if (min < first) {
    min = first;
    max = first + width;
  }
  if (max > last) {
    max = last;
    min = Math.max(first, last - width);
  }
  state.charts[0].setScale("x", { min: min, max: max });
}

// the viewer a control button belongs to
function logViewStateOf(event) {
  return logViewStates[event.currentTarget.dataset.logviewHost];
}

function logViewZoomIn(event) {
  const state = logViewStateOf(event);
  if (!state || !state.charts.length) {
    return;
  }
  const scale = state.charts[0].scales.x;
  const width = (scale.max - scale.min) / 2;
  let centre = (scale.min + scale.max) / 2;
  if (state.follow) {
    centre = scale.max - width / 2; // following the newest samples: zoom in on the end
  }
  logViewSetRange(state, centre - width / 2, centre + width / 2);
}

function logViewZoomOut(event) {
  const state = logViewStateOf(event);
  if (!state || !state.charts.length) {
    return;
  }
  const scale = state.charts[0].scales.x;
  const width = (scale.max - scale.min) * 2;
  const centre = (scale.min + scale.max) / 2;
  logViewSetRange(state, centre - width / 2, centre + width / 2);
}

function logViewPanBack(event) {
  const state = logViewStateOf(event);
  if (!state || !state.charts.length) {
    return;
  }
  const scale = state.charts[0].scales.x;
  const step = (scale.max - scale.min) / 2;
  logViewSetRange(state, scale.min - step, scale.max - step);
}

function logViewPanOn(event) {
  const state = logViewStateOf(event);
  if (!state || !state.charts.length) {
    return;
  }
  const scale = state.charts[0].scales.x;
  const step = (scale.max - scale.min) / 2;
  logViewSetRange(state, scale.min + step, scale.max + step);
}

function logViewShowAll(event) {
  const state = logViewStateOf(event);
  if (!state || !state.charts.length) {
    return;
  }
  logViewSetRange(state, state.log.time[0], state.log.time[state.log.time.length - 1]);
}

// keeps the graphs as wide as their box when the phone turns / the window changes size
function logViewResize() {
  const hostIds = Object.keys(logViewStates);
  for (let i = 0; i < hostIds.length; i++) {
    const host = document.getElementById(hostIds[i]);
    const state = logViewStates[hostIds[i]];
    if (!host || !host.clientWidth) {
      continue; // its tab isn't showing
    }
    for (let j = 0; j < state.charts.length; j++) {
      state.charts[j].setSize({ width: host.clientWidth, height: LOGVIEW_CHART_HEIGHT });
    }
  }
}
window.addEventListener("resize", logViewResize);

// the log as CSV text: "Time (s)" then each channel with its unit; an empty cell where there was no value.
// Opens in Excel, MegaLogViewer, Datazap and the like.
function logViewCsvText(log) {
  const header = ["Time (s)"];
  for (let channel = 0; channel < log.names.length; channel++) {
    let column = log.names[channel];
    if (log.units[channel]) {
      column = column + " (" + log.units[channel] + ")";
    }
    header.push('"' + column.replace(/"/g, "'") + '"');
  }
  const lines = [header.join(",")];
  for (let i = 0; i < log.time.length; i++) {
    const cells = [log.time[i].toFixed(3)];
    for (let channel = 0; channel < log.names.length; channel++) {
      const value = log.values[channel][i];
      if (value === null || isNaN(value)) {
        cells.push("");
      } else {
        cells.push(String(Math.round(value * 10000) / 10000)); // up to 4 decimals, no trailing zeros
      }
    }
    lines.push(cells.join(","));
  }
  return lines.join("\r\n") + "\r\n";
}

// hands text to the phone / computer as a file to save
function logViewDownload(text, fileName, mimeType) {
  const blob = new Blob([text], { type: mimeType || "text/csv" });
  const link = document.createElement("a");
  link.href = URL.createObjectURL(blob);
  link.download = fileName;
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
  setTimeout(logViewForgetLink, 10000, link.href); // free the copy once the save has had time to start
}

function logViewForgetLink(url) {
  URL.revokeObjectURL(url);
}

// a file name with the phone's date and time: "can2gauge_2026-10-18_1432.csv"
function logViewFileName(product, extension) {
  const now = new Date();
  const date = now.getFullYear() + "-" + String(now.getMonth() + 1).padStart(2, "0") + "-" + String(now.getDate()).padStart(2, "0");
  const time = String(now.getHours()).padStart(2, "0") + String(now.getMinutes()).padStart(2, "0");
  return product + "_" + date + "_" + time + "." + extension;
}
