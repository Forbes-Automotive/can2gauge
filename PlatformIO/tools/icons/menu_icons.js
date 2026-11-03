// Mode-menu and settings icons - the single source for the glass (firmware bitmaps via make_menu_icons.py) and the sim.
//
// The concept's icons (docs/ui-concept/can2gauge_ui_concept.html, ICONS, 28 Sep 2026) are copied here unchanged apart
// from the colours, which go through K so each colour can be drawn as its own layer: K.acc() / K.acc2() are the theme's
// two accents, K.muted() / K.line() the greys. output, dsg, ble and chain are new (menu items the concept didn't have),
// drawn the same way. Icons are drawn around (0, 0) in a box of about +-90 x +-75; the menu draws them at scale 1.15.
//
// 0.37.6: no text in any icon (text icons become basic icons): the K-line's "ECU" / "K", the ADC's "5V", the output's
// "OUT", the termination's Ohm sign, info's "i", the gearbox's "P R N D S" / "+ -" and the CAN scaling caption are drawn
// shapes now. 0.38.0: except the gearbox, whose letters are back (Adam preferred them).
// Use: MenuIcons.draw(ctx, name, cx, cy, scale, K)   K = {acc, acc2, muted, line} returning CSS colours
"use strict";
(function (root) {
  const TAU = Math.PI * 2, A0 = Math.PI * 0.75, SPAN = Math.PI * 1.5;
  const GF = 'Montserrat, "Segoe UI", system-ui, sans-serif', MONO = '"Roboto Mono", ui-monospace, Consolas, monospace';
  let ctx, K;
  const acc = () => K.acc(), acc2 = () => K.acc2();
  const COL = { get muted() { return K.muted(); }, get line() { return K.line(); } };
  function font(w, px, fam) { ctx.font = `${w} ${px}px ${fam || GF}`; }
  function txt(s, x, y, col) { ctx.fillStyle = col; ctx.textAlign = "center"; ctx.textBaseline = "middle"; ctx.fillText(s, x, y); }
  function ln(x1, y1, x2, y2) { ctx.beginPath(); ctx.moveTo(x1, y1); ctx.lineTo(x2, y2); ctx.stroke(); }
  function rr(x, y, w, h, r) { ctx.beginPath(); ctx.roundRect(x, y, w, h, r); }

  const ICONS = {
    // ---- from the concept, unchanged
    obd() {
      ctx.beginPath(); ctx.moveTo(-72, -40); ctx.lineTo(72, -40); ctx.lineTo(58, 40); ctx.lineTo(-58, 40); ctx.closePath(); ctx.strokeStyle = acc(); ctx.stroke();
      for (let r = 0; r < 2; r++) for (let i = 0; i < 8; i++) {
        const x = r === 0 ? -52 + i * 14.8 : -46 + i * 13.2, y = r === 0 ? -16 : 16, pin = r * 8 + i + 1;
        const can = pin === 6 || pin === 14, pw = pin === 4 || pin === 5 || pin === 16, k = pin === 7;
        ctx.globalAlpha = can || pw || k ? 1 : .35; ctx.fillStyle = can ? acc() : pw ? COL.muted : k ? acc2() : COL.muted;
        ctx.fillRect(x - 3.5, y - 8, 7, 16);
      }
      ctx.globalAlpha = 1;
    },
    can() {
      ctx.strokeStyle = acc(); ln(-82, 24, 82, 24); ctx.strokeStyle = acc2(); ln(-82, 42, 82, 42);
      for (const x of [-54, 0, 54]) {
        ctx.lineWidth = 5; ctx.strokeStyle = acc(); rr(x - 19, -46, 38, 28, 7); ctx.stroke();
        ctx.lineWidth = 3; ln(x - 7, -18, x - 7, 24); ctx.strokeStyle = acc2(); ln(x + 7, -18, x + 7, 42);
      }
    },
    ccan() {
      const s = 20, g = 5, x0 = -86, y0 = -24;
      for (let r = 0; r < 2; r++) for (let c = 0; c < 4; c++) {
        rr(x0 + c * (s + g), y0 + r * (s + g), s, s, 4);
        if (r === 0 && c === 1) { ctx.fillStyle = acc(); ctx.fill(); } else { ctx.lineWidth = 3; ctx.strokeStyle = COL.muted; ctx.stroke(); }
      }
      ctx.strokeStyle = acc(); ctx.lineWidth = 4; ln(18, 0, 32, 0); ln(26, -6, 32, 0); ln(26, 6, 32, 0);
      ctx.lineWidth = 7; ctx.beginPath(); ctx.arc(66, 6, 26, A0, A0 + SPAN); ctx.strokeStyle = COL.line; ctx.stroke();
      ctx.beginPath(); ctx.arc(66, 6, 26, A0, A0 + SPAN * .62); ctx.strokeStyle = acc(); ctx.stroke();
      const a = A0 + SPAN * .62; ctx.lineWidth = 4; ln(66, 6, 66 + Math.cos(a) * 17, 6 + Math.sin(a) * 17);
    },
    haldex() {
      ctx.lineWidth = 4; ctx.strokeStyle = COL.muted;
      for (const x of [-52, 52]) for (const y of [-44, 44]) { rr(x - 9, y - 18, 18, 36, 5); ctx.stroke(); }
      ctx.lineWidth = 5; ctx.strokeStyle = acc(); ln(-43, -44, 43, -44); ln(-43, 44, 43, 44); ln(0, -44, 0, 20);
      ctx.beginPath(); ctx.arc(0, 30, 11, 0, TAU); ctx.fillStyle = acc2(); ctx.fill();
      rr(-20, -72, 40, 16, 4); ctx.lineWidth = 3; ctx.strokeStyle = COL.muted; ctx.stroke();
    },
    kline() {
      ctx.strokeStyle = COL.muted; ctx.lineWidth = 4; rr(-88, -36, 58, 72, 8); ctx.stroke();
      ctx.lineWidth = 3; for (const y of [-18, -6, 6, 18]) ln(-76, y, -42, y); // the module's circuit, as lines
      ctx.strokeStyle = acc(); ctx.lineWidth = 5; ctx.beginPath();
      [[-30, 10], [-6, 10], [-6, -14], [10, -14], [10, 10], [24, 10], [24, -14], [48, -14], [48, 10], [86, 10]].forEach((p, i) => i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1]));
      ctx.stroke();
    },
    adc() {
      ctx.strokeStyle = COL.muted; ctx.lineWidth = 4; ctx.beginPath();
      for (let i = 0; i < 6; i++) { const a = i * TAU / 6, x = -60 + Math.cos(a) * 22, y = -8 + Math.sin(a) * 22; i ? ctx.lineTo(x, y) : ctx.moveTo(x, y); }
      ctx.closePath(); ctx.stroke(); ctx.beginPath(); ctx.arc(-60, -8, 8, 0, TAU); ctx.stroke(); rr(-68, 16, 16, 20, 3); ctx.stroke();
      ctx.lineWidth = 3; ln(-10, -44, -10, 40); ln(-10, 40, 86, 40);
      ctx.strokeStyle = acc(); ctx.lineWidth = 5; ln(-2, 32, 74, -34);
      ctx.setLineDash([6, 6]); ctx.lineWidth = 2; ctx.strokeStyle = acc2(); ln(-10, -34, 86, -34); ctx.setLineDash([]);
    },
    settings() {
      ctx.strokeStyle = acc(); ctx.beginPath(); const n = 8;
      for (let i = 0; i < n; i++) {
        const a = i * TAU / n; [[-.24, 44], [-.14, 60], [.14, 60], [.24, 44]].forEach((p, j) => { const x = Math.cos(a + p[0]) * p[1], y = Math.sin(a + p[0]) * p[1]; (i || j) ? ctx.lineTo(x, y) : ctx.moveTo(x, y); });
        const a2 = a + TAU / n - .24; ctx.arc(0, 0, 44, a + .24, a2);
      }
      ctx.closePath(); ctx.stroke(); ctx.beginPath(); ctx.arc(0, 0, 18, 0, TAU); ctx.stroke();
    },
    sun() { ctx.strokeStyle = acc(); ctx.beginPath(); ctx.arc(0, 0, 20, 0, TAU); ctx.stroke(); for (let i = 0; i < 8; i++) { const a = i * TAU / 8; ln(Math.cos(a) * 32, Math.sin(a) * 32, Math.cos(a) * 44, Math.sin(a) * 44); } },
    sweep() { ctx.lineWidth = 7; ctx.beginPath(); ctx.arc(0, 8, 42, A0, A0 + SPAN); ctx.strokeStyle = COL.line; ctx.stroke(); ctx.beginPath(); ctx.arc(0, 8, 42, A0, A0 + SPAN * .8); ctx.strokeStyle = acc(); ctx.stroke(); const a = A0 + SPAN * .8; ctx.lineWidth = 4; ln(0, 8, Math.cos(a) * 30, 8 + Math.sin(a) * 30); },
    // the concept cut the crescent with a black disc; a bitmap layer needs the cut-out instead
    moon() { ctx.beginPath(); ctx.arc(0, 0, 36, 0, TAU); ctx.fillStyle = acc(); ctx.fill(); ctx.save(); ctx.globalCompositeOperation = "destination-out"; ctx.beginPath(); ctx.arc(16, -12, 30, 0, TAU); ctx.fillStyle = "#000"; ctx.fill(); ctx.restore(); },
    ohm() { // a resistor: leads and a zigzag
      ctx.strokeStyle = acc(); ctx.lineWidth = 6; ln(-70, 0, -40, 0); ln(40, 0, 70, 0);
      ctx.beginPath(); [[-40, 0], [-30, -20], [-15, 20], [0, -20], [15, 20], [30, -20], [40, 0]].forEach((p, i) => i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1])); ctx.stroke();
    },
    wifi() { ctx.strokeStyle = acc(); for (const r of [16, 34, 52]) { ctx.beginPath(); ctx.arc(0, 30, r, -Math.PI * .75, -Math.PI * .25); ctx.stroke(); } ctx.beginPath(); ctx.arc(0, 30, 5, 0, TAU); ctx.fillStyle = acc(); ctx.fill(); },
    info() { // a circle with the dot and the stem drawn, not a letter
      ctx.strokeStyle = acc(); ctx.beginPath(); ctx.arc(0, 0, 40, 0, TAU); ctx.stroke();
      ctx.beginPath(); ctx.arc(0, -17, 5, 0, TAU); ctx.fillStyle = acc(); ctx.fill();
      ctx.lineWidth = 9; ln(0, -3, 0, 20);
    },
    back() { ctx.strokeStyle = acc(); ln(32, 0, -32, 0); ln(-32, 0, -12, -20); ln(-32, 0, -12, 20); },

    // ---- new, in the same style
    output() { // the DRV8876 output switching a lamp, the dash-light input as the glow
      ctx.strokeStyle = COL.muted; ctx.lineWidth = 4; rr(-88, -32, 50, 64, 8); ctx.stroke();
      ln(-75, 0, -52, 0); ln(-60, -8, -52, 0); ln(-60, 8, -52, 0); // an arrow out of the box
      ctx.strokeStyle = acc(); ctx.lineWidth = 5; ln(-38, 0, 2, 0);
      ctx.beginPath(); ctx.arc(36, -6, 26, Math.PI * .72, Math.PI * 2.28); ctx.stroke();
      ln(26, 18, 26, 34); ln(46, 18, 46, 34); ln(24, 34, 48, 34);
      ctx.strokeStyle = acc2(); ctx.lineWidth = 4;
      for (const a of [-1.15, -0.5, 0.15]) { const c = Math.cos(a - Math.PI / 2), s = Math.sin(a - Math.PI / 2); ln(36 + c * 36, -6 + s * 36, 36 + c * 48, -6 + s * 48); }
    },
    dsg() { // the selector strip with D lit. 0.38.0: the letters are back (Adam preferred them to 0.37.6's dots)
      const L = ["P", "R", "N", "D", "S"];
      font(700, 26);
      L.forEach((l, i) => txt(l, -72 + i * 36, -8, l === "D" ? acc() : COL.muted));
      ctx.strokeStyle = acc(); ctx.lineWidth = 4; rr(18, -30, 36, 44, 9); ctx.stroke();
      ctx.strokeStyle = COL.line; ctx.lineWidth = 3; ln(-86, 30, 86, 30);
      font(700, 15); txt("+  −", 0, 50, acc2()); // + and - (a real minus sign)
    },
    ble() { // the OBD plug and the Bluetooth rune
      ctx.save(); ctx.translate(-40, 0); ctx.scale(.55, .55); ctx.lineWidth = 5 / .55; ICONS.obd(); ctx.restore();
      ctx.strokeStyle = acc2(); ctx.lineWidth = 5; ctx.beginPath();
      [[46, -18], [78, 14], [62, 30], [62, -30], [78, -14], [46, 18]].forEach((p, i) => i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1]));
      ctx.stroke();
    },
    chain() { // two gauges on one link
      for (const x of [-50, 50]) {
        ctx.lineWidth = 6; ctx.beginPath(); ctx.arc(x, 0, 32, A0, A0 + SPAN); ctx.strokeStyle = COL.line; ctx.stroke();
        ctx.beginPath(); ctx.arc(x, 0, 32, A0, A0 + SPAN * (x < 0 ? .7 : .4)); ctx.strokeStyle = acc(); ctx.stroke();
        const a = A0 + SPAN * (x < 0 ? .7 : .4); ctx.lineWidth = 4; ln(x, 0, x + Math.cos(a) * 20, Math.sin(a) * 20);
      }
      ctx.strokeStyle = acc2(); ctx.lineWidth = 4; ctx.setLineDash([2, 8]); ln(-12, 0, 12, 0); ctx.setLineDash([]);
    },
    gauges() { // the gauge pages: an arc dial with its scale and needle, the page dots under it
      ctx.lineWidth = 9; ctx.beginPath(); ctx.arc(0, 0, 56, A0, A0 + SPAN); ctx.strokeStyle = COL.line; ctx.stroke();
      ctx.beginPath(); ctx.arc(0, 0, 56, A0, A0 + SPAN * .62); ctx.strokeStyle = acc(); ctx.stroke();
      ctx.strokeStyle = COL.muted; ctx.lineWidth = 3;
      for (let i = 0; i <= 8; i++) { const a = A0 + SPAN * i / 8; ln(Math.cos(a) * 34, Math.sin(a) * 34, Math.cos(a) * 42, Math.sin(a) * 42); }
      const a = A0 + SPAN * .62; ctx.strokeStyle = acc2(); ctx.lineWidth = 5; ln(0, 0, Math.cos(a) * 38, Math.sin(a) * 38);
      ctx.beginPath(); ctx.arc(0, 0, 7, 0, TAU); ctx.fillStyle = acc2(); ctx.fill();
      for (let i = -1; i <= 1; i++) { ctx.beginPath(); ctx.arc(i * 16, 68, i === 0 ? 5 : 3.5, 0, TAU); ctx.fillStyle = i === 0 ? acc() : COL.line; ctx.fill(); }
    },
    drag() { // 0.38.0: the drag timer - a stopwatch with its ring part way round, and the chequered flag beside it
      ctx.lineWidth = 8; ctx.beginPath(); ctx.arc(-26, 10, 46, 0, TAU); ctx.strokeStyle = COL.line; ctx.stroke();
      ctx.beginPath(); ctx.arc(-26, 10, 46, -Math.PI / 2, -Math.PI / 2 + TAU * .7); ctx.strokeStyle = acc(); ctx.stroke();
      ctx.strokeStyle = COL.muted; ctx.lineWidth = 5; ln(-26, -36, -26, -50); ln(-38, -52, -14, -52); // the button on top
      const a = -Math.PI / 2 + TAU * .7; ctx.strokeStyle = acc(); ctx.lineWidth = 5; ln(-26, 10, -26 + Math.cos(a) * 30, 10 + Math.sin(a) * 30);
      ctx.beginPath(); ctx.arc(-26, 10, 6, 0, TAU); ctx.fillStyle = acc(); ctx.fill();
      ctx.strokeStyle = COL.muted; ctx.lineWidth = 4; ln(44, -48, 44, 56); // the flag's pole
      ctx.fillStyle = acc2();
      for (let row = 0; row < 3; row++) {
        for (let column = 0; column < 4; column++) {
          if ((row + column) % 2 === 0) ctx.fillRect(47 + column * 10, -46 + row * 10, 10, 10);
        }
      }
      ctx.strokeStyle = acc2(); ctx.lineWidth = 2; ctx.strokeRect(47, -46, 40, 30); // the cloth's edge
    },
    nav() { // the nav card: its turn arrow, and the journey ring's part done (second accent) from its start dot. No grey
      // track for the rest of the ring: every layer is a bitmap the size of its ink, and the track alone was ~24 KB of flash
      ctx.lineWidth = 7; ctx.beginPath(); ctx.arc(0, 4, 68, A0, A0 + SPAN * .4); ctx.strokeStyle = acc2(); ctx.stroke();
      ctx.beginPath(); ctx.arc(Math.cos(A0) * 68, 4 + Math.sin(A0) * 68, 8, 0, TAU); ctx.fillStyle = acc2(); ctx.fill(); // the start dot
      ctx.strokeStyle = acc(); ctx.lineWidth = 12; ctx.lineCap = "butt"; // the turn: up the road, then right
      ctx.beginPath(); ctx.moveTo(-14, 48); ctx.lineTo(-14, -2); ctx.arcTo(-14, -18, 2, -18, 16); ctx.lineTo(14, -18); ctx.stroke();
      ctx.lineCap = "round";
      ctx.beginPath(); ctx.moveTo(12, -40); ctx.lineTo(42, -18); ctx.lineTo(12, 4); ctx.closePath(); ctx.fillStyle = acc(); ctx.fill(); // the arrowhead
    },
    ecu() { // usb2ip: the ECU, its USB plug, and the values going out over WiFi
      ctx.strokeStyle = COL.muted; ctx.lineWidth = 4; rr(-88, -32, 64, 64, 8); ctx.stroke(); // the ECU
      ctx.lineWidth = 3; for (const y of [-14, 0, 14]) ln(-76, y, -36, y); // its circuit, as lines (like the K-line's module)
      ctx.strokeStyle = acc(); ctx.lineWidth = 5; ln(-24, 0, -6, 0); // the cable
      rr(-6, -15, 32, 30, 4); ctx.stroke(); // the USB plug
      ctx.fillStyle = acc(); ctx.fillRect(2, -7, 6, 5); ctx.fillRect(12, -7, 6, 5); // its contacts
      ctx.strokeStyle = acc2(); ctx.lineWidth = 5;
      for (const r of [16, 30, 44]) { ctx.beginPath(); ctx.arc(36, 0, r, -Math.PI / 4, Math.PI / 4); ctx.stroke(); } // WiFi
    },
    oil() { // the cluster's oil can, a drop falling from its spout
      ctx.strokeStyle = COL.line; ctx.lineWidth = 3; ln(-80, 44, 84, 44); // what it stands on
      ctx.strokeStyle = acc(); ctx.lineWidth = 5;
      rr(-40, -6, 66, 40, 6); ctx.stroke(); // the can
      ln(-14, -6, -14, -20); ln(-26, -20, -2, -20); // its filler cap
      ctx.beginPath(); ctx.moveTo(-40, 4); ctx.lineTo(-66, -10); ctx.lineTo(-66, 18); ctx.lineTo(-40, 26); ctx.stroke(); // the handle
      ctx.beginPath(); ctx.moveTo(26, 2); ctx.lineTo(62, -24); ctx.lineTo(68, -16); ctx.lineTo(26, 18); ctx.stroke(); // the spout
      ctx.fillStyle = acc2(); ctx.beginPath(); ctx.moveTo(70, -4); ctx.lineTo(62, 12); ctx.lineTo(78, 12); ctx.closePath(); ctx.fill();
      ctx.beginPath(); ctx.arc(70, 15, 8, 0, TAU); ctx.fill(); // the drop
    },
    triggers() { // a reading rising through its limit (dashed), the point it fired lit (no axes: they cost flash, see nav)
      ctx.setLineDash([8, 8]); ctx.strokeStyle = acc2(); ctx.lineWidth = 4; ln(-66, -14, 66, -14); ctx.setLineDash([]); // the limit
      ctx.strokeStyle = acc(); ctx.lineWidth = 6; ctx.beginPath();
      [[-62, 34], [-42, 22], [-24, 32], [-4, 6], [12, 14], [30, -36], [46, -24], [64, -46]].forEach((p, i) => i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1]));
      ctx.stroke();
      ctx.beginPath(); ctx.arc(22, -14, 9, 0, TAU); ctx.fillStyle = acc2(); ctx.fill(); // where it crossed the limit
    },
  };

  function draw(c, name, cx, cy, s, k) {
    ctx = c; K = k;
    ctx.save(); ctx.translate(cx, cy); ctx.scale(s, s);
    ctx.lineWidth = 5; ctx.lineCap = "round"; ctx.lineJoin = "round";
    if (ICONS[name]) ICONS[name]();
    ctx.restore();
  }
  root.MenuIcons = { draw, names: Object.keys(ICONS) };
})(typeof window !== "undefined" ? window : globalThis);
