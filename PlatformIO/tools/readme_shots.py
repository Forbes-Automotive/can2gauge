#!/usr/bin/env python3
"""
README screenshot generator for the Forbes Automotive web UIs.

Renders the REAL web UI (the project's data/ folder) against a mocked device
API carrying example data, screenshots each page with headless Chrome/Edge at
phone size, then composes the README images (the big compiled "overview"
poster, the 4-up strip, single card crops) as HTML pages in the shared theme
and screenshots those too. Nothing is hand-drawn: every pixel of UI comes from
the same index.html / style.css / app.js the device serves.

    python tools/readme_shots.py tools/readme_shots/openhaldex.json
    python tools/readme_shots.py <config> --only poster     # just recompose
    python tools/readme_shots.py <config> --keep            # keep the temp dir

Everything project-specific lives in the JSON config: the mock API responses,
the list of page shots (page tab, which cards, viewport / fit), and the
poster(s) / strip / crop layouts. See tools/readme_shots/openhaldex.json and
_SharedTheme/tools/README.md for the option reference.

Convention: one shot per README section, listing exactly the cards that
section describes ("cards"), all expanded, at the document's real height
("fit": true). Two posters ("posters": [...]) when eight cells are not enough.

Requirements: Python 3 (stdlib only) and Chrome or Edge installed.
"""
import argparse
import base64
import http.server
import json
import os
import shutil
import socketserver
import subprocess
import sys
import tempfile
import threading
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BROWSERS = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
]

# Injected into index.html so a plain URL can drive the single-page UI:
#   ?w=390                  pin the layout width (headless Chrome won't open a
#                           window narrower than ~500px, so the document is
#                           constrained instead and the capture cropped to w)
#   ?page=expert            click that nav tab
#   ?open=Card A|Card B     expand those collapsed cards (by <h2> text)
#   ?openall=1              expand every collapsed card
#   ?hide=Card A|Card B     remove those cards from the page
#   ?focus=Card             expand that card and remove every card above it,
#                           so it sits directly under the nav (no scrolling -
#                           scroll position is not reliable in a headless capture)
#   ?cards=Card A|Card B    show only those cards (expanded, page order), keep the
#                           topbar/nav - one README section's worth of UI
#   ?show=Card A|Card B     force cards the UI keeps hidden until a bus / module
#                           is detected (display:none) to render
#   ?only=Card              show only that card (no topbar/nav) for a crop
#   ?click=Label A|Label B  press those buttons (by visible text) after the tab
#                           switch - to open a card the UI reveals on demand
#   ?fit=<id>               report the settled document height back to the mock
#                           server so the capture can be re-shot at exactly that
#                           height (nothing cut off, no empty space below)
# The card-visibility edits are re-applied every 300 ms for the rest of the
# capture so the UI's own polling (which toggles display on some cards) cannot
# undo them. Where a card's visibility follows mock data (board revision, speed
# source...), a per-shot "api" override is cleaner than "show".
# Any intro animation is short-circuited so the capture shows settled values.
# The config's "js" (top level: every shot; per shot: that shot only) is
# spliced in at /*SHOT_JS*/ and runs on every apply() tick, so it must be
# idempotent. It is for UI state no URL option reaches: painting canvases the
# page only repaints from requestAnimationFrame, opening a <details>, seeding
# a history graph, typing into a filter box...
DRIVER_JS = r"""
<script>
(function () {
  var q = new URLSearchParams(location.search);
  var page = q.get("page"), focus = q.get("focus"), only = q.get("only"), w = q.get("w"), fit = q.get("fit");
  function list(k) { return (q.get(k) || "").split("|").filter(Boolean); }
  var open = list("open"), hide = list("hide"), keepCards = list("cards"), show = list("show"), clicks = list("click");
  if (w) {
    document.documentElement.style.width = w + "px";
    document.documentElement.style.overflowX = "hidden";
    document.body.style.width = w + "px";
    document.body.style.overflowX = "hidden";
  }
  function cards() { return Array.prototype.slice.call(document.querySelectorAll(".card")); }
  function cardByTitle(t) {
    // exact match first, then prefix (titles like "CAN Output - 0x1E0" carry live values)
    var hit = null, pre = null;
    cards().forEach(function (c) {
      var h = c.querySelector("h2"); if (!h) return;
      var s = h.textContent.replace(/\s+/g, " ").trim();
      if (s === t) hit = hit || c; else if (s.indexOf(t) === 0) pre = pre || c;
    });
    return hit || pre;
  }
  function settle() {
    // Headless virtual time does not drive requestAnimationFrame, so any
    // rAF-based intro (e.g. the gauge sweep) never lands. Turn it off and
    // pull one more poll so the gauges show the mocked values.
    try { if (typeof introSweep !== "undefined") { introSweep.active = false; introSweep.pending = null; } } catch (e) {}
    try { if (typeof refreshStatus === "function") refreshStatus(); } catch (e) {}
  }
  function hook() {
    try {
/*SHOT_JS*/
    } catch (e) { console.warn("shot js failed", e); }
  }
  function apply() {
    show.forEach(function (t) { var c = cardByTitle(t); if (c) { c.style.display = ""; c.hidden = false; c.classList.remove("hidden"); } });
    if (q.get("openall")) cards().forEach(function (c) { c.classList.remove("collapsed"); });
    open.forEach(function (t) { var c = cardByTitle(t); if (c) c.classList.remove("collapsed"); });
    hide.forEach(function (t) { var c = cardByTitle(t); if (c) c.style.display = "none"; });
    if (keepCards.length) {
      var keep = keepCards.map(cardByTitle).filter(Boolean);
      cards().forEach(function (c) {
        if (keep.indexOf(c) >= 0) { c.classList.remove("collapsed"); c.style.display = ""; }
        else c.style.display = "none";
      });
    }
    if (only) {
      var k = cardByTitle(only);
      if (k) {
        k.classList.remove("collapsed");
        cards().forEach(function (c) { if (c !== k) c.style.display = "none"; });
        var tb = document.querySelector(".topbar"), nv = document.querySelector(".nav-tabs") || document.querySelector(".tabs");
        if (tb) tb.style.display = "none";
        if (nv) nv.style.display = "none";
      }
    }
    if (focus) {
      var f = cardByTitle(focus);
      if (f) {
        f.classList.remove("collapsed");
        var pg = f.closest(".page") || f.closest(".panel") || document.body;
        var before = true;
        Array.prototype.slice.call(pg.querySelectorAll(".card")).forEach(function (c) {
          if (c === f) { before = false; return; }
          if (before) c.style.display = "none";
        });
      }
    }
    hook();
    window.scrollTo(0, 0);
  }
  function report() {
    if (!fit) return;
    var h = Math.max(document.documentElement.scrollHeight, document.body.scrollHeight);
    try { fetch("/__shot/height?id=" + encodeURIComponent(fit) + "&h=" + h); } catch (e) {}
  }
  function run() {
    if (page) {
      var b = document.querySelector('.nav-tab[data-page="' + page + '"], .tab[data-tab="' + page + '"], [data-page="' + page + '"], [data-tab="' + page + '"]');
      if (b) b.click();
    }
    clicks.forEach(function (t) {
      var btns = Array.prototype.slice.call(document.querySelectorAll("button, .btn, [role=button]"));
      var hit = btns.filter(function (b) { return b.textContent.replace(/\s+/g, " ").trim() === t; })[0];
      if (hit) hit.click();
    });
    apply();
    settle();
    setTimeout(function () { settle(); apply(); }, 400);
    setTimeout(function () { apply(); report(); }, 1400);
    setInterval(apply, 300); // keep winning against the UI's own poll-driven show/hide
    document.documentElement.setAttribute("data-shot-ready", "1");
  }
  window.addEventListener("load", function () { setTimeout(run, 2500); });
})();
</script>
"""


def find_browser(cfg):
    for p in cfg.get("chrome", []) + BROWSERS:
        if p and os.path.isfile(p):
            return p
    sys.exit("no Chrome/Edge found - set \"chrome\" in the config")


class MockHandler(http.server.SimpleHTTPRequestHandler):
    """Static files from data_dir; JSON from the config's "api" map; extra
    roots (the temp dir with rendered shots / poster) under their own prefixes."""

    cfg = None
    data_dir = None
    extra = {}  # url prefix -> directory
    api_override = {}  # per-shot: path -> dict merged over cfg["api"][path]
    shot_js = ""  # per-shot "js", spliced into the driver after the config's
    heights = {}  # fit id -> document height reported by the driver
    served = set()  # paths already answered once in this shot (for _first/_then)

    def log_message(self, *a):  # quiet
        pass

    def _api_key(self, path, query):
        # A mock keyed with its query string ("/api/gauges?group=2") answers
        # that exact request; the bare path is the fallback for everything
        # else (the usual case - queries are ignored).
        full = path + "?" + query if query else path
        api = self.cfg.get("api", {})
        if query and (full in api or full in self.api_override):
            return full
        if path in api or path in self.api_override:
            return path
        return None

    def _api(self, path):
        base = self.cfg.get("api", {}).get(path)
        over = self.api_override.get(path)
        if over is None:
            val = base
        elif isinstance(base, dict) and isinstance(over, dict):
            val = dict(base)
            val.update(over)
        else:
            val = over
        # {"_first": X, "_then": Y}: X on the first request of a shot, Y after -
        # for incremental feeds (?since= logs) the UI would otherwise re-append
        # on every poll.
        if isinstance(val, dict) and "_first" in val:
            first = path not in self.served
            self.served.add(path)
            return val["_first"] if first else val.get("_then", {})
        return val

    def _send_json(self, obj, code=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_bytes(self, body, ctype):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _route(self):
        split = urllib.parse.urlsplit(self.path)
        path = split.path
        if path == "/__shot/height":
            qs = urllib.parse.parse_qs(split.query)
            try:
                self.heights[qs.get("id", [""])[0]] = int(qs.get("h", ["0"])[0])
            except ValueError:
                pass
            self._send_json({"ok": True})
            return True
        key = self._api_key(path, split.query)
        if key is not None:
            val = self._api(key)
            # {"_b64": "...", "_type": "..."}: a binary answer (e.g. a data-log piece), base64 in the config
            if isinstance(val, dict) and "_b64" in val:
                self._send_bytes(base64.b64decode(val["_b64"]), val.get("_type", "application/octet-stream"))
                return True
            self._send_json(val)
            return True
        for prefix, d in self.extra.items():
            if path.startswith(prefix):
                fp = os.path.join(d, path[len(prefix):].lstrip("/"))
                if os.path.isfile(fp):
                    ctype = "text/html; charset=utf-8" if fp.endswith(".html") else "image/png"
                    with open(fp, "rb") as f:
                        self._send_bytes(f.read(), ctype)
                    return True
        if path in ("/", "/index.html"):
            with open(os.path.join(self.data_dir, "index.html"), "r", encoding="utf-8") as f:
                html = f.read()
            driver = DRIVER_JS.replace("/*SHOT_JS*/", self.cfg.get("js", "") + "\n" + self.shot_js)
            html = html.replace("</body>", driver + "</body>") if "</body>" in html else html + driver
            self._send_bytes(html.encode("utf-8"), "text/html; charset=utf-8")
            return True
        fp = os.path.join(self.data_dir, path.lstrip("/"))
        if os.path.isfile(fp):
            ctype = {"css": "text/css", "js": "application/javascript", "json": "application/json",
                     "png": "image/png", "svg": "image/svg+xml"}.get(fp.rsplit(".", 1)[-1], "application/octet-stream")
            with open(fp, "rb") as f:
                self._send_bytes(f.read(), ctype)
            return True
        return False

    def do_GET(self):
        if not self._route():
            self._send_json({"error": "not mocked: " + self.path}, 404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n:
            self.rfile.read(n)
        split = urllib.parse.urlsplit(self.path)
        key = self._api_key(split.path, split.query)
        self._send_json((self._api(key) if key else None) or {"ok": True})


def start_server(cfg, data_dir, extra):
    MockHandler.cfg = cfg
    MockHandler.data_dir = data_dir
    MockHandler.extra = extra
    socketserver.TCPServer.allow_reuse_address = True
    srv = socketserver.ThreadingTCPServer(("127.0.0.1", 0), MockHandler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def shoot(browser, url, out, width, height, scale, profile, budget_ms=9000):
    cmd = [
        browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--no-first-run",
        "--disable-extensions", "--mute-audio",
        "--force-device-scale-factor=%s" % scale,
        "--window-size=%d,%d" % (width, height),
        "--virtual-time-budget=%d" % budget_ms,
        "--user-data-dir=%s" % profile,
        "--screenshot=%s" % out,
        url,
    ]
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=120)
    if not os.path.isfile(out):
        sys.exit("screenshot failed for %s\n%s" % (url, r.stderr.decode("utf-8", "replace")[-2000:]))


def shot_url(port, s, width, fit_id=None):
    q = {"w": width}
    for k in ("page", "open", "openall", "hide", "focus", "only", "cards", "show", "click"):
        if k in s:
            q[k] = s[k]
    if fit_id:
        q["fit"] = fit_id
    return "http://127.0.0.1:%d/index.html?%s" % (port, urllib.parse.urlencode(q))


# "fit": true shots are captured at exactly the document height: pass one
# renders at a short viewport (so scrollHeight is the content height, not the
# window), the driver reports that height to the mock server, and pass two
# re-shoots at that height. Bounded so a runaway page cannot produce a
# gigantic PNG.
FIT_MIN, FIT_MAX, FIT_PROBE = 480, 2600, 420


def shoot_page(browser, port, s, w, h, scale, profile, tmp):
    out = os.path.join(tmp, s["file"])
    MockHandler.api_override = s.get("api", {})
    MockHandler.shot_js = s.get("js", "")
    budget = s.get("budget_ms", 9000)

    def one(url, height):
        MockHandler.served = set()
        shoot(browser, url, out, w, height, scale, profile, budget)

    try:
        if not s.get("fit"):
            one(shot_url(port, s, w), h)
            return w, h
        MockHandler.heights.pop(s["file"], None)
        one(shot_url(port, s, w, s["file"]), FIT_PROBE)
        doc_h = MockHandler.heights.get(s["file"])
        if not doc_h:
            print("  warn  %s: no height reported, using %dpx" % (s["file"], h))
            doc_h = h
        fit_h = max(FIT_MIN, min(FIT_MAX, doc_h + int(s.get("fit_pad", 0))))
        one(shot_url(port, s, w), fit_h)
        return w, fit_h
    finally:
        MockHandler.api_override = {}
        MockHandler.shot_js = ""


# ---------------------------------------------------------------------------
# Composition pages. These are HTML in the shared theme's palette; the page
# shots are dropped in as <img>. Rendered by the same headless browser.
# ---------------------------------------------------------------------------
THEME_CSS = """
:root{--bg:#0b0f14;--panel:#131a22;--panel-2:#182129;--line:#24303b;--text:#e6edf3;--muted:#7d8ea0;--accent:#24d1c4;--accent-2:#ffb02e;--ok:#3fd07f}
*{box-sizing:border-box;margin:0;padding:0}
html,body{background:var(--bg);color:var(--text);font-family:"Segoe UI",Roboto,system-ui,sans-serif;-webkit-font-smoothing:antialiased}
body{position:relative;overflow:hidden}
.grid-bg{position:absolute;inset:0;background-image:linear-gradient(var(--line) 1px,transparent 1px),linear-gradient(90deg,var(--line) 1px,transparent 1px);background-size:48px 48px;opacity:.28;mask-image:radial-gradient(ellipse at 50% 30%,#000 30%,transparent 80%);-webkit-mask-image:radial-gradient(ellipse at 50% 30%,#000 30%,transparent 80%)}
.glow{position:absolute;left:-10%;top:-30%;width:60%;height:60%;background:radial-gradient(closest-side,rgba(36,209,196,.16),transparent);pointer-events:none}
.wrap{position:relative}
"""

POSTER_HTML = """<!doctype html><html><head><meta charset="utf-8"><style>
%(theme)s
.wrap{padding:44px 48px 36px}
h1{font-size:54px;font-weight:800;letter-spacing:-1px;line-height:1}
h1 span{color:var(--accent)}
.tag{margin-top:12px;font-size:13px;letter-spacing:4px;text-transform:uppercase;color:var(--muted)}
.rule{height:2px;margin:18px 0 26px;background:linear-gradient(90deg,var(--accent),rgba(36,209,196,0));position:relative}
.rule::before{content:"";position:absolute;left:0;top:-3px;width:8px;height:8px;border-radius:50%%;background:var(--accent);box-shadow:0 0 12px var(--accent)}
.grid{display:grid;grid-template-columns:repeat(%(cols)d,1fr);gap:26px %(gapx)dpx}
.cell{text-align:center}
.lbl{font-size:11px;letter-spacing:3px;text-transform:uppercase;color:var(--accent);margin-bottom:12px}
.phone{position:relative;width:%(pw)dpx;height:%(ph)dpx;margin:0 auto;border-radius:34px;background:#0b0f14;border:1px solid #2a3642;box-shadow:0 0 0 6px #10161d,0 0 0 7px #2a3642,0 30px 60px rgba(0,0,0,.6);overflow:hidden}
/* status bar: the app starts below it, like a real phone, and the island sits in it */
.phone .bar{position:absolute;left:0;right:0;top:0;height:36px;background:#0b0f14;z-index:2}
.phone .bar::after{content:"";position:absolute;left:50%%;top:9px;width:78px;height:18px;margin-left:-39px;border-radius:12px;background:#05070a;border:1px solid #1e2732}
.phone .bar .t{position:absolute;left:22px;top:11px;font-size:10px;font-weight:600;color:var(--text)}
.phone .bar .s{position:absolute;right:20px;top:12px;width:34px;height:8px;border-radius:2px;background:linear-gradient(90deg,var(--text) 0 82%%,transparent 82%%);opacity:.75}
.phone img{position:absolute;left:0;top:36px;width:100%%;height:auto;display:block}
.phone .fade{position:absolute;left:0;right:0;bottom:0;height:70px;background:linear-gradient(180deg,rgba(11,15,20,0),rgba(11,15,20,.94));z-index:1}
.foot{display:flex;flex-direction:column;align-items:stretch;margin-top:30px;gap:14px}
.foot .link{align-self:flex-end}
.pills{display:flex;flex-wrap:wrap;gap:8px}
.pill{font-size:11px;letter-spacing:1px;padding:5px 12px;border-radius:999px;border:1px solid rgba(36,209,196,.55);color:var(--accent);background:rgba(36,209,196,.06)}
.link{font-size:12px;color:var(--text);display:flex;align-items:center;gap:8px;white-space:nowrap}
.link svg{width:16px;height:16px;fill:var(--text)}
</style></head><body><div class="grid-bg"></div><div class="glow"></div><div class="wrap">
<h1>%(title)s</h1><div class="tag">%(tagline)s</div><div class="rule"></div>
<div class="grid">%(cells)s</div>
<div class="foot"><div class="pills">%(pills)s</div><div class="link"><svg viewBox="0 0 16 16"><path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z"/></svg>%(link)s</div></div>
</div></body></html>"""

STRIP_HTML = """<!doctype html><html><head><meta charset="utf-8"><style>
%(theme)s
.wrap{padding:36px 40px}
h1{text-align:center;font-size:40px;font-weight:800;letter-spacing:-.5px}
h1 span{color:var(--accent)}
.sub{text-align:center;color:var(--muted);font-size:14px;margin-top:6px;letter-spacing:2px;text-transform:uppercase}
.row{display:grid;grid-template-columns:repeat(%(cols)d,1fr);gap:%(gap)dpx;margin-top:30px}
.pane{position:relative;height:%(ph)dpx;border-radius:18px;overflow:hidden;border:1px solid var(--line);background:#0a0d11;box-shadow:0 24px 50px rgba(0,0,0,.55)}
.pane img{width:100%%;height:auto;display:block}
.pane .fade{position:absolute;left:0;right:0;bottom:0;height:60px;background:linear-gradient(180deg,rgba(10,13,17,0),rgba(10,13,17,.95))}
.cap{margin-top:10px;text-align:center;font-size:12px;letter-spacing:3px;text-transform:uppercase;color:var(--accent)}
</style></head><body><div class="grid-bg"></div><div class="glow"></div><div class="wrap">
<h1>%(title)s</h1><div class="sub">%(subtitle)s</div>
<div class="row">%(panes)s</div>
</div></body></html>"""


def esc(s):
    return (s or "").replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def rel(p):
    try:
        return os.path.relpath(p, ROOT)
    except ValueError:  # different drive on Windows
        return p


def title_html(t):
    # "OpenHaldex-C6" -> "Open<span>Haldex</span>-C6" when the config gives "Open|Haldex|-C6"
    parts = t.split("|")
    if len(parts) == 3:
        return esc(parts[0]) + "<span>" + esc(parts[1]) + "</span>" + esc(parts[2])
    return esc(t)


def compose_poster(p, shots_by_file, tmp, html_name="poster.html"):
    cols = int(p.get("cols", 4))
    pw, ph = p.get("phone", [236, 430])
    cells = []
    for ref in p["shots"]:
        s = shots_by_file[ref]
        cells.append('<div class="cell"><div class="lbl">%s</div><div class="phone"><div class="bar"><span class="t">%s</span><span class="s"></span></div>'
                     '<img src="/tmp/%s"><div class="fade"></div></div></div>'
                     % (esc(s.get("label", "")), esc(p.get("clock", "12:00")), s["file"]))
    pills = "".join('<span class="pill">%s</span>' % esc(x) for x in p.get("pills", []))
    html = POSTER_HTML % {
        "theme": THEME_CSS, "cols": cols, "gapx": int(p.get("gapx", 28)), "pw": pw, "ph": ph,
        "title": title_html(p["title"]), "tagline": esc(p.get("tagline", "")),
        "cells": "".join(cells), "pills": pills, "link": esc(p.get("link", "")),
    }
    with open(os.path.join(tmp, html_name), "w", encoding="utf-8") as f:
        f.write(html)


def compose_strip(p, shots_by_file, tmp, html_name="strip.html"):
    panes = []
    for ref in p["shots"]:
        s = shots_by_file[ref]
        panes.append('<div><div class="pane"><img src="/tmp/%s"><div class="fade"></div></div><div class="cap">%s</div></div>'
                     % (s["file"], esc(s.get("label", ""))))
    html = STRIP_HTML % {
        "theme": THEME_CSS, "cols": len(p["shots"]), "gap": int(p.get("gap", 24)), "ph": int(p.get("pane_height", 640)),
        "title": title_html(p["title"]), "subtitle": esc(p.get("subtitle", "")), "panes": "".join(panes),
    }
    with open(os.path.join(tmp, html_name), "w", encoding="utf-8") as f:
        f.write(html)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("config")
    ap.add_argument("--only", choices=["shots", "poster"], help="shots = page screenshots only; poster = recompose from existing shots")
    ap.add_argument("--keep", action="store_true", help="keep the temp dir (rendered HTML + raw shots)")
    args = ap.parse_args()

    with open(args.config, encoding="utf-8") as f:
        cfg = json.load(f)
    cfg_dir = os.path.dirname(os.path.abspath(args.config))
    # "api_include": more mock endpoints from files next to the config - for a
    # generated catalogue too big to keep the config readable.
    for inc in cfg.get("api_include", []):
        with open(os.path.join(cfg_dir, inc), encoding="utf-8") as f:
            cfg.setdefault("api", {}).update(json.load(f))
    data_dir = os.path.normpath(os.path.join(ROOT, cfg.get("data_dir", "data")))
    out_dir = os.path.normpath(os.path.join(ROOT, cfg.get("out_dir", "Images")))
    os.makedirs(out_dir, exist_ok=True)
    browser = find_browser(cfg)
    vw, vh = cfg.get("viewport", [390, 844])
    scale = cfg.get("scale", 2)

    tmp = tempfile.mkdtemp(prefix="readme_shots_")
    profile = os.path.join(tmp, "profile")
    srv, port = start_server(cfg, data_dir, {"/tmp/": tmp})
    print("serving %s on :%d (browser: %s)" % (data_dir, port, os.path.basename(browser)))

    shots = cfg.get("shots", [])
    by_file = {s["file"]: s for s in shots}
    # Every page shot is published to <out_dir> (or <out_dir>/<shots_subdir>
    # when the config sets one) - they go in the README on their own, and they
    # are the raw material --only poster recomposes from.
    ui_dir = os.path.normpath(os.path.join(out_dir, cfg.get("shots_subdir", "")))
    os.makedirs(ui_dir, exist_ok=True)
    try:
        if args.only != "poster":
            for s in shots:
                w, h = s.get("viewport", [vw, vh])
                w, h = shoot_page(browser, port, s, w, h, scale, profile, tmp)
                shutil.copy2(os.path.join(tmp, s["file"]), os.path.join(ui_dir, s["file"]))
                print("  shot  %-28s %dx%d @%sx  -> %s/" % (s["file"], w, h, scale, rel(ui_dir)))
        else:
            for s in shots:
                src = os.path.join(ui_dir, s["file"])
                if os.path.isfile(src):
                    shutil.copy2(src, os.path.join(tmp, s["file"]))

        if args.only != "shots":
            # "poster" (one) or "posters" (a list - a two-page overview when one
            # 4x2 grid cannot hold every card the README talks about), plus "strip".
            jobs = [(p, compose_poster, "poster%d.html" % i)
                    for i, p in enumerate(cfg.get("posters") or ([cfg["poster"]] if cfg.get("poster") else []))]
            if cfg.get("strip"):
                jobs.append((cfg["strip"], compose_strip, "strip.html"))
            for p, fn, html in jobs:
                key = "strip" if fn is compose_strip else "poster"
                missing = [r for r in p["shots"] if not os.path.isfile(os.path.join(tmp, r))]
                if missing:
                    sys.exit("%s needs shots that were not rendered: %s" % (p["file"], ", ".join(missing)))
                fn(p, by_file, tmp, html)
                w, h = p["size"]
                out = os.path.join(out_dir, p["file"])
                shoot(browser, "http://127.0.0.1:%d/tmp/%s" % (port, html), out, w, h, p.get("scale", scale), profile, 6000)
                print("  %-6s%-28s %dx%d @%sx  -> Images/" % (key, p["file"], w, h, p.get("scale", scale)))
    finally:
        srv.shutdown()
        if args.keep:
            print("temp kept: " + tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
