// can2gauge - ME7 flash read UI (optional feature).
// Talks to /api/me7read/{start,cancel,status}. Self-contained: remove this file, its <script> tag and the
// #me7read-card section from index.html to drop the whole UI. Reuses helpers from app.js ($, api, action, toast).
(function () {
  const card = document.getElementById("me7read-card");
  if (!card) return;

  const el = (id) => document.getElementById(id);
  let poll = null;

  function fmtBytes(b) { return b >= 1024 * 1024 ? (b / 1048576).toFixed(2) + " MB" : Math.round(b / 1024) + " KB"; }
  function fmtSecs(ms) { const s = Math.round(ms / 1000); return s >= 60 ? Math.floor(s / 60) + "m " + (s % 60) + "s" : s + "s"; }

  async function refresh() {
    let s;
    try { s = await api("/api/me7read/status"); } catch (e) { return; }
    el("m7rState").textContent = s.state + (s.baud ? "  @ " + s.baud + " baud" : "");
    el("m7rBar").style.width = (s.percent || 0) + "%";
    let info = s.message || "";
    if (s.ecuId) info = s.ecuId + "  |  " + info;
    if (s.busy && s.bytesTotal) {
      info += `  |  ${fmtBytes(s.bytesRead)} / ${fmtBytes(s.bytesTotal)}`;
      if (s.etaMs) info += `  |  ~${fmtSecs(s.etaMs)} left`;
      if (s.elapsedMs) info += `  |  ${fmtSecs(s.elapsedMs)} elapsed`;
    }
    el("m7rInfo").textContent = info;

    // 0.17.4: the read is held in memory (dumpBytes) and saved as a file only when the gauge has room for it
    const f = s.lastFile;
    if (f && !s.busy) {
      el("m7rDownload").innerHTML = `<a href="/download?path=${encodeURIComponent(f)}">Download ${f}</a>`;
    } else if (s.dumpBytes && !s.busy) {
      el("m7rDownload").innerHTML = `<a href="/api/me7read/bin" download="me7_read.bin">Download the read (${fmtBytes(s.dumpBytes)})</a>` +
        " - kept in memory only (no room to save it on the gauge): download it before switching off";
    } else {
      el("m7rDownload").textContent = "";
    }

    el("m7rStart").disabled = s.busy;
    // poll fast while a read runs, slowly (or stop) otherwise
    if (s.busy && !poll) poll = setInterval(refresh, 750);
    if (!s.busy && poll) { clearInterval(poll); poll = null; }
  }

  el("m7rStart").addEventListener("click", async () => {
    if (!confirm("Start reading the ECU flash?\n\nEngine OFF, ignition ON. This can take several minutes. The engine will not start again until you cycle the ignition afterwards.")) return;
    const body = {
      fastInit: el("m7r.fastInit").value === "1",
      address: parseInt(el("m7r.address").value, 16) || 0x01,
      baud: Number(el("m7r.baud").value),
      mirrorCheck: el("m7r.mirror").value === "1",
      verify: el("m7r.verify").value === "1",
      ramUnlock: el("m7r.ramUnlock").value === "1", // 0.18.0: forge the XRAM unlock words for an ECU that refuses 0x27
      permitValue: parseInt(el("m7r.permit").value, 16) || 0x2620, // 0.18.0: per-ECU flash-read permit (the gate value)
    };
    const r = await api("/api/me7read/start", "POST", body);
    toast(r.status || r.error || "started");
    refresh();
  });

  el("m7rCancel").addEventListener("click", async () => {
    await api("/api/me7read/cancel", "POST", {});
    toast("cancelling");
    refresh();
  });

  // refresh whenever the K-line tab is shown, plus a slow idle poll so a read started elsewhere still shows
  document.querySelector('.nav-tab[data-page="kline"]').addEventListener("click", refresh);
  setInterval(() => { if (el("kline-page").classList.contains("active")) refresh(); }, 3000);
  refresh();
})();
