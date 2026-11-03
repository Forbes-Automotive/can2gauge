#!/usr/bin/env python3
"""
README renders of the assembled gauge: iso, front, rear (ports called out), rear with the cap off, exploded, and a
turntable GIF.

The housing is the real printable V3.6 STLs (enclosure/can2gauge_52mm_STL_V3-6.zip); the board is the routed EasyEDA
file (newest pcb/*.epro2): its outline, and every component as a box at its real position, side and footprint size
(the same method as enclosure/assembly4.py, without numpy). The glass shows a real page from the simulator
(images/glass-page-*.png, made by glass_shots.py). three.js renders it in headless Chrome (WebGL on SwiftShader);
Pillow adds the callouts.

    py tools/readme_shots/unit_renders.py            every view -> images/unit-*.png, unit-turntable.gif
    py tools/readme_shots/unit_renders.py rear open  only those views

Needs Python 3 with Pillow, Chrome or Edge, and internet for three.js (cdn.jsdelivr.net).
"""
import base64
import collections
import glob
import http.server
import io
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import zipfile

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "images")
sys.path.insert(0, os.path.join(ROOT, "pcb", "generator"))
sys.path.insert(0, os.path.join(ROOT, "enclosure"))
sys.path.insert(0, HERE)
import epru  # noqa: E402
import fp  # noqa: E402
import geom  # noqa: E402
import cap4  # noqa: E402
from glass_shots import font, BG, TEXT, MUTED, ACCENT, BROWSERS  # noqa: E402

STL_ZIP = os.path.join(ROOT, "enclosure", "can2gauge_52mm_STL_V3-6.zip")
MIL = 39.3700787

# the stack, from enclosure pod5.py (V3.6) - model frame M: seen from the front, z = depth back from the glass front
LCM_T, GLASS_R, AA_R = 2.26, 58.18 / 2, 53.28 / 2
NECK_HW, NECK_Y = 33.6 / 2, 58.18 / 2 - 60.71
Z_PCB_F, Z_PCB_B = 6.0, 7.6
CAP_Z0, CAP_Z1, STUD_H = 16.3, 19.3, 5.0
SCREW_L = 12.0

# component heights / colours, as enclosure/assembly4.py
HT = [(r'ESP32', 3.2), (r'TYPE-C|TYPE C', 3.2), (r'PASK', 9.0), (r'FPC-05FB|FPC-SMD', 2.0), (r'TS-1088', 1.6), (r'RS0530', 3.0),
      (r'FNR40|4030', 3.0), (r'SS510|SS14|SMA', 2.4), (r'SOT-223|AMS1117', 1.8), (r'SOD-123', 1.1), (r'HTSSOP|TSSOP', 1.2),
      (r'SOIC-8|L9637', 1.75), (r'VSON', 1.0), (r'SOT-583|TPS62933', 0.6), (r'SOT', 1.2), (r'LED0603|XL-1608', 0.8),
      (r'1206', 0.9), (r'0805', 1.3), (r'0603', 0.9)]
CAT = {'module': '#c4c7cc', 'usb': '#aab0b8', 'xh': '#efe6c8', 'fpc': '#3b3128', 'ic': '#1b1b1b', 'passive': '#a8926a',
       'res': '#262626', 'ind': '#3a3a3a', 'sw': '#50555c', 'led': '#39c46a', 'diode': '#2c2c2c'}


def height(t):
    for p, h in HT:
        if re.search(p, t, re.I):
            return h
    return 1.0


def classify(t):
    if 'ESP32' in t: return 'module'
    if 'TYPE-C' in t or 'TYPE C' in t: return 'usb'
    if 'PASK' in t: return 'xh'
    if 'FPC' in t: return 'fpc'
    if 'TS-1088' in t: return 'sw'
    if 'XL-1608' in t or 'LED0603' in t: return 'led'
    if 'RS0530' in t or 'FNR40' in t: return 'ind'
    if any(k in t for k in ('SS510', 'SMF', 'SS14')): return 'diode'
    if any(k in t for k in ('CL10', 'CL21', 'HGC', 'BSMD', 'GRM', 'CC0603', 'C0805', 'C0603')): return 'passive'
    if re.search(r'R0603|R1206|0603W|FRC|RT0603|1206W', t): return 'res'
    return 'ic'


class Shim:
    def __init__(self, fpd):
        self.dev = (None, None, None, fpd)


def board(epro2):
    """component boxes (model frame M), and the reference positions used for the callouts"""
    with zipfile.ZipFile(epro2) as z:
        name = [n for n in z.namelist() if n.endswith(".epru")][0]
        tmp = os.path.join(tempfile.mkdtemp(prefix="unit_"), "board.epru")
        open(tmp, "wb").write(z.read(name))
    docs = epru.parse(tmp)
    byu = {d['body'].get('uuid'): d for d in docs}
    pcb = [d for d in docs if d['body'].get('docType') == 'PCB'][0]
    comps = {h['id']: b for h, b in epru.records_of(pcb, 'COMPONENT') if b}
    pat = collections.defaultdict(dict)
    for h, b in epru.records_of(pcb, 'ATTR'):
        if b and b.get('parentId') in comps:
            pat[b['parentId']][b['key']] = b.get('value')
    boxes, refs = [], {}
    for cid, a in pat.items():
        r = a.get('Designator')
        fpd, dev = byu.get(a.get('Footprint')), byu.get(a.get('Device'))
        if not r or not fpd:
            continue
        c = comps[cid]
        x, y, ang, side = c['x'] / MIL, -c['y'] / MIL, c.get('angle') or 0, 'B' if c['layerId'] == 2 else 'T'
        dt = dev['records'][0][1]['title'] if dev else ''
        ft = fpd['records'][0][1].get('title', '') if fpd['records'] and fpd['records'][0][0]['type'] == 'META' else ''
        t = dt + ' ' + ft
        sh, bot = Shim(fpd), side == 'B'
        a2 = (-ang) % 360 if side == 'T' else (180 - ang) % 360
        cat, hh = classify(t), height(t)
        if cat == 'module':
            pads = fp.world_pads(sh, x, y, a2, bot)
            ys, xs = [p[2] for p in pads], [p[1] for p in pads]
            cx, top = (min(xs) + max(xs)) / 2, max(ys) + 1.2
            x0, x1, y0, y1 = cx - 9, cx + 9, top - 25.5 + 6.5, top + 6.5
            boxes.append(dict(cat='ic', b=[x0, x1, y0, y1, Z_PCB_B, Z_PCB_B + 0.8]))
            boxes.append(dict(cat='module', b=[x0 + 0.8, x1 - 0.8, y0 + 0.8, y1 - 6.8, Z_PCB_B + 0.8, Z_PCB_B + 3.2]))
            refs[r] = ((x0 + x1) / 2, (y0 + y1) / 2 - 3, Z_PCB_B + 3.2)
            continue
        body = fp.world_body(sh, x, y, a2, bot)
        if body:
            xs, ys = [p[0] for p in body], [p[1] for p in body]
            bx = (min(xs), max(xs), min(ys), max(ys))
        else:
            bx = fp.world_box(sh, x, y, a2, bot, 0.0)
        z0, z1 = (Z_PCB_F - hh, Z_PCB_F) if bot else (Z_PCB_B, Z_PCB_B + hh)
        boxes.append(dict(cat=cat, b=[bx[0], bx[1], bx[2], bx[3], z0, z1]))
        refs[r] = ((bx[0] + bx[1]) / 2, (bx[2] + bx[3]) / 2, z1 if not bot else z0)
    return boxes, refs


def t3(p):
    """model frame M -> three.js (front towards +z)"""
    return [p[0], p[1], -p[2]]


# ------------------------------------------------------------------------------------------------------------------------
PAGE = r"""<!doctype html><html><head><meta charset="utf-8">
<style>html,body{margin:0;background:#0b0f14}canvas{display:block}</style>
<script type="importmap">{"imports":{"three":"https://cdn.jsdelivr.net/npm/three@0.160.0/build/three.module.js",
"three/addons/":"https://cdn.jsdelivr.net/npm/three@0.160.0/examples/jsm/"}}</script></head><body>
<script type="module">
import * as THREE from 'three';
import { STLLoader } from 'three/addons/loaders/STLLoader.js';
import { RoomEnvironment } from 'three/addons/environments/RoomEnvironment.js';
const post = (path, body) => fetch(path, { method: 'POST', body });
try {
const D = await (await fetch('scene.json')).json();
const W = D.size[0], H = D.size[1];
const renderer = new THREE.WebGLRenderer({ antialias: true, preserveDrawingBuffer: true, alpha: true });
renderer.setPixelRatio(D.ss); renderer.setSize(W, H);   // supersampled: SwiftShader has no MSAA
renderer.toneMapping = THREE.ACESFilmicToneMapping; renderer.toneMappingExposure = 1.3;
renderer.outputColorSpace = THREE.SRGBColorSpace;
document.body.appendChild(renderer.domElement);
const scene = new THREE.Scene();
const pmrem = new THREE.PMREMGenerator(renderer);
scene.environment = pmrem.fromScene(new RoomEnvironment(), 0.04).texture;
scene.add(new THREE.HemisphereLight(0xffffff, 0x404650, 0.6));
const key = new THREE.DirectionalLight(0xffffff, 1.7); key.position.set(-120, 160, 200); scene.add(key);
const rim = new THREE.DirectionalLight(0x9fe9e2, 0.9); rim.position.set(160, 60, -200); scene.add(rim);
const fill = new THREE.DirectionalLight(0xffffff, 0.7); fill.position.set(140, 30, 240); scene.add(fill);
const back = new THREE.DirectionalLight(0xffffff, 1.1); back.position.set(-60, 120, -260); scene.add(back);

// ---- groups that move as one in the exploded view
const G = {}; for (const k of ['bezel', 'glass', 'body', 'pcb', 'cap', 'screws', 'bracket']) { G[k] = new THREE.Group(); scene.add(G[k]); }
const std = (c, m = 0, r = 0.6) => new THREE.MeshStandardMaterial({ color: c, metalness: m, roughness: r });
const loader = new STLLoader();
for (const p of D.stl) {
  const geo = loader.parse(await (await fetch(p.file)).arrayBuffer());
  geo.scale(-1, 1, -1);            // the printable part -> three.js frame (a 180 deg turn about y)
  geo.computeVertexNormals();
  G[p.group].add(new THREE.Mesh(geo, std(p.color, p.metal, p.rough)));
}
// glass: the LCM (disc + neck), the lit screen, a glossy cover
const lcm = new THREE.Group();
const disc = new THREE.Mesh(new THREE.CylinderGeometry(D.glassR, D.glassR, D.lcmT, 96), std(0x0c0e11, 0.3, 0.4));
disc.rotation.x = Math.PI / 2; disc.position.z = -D.lcmT / 2; lcm.add(disc);
const neck = new THREE.Mesh(new THREE.BoxGeometry(2 * D.neckHW, 14, D.lcmT), std(0x0c0e11, 0.3, 0.4));
neck.position.set(0, D.neckY + 7, -D.lcmT / 2); lcm.add(neck);
const tex = await new THREE.TextureLoader().loadAsync(D.screen); tex.colorSpace = THREE.SRGBColorSpace;
const scr = new THREE.Mesh(new THREE.CircleGeometry(D.aaR, 128), new THREE.MeshBasicMaterial({ map: tex, toneMapped: false }));
scr.position.z = 0.02; lcm.add(scr);
const cover = new THREE.Mesh(new THREE.CircleGeometry(D.glassR, 128), new THREE.MeshPhysicalMaterial({ color: 0x000000, metalness: 0, roughness: 0.05, transparent: true, opacity: 0.18, clearcoat: 1 }));
cover.position.z = 0.05; lcm.add(cover);
G.glass.add(lcm);
// the board: outline extruded 1.6 mm, every component as a box
const sh = new THREE.Shape(D.outline.map(([x, y]) => new THREE.Vector2(x, y)));
const pcbM = new THREE.Mesh(new THREE.ExtrudeGeometry(sh, { depth: D.pcb[1] - D.pcb[0], bevelEnabled: false, curveSegments: 64 }), std(0x14532d, 0.1, 0.55));
pcbM.position.z = -D.pcb[1]; G.pcb.add(pcbM);
const mats = {}; for (const [k, c] of Object.entries(D.cat)) mats[k] = std(c, k === 'module' || k === 'usb' ? 0.8 : 0.05, k === 'module' || k === 'usb' ? 0.35 : 0.6);
for (const b of D.boxes) {
  const [x0, x1, y0, y1, z0, z1] = b.b;
  const m = new THREE.Mesh(new THREE.BoxGeometry(Math.max(0.1, x1 - x0), Math.max(0.1, y1 - y0), Math.max(0.1, z1 - z0)), mats[b.cat]);
  m.position.set((x0 + x1) / 2, (y0 + y1) / 2, -(z0 + z1) / 2); G.pcb.add(m);
}
// screws + inserts
for (const [x, y] of D.holes) {
  const s = new THREE.Mesh(new THREE.CylinderGeometry(1.0, 1.0, D.screwL, 24), std(0x1a1c20, 0.6, 0.4));
  s.rotation.x = Math.PI / 2; s.position.set(x, y, -(D.capZ0 - D.screwL / 2)); G.screws.add(s);
  const h = new THREE.Mesh(new THREE.CylinderGeometry(1.9, 1.9, 2.0, 32), std(0x1a1c20, 0.6, 0.4));
  h.rotation.x = Math.PI / 2; h.position.set(x, y, -(D.capZ0 + 1.0)); G.screws.add(h);
}
// ---- views
const ret = [];
for (const v of D.views) {
  for (const [k, g] of Object.entries(G)) { g.visible = !(v.hide || []).includes(k); g.position.set(0, 0, 0); g.rotation.set(0, 0, 0); }
  for (const [k, off] of Object.entries(v.explode || {})) G[k].position.set(off[0], off[1], off[2]);
  const cam = v.ortho ? new THREE.OrthographicCamera(-v.ortho * W / H, v.ortho * W / H, v.ortho, -v.ortho, 1, 2000)
                      : new THREE.PerspectiveCamera(v.fov || 22, W / H, 1, 2000);
  cam.position.set(...v.cam); cam.up.set(0, 1, 0); cam.lookAt(...v.look);
  const frames = v.turn || 1;
  for (let i = 0; i < frames; i++) {
    if (v.turn) for (const g of Object.values(G)) g.rotation.y = (i / frames) * Math.PI * 2;
    renderer.render(scene, cam);
    const anchors = {};
    for (const [name, p] of Object.entries(v.labels || {})) {
      const q = new THREE.Vector3(...p); const g = G[v.labelGroup && v.labelGroup[name] || 'pcb'];
      q.add(g.position); const s = q.project(cam);
      anchors[name] = [(s.x + 1) / 2 * W, (1 - s.y) / 2 * H];
    }
    await post('/__frame?view=' + v.name + '&i=' + i, JSON.stringify({ png: renderer.domElement.toDataURL('image/png'), anchors }));
  }
}
await post('/__done', '');
} catch (e) { await post('/__done?error=' + encodeURIComponent(String(e && e.stack || e)), ''); }
</script></body></html>"""


def views(refs):
    """camera + visibility + exploded offsets + callout anchors (three.js frame) for each view"""
    def anc(ref, dz=0.0):
        x, y, z = refs[ref]
        return t3((x, y, z + dz))
    win = {k: ((v[0] + v[1]) / 2, (v[2] + v[3]) / 2) for k, v in cap4.WINDOWS.items()}
    plug_top = Z_PCB_B + cap4.HEIGHTS['J_BUS1']
    rear = {
        # J_BUS2 / J_OUT sit side by side: anchor one high and one low in its window so their leaders don't cross
        "J_BUS1": t3((*win['J_BUS1'], plug_top)), "J_BUS2": t3((win['J_BUS2'][0], cap4.WINDOWS['J_BUS2'][3] - 2.0, plug_top)),
        "J_OUT": t3((win['J_OUT'][0], cap4.WINDOWS['J_OUT'][2] + 2.5, plug_top)),
        "USB-C": anc('USB_ESP'), "BOOT": anc('SW_BOOT'), "RESET": anc('SW_RESET'), "LED": anc('LED_PWR'),
        "SCREW": t3((*geom.HOLES[0], CAP_Z0 + 2.0)), "STUD": t3((-2.0, -12.0, CAP_Z1 + STUD_H)),
    }
    lg_cap = {k: 'cap' for k in ('SCREW', 'STUD')}
    open_ = {"ESP32": anc('U8'), "J_LCD": anc('J_LCD'), "USB-C": anc('USB_ESP'), "J_BUS1": anc('J_BUS1'), "J_BUS2": anc('J_BUS2'),
             "J_OUT": anc('J_OUT'), "BOOT": anc('SW_BOOT'), "RESET": anc('SW_RESET')}
    ex = {"bezel": [0, 0, 34], "glass": [0, 0, 18], "pcb": [0, 0, -34], "cap": [0, 0, -52], "screws": [0, 0, -74], "bracket": [0, 0, -66]}
    exl = {"BEZEL": [0, 32, 1.0], "GLASS": [0, 22, 0], "BODY": [0, 30, -4], "PCB": [0, 18, -7.6], "CAP": [0, 20, -19.3],
           "SCREWS": [*geom.HOLES[0], -(CAP_Z0 + 2.0)], "BRACKET": [6, 30, -24]}
    exg = {"BEZEL": "bezel", "GLASS": "glass", "BODY": "body", "PCB": "pcb", "CAP": "cap", "SCREWS": "screws", "BRACKET": "bracket"}
    return [
        dict(name="iso", cam=[-190, 105, 150], look=[0, -3, -11], fov=21),
        dict(name="front", cam=[0, 60, 420], look=[0, 0, -4], fov=13, hide=["bracket"]),
        dict(name="rear", cam=[0, 0, -400], look=[0, 0, -12], ortho=40, hide=["bracket"], labels=rear, labelGroup=lg_cap),
        dict(name="open", cam=[55, 70, -175], look=[0, 0, -8], fov=22, hide=["cap", "screws", "bracket"], labels=open_),
        dict(name="exploded", cam=[-250, 100, 120], look=[0, -2, -20], fov=27, explode=ex, labels=exl, labelGroup=exg),
        dict(name="turn", cam=[0, 18, 230], look=[0, 0, -10], fov=22, turn=36, size=[560, 560]),
    ]


CALLOUTS = {
    "rear": [("J_BUS1", "J_BUS1", "12 V · GND · CAN-H · CAN-L"), ("J_BUS2", "J_BUS2", "the same pins - chain on to the next gauge"),
             ("J_OUT", "J_OUT", "output · K-line · dash lights"), ("USB-C", "USB-C", "flashing + console"),
             ("BOOT", "BOOT", "pin hole"), ("RESET", "RESET", "pin hole"), ("LED", "Power LED", "window in the cap"),
             ("SCREW", "3 × M2 × 12", "into inserts in the head"), ("STUD", "2 × M3 studs", "for the U-bracket")],
    "open": [("ESP32", "ESP32-S3-WROOM-1", "N16R8, antenna up"), ("J_LCD", "J_LCD", "40-pin FPC from the glass"),
             ("USB-C", "USB-C", ""), ("BOOT", "BOOT", ""), ("RESET", "RESET", ""),
             ("J_BUS1", "J_BUS1", "power + CAN"), ("J_BUS2", "J_BUS2", "chain"), ("J_OUT", "J_OUT", "OUT · K · lights")],
    "exploded": [("BEZEL", "Bezel ring", "snap-on"), ("GLASS", "2.1\" 480 × 480 IPS", "Saef, ST7701S"),
                 ("BODY", "Head + body", "Ø52 through the dash"), ("PCB", "Ø48 PCB", "ESP32-S3, plugs on the rear"),
                 ("CAP", "Rear cap", "windows for every plug"), ("SCREWS", "3 × M2 × 12", ""), ("BRACKET", "U-bracket", "clamps it to the dash")],
}


def callouts(img, anchors, items):
    """leader lines to two label columns, de-overlapped, in the house style"""
    W, H = img.size
    d = ImageDraw.Draw(img)
    f1, f2 = font(22, True), font(17)
    pts = [(k, t, s, anchors[k]) for k, t, s in items if k in anchors]
    if not pts:
        return
    mid = sum(p[3][0] for p in pts) / len(pts)
    # ties (anchors at about the same height): the anchor nearer its column takes the upper slot, so leaders don't cross
    cols = {"L": sorted([p for p in pts if p[3][0] < mid], key=lambda p: (round(p[3][1] / 40), p[3][0])),
            "R": sorted([p for p in pts if p[3][0] >= mid], key=lambda p: (round(p[3][1] / 40), -p[3][0]))}
    for side, lst in cols.items():
        gap, prev = 66, 40
        ys = []
        for p in lst:
            y = max(p[3][1], prev + gap)
            ys.append(y)
            prev = y
        over = (ys[-1] if ys else 0) - (H - 50)
        if over > 0:
            ys = [y - over for y in ys]
        for (k, t, s, (ax, ay)), ty in zip(lst, ys):
            tx = 40 if side == "L" else W - 40
            tw = max(d.textlength(t, font=f1), d.textlength(s, font=f2))
            ex = tx + tw + 14 if side == "L" else tx - tw - 14
            d.line([(ex, ty), (ex + (30 if side == "L" else -30), ty), (ax, ay)], fill=ACCENT, width=2)
            d.ellipse((ax - 5, ay - 5, ax + 5, ay + 5), fill=ACCENT, outline=BG, width=2)
            anchor = "ls" if side == "L" else "rs"
            d.text((tx, ty - 2), t, font=f1, fill=TEXT, anchor=anchor)
            if s:
                d.text((tx, ty + 20), s, font=f2, fill=MUTED, anchor=anchor)


def backdrop(size):
    W, H = size
    bg = Image.new("RGB", size, BG)
    glow = Image.radial_gradient("L").resize((int(W * 1.3), int(H * 1.3)))
    glow = glow.point(lambda v: int(max(0, 255 - v) * 0.35))
    tint = Image.new("RGB", glow.size, (34, 48, 60))
    bg.paste(tint, (int(-W * 0.15), int(-H * 0.2)), glow)
    return bg


def main():
    want = sys.argv[1:]
    epro2 = max(glob.glob(os.path.join(ROOT, "pcb", "*.epro2")), key=os.path.getmtime)
    print("board: %s" % os.path.relpath(epro2, ROOT))
    boxes, refs = board(epro2)
    print("  %d component boxes" % len(boxes))
    srv_dir = tempfile.mkdtemp(prefix="unit_renders_")
    stl = []
    with zipfile.ZipFile(STL_ZIP) as z:
        for n in z.namelist():
            m = re.match(r"can2gauge_52mm_(\w+)_V[\d-]+\.stl$", n)
            if not m:
                continue
            part = m.group(1)
            open(os.path.join(srv_dir, part + ".stl"), "wb").write(z.read(n))
            col = {"body": ("#4a5059", 0.0, 0.7), "bezel": ("#2c3138", 0.2, 0.4), "cap": ("#565d67", 0.0, 0.75),
                   "bracket": ("#9ba1a9", 0.7, 0.35)}[part]
            stl.append(dict(file=part + ".stl", group=part, color=col[0], metal=col[1], rough=col[2]))
    # the screen: a real page from the simulator, cropped out of its bezel (glass_shots.round_glass at 420 px: glass at 38)
    shot = Image.open(os.path.join(OUT, "glass-page-needle.png")).convert("RGB").crop((38, 38, 458, 458)).resize((1024, 1024), Image.LANCZOS)
    shot.save(os.path.join(srv_dir, "screen.png"))
    V = [v for v in views(refs) if not want or v["name"] in want]
    W, H = 1600, 1150
    results, done, err = {}, threading.Event(), []

    class H_(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **k):
            super().__init__(*a, directory=srv_dir, **k)

        def log_message(self, *a):
            pass

        def do_POST(self):
            u = urllib.parse.urlsplit(self.path)
            q = urllib.parse.parse_qs(u.query)
            body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
            if u.path == "/__frame":
                j = json.loads(body)
                img = Image.open(io.BytesIO(base64.b64decode(j["png"].split(",", 1)[1]))).convert("RGBA")
                img = img.resize((img.width // 2, img.height // 2), Image.LANCZOS)  # ss = 2 -> back to the view size
                results.setdefault(q["view"][0], []).append((int(q["i"][0]), img, j["anchors"]))
            elif u.path == "/__done":
                if q.get("error"):
                    err.append(q["error"][0])
                done.set()
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.end_headers()

    browser = next(p for p in BROWSERS if os.path.isfile(p))
    for v in V:  # one page load per view: each can have its own size
        size = v.pop("size", [W, H])
        json.dump(dict(size=size, stl=stl, boxes=boxes, cat=CAT, outline=geom.outline_pts(), pcb=[Z_PCB_F, Z_PCB_B],
                       holes=geom.HOLES, capZ0=CAP_Z0, screwL=SCREW_L, glassR=GLASS_R, aaR=AA_R, lcmT=LCM_T, neckHW=NECK_HW,
                       neckY=NECK_Y, screen="screen.png", ss=2, views=[v]), open(os.path.join(srv_dir, "scene.json"), "w"))
        open(os.path.join(srv_dir, "index.html"), "w", encoding="utf-8").write(PAGE)
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H_)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        done.clear()
        prof = tempfile.mkdtemp(prefix="unit_prof_")
        cmd = [browser, "--headless=new", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist",
               "--hide-scrollbars", "--no-first-run", "--force-device-scale-factor=1", "--remote-debugging-port=0",
               "--window-size=%d,%d" % (size[0], size[1]), "--user-data-dir=" + prof,
               "http://127.0.0.1:%d/index.html" % srv.server_address[1]]
        t0 = time.time()
        p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        ok = done.wait(900)
        p.terminate()
        srv.shutdown()
        if not ok or err:
            sys.exit("view %s failed: %s" % (v["name"], err[0] if err else "timed out"))
        frames = sorted(results[v["name"]], key=lambda r: r[0])
        print("  %-9s %d frame(s) in %.0f s" % (v["name"], len(frames), time.time() - t0))
        if v["name"] == "turn":
            imgs = []
            for _, im, _ in frames:
                b = backdrop(im.size)
                b.paste(im, (0, 0), im)
                imgs.append(b.resize((420, 420), Image.LANCZOS))
            pal = [im.quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE) for im in imgs]
            pal[0].save(os.path.join(OUT, "unit-turntable.gif"), save_all=True, append_images=pal[1:], duration=90, loop=0, optimize=True)
            print("  -> unit-turntable.gif")
            continue
        _, im, anchors = frames[0]
        pad = 300 if CALLOUTS.get(v["name"]) else 0   # room for the label columns beside the model
        b = backdrop((im.width + 2 * pad, im.height))
        b.paste(im, (pad, 0), im)
        anchors = {k: (x + pad, y) for k, (x, y) in anchors.items()}
        callouts(b, anchors, CALLOUTS.get(v["name"], []))
        out = "unit-%s.png" % v["name"]
        b.save(os.path.join(OUT, out), optimize=True)
        print("  -> " + out)
    shutil.rmtree(srv_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
