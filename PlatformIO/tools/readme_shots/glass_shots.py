#!/usr/bin/env python3
"""
README images of the gauge face: stills, posters and GIFs drawn by the simulator's glass.

The glass (sim/www/glass.js) is GaugeUI drawn on a canvas and animates on requestAnimationFrame, so the shared
readme_shots.py (headless Chrome with virtual time) can't capture it. This tool:

  1. starts the simulator in-process (sim/server.py) on a free port, with three extra routes:
       GET  /__cap.html   a page with just the glass (480 x 480 canvas) and a timeline runner
       POST /__py         runs a Python hook on the live Sim (pages, theme, car values ...)
       POST /__frame      receives a canvas frame (PNG data URL)
  2. runs each job's timeline in headless Chrome in real time (screens, swipes, stills, recordings);
  3. composes the README images with Pillow: the round glass in a bezel, poster grids, GIFs.

    py tools/readme_shots/glass_shots.py               every job -> images/
    py tools/readme_shots/glass_shots.py pages themes  only those jobs
    py tools/readme_shots/glass_shots.py --list

Needs Python 3 with Pillow (the boot logos are decoded straight from data/logos/*.bin, no browser) and Chrome or Edge.
Fonts: Montserrat comes from Google Fonts like the sim (internet needed); the poster text uses Segoe UI.
"""
import argparse
import base64
import io
import json
import math
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SIM_DIR = os.path.join(ROOT, "sim")
OUT = os.path.join(ROOT, "images")
sys.path.insert(0, SIM_DIR)

BROWSERS = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
]

# house palette (_SharedTheme readme_shots.py THEME_CSS)
BG = (11, 15, 20)
LINE = (36, 48, 59)
TEXT = (230, 237, 243)
MUTED = (125, 142, 160)
ACCENT = (36, 209, 196)

CAP_HTML = r"""<!doctype html><html><head><meta charset="utf-8">
<link href="https://fonts.googleapis.com/css2?family=Montserrat:wght@500;600;700&display=swap" rel="stylesheet">
<style>html,body{margin:0;background:#000}canvas{display:block;width:480px;height:480px}</style></head><body>
<canvas id="glass"></canvas>
<script src="/sim/menu_icons.js"></script>
<script src="/sim/sprite.js"></script>
<script src="/sim/runner.js"></script>
<script src="/sim/glass.js"></script>
<script>
"use strict";
const q = new URLSearchParams(location.search), JOB = q.get("job"), RES = Number(q.get("res")) || 1;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const cv = document.getElementById("glass");
let n = 0;
async function frame(name) {
  const url = cv.toDataURL("image/png");
  await fetch("/__frame?job=" + JOB + "&name=" + encodeURIComponent(name) + "&i=" + (n++), { method: "POST", body: url });
}
async function py(fn, args) {
  await fetch("/__py", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ fn, args: args || {} }) });
}
async function run() {
  try { await document.fonts.load("500 48px Montserrat"); } catch (e) {}
  await Glass.init(cv, RES);
  const steps = await (await fetch("/__steps?job=" + JOB)).json();
  await sleep(1500); // first poll, themes, icons, logo files
  for (const s of steps) {
    if (s.py) await py(s.py, s.args);
    if (s.screen) Glass.setScreen(s.screen);
    if (s.swipe) Glass.swipe(s.swipe);
    if (s.menu) Glass.menu();
    if (s.tap) { Glass.tap(); if (s.tap === 2) { await sleep(120); Glass.tap(); } } // 0.38.0: tap: 1 = a tap, 2 = a double-tap
    if (s.wait) await sleep(s.wait);
    if (s.shot) await frame(s.shot);
    if (s.rec) { // {rec: name, ms, fps, swipeEvery?, swipeDir?}
      const dt = 1000 / (s.fps || 15), t0 = performance.now();
      let i = 0, lastSwipe = t0;
      while (performance.now() - t0 < s.ms) {
        const due = t0 + i * dt;
        if (performance.now() < due) await sleep(due - performance.now());
        if (s.swipeEvery && performance.now() - lastSwipe >= s.swipeEvery) { Glass.swipe(s.swipeDir || 1); lastSwipe = performance.now(); }
        if (s.pyEvery && performance.now() - lastSwipe >= s.pyEvery) { lastSwipe = performance.now(); py(s.pyFn, s.pyArgs); }
        await frame(s.rec + "/" + String(i).padStart(4, "0"));
        i++;
      }
    }
  }
  await fetch("/__done?job=" + JOB, { method: "POST", body: "" });
}
run().catch((e) => fetch("/__done?job=" + JOB + "&error=" + encodeURIComponent(String(e && e.stack || e)), { method: "POST", body: "" }));
</script></body></html>"""


# ------------------------------------------------------------------------------------------------------------------------
# the simulator, in-process
# ------------------------------------------------------------------------------------------------------------------------
class Harness:
    def __init__(self):
        import server  # sim/server.py
        self.server = server
        self.port = self._free_port()
        self.httpd = server.serve("127.0.0.1", self.port)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.frames = {}        # job -> {name: PNG bytes}
        self.done = {}          # job -> threading.Event
        self.errors = {}
        self.steps = {}         # job -> [step]
        self.hooks = {}         # name -> callable(sim, **args)
        self._patch()

    @staticmethod
    def _free_port():
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        p = s.getsockname()[1]
        s.close()
        return p

    @property
    def sim(self):
        return self.server.SIM

    def _patch(self):
        h = self
        Handler = self.server.Handler
        orig_get, orig_post = Handler.do_GET, Handler.do_POST

        def reply(self, code, body, ctype="application/json"):
            if not isinstance(body, (bytes, bytearray)):
                body = (body if isinstance(body, str) else json.dumps(body)).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            u = urllib.parse.urlsplit(self.path)
            q = urllib.parse.parse_qs(u.query)
            if u.path == "/__cap.html":
                return reply(self, 200, CAP_HTML, "text/html; charset=utf-8")
            if u.path == "/__steps":
                return reply(self, 200, h.steps.get(q.get("job", [""])[0], []))
            return orig_get(self)

        def do_POST(self):
            u = urllib.parse.urlsplit(self.path)
            q = urllib.parse.parse_qs(u.query)
            if u.path in ("/__frame", "/__py", "/__done"):
                n = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(n) if n else b""
                job = q.get("job", [""])[0]
                if u.path == "/__frame":
                    data = body.decode("ascii").split(",", 1)[1]
                    h.frames.setdefault(job, {})[q["name"][0]] = base64.b64decode(data)
                elif u.path == "/__py":
                    b = json.loads(body.decode("utf-8"))
                    with h.sim.lock:
                        h.hooks[b["fn"]](h.sim, **b.get("args", {}))
                else:
                    if q.get("error"):
                        h.errors[job] = q["error"][0]
                    h.done.setdefault(job, threading.Event()).set()
                return reply(self, 200, {"ok": True})
            return orig_post(self)

        Handler.do_GET, Handler.do_POST = do_GET, do_POST

    def hook(self, fn):
        self.hooks[fn.__name__] = fn
        return fn

    def run_job(self, browser, name, steps, res=1, timeout=180):
        self.steps[name] = steps
        self.frames[name] = {}
        ev = self.done[name] = threading.Event()
        prof = tempfile.mkdtemp(prefix="glass_shots_")
        url = "http://127.0.0.1:%d/__cap.html?job=%s&res=%s" % (self.port, name, res)
        cmd = [browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--no-first-run", "--mute-audio",
               "--disable-background-timer-throttling", "--disable-renderer-backgrounding",
               "--force-device-scale-factor=1", "--window-size=520,520", "--remote-debugging-port=0",
               "--user-data-dir=" + prof, url]
        p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            if not ev.wait(timeout):
                sys.exit("job %s timed out" % name)
        finally:
            p.terminate()
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
        if name in self.errors:
            sys.exit("job %s failed in the page:\n%s" % (name, self.errors[name]))
        return {k: Image.open(io.BytesIO(v)).convert("RGB") for k, v in self.frames[name].items()}


# ------------------------------------------------------------------------------------------------------------------------
# composition (Pillow)
# ------------------------------------------------------------------------------------------------------------------------
def font(size, bold=False):
    for f in (("seguisb.ttf" if bold else "segoeui.ttf"), "arial.ttf", "DejaVuSans.ttf"):
        for d in (r"C:\Windows\Fonts", "/usr/share/fonts/truetype/dejavu"):
            p = os.path.join(d, f)
            if os.path.isfile(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def _mask(size, inset=0, ss=4):
    m = Image.new("L", (size * ss, size * ss), 0)
    ImageDraw.Draw(m).ellipse((inset * ss, inset * ss, (size - inset) * ss - 1, (size - inset) * ss - 1), fill=255)
    return m.resize((size, size), Image.LANCZOS)


_bezel_cache = {}


def bezel(size):
    """the 52 mm gauge's bezel as an RGBA ring for a glass of `size` px: a dark rim, a lit edge, a soft shadow"""
    if size in _bezel_cache:
        return _bezel_cache[size]
    ring = max(10, size // 22)
    full = size + 2 * ring
    pad = ring
    W = full + 2 * pad
    ss = 3
    img = Image.new("RGBA", (W * ss, W * ss), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = W * ss / 2
    # drop shadow
    sh = Image.new("L", (W * ss, W * ss), 0)
    ImageDraw.Draw(sh).ellipse((c - full * ss / 2, c - full * ss / 2 + ring * ss * 0.5, c + full * ss / 2, c + full * ss / 2 + ring * ss * 0.5), fill=150)
    sh = sh.filter(ImageFilter.GaussianBlur(ring * ss * 0.6))
    img.paste((0, 0, 0, 255), (0, 0), sh)
    # metal rim: a vertical gradient, light at the top
    r0 = full * ss / 2
    rim = Image.new("RGBA", (W * ss, W * ss), (0, 0, 0, 0))
    grad = Image.linear_gradient("L").resize((W * ss, W * ss))
    top, bot = (78, 88, 100), (20, 24, 29)
    col = Image.merge("RGB", [grad.point(lambda v, a=a, b=b: int(a + (b - a) * v / 255)) for a, b in zip(top, bot)])
    m = Image.new("L", (W * ss, W * ss), 0)
    ImageDraw.Draw(m).ellipse((c - r0, c - r0, c + r0, c + r0), fill=255)
    rim.paste(col, (0, 0), m)
    img = Image.alpha_composite(img, rim)
    d = ImageDraw.Draw(img)
    r1 = r0 - ring * ss * 0.35
    d.ellipse((c - r1, c - r1, c + r1, c + r1), fill=(12, 14, 17, 255))
    r2 = size * ss / 2 + 1
    d.ellipse((c - r2 - 2 * ss, c - r2 - 2 * ss, c + r2 + 2 * ss, c + r2 + 2 * ss), outline=(48, 56, 66, 255), width=ss)
    out = img.resize((W, W), Image.LANCZOS)
    _bezel_cache[size] = (out, pad + ring)
    return _bezel_cache[size]


def round_glass(frame, size, bg=None):
    """frame (the square canvas) -> the glass in its bezel; bg None = transparent, else an RGB fill"""
    g = frame.resize((size, size), Image.LANCZOS).convert("RGBA")
    ring, off = bezel(size)
    out = Image.new("RGBA", ring.size, (0, 0, 0, 0) if bg is None else bg + (255,))
    out = Image.alpha_composite(out, ring)
    out.paste(g, (off, off), _mask(size))
    return out


def poster(cells, cols, title, tagline, file, cell=300, label_h=34, pad=48):
    """a grid of round gauges with a label each, in the house style (dark, grid, accent rule)"""
    sample = round_glass(cells[0][1], cell)
    cw = sample.size[0]
    rows = math.ceil(len(cells) / cols)
    gap = 26
    W = pad * 2 + cols * cw + (cols - 1) * gap
    head = 120 if title else 20
    H = head + rows * (cw + label_h) + (rows - 1) * gap + pad
    img = Image.new("RGBA", (W, H), BG + (255,))
    d = ImageDraw.Draw(img)
    for x in range(0, W, 48):  # faint grid like the UI posters
        d.line((x, 0, x, H), fill=(18, 25, 32, 255))
    for y in range(0, H, 48):
        d.line((0, y, W, y), fill=(18, 25, 32, 255))
    if title:
        f1, f2 = font(44, True), font(14)
        x = pad
        for part, colr in title:
            d.text((x, 34), part, font=f1, fill=colr)
            x += d.textlength(part, font=f1)
        d.text((pad, 88), tagline.upper(), font=f2, fill=MUTED)
        d.line((pad, 112, W - pad, 112), fill=ACCENT, width=2)
    fl = font(15, True)
    for i, (lab, fr) in enumerate(cells):
        r, c = divmod(i, cols)
        x = pad + c * (cw + gap)
        y = head + r * (cw + label_h + gap)
        g = round_glass(fr, cell)
        img.alpha_composite(g, (x, y))
        tw = d.textlength(lab.upper(), font=fl)
        d.text((x + (cw - tw) / 2, y + cw + 4), lab.upper(), font=fl, fill=ACCENT)
    img.convert("RGB").save(os.path.join(OUT, file), optimize=True)
    print("  poster %-28s %dx%d" % (file, W, H))


def gif(frames, file, size=360, fps=15, bg=BG, hold_last=0):
    imgs = []
    for f in frames:
        g = round_glass(f, size, bg)
        imgs.append(g.convert("RGB"))
    if hold_last:
        imgs += [imgs[-1]] * hold_last
    # identical neighbours become one longer frame (the boot-logo holds, a still glass)
    frames_d = []
    for im in imgs:
        if frames_d and im.tobytes() == frames_d[-1][0].tobytes():
            frames_d[-1][1] += 1
        else:
            frames_d.append([im, 1])
    pal = [im.quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE) for im, _ in frames_d]
    dur = [int(1000 / fps) * k for _, k in frames_d]
    pal[0].save(os.path.join(OUT, file), save_all=True, append_images=pal[1:], duration=dur, loop=0,
                optimize=True, disposal=1)
    kb = os.path.getsize(os.path.join(OUT, file)) / 1024
    print("  gif    %-28s %d frames, %dx%d, %d kB" % (file, len(imgs), imgs[0].size[0], imgs[0].size[1], kb))


def still(frame, file, size=420):
    round_glass(frame, size).save(os.path.join(OUT, file), optimize=True)
    print("  still  %-28s" % file)


def seq(frames, prefix):
    return [frames[k] for k in sorted(k for k in frames if k.startswith(prefix + "/"))]


# ------------------------------------------------------------------------------------------------------------------------
# C2GL boot logos (GaugeUI loadLogo / rleUnpack), decoded in Python
# ------------------------------------------------------------------------------------------------------------------------
def c2gl(path):
    b = open(path, "rb").read()
    if b[:4] != b"C2GL":
        raise ValueError(path + ": not C2GL")
    w, h = b[6] | b[7] << 8, b[8] | b[9] << 8
    rle = b[5] & 1
    px = []
    i = 16
    if not rle:
        px = [b[k] | b[k + 1] << 8 for k in range(16, 16 + w * h * 2, 2)]
    else:
        while i < len(b) and len(px) < w * h:
            c = b[i]
            i += 1
            if c & 0x80:
                v = b[i] | b[i + 1] << 8
                i += 2
                px.extend([v] * ((c & 0x7F) + 2))
            else:
                for _ in range(c + 1):
                    px.append(b[i] | b[i + 1] << 8)
                    i += 2
    px = px[: w * h]
    raw = bytearray()
    for v in px:
        raw += bytes((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
    img = Image.frombytes("RGB", (w, h), bytes(raw))
    out = Image.new("RGB", (480, 480), (0, 0, 0))
    out.paste(img, ((480 - w) // 2, (480 - h) // 2))
    return out


# ------------------------------------------------------------------------------------------------------------------------
# jobs
# ------------------------------------------------------------------------------------------------------------------------
STYLE = {"Arc": 0, "Needle": 1, "Bar": 2, "Single": 3, "Dual": 4, "Haldex": 5, "Quad": 6, "Gear": 7, "Oil (PQ)": 8, "Custom": 9,
         "Graph": 10, "Triple": 11}  # 0.31.0: Numeric shown as Single, Oil (OEM) as Oil (PQ); Graph and Triple added

PAGE_SHOTS = [  # (file label, page config) - one page each, shown alone, in the web UI's Style list order
    ("Needle", dict(style=1, signal="rpm", label="RPM", min=0, max=8000, redline=6800)),
    ("Arc", dict(style=0, signal="boost", label="BOOST", min=-1, max=2, redline=1.6, decimals=2)),
    ("Bar", dict(style=2, signal="speed", label="SPEED", min=0, max=280)),
    ("Graph", dict(style=10, signal="rpm", label="RPM", min=0, max=8000, redline=6800)),
    ("Single", dict(style=3, signal="batt", label="BATTERY", min=10, max=16, redline=15, decimals=1)),
    ("Dual", dict(style=4, signal="coolant", signal2="oiltemp", label="TEMPS", min=0, max=140, redline=110)),
    ("Triple", dict(style=11, signal="coolant", label="TEMPS", min=0, max=140, redline=110, slots=["oiltemp", "iat", "none"])),
    ("Quad", dict(style=6, signal="coolant", label="ENGINE", min=0, max=140, redline=110, slots=["iat", "oiltemp", "map"])),
    ("Haldex", dict(style=5, signal="hdxActual", label="HALDEX", min=0, max=100)),
    ("Gear (DSG)", dict(style=7, signal="selector", label="DSG", min=0, max=280)),
    ("Oil (PQ)", dict(style=8, signal="oiltemp", label="OIL", min=40, max=150, redline=130)),
    ("Custom", dict(style=9, signal="boost", label="BOOST", min=-1, max=2, redline=1.6, decimals=2, slots=["coolant", "oiltemp", "batt"])),
]

# a good-looking moment on the bench car: warm, on boost, Haldex locking, DSG in D4
PIN_PULL = {"rpm": 4650, "speed": 96, "map_mbar": 2180, "pedal": 78, "coolant": 91, "oil": 104, "iat": 31,
            "battery": 14.0, "hdx_actual": 72, "fuel": 41}


def setup(h):
    """hooks the timelines call (POST /__py)"""
    fw = sys.modules["firmware"]

    @h.hook
    def warm(sim, scenario="manual", pins=None, theme=0, dsg=True):
        c = sim.car
        c.cfg.update(scenario=scenario, timeScale=10, dsg=dsg, dsgSel="D", pedal=60)
        c.coolant, c.oil, c.iat = 90.0, 98.0, 28.0
        c.overrides.clear()
        c.overrides.update({k: float(v) for k, v in (pins or {}).items()})
        sim.settings["display"]["theme"] = theme
        sim.rt["dashLights"] = False

    @h.hook
    def show_number(sim, row=0, name=""):
        """0.29.0: Show Numbers on the glass (row 0 = hide it and take the gauge out of the row again)"""
        sim.settings["chain"].update(rowPosition=row, nodeName=name, role=0)
        sim.rt["numberUntil"] = fw.now_ms() + 60000 if row else 0

    @h.hook
    def show_nav(sim, on=True):
        """0.30.0: the nav card's test turn on the glass (Show Test Turn), or take it away"""
        sim.rt["navTestUntil"] = fw.now_ms() + 60000 if on else 0

    @h.hook
    def one_page(sim, cfg):
        """settings.pages: only page 0, configured as given"""
        base = fw.deepcopy(sim.settings["pages"][0])
        base.update(enabled=1, source="can", signal2="none", decimals=0, redline=0)
        base.update(cfg)
        for i, p in enumerate(sim.settings["pages"]):
            p["enabled"] = 0
        sim.settings["pages"][0] = base
        sim.rt["activePage"] = 0

    @h.hook
    def pages(sim, cfgs):
        for i, p in enumerate(sim.settings["pages"]):
            p["enabled"] = 0
        for i, cfg in enumerate(cfgs):
            base = fw.deepcopy(sim.settings["pages"][i])
            base.update(enabled=1, source="can", signal2="none", decimals=0, redline=0)
            base.update(cfg)
            sim.settings["pages"][i] = base
        sim.rt["activePage"] = 0

    @h.hook
    def theme(sim, id=0):
        sim.settings["display"]["theme"] = id

    @h.hook
    def boot(sim, logo=6, make=""):
        sim.settings["boot"].update(logo=logo, make=make)
        sim.action({"action": "bootShow"})

    @h.hook
    def drive_from(sim, phase=3, t=0.0):
        c = sim.car
        c.overrides.clear()
        c.cfg.update(scenario="drive")
        c.phase_i, c.phase_t = phase, t
        c.speed, c.rpm, c.gear = 0.0, 850.0, 1
        c.coolant, c.oil = 90.0, 99.0

    @h.hook
    def alert(sim, text="OIL PRESSURE", latch=True):
        sim.alert(text, latch)

    @h.hook
    def clear_alert(sim):
        sim.clear_alert()

    @h.hook
    def elm_on(sim):
        sim.settings["elm"].update(enabled=1, mac="a4:c1:38:5e:12:7b")

    @h.hook
    def dbc(sim, name="vw_pq"):
        import gzip
        text = gzip.open(os.path.join(ROOT, "data", "dbc", name + ".c2d.gz"), "rt", encoding="utf-8").read()
        sim.can_dbc_load(text)

    @h.hook
    def night(sim, on=True):
        sim.rt["dashLights"] = on

    @h.hook
    def first_boot(sim, card="ask"):
        """the first boot cards (lib/GaugeUI/src/FirstBootPrompt.cpp): "ask" the question, "login" after Yes, "" none"""
        if card:
            sim.firstboot = {"state": "ask" if card == "ask" else "waiting", "glass": True, "card": card}
        else:
            sim.firstboot = {"state": "done", "glass": False, "card": "ask"}

    @h.hook
    def act(sim, action="saverShow"):
        """a web UI action, e.g. saverShow (the screensaver's Show It Now) or shutdownShow (Play It Now)"""
        sim.action({"action": action})

    @h.hook
    def drag_runs(sim, times=()):
        """0.38.0: drag timer runs on the scoreboard (ms each, 0-100 km/h), as if timed on the glass"""
        for ms in times:
            sim.drag_add(0, int(ms))

    @h.hook
    def oil_src(sim, source=1):
        """Oil tab source: 0 CAN (Kombi_2, temperature only), 1 K-line (cluster 0x17: temperature + level)"""
        sim.settings["oil"]["source"] = source
        sim.restart_kline = True


def job_steps():
    """name -> (res, steps, compose(frames))"""
    J = {}

    # ---- every page style, still -----------------------------------------------------------------------------------
    st = [{"py": "warm", "args": {"pins": PIN_PULL}}, {"wait": 9000}]  # the K-line engine session wakes (battery, intake)
    for lab, cfg in PAGE_SHOTS:
        if lab.startswith("Oil"):  # the cluster session gives LEVEL OK as well as the temperature
            st += [{"py": "oil_src", "args": {"source": 1}}]
        wait = 1700
        if lab.startswith("Oil"):
            wait = 7000  # the cluster session
        elif lab == "Graph":
            wait = 31000  # 0.31.0: a full 30 s of line, with rpm left to move (the rest stay pinned)
            loose = {k: v for k, v in PIN_PULL.items() if k not in ("rpm", "speed", "pedal", "map_mbar")}
            st += [{"py": "warm", "args": {"scenario": "drive", "pins": loose}}]  # the drive cycle: rpm up and down the gears
        st += [{"py": "one_page", "args": {"cfg": cfg}}, {"screen": "page", "wait": wait},
               {"shot": lab}]
        if lab == "Graph":
            st += [{"py": "warm", "args": {"pins": PIN_PULL}}, {"wait": 3000}]  # back to the pinned moment for the rest

    def c_pages(fr):
        cells = [(lab, fr[lab]) for lab, _ in PAGE_SHOTS]
        poster(cells, 4, [("can", TEXT), ("2", ACCENT), ("gauge", TEXT), ("  page styles", MUTED)],
               "12 page styles - any signal from CAN, K-line, OBD, ADC or the gauge itself", "glass-pages.png")
        for lab, _ in PAGE_SHOTS:
            still(fr[lab], "glass-page-%s.png" % lab.split(" ")[0].lower())
    J["pages"] = (2, st, c_pages)

    # ---- the hero: a pull through the gears, swiping across pages ---------------------------------------------------
    hero_pages = [dict(style=1, signal="rpm", label="RPM", min=0, max=8000, redline=6800),
                  dict(style=0, signal="boost", label="BOOST", min=-1, max=2, redline=1.6, decimals=2),
                  dict(style=9, signal="speed", label="SPEED", min=0, max=240, slots=["coolant", "oiltemp", "batt"]),
                  dict(style=5, signal="hdxActual", label="HALDEX", min=0, max=100)]
    J["hero"] = (1, [{"py": "warm", "args": {"pins": PIN_PULL}}, {"py": "pages", "args": {"cfgs": hero_pages}},
                     {"screen": "page", "wait": 9000},  # K-line wakes: oil temp / battery in the Custom page's slots
                     {"py": "drive_from", "args": {"phase": 3, "t": 0.0}}, {"wait": 600},
                     {"rec": "hero", "ms": 13000, "fps": 12, "swipeEvery": 3200}],
                 lambda fr: gif(seq(fr, "hero"), "glass-hero.gif", size=340, fps=12))

    # ---- the mode menu: one item per screen, swipe through ---------------------------------------------------------
    J["menu"] = (1, [{"py": "warm", "args": {"pins": PIN_PULL}}, {"py": "elm_on"}, {"screen": "menu", "wait": 900},
                     {"rec": "menu", "ms": 15500, "fps": 12, "swipeEvery": 1500}],
                 lambda fr: gif(seq(fr, "menu"), "glass-menu.gif", size=340, fps=12))

    # ---- the mode screens behind the menu --------------------------------------------------------------------------
    ms = [{"py": "warm", "args": {"pins": PIN_PULL}}, {"py": "dbc"}, {"py": "elm_on"},
          {"py": "one_page", "args": {"cfg": PAGE_SHOTS[0][1]}}, {"wait": 6000}]
    MODES = [("Haldex", "haldex", 1800), ("CAN bus", "mode-can", 2500), ("K-line", "mode-kline", 9000),
             ("Bluetooth OBD", "mode-ble", 9000), ("Gearbox", "dsg", 1500), ("ADC", "mode-adc", 2500),
             ("Output / lights", "mode-output", 1500), ("Daisy chain", "mode-chain", 1500), ("Settings", "settings", 1200)]
    for lab, scr, w in MODES:
        ms += [{"screen": scr, "wait": w}, {"shot": lab}]
    ms += [{"screen": "menu", "wait": 900}, {"shot": "Menu"}]

    def c_modes(fr):
        cells = [("Menu", fr["Menu"])] + [(lab, fr[lab]) for lab, _, _ in MODES]
        poster(cells, 5, [("can", TEXT), ("2", ACCENT), ("gauge", TEXT), ("  on the screen", MUTED)],
               "hold for the menu - every interface has a live screen", "glass-modes.png")
    J["modes"] = (2, ms, c_modes)

    # ---- themes: the needle page across manufacturer themes --------------------------------------------------------
    TH = [(0, "Forbes teal"), (77, "VW blue"), (76, "VW green"), (19, "Audi red"), (21, "BMW amber"), (68, "Skoda green"),
          (50, "Mercedes"), (62, "Porsche"), (47, "Mazda red"), (71, "Subaru red"), (54, "Nissan amber"), (65, "Saab green")]
    tst = [{"py": "warm", "args": {"pins": PIN_PULL}},
           {"py": "one_page", "args": {"cfg": dict(style=1, signal="rpm", label="RPM", min=0, max=8000, redline=6800)}},
           {"screen": "page"}]
    for tid, lab in TH:
        tst += [{"py": "theme", "args": {"id": tid}}, {"wait": 5600}, {"shot": lab}]  # the glass reloads themes every 5 s

    def c_themes(fr):
        cells = [(lab, fr[lab]) for _, lab in TH]
        poster(cells, 6, [("can", TEXT), ("2", ACCENT), ("gauge", TEXT), ("  themes", MUTED)],
               "71 themes - 65 manufacturer cluster colours across 31 makes, picked from the VIN with Match car", "glass-themes.png", cell=240)
        gif([fr[lab] for _, lab in TH], "glass-themes.gif", size=320, fps=1)
    J["themes"] = (1, tst, c_themes)

    # ---- 0.38.0: the drag timer - ready, the countdown, a finished run and the scoreboard -------------------------------
    # the car is pinned still, the run started with a tap, then the speed is pinned past 100 km/h after the flag drops
    drag_still = dict(PIN_PULL, speed=0, rpm=850, map_mbar=1000, pedal=0)
    drag_mid = dict(PIN_PULL, speed=64)
    drag_fast = dict(PIN_PULL, speed=112)
    dr = [{"py": "warm", "args": {"pins": drag_still}}, {"py": "drag_runs", "args": {"times": [5240, 4910, 5630, 6020]}},
          {"screen": "drag", "wait": 1500}, {"shot": "Ready"},
          {"tap": 1, "wait": 2600}, {"shot": "Countdown"},  # 2.4 s left
          {"wait": 3400}, {"py": "warm", "args": {"pins": drag_mid}}, {"wait": 1500}, {"shot": "Running"},  # the flag dropped 1 s
          {"wait": 2300}, {"py": "warm", "args": {"pins": drag_fast}}, {"wait": 1600}, {"shot": "Finished"},  # ... ago
          {"tap": 2, "wait": 900}, {"shot": "Scoreboard"}, {"tap": 1, "wait": 600}]

    def c_drag(fr):
        cells = [(lab, fr[lab]) for lab in ("Ready", "Countdown", "Running", "Finished", "Scoreboard")]
        poster(cells, 5, [("Drag", TEXT), (" Timer", ACCENT)],
               "tap to start, the flag drops, the time when it hits 100 - double-tap for the scores", "glass-drag.png")
    J["drag"] = (1, dr, c_drag)

    # ---- first boot: the question on a new gauge, then how to log in ----------------------------------------------
    def c_firstboot(fr):
        still(fr["ask"], "glass-firstboot-ask.png")
        still(fr["login"], "glass-firstboot-login.png")
    J["firstboot"] = (2, [{"py": "warm", "args": {"pins": PIN_PULL}}, {"screen": "page", "wait": 800},
                          {"py": "first_boot", "args": {"card": "ask"}}, {"wait": 900}, {"shot": "ask"},
                          {"py": "first_boot", "args": {"card": "login"}}, {"wait": 900}, {"shot": "login"},
                          {"py": "first_boot", "args": {"card": ""}}],
                      c_firstboot)

    # ---- 0.29.0: Show Numbers - the gauge's number in the row over its page ------------------------------------------
    J["number"] = (1, [{"py": "warm", "args": {"pins": PIN_PULL}}, {"screen": "page", "wait": 800},
                       {"py": "show_number", "args": {"row": 2, "name": "Boost"}}, {"wait": 900}, {"shot": "number"},
                       {"py": "show_number", "args": {"row": 0}}],
                   lambda fr: still(fr["number"], "glass-number.png"))

    # ---- 0.30.0: the nav card (Google Maps' next turn, Android Auto style) ------------------------------------------
    J["nav"] = (1, [{"py": "warm", "args": {"pins": PIN_PULL}}, {"screen": "page", "wait": 800},
                    {"py": "show_nav", "args": {"on": True}}, {"wait": 900}, {"shot": "nav"},
                    {"py": "show_nav", "args": {"on": False}}],
                lambda fr: still(fr["nav"], "glass-nav.png"))

    # ---- Rev, the mascot: a page with its Sprite switch on, in each mood the pack has (docs/mascot/pack.json) --------
    rpm_sprite = dict(style=1, signal="rpm", label="RPM", min=0, max=8000, redline=6800, spriteOn=1, sprite="rev")
    boost_sprite = dict(style=0, signal="boost", label="BOOST", min=-1, max=2, redline=1.6, decimals=2, spriteOn=1, sprite="rev")
    haldex_sprite = dict(style=5, signal="hdxActual", label="HALDEX", min=0, max=100, spriteOn=1, sprite="rev")
    still_car = {"speed": 0, "oil": 98}
    REV = [  # (label, page, pinned car values)
        ("Idle", rpm_sprite, dict(still_car, rpm=900)),
        ("Running", rpm_sprite, dict(still_car, rpm=3200)),
        ("Sprinting", rpm_sprite, dict(still_car, rpm=5600)),
        ("Redline", rpm_sprite, dict(still_car, rpm=7000)),
        ("Boost", boost_sprite, dict(still_car, rpm=4800, map_mbar=2650)),
        ("Haldex lock", haldex_sprite, dict(still_car, rpm=3000, hdx_actual=98)),
        ("Oil hot", rpm_sprite, dict(still_car, rpm=3000, oil=135)),
        ("Oil cold", rpm_sprite, dict(still_car, rpm=1100, oil=8)),
    ]
    rv = []
    for lab, page, pins in REV:
        rv += [{"py": "warm", "args": {"pins": pins}}, {"py": "one_page", "args": {"cfg": page}},
               {"screen": "page", "wait": 3200}, {"shot": lab}]
    rv += [{"py": "warm", "args": {"pins": dict(still_car, rpm=850)}}, {"py": "one_page", "args": {"cfg": rpm_sprite}},
           {"screen": "page", "wait": 1500}, {"py": "act", "args": {"action": "saverShow"}}, {"wait": 3500},
           {"shot": "Screensaver"},
           {"py": "warm", "args": {"pins": dict(still_car, rpm=850)}}, {"screen": "page", "wait": 1500},
           {"py": "act", "args": {"action": "shutdownShow"}}, {"wait": 1400}, {"shot": "Goodbye"},
           {"wait": 6000},
           {"py": "warm", "args": {"pins": PIN_PULL}}, {"screen": "page", "wait": 1500},
           {"py": "drive_from", "args": {"phase": 3, "t": 0.0}}, {"wait": 600},
           {"rec": "revpull", "ms": 12000, "fps": 12}]

    def c_rev(fr):
        labels = [lab for lab, _, _ in REV] + ["Screensaver", "Goodbye"]
        cells = [(lab, fr[lab]) for lab in labels]
        poster(cells, 5, [("can", TEXT), ("2", ACCENT), ("gauge", TEXT), ("  Rev", MUTED)],
               "the mascot - any page can switch to its sprite, and Rev follows rpm, boost, Haldex lock and oil temperature",
               "glass-rev.png")
        gif(seq(fr, "revpull"), "glass-rev.gif", size=320, fps=12)
    J["rev"] = (2, rv, c_rev)

    # ---- an alert pulsing over a page -----------------------------------------------------------------------------
    J["alert"] = (1, [{"py": "warm", "args": {"pins": dict(PIN_PULL, coolant=118)}},
                      {"py": "one_page", "args": {"cfg": dict(style=0, signal="coolant", label="COOLANT", min=0, max=140, redline=110)}},
                      {"screen": "page", "wait": 1500}, {"py": "alert", "args": {"text": "COOLANT 118 °C"}}, {"wait": 200},
                      {"rec": "alert", "ms": 2400, "fps": 15}],
                  lambda fr: gif(seq(fr, "alert"), "glass-alert.gif", size=320, fps=15))

    # ---- night dimming: the same page, lights on ------------------------------------------------------------------
    return J


def boot_logos():
    """boot logos straight from the C2GL files: a GIF carousel and a sheet"""
    order = ["can2gauge", "forbes", "vw", "audi", "seat", "skoda", "porsche", "bmw", "mercedes", "ford", "vauxhall",
             "toyota", "honda", "nissan", "mazda", "subaru", "volvo", "alfa", "mini", "landrover"]
    imgs = {}
    for k in order:
        p = os.path.join(ROOT, "data", "logos", k + ".bin")
        if os.path.isfile(p):
            imgs[k] = c2gl(p)
    if "can2gauge" in imgs:
        imgs["can2gauge"] = settled_dial(imgs["can2gauge"])  # 0.25.2: the gauge draws the sweep, the file's dial is empty
    # carousel: hold each, cross-fade through black like the gauge's boot -> page
    frames = []
    keys = list(imgs)
    for i, k in enumerate(keys):
        a, b = imgs[k], imgs[keys[(i + 1) % len(keys)]]
        frames += [a] * 10
        for t in (1, 2):
            frames.append(Image.blend(a, Image.new("RGB", a.size), t / 3))
        frames.append(Image.new("RGB", a.size))
        for t in (1, 2):
            frames.append(Image.blend(Image.new("RGB", a.size), b, t / 3))
    gif(frames, "glass-bootlogos.gif", size=300, fps=10)
    all_keys = sorted(k[:-4] for k in os.listdir(os.path.join(ROOT, "data", "logos")) if k.endswith(".bin"))
    first = ["can2gauge", "forbes"]
    all_keys = first + [k for k in all_keys if k not in first]
    cells = [(k, imgs[k] if k in imgs else c2gl(os.path.join(ROOT, "data", "logos", k + ".bin"))) for k in all_keys]
    poster(cells, 7, [("can", TEXT), ("2", ACCENT), ("gauge", TEXT), ("  boot logos", MUTED)],
           "33 make logos + can2gauge + Forbes - or convert your own in the web UI", "glass-bootlogos.png", cell=200)
    still(imgs["can2gauge"], "glass-boot-can2gauge.png", size=420)  # 0.25.2: its own dial, no ring round the edge


def settled_dial(logo):
    """0.25.2: the can2gauge logo as it rests after its boot sweep (GaugeUI bootSweepDial, BOOT_DIAL_*): the band to
    72 %, teal for the first 35 % of the sweep into amber by 72 %, ticks lit white up to it. The logo file's dial is empty."""
    ss = 3
    big = logo.convert("RGB").resize((480 * ss, 480 * ss), Image.LANCZOS)
    px = big.load()
    cx, cy, r, w, rest = 240 * ss, 200 * ss, 122 * ss, 16 * ss, 0.72
    teal, amber = (0x24, 0xD1, 0xC4), (0xFF, 0xB0, 0x2E)
    ends = [(cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a))) for a in (135, 135 + 270 * rest)]
    for y in range(cy - r - w, cy + r + w):
        for x in range(cx - r - w, cx + r + w):
            part = ((math.degrees(math.atan2(y - cy, x - cx)) - 135) % 360) / 270
            in_band = abs(math.hypot(x - cx, y - cy) - r) <= w / 2 and part <= rest
            in_cap = any(math.hypot(x - ex, y - ey) <= w / 2 for ex, ey in ends)
            if not (in_band or in_cap):
                continue
            if part > 1:
                part = 0 if part > 1 + 45 / 270 else 1
            t = min(1.0, max(0.0, (part - 0.35) / 0.37))
            px[x, y] = tuple(int(teal[i] + (amber[i] - teal[i]) * t) for i in range(3))
    d = ImageDraw.Draw(big)
    for i in range(11):
        if i / 10 > rest:
            continue
        a = math.radians(135 + 27 * i)
        ri, ro = r - w * 1.55, r - w * 0.95
        d.line((cx + ri * math.cos(a), cy + ri * math.sin(a), cx + ro * math.cos(a), cy + ro * math.sin(a)),
               fill=(0xF2, 0xF4, 0xF8), width=(4 if i % 5 == 0 else 3) * ss)
    return big.resize((480, 480), Image.LANCZOS)


def sweep_ring(logo):
    """the boot sweep's ring (GaugeUI bootSweepRing) at the end of its sweep, over a 480 x 480 logo: 440 px across,
    18 px wide, 270 degrees from bottom-left round to bottom-right, round ends, in the accent"""
    ss = 3
    big = logo.convert("RGB").resize((480 * ss, 480 * ss), Image.LANCZOS)
    d = ImageDraw.Draw(big)
    box = (20 * ss, 20 * ss, 460 * ss - 1, 460 * ss - 1)
    d.arc(box, 135, 405, fill=ACCENT, width=18 * ss)
    mid = (220 - 9) * ss  # the ring's centre line, from the middle of the glass
    for a in (135, 405):
        x = 240 * ss + mid * math.cos(math.radians(a))
        y = 240 * ss + mid * math.sin(math.radians(a))
        d.ellipse((x - 9 * ss, y - 9 * ss, x + 9 * ss, y + 9 * ss), fill=ACCENT)
    return big.resize((480, 480), Image.LANCZOS)


def find_browser():
    for p in BROWSERS:
        if os.path.isfile(p):
            return p
    sys.exit("no Chrome / Edge found")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("jobs", nargs="*", help="jobs to run (default: all)")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    J = job_steps()
    names = list(J) + ["bootlogos"]
    if a.list:
        print("\n".join(names))
        return
    todo = a.jobs or names
    os.makedirs(OUT, exist_ok=True)
    if "bootlogos" in todo:
        print("bootlogos")
        boot_logos()
    todo = [n for n in todo if n in J]
    if not todo:
        return
    browser = find_browser()
    h = Harness()
    setup(h)
    print("sim on :%d (%s)" % (h.port, os.path.basename(browser)))
    for n in todo:
        res, steps, compose = J[n]
        t0 = time.time()
        print("%s ..." % n)
        fr = h.run_job(browser, n, steps, res)
        print("  %d frames in %.0f s" % (len(fr), time.time() - t0))
        compose(fr)


if __name__ == "__main__":
    main()
