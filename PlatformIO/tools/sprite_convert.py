#!/usr/bin/env python3
"""
can2gauge sprite packs: frames + pack.json -> one "C2GS" .c2gs file for the gauge (docs/SPRITES.md, include/c2gs.h).

    python tools/sprite_convert.py docs/mascot                    # docs/mascot/pack.json -> docs/mascot/rev.c2gs
    python tools/sprite_convert.py path/to/pack -o out.c2gs --preview previews/ [--accent "#24D1C4"]
    python tools/sprite_convert.py --demo -o blob.c2gs            # a generated test pack (baked, value bands)
    python tools/sprite_convert.py --info rev.c2gs [--preview previews/]   # what is inside (decodes every frame)

Upload the .c2gs from the web UI (Settings > Sprites), then pick it for a Sprite page (Gauges), the screensaver
(Settings > Sprites) or the shutdown sprite (Output / Illumination > Ignition).

pack.json - two kinds of art:

  layered (the Rev mascot, docs/mascot/pack.json): "layered": true and "source" = a zip or folder holding
  manifest.json (clips: frames, fps, loop, box, glow, glow_gain), white/<clip>/NNNN.png (RGBA, theme bits keyed
  magenta, docs/mascot/MASCOT.md) and glow/<clip>/NNNN.png (quarter-resolution alpha). The gauge themes it at draw
  time; each clip gets a palette of up to 256 (literal colour, accent, cover) entries.

  baked (anything else): each state names its frames - a glob, one image (an animated GIF / PNG / WebP gives all its
  frames) or a sprite sheet {"sheet", "cols", "rows", "count"?} - drawn as they are (transparency kept).

    {
      "name": "Rev", "key": "rev",            shown in the web UI / the file name on the gauge (15 characters)
      "canvas": 480,                          the glass area the boxes sit in (default: the largest frame)
      "scale": 1,                             2 = store at half size, scaled up on the gauge (a quarter of the flash)
      "quality": {"lit": 4, "alpha": 5, "glow": 4},   layered: low bits dropped (literal channels / accent and cover /
                                                      glow) - more = smaller and coarser
      "layered": true, "source": "rev_frames.zip",
      "states": [                             the first mood whose rule holds plays: put the overrides first
        {"role": "mood", "clip": "oil_hot", "sig": "oiltemp", "above": 125, "in": "both"},
        {"role": "mood", "clip": "rpm_run", "sig": "rpm", "above": 1300, "page": "rpm", "in": "both"},
        {"role": "event", "clip": "lock_slam", "sig": "hdxActual", "rises": 92},
        {"role": "event", "clip": "boost_blowoff", "sig": "boost", "from": 0.9, "drops": 0.5},
        {"role": "mood", "frames": "idle/*.png", "band": [0, 40], "fps": [6, 12]},   baked: % of the page range
        {"role": "mood", "frames": "panic/*.png", "redline": true},
        {"role": "mood", "frames": "sleep/*.png", "nodata": true},
        {"role": "mood", "clip": "oil_normal"},                 no rule = always (a page's fallback)
        {"role": "tap", "clip": "poke"},
        {"role": "saver", "clip": "saver_loop"},
        {"role": "shutdown", "clip": "ign_off"}, {"role": "sleep", "clip": "sleep_loop"}, {"role": "wake", "clip": "ign_on"},
        {"role": "row_left", "clip": "row_left"}, {"role": "row_right", "clip": "row_right"}   the row wave (0.12.0):
                                              looks at the gauge on that side and waves; ends looking that way, arm down
      ],
      "texts": [
        {"clip": "rpm_limiter", "text": "REDLINE", "colour": "#FF4A2A", "x": 0, "y": -168, "size": 60, "rot": -3,
         "repop": 8, "jitter": 3},
        {"clip": "sleep_loop", "zzz": true, "x": -60, "y": -20, "size": 42}
      ]
    }
  Rule keys: "sig" (a signal key from src/can2gauge_signals.cpp; omit = the page's own value), "above" / "below"
  (metric units; both = a range), "band": [from %, to %] of the page range, "redline": true, "nodata": true,
  "rises": x, "drops": x with "from": y. "page": "<signal key>" = only on a page showing that signal. "in": "page",
  "saver" or "both" (default "page"; "saver" rules play on the screensaver with "React to the car" on).
  "fps": n or [at the band's bottom, at its top] (layered: the manifest's). "once": true plays once (events, tap,
  shutdown, wake and the row clips always do). Baked states name a "clip" too when a text refers to them. Text: "colour" "accent"
  (default) or "#RRGGBB", "frames": [from, to] of the clip (default all).

Needs Pillow and numpy (pip install pillow numpy).
"""
import argparse
import fnmatch
import glob
import hashlib
import io
import json
import math
import re
import struct
import sys
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageSequence

ROOT = Path(__file__).resolve().parent.parent
ROLES = {"mood": 0, "event": 1, "tap": 2, "saver": 3, "shutdown": 4, "sleep": 5, "wake": 6, "row_left": 7, "row_right": 8}
ONCE_ROLES = (1, 2, 4, 6, 7, 8)  # events, tap, shutdown, wake and the row wave clips always play once
ROLE_NAMES = {v: k for k, v in ROLES.items()}
ALWAYS, RANGE, PERMILLE, RISE, DROP, REDLINE, NODATA = range(7)
COND_NAMES = ["always", "range", "band", "rises", "drops", "redline", "nodata"]
ONCE, IN_PAGE, IN_SAVER = 1, 2, 4
F_ALPHA, F_RLE, F_LAYERED = 1, 2, 4
GLOW_ACCENT = 0x01000000
GLOW_DIV = 8
MAX_SIDE, MAX_CLIPS, MAX_STATES, MAX_TEXTS, KEY_LEN = 480, 48, 48, 32, 15
BIG = 1e9
HEAD, CLIP, STATE, TEXT, FRAME = "<4sBBHHHBBBB16s", "<HHHHHHHBBII8s", "<BBBBBBBBff8s", "<BBHH12shhBbBBIH", "<IIIII"


def signal_ids():
    """{key: signalId_t} from the firmware's catalogue (src/can2gauge_signals.cpp, table order = the enum)"""
    src = (ROOT / "src" / "can2gauge_signals.cpp").read_text(encoding="utf-8")
    table = src[src.index("signalTable[] = {"):]
    table = table[:table.index("};")]
    return {k: i for i, k in enumerate(re.findall(r'^\s*\{"([A-Za-z0-9_]+)",', table, re.M))}


def hex_rgb(s):
    return GLOW_ACCENT if s in (None, "accent") else int(s.lstrip("#"), 16) & 0xFFFFFF


# ---------------------------------------------------------------------------------------------------------------------
# planes
# ---------------------------------------------------------------------------------------------------------------------
def rle(values: np.ndarray):
    """C2GL packets over a uint8 / uint16 array"""
    v = values.ravel()
    width = v.dtype.itemsize
    n = len(v)
    if n == 0:
        return b""
    starts = np.concatenate(([0], np.flatnonzero(v[1:] != v[:-1]) + 1))
    lens = np.diff(np.concatenate((starts, [n])))
    raw = v.astype("<u2" if width == 2 else "u1").tobytes()
    out = bytearray()
    lit_from = None

    def flush_lit(a, b):
        while a < b:
            k = min(128, b - a)
            out.append(k - 1)
            out.extend(raw[a * width:(a + k) * width])
            a += k

    for s, L in zip(starts.tolist(), lens.tolist()):
        if L == 1:
            if lit_from is None:
                lit_from = s
            continue
        if lit_from is not None:
            flush_lit(lit_from, s)
            lit_from = None
        val = raw[s * width:(s + 1) * width]
        end = s + L
        while L >= 2:
            k = min(129, L)
            out.append(0x80 | (k - 2))
            out.extend(val)
            L -= k
        if L == 1:  # one left over from a long run: it starts the next literal stretch
            lit_from = end - 1
    if lit_from is not None:
        flush_lit(lit_from, n)
    return bytes(out)


def unrle(data, count, width):
    out = np.zeros(count, dtype=np.uint16 if width == 2 else np.uint8)
    i = o = 0
    while i < len(data) and o < count:
        c = data[i]
        i += 1
        if c & 0x80:
            v = struct.unpack_from("<H", data, i)[0] if width == 2 else data[i]
            i += width
            k = min((c & 0x7F) + 2, count - o)
            out[o:o + k] = v
            o += k
        else:
            k = c + 1
            vals = np.frombuffer(data, dtype="<u2" if width == 2 else "u1", count=k, offset=i)
            out[o:o + k] = vals[:count - o]
            i += k * width
            o += k
    if o < count:
        raise ValueError("RLE plane ends early (%d of %d)" % (o, count))
    return out


def to565(rgb: np.ndarray):
    r = (rgb[..., 0].astype(np.uint32) * 31 + 127) // 255
    g = (rgb[..., 1].astype(np.uint32) * 63 + 127) // 255
    b = (rgb[..., 2].astype(np.uint32) * 31 + 127) // 255
    return (r << 11 | g << 5 | b).astype(np.uint16)


def from565(v: np.ndarray):
    v = v.astype(np.uint32)
    return np.stack(((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31), -1).astype(np.uint8)


def stored(n, scale):
    return (n + scale - 1) // scale


# ---------------------------------------------------------------------------------------------------------------------
# sources
# ---------------------------------------------------------------------------------------------------------------------
def natural(p):
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r"(\d+)", str(p))]


class Source:
    """a folder or a zip"""

    def __init__(self, path: Path):
        self.zip = zipfile.ZipFile(path) if path.suffix.lower() == ".zip" else None
        self.base = path
        self.names = sorted(self.zip.namelist()) if self.zip else None

    def read(self, rel):
        return self.zip.read(rel) if self.zip else (self.base / rel).read_bytes()

    def glob(self, pattern):
        if self.zip:
            return sorted(fnmatch.filter(self.names, pattern), key=natural)
        return sorted((Path(p).relative_to(self.base).as_posix() for p in glob.glob(str(self.base / pattern))), key=natural)

    def image(self, rel):
        return Image.open(io.BytesIO(self.read(rel)))


def baked_frames(spec, src: Source):
    if isinstance(spec, dict):
        sheet = src.image(spec["sheet"]).convert("RGBA")
        cols, rows = int(spec.get("cols", 1)), int(spec.get("rows", 1))
        count = int(spec.get("count", cols * rows))
        fw, fh = sheet.width // cols, sheet.height // rows
        return [sheet.crop(((i % cols) * fw, (i // cols) * fh, (i % cols + 1) * fw, (i // cols + 1) * fh)) for i in range(count)]
    paths = src.glob(spec) if any(c in spec for c in "*?[") else [spec]
    if not paths:
        raise SystemExit("no frames match %r" % spec)
    out = []
    for p in paths:
        im = src.image(p)
        out += [f.convert("RGBA") for f in ImageSequence.Iterator(im)] if getattr(im, "n_frames", 1) > 1 else [im.convert("RGBA")]
    return out


# ---------------------------------------------------------------------------------------------------------------------
# encoders
# ---------------------------------------------------------------------------------------------------------------------
def encode_baked(img: Image.Image, alpha: bool, scale: int):
    if scale > 1:
        img = img.resize((stored(img.width, scale), stored(img.height, scale)), Image.LANCZOS)
    a = np.asarray(img.convert("RGBA"))
    rgb, al = a[..., :3].astype(np.uint16), a[..., 3].astype(np.uint16)
    if not alpha:  # drawn on the screen's black
        rgb = rgb * al[..., None] // 255
    rgb = np.where((al == 0)[..., None], 0, rgb).astype(np.uint8)
    return [rle(to565(rgb)), rle(al.astype(np.uint8)) if alpha else b"", b"", b""]


def deq(q, bits):
    """a value with `bits` low bits dropped, back to 0..255 (0 stays 0, the top level is 255)"""
    levels = (256 >> bits) - 1
    return (q * 255 + levels // 2) // max(1, levels)


class LayeredClip:
    """the white + glow frames of one clip -> a palette and per-frame (index plane, glow plane)"""

    def __init__(self, whites, glows, box, scale, quality):
        self.box, self.scale = box, scale
        lq, aq, gq = quality.get("lit", 4), quality.get("alpha", 5), quality.get("glow", 4)
        bw, bh = box[2], box[3]
        sw, sh = stored(bw, scale), stored(bh, scale)
        gw, gh = stored(sw, GLOW_DIV), stored(sh, GLOW_DIV)
        codes, self.glows = [], []
        for wi, gi in zip(whites, glows):
            wi = wi.convert("RGBA")
            if wi.size != (bw, bh):
                raise SystemExit("white frame is %dx%d, its clip box is %dx%d" % (wi.width, wi.height, bw, bh))
            if scale > 1:
                wi = wi.resize((sw, sh), Image.LANCZOS)
            a = np.asarray(wi).astype(np.int32)
            r, g, b, al = a[..., 0], a[..., 1], a[..., 2], a[..., 3]
            keyed = (r > g) & (b > g)  # MASCOT.md: magenta-keyed = theme-tinted
            acc = np.where(keyed, np.minimum(r, b) - g, 0) * al // 255
            lit = np.where(keyed[..., None], g[..., None].repeat(3, -1), a[..., :3]) * al[..., None] // 255
            lit = (lit >> lq).astype(np.int64)
            code = (lit[..., 0] << 24 | lit[..., 1] << 16 | lit[..., 2] << 8 | (acc >> aq) << 4 | (al >> aq)).astype(np.int64)
            codes.append(code.ravel())
            gl = np.asarray(gi.convert("L").resize((gw, gh), Image.BILINEAR)).astype(np.int32)
            self.glows.append(((gl >> gq) << gq | ((gl >> gq) << gq) >> (8 - gq)).clip(0, 255).astype(np.uint8))
        allc = np.concatenate(codes)
        uniq, inv, counts = np.unique(allc, return_inverse=True, return_counts=True)
        keep = np.argsort(-counts)[:256]  # the commonest 256; the rest map to the nearest of those
        comp = lambda c: np.stack(((c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255, (c >> 4) & 15, c & 15), -1).astype(np.float64)
        kc, uc = comp(uniq[keep]), comp(uniq)
        wts = np.array([1.0, 1.0, 1.0, 16.0 * (1 << lq) / (1 << aq), 16.0 * (1 << lq) / (1 << aq)])
        remap = np.empty(len(uniq), dtype=np.uint8)
        for i0 in range(0, len(uniq), 4096):  # nearest kept entry for every distinct code
            d = (((uc[i0:i0 + 4096, None, :] - kc[None, :, :]) * wts) ** 2).sum(-1)
            remap[i0:i0 + 4096] = d.argmin(1)
        self.mapped = int((counts[np.setdiff1d(np.arange(len(uniq)), keep)]).sum()) if len(uniq) > 256 else 0
        self.distinct = len(uniq)
        idx = remap[inv]
        self.planes = []
        o = 0
        for c in codes:
            self.planes.append(idx[o:o + len(c)].astype(np.uint8))
            o += len(c)
        k = uniq[keep]
        lit = np.stack((((k >> 24) & 255) << lq, ((k >> 16) & 255) << lq, ((k >> 8) & 255) << lq), -1)
        lit = np.minimum(255, lit + ((lit > 0) * ((1 << lq) - 1) // 2))  # middle of the dropped range (0 stays black)
        self.pal = b"".join(struct.pack("<HBB", int(to565(lit[i].astype(np.uint8))), deq(int(k[i] >> 4 & 15), aq), deq(int(k[i] & 15), aq))
                            for i in range(len(k)))

    def frame_planes(self, i):
        return [rle(self.planes[i]), rle(self.glows[i]), b"", b""]


# ---------------------------------------------------------------------------------------------------------------------
# the file
# ---------------------------------------------------------------------------------------------------------------------
def build(name, flags, cw, ch, scale, clips, states, texts):
    """clips: dicts (frames: [4 plane bytes], box, pal: bytes, glow, gain); states / texts: dicts"""
    n_frames = sum(len(c["frames"]) for c in clips)
    head = struct.pack(HEAD, b"C2GS", 1, flags, cw, ch, n_frames, len(states), len(texts), len(clips), scale,
                       name.encode("utf-8")[:16].ljust(16, b"\0"))
    tables = len(head) + 32 * len(clips) + 24 * len(states) + 32 * len(texts) + 20 * n_frames
    pal_off, pals = [], bytearray()
    for c in clips:
        pal_off.append(tables + len(pals) if c.get("pal") else 0)
        pals += c.get("pal") or b""
    data_at = tables + len(pals)
    body, frame_tab, seen = bytearray(), bytearray(), {}
    first = 0
    clip_tab = bytearray()
    for c, po in zip(clips, pal_off):
        for planes in c["frames"]:
            blob = b"".join(planes)
            h = hashlib.sha1(blob).digest()
            if h not in seen:  # an identical frame is stored once
                seen[h] = data_at + len(body)
                body += blob
            frame_tab += struct.pack(FRAME, seen[h], *[len(p) for p in planes])
        x, y, w, h = c["box"]
        clip_tab += struct.pack(CLIP, first, len(c["frames"]), x, y, w, h, len(c.get("pal") or b"") // 4, c.get("gain", 64), 0,
                                po, c.get("glow", GLOW_ACCENT), b"\0" * 8)
        first += len(c["frames"])
    st = b"".join(struct.pack(STATE, s["clip"], s["role"], s["flags"], s["fpsLo"], s["fpsHi"], s["sig"], s["family"], s["cond"],
                              s["lo"], s["hi"], b"\0" * 8) for s in states)
    tx = b"".join(struct.pack(TEXT, t["clip"], t["kind"], t["from"], t["to"], t["text"].encode("utf-8")[:12].ljust(12, b"\0"),
                              t["x"], t["y"], t["size"], t["rot"], t["jitter"], t["repop"], t["colour"], 0) for t in texts)
    return head + bytes(clip_tab) + st + tx + bytes(frame_tab) + bytes(pals) + bytes(body)


def parse(data):
    if len(data) < 32 or data[:4] != b"C2GS" or data[4] != 1:
        raise SystemExit("not a C2GS v1 sprite pack")
    _, _, flags, cw, ch, nframes, nstates, ntexts, nclips, scale, name = struct.unpack_from(HEAD, data, 0)
    name = name.split(b"\0")[0].decode("utf-8", "replace")
    if not (0 < cw <= MAX_SIDE and 0 < ch <= MAX_SIDE and 0 < nstates <= MAX_STATES and ntexts <= MAX_TEXTS and
            0 < nclips <= MAX_CLIPS and nframes and scale in (1, 2)):
        raise SystemExit("bad header")
    o = 32
    ck = ("first", "count", "x", "y", "w", "h", "palCount", "gain", "r1", "palOffset", "glow", "r2")
    clips = [dict(zip(ck, struct.unpack_from(CLIP, data, o + 32 * i))) for i in range(nclips)]
    o += 32 * nclips
    sk = ("clip", "role", "flags", "fpsLo", "fpsHi", "sig", "family", "cond", "lo", "hi", "r")
    states = [dict(zip(sk, struct.unpack_from(STATE, data, o + 24 * i))) for i in range(nstates)]
    o += 24 * nstates
    tk = ("clip", "kind", "from", "to", "text", "x", "y", "size", "rot", "jitter", "repop", "colour", "r")
    texts = [dict(zip(tk, struct.unpack_from(TEXT, data, o + 32 * i))) for i in range(ntexts)]
    o += 32 * ntexts
    frames = [struct.unpack_from(FRAME, data, o + 20 * i) for i in range(nframes)]
    for c in clips:
        if not c["count"] or c["first"] + c["count"] > nframes or c["x"] + c["w"] > cw or c["y"] + c["h"] > ch:
            raise SystemExit("a clip points outside the frames / canvas")
        if flags & F_LAYERED and not (0 < c["palCount"] <= 256 and c["palOffset"] + 4 * c["palCount"] <= len(data)):
            raise SystemExit("a clip's palette is missing")
    for s in states:
        if s["clip"] >= nclips:
            raise SystemExit("a state names a missing clip")
    for off, *lens in frames:
        if off + sum(lens) > len(data):
            raise SystemExit("a frame runs past the end of the file")
    return dict(name=name, flags=flags, w=cw, h=ch, scale=scale, clips=clips, states=states, texts=texts, frames=frames, data=data)


def render(pack, clip, k, accent=0x24D1C4):
    """frame k of a clip as the gauge draws it: RGB on the black canvas"""
    c, data, sc = pack["clips"][clip], pack["data"], pack["scale"]
    off, *lens = pack["frames"][c["first"] + k]
    sw, sh = stored(c["w"], sc), stored(c["h"], sc)
    planes, p = [], off
    for L in lens:
        planes.append(data[p:p + L])
        p += L
    if pack["flags"] & F_LAYERED:
        pal = np.frombuffer(data, dtype=np.dtype([("lit", "<u2"), ("acc", "u1"), ("cov", "u1")]), count=c["palCount"], offset=c["palOffset"])
        idx = unrle(planes[0], sw * sh, 1).reshape(sh, sw)
        lit = from565(pal["lit"][idx]).astype(np.int32)
        acc, cov = pal["acc"][idx].astype(np.int32), pal["cov"][idx].astype(np.int32)
        gw, gh = stored(sw, GLOW_DIV), stored(sh, GLOW_DIV)
        g = unrle(planes[1], gw * gh, 1).reshape(gh, gw)
        g = np.asarray(Image.fromarray(g).resize((gw * GLOW_DIV, gh * GLOW_DIV), Image.BILINEAR))[:sh, :sw].astype(np.int32)
        gc = accent if c["glow"] & GLOW_ACCENT else c["glow"]
        A = np.array([accent >> 16 & 255, accent >> 8 & 255, accent & 255])
        G = np.array([gc >> 16 & 255, gc >> 8 & 255, gc & 255])
        gw_ = np.minimum(255, g * (255 - cov) // 255 * c["gain"] // 64)
        rgb = np.minimum(255, lit + acc[..., None] * A // 255 + gw_[..., None] * G // 255).astype(np.uint8)
        clip_img = Image.fromarray(rgb, "RGB").convert("RGBA")
    else:
        rgb = from565(unrle(planes[0], sw * sh, 2)).reshape(sh, sw, 3)
        al = unrle(planes[1], sw * sh, 1).reshape(sh, sw) if pack["flags"] & F_ALPHA else np.full((sh, sw), 255, np.uint8)
        clip_img = Image.fromarray(np.dstack((rgb, al)).astype(np.uint8), "RGBA")
    if sc > 1:
        clip_img = clip_img.resize((c["w"], c["h"]), Image.BILINEAR)
    canvas = Image.new("RGBA", (pack["w"], pack["h"]), (0, 0, 0, 255))
    canvas.alpha_composite(clip_img, (c["x"], c["y"]))
    return canvas.convert("RGB")


def describe(path, pack, sig_names):
    data = pack["data"]
    kind = "layered (themed on the gauge)" if pack["flags"] & F_LAYERED else ("baked, with alpha" if pack["flags"] & F_ALPHA else "baked")
    print('%s: "%s" %dx%d canvas, %s%s, %d clips, %d frames (%d stored), %d states, %d texts, %.2f MB' % (
        path, pack["name"], pack["w"], pack["h"], kind, ", stored at 1/%d" % pack["scale"] if pack["scale"] > 1 else "",
        len(pack["clips"]), len(pack["frames"]), len({f[0] for f in pack["frames"]}), len(pack["states"]), len(pack["texts"]),
        len(data) / 1048576))
    for i, s in enumerate(pack["states"]):
        c = pack["clips"][s["clip"]]
        sig = sig_names.get(s["sig"], "?") if s["sig"] else "page"
        rule = {RANGE: lambda: "%s %s" % (sig, ("%g..%g" % (s["lo"], s["hi"])) if abs(s["lo"]) < BIG / 2 and abs(s["hi"]) < BIG / 2
                                          else ("> %g" % s["lo"]) if abs(s["lo"]) < BIG / 2 else ("< %g" % s["hi"])),
                PERMILLE: lambda: "band %g-%g %%" % (s["lo"] / 10, s["hi"] / 10),
                RISE: lambda: "%s rises through %g" % (sig, s["lo"]),
                DROP: lambda: "%s drops %g from %g" % (sig, s["hi"], s["lo"])}.get(s["cond"], lambda: COND_NAMES[s["cond"]])()
        where = "/".join(x for x, f in (("page", IN_PAGE), ("saver", IN_SAVER)) if s["flags"] & f)
        fam = (" on %s pages" % sig_names.get(s["family"], "?")) if s["family"] else ""
        fps = "%d-%d fps" % (s["fpsLo"], s["fpsHi"]) if s["fpsHi"] != s["fpsLo"] else "%d fps" % s["fpsLo"]
        print("  %2d %-8s clip %2d %3d frames %s%s  box %d,%d %dx%d  %s" % (
            i, ROLE_NAMES.get(s["role"], "?"), s["clip"], c["count"], fps, ", once" if s["flags"] & ONCE else "",
            c["x"], c["y"], c["w"], c["h"], ("%s [%s]%s" % (rule, where, fam)) if s["role"] in (0, 1) else ""))
    for t in pack["texts"]:
        print("     text on clip %d: %s" % (t["clip"], "zzz" if t["kind"] else t["text"].split(b"\0")[0].decode()))
    side = max(max(c["w"], c["h"]) for c in pack["clips"])
    print("  on the gauge: about %.0f kB of PSRAM per screen showing it (frames stream from flash)" % (side * side * 2 / 1024))


def previews(pack, outdir: Path, key, accent):
    outdir.mkdir(parents=True, exist_ok=True)
    for i, s in enumerate(pack["states"]):
        c = pack["clips"][s["clip"]]
        imgs = [render(pack, s["clip"], k, accent).resize((240, 240), Image.LANCZOS) for k in range(c["count"])]
        p = outdir / ("%s_%02d_%s.gif" % (key, i, ROLE_NAMES.get(s["role"], "x")))
        imgs[0].save(p, save_all=True, append_images=imgs[1:], duration=int(1000 / max(1, s["fpsLo"])), loop=0)
    print("previews in", outdir)


# ---------------------------------------------------------------------------------------------------------------------
# pack.json
# ---------------------------------------------------------------------------------------------------------------------
def rule_of(s, sig_ids):
    def sid(key):
        if key not in sig_ids:
            raise SystemExit("unknown signal %r (keys: src/can2gauge_signals.cpp)" % key)
        return sig_ids[key]
    sig = sid(s["sig"]) if "sig" in s else 0
    if "band" in s:
        return 0, PERMILLE, float(s["band"][0]) * 10, float(s["band"][1]) * 10
    if s.get("redline"):
        return 0, REDLINE, 0.0, 0.0
    if s.get("nodata"):
        return 0, NODATA, 0.0, 0.0
    if "rises" in s:
        return sig, RISE, float(s["rises"]), 0.0
    if "drops" in s:
        return sig, DROP, float(s.get("from", -BIG)), float(s["drops"])
    if "above" in s or "below" in s:
        return sig, RANGE, float(s.get("above", -BIG)), float(s.get("below", BIG))
    return sig, ALWAYS, 0.0, 0.0


def convert(folder: Path, out: Path = None):
    cfg = json.loads((folder / "pack.json").read_text(encoding="utf-8"))
    sig_ids = signal_ids()
    key = cfg.get("key") or folder.name
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,%d}" % KEY_LEN, key):
        raise SystemExit("key %r: 1-%d letters, digits, _ or -" % (key, KEY_LEN))
    layered = bool(cfg.get("layered"))
    scale = int(cfg.get("scale", 1))
    if scale not in (1, 2):
        raise SystemExit('"scale" is 1 or 2')
    src = Source(folder / cfg["source"]) if "source" in cfg else Source(folder)
    manifest = json.loads(src.read("manifest.json")) if layered else None
    clip_ix, clips, states = {}, [], []
    baked = []  # (clip index, images): placed once the canvas is known
    for s in cfg["states"]:
        role = ROLES.get(s.get("role", "mood"))
        if role is None:
            raise SystemExit("unknown role %r (%s)" % (s.get("role"), ", ".join(ROLES)))
        sig, cond, lo, hi = rule_of(s, sig_ids) if role in (0, 1) else (0, ALWAYS, 0.0, 0.0)
        where = s.get("in", "page")
        flags = (IN_PAGE if where in ("page", "both") else 0) | (IN_SAVER if where in ("saver", "both") else 0)
        if role in ONCE_ROLES or s.get("once"):
            flags |= ONCE
        family = sig_ids[s["page"]] if "page" in s else 0
        name = s.get("clip") or json.dumps(s["frames"], sort_keys=True)
        if layered:
            m = manifest["clips"][name]
            if not m.get("loop", True):
                flags |= ONCE
            fps = s.get("fps", m["fps"])
        else:
            fps = s.get("fps", 12)
        if name not in clip_ix:
            clip_ix[name] = len(clips)
            if layered:
                whites, glows = src.glob("white/%s/*.png" % name), src.glob("glow/%s/*.png" % name)
                if not whites or len(whites) != len(glows):
                    raise SystemExit("clip %s: %d white / %d glow frames" % (name, len(whites), len(glows)))
                lc = LayeredClip([src.image(p) for p in whites], [src.image(p) for p in glows], m["box"], scale, cfg.get("quality", {}))
                frames = [lc.frame_planes(i) for i in range(len(whites))]
                clips.append(dict(name=name, box=tuple(m["box"]), frames=frames, pal=lc.pal, glow=hex_rgb(m.get("glow", "accent")),
                                  gain=max(1, min(255, round(float(m.get("glow_gain", 1.0)) * 64)))))
                print("  %-14s %3d frames %4.0f kB, palette %3d%s" % (name, len(frames), sum(len(b) for f in frames for b in f) / 1024,
                      len(lc.pal) // 4, " (%d colours merged)" % (lc.distinct - 256) if lc.distinct > 256 else ""))
            else:
                clips.append(dict(name=name, box=None, frames=None))
                baked.append((len(clips) - 1, baked_frames(s["frames"], src)))
        lo_fps, hi_fps = (fps if isinstance(fps, list) else [fps, fps])[:2]
        states.append(dict(clip=clip_ix[name], role=role, flags=flags, fpsLo=max(1, min(60, int(lo_fps))),
                           fpsHi=max(1, min(60, int(hi_fps))), sig=sig, family=family, cond=cond, lo=lo, hi=hi))
    if layered:
        cw = ch = int(cfg.get("canvas", manifest.get("size", 480)))
        flags = F_RLE | F_LAYERED
    else:
        cw = int(cfg.get("canvas", max(max(i.width for i in imgs) for _, imgs in baked)))
        ch = int(cfg.get("canvas", max(max(i.height for i in imgs) for _, imgs in baked)))
        alpha = cfg.get("alpha", any(np.asarray(i.convert("RGBA"))[..., 3].min() < 255 for _, imgs in baked for i in imgs))
        flags = F_RLE | (F_ALPHA if alpha else 0)
        for ci, imgs in baked:  # each clip centred on the canvas
            iw, ih = max(i.width for i in imgs), max(i.height for i in imgs)
            frames = []
            for im in imgs:
                c = Image.new("RGBA", (iw, ih), (0, 0, 0, 0))
                c.alpha_composite(im.convert("RGBA"), ((iw - im.width) // 2, (ih - im.height) // 2))
                frames.append(encode_baked(c, alpha, scale))
            clips[ci].update(box=((cw - iw) // 2, (ch - ih) // 2, iw, ih), frames=frames)
    if cw > MAX_SIDE or ch > MAX_SIDE or not 0 < len(states) <= MAX_STATES or len(clips) > MAX_CLIPS:
        raise SystemExit("canvas up to %d, 1-%d states, %d clips at most" % (MAX_SIDE, MAX_STATES, MAX_CLIPS))
    texts = []
    for t in cfg.get("texts", []):
        if t["clip"] not in clip_ix:
            raise SystemExit("text for unknown clip %r" % t["clip"])
        fr = t.get("frames", [0, 65535])
        texts.append(dict(clip=clip_ix[t["clip"]], kind=1 if t.get("zzz") else 0, text=t.get("text", "z"), x=int(t.get("x", 0)),
                          y=int(t.get("y", 0)), size=int(t.get("size", 48)), rot=int(t.get("rot", 0)), jitter=int(t.get("jitter", 0)),
                          repop=int(t.get("repop", 0)), colour=hex_rgb(t.get("colour")), **{"from": int(fr[0]), "to": int(fr[1])}))
    if len(texts) > MAX_TEXTS:
        raise SystemExit("%d texts: %d at most" % (len(texts), MAX_TEXTS))
    data = build(cfg.get("name") or key, flags, cw, ch, scale, clips, states, texts)
    out = out or folder / (key + ".c2gs")
    out.write_bytes(data)
    describe(out, parse(data), {v: k for k, v in sig_ids.items()})
    return out, data, key


# ---------------------------------------------------------------------------------------------------------------------
# --demo: a generated blob (baked) with value bands, for testing a gauge before real art exists
# ---------------------------------------------------------------------------------------------------------------------
SS = 4


def blob(size, t, mood="happy", sx=1.0, sy=1.0, dy=0.0, eyes=1.0, colour=(36, 209, 196), sweat=False, lines=0, wave=0.0):
    W = size * SS
    im = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    cx, cy = W / 2, W * 0.58 + dy * SS
    rx, ry = W * 0.30 * sx, W * 0.26 * sy
    for i in range(lines):
        y = cy - ry * 0.6 + i * ry * 0.6
        x0 = cx - rx - W * 0.08 - ((t * 37 + i * 13) % 20) * SS
        d.line((x0 - W * 0.14, y, x0, y), fill=(255, 255, 255, 170), width=3 * SS)
    d.ellipse((cx - rx, cy - ry, cx + rx, cy + ry), fill=colour + (255,))
    ex, ey, er = rx * 0.38, cy - ry * 0.12, W * 0.045
    for s in (-1, 1):
        x = cx + s * ex
        if eyes > 0.15:
            d.ellipse((x - er, ey - er * eyes, x + er, ey + er * eyes), fill=(20, 24, 30, 255))
        else:
            d.arc((x - er, ey - er, x + er, ey + er), 20, 160, fill=(20, 24, 30, 255), width=int(1.6 * SS))
    my = cy + ry * 0.32
    if mood == "happy":
        d.arc((cx - rx * 0.25, my - ry * 0.2, cx + rx * 0.25, my + ry * 0.2), 20, 160, fill=(20, 24, 30, 255), width=2 * SS)
    elif mood in ("wow", "panic"):
        d.ellipse((cx - W * 0.03, my - W * 0.02, cx + W * 0.03, my + W * 0.05), fill=(20, 24, 30, 255))
    if sweat:
        for k, ox in enumerate((0.75, -0.8)):
            yy = cy - ry * 0.6 + ((t * 6 + k * 9) % 18) * SS
            x = cx + rx * ox
            d.ellipse((x - W * 0.025, yy, x + W * 0.025, yy + W * 0.05), fill=(120, 200, 255, 230))
    if wave:
        ang = math.radians(-60 + 35 * math.sin(wave))
        x0, y0 = cx + rx * 0.9, cy - ry * 0.1
        d.line((x0, y0, x0 + math.cos(ang) * W * 0.16, y0 + math.sin(ang) * W * 0.16), fill=colour + (255,), width=7 * SS)
    return im.resize((size, size), Image.LANCZOS)


def demo(out: Path):
    S = 200
    sig = signal_ids()
    art = {
        "idle": [blob(S, i, sy=1 + 0.04 * math.sin(i / 12 * 2 * math.pi), eyes=0.1 if i in (9, 10) else 1.0) for i in range(12)],
        "bounce": [blob(S, i, mood="wow", dy=-abs(math.sin(i / 12 * math.pi)) * 34) for i in range(12)],
        "zoom": [blob(S, i, mood="wow", sx=1.18, sy=0.86, dy=2 * math.sin(i), lines=3) for i in range(8)],
        "panic": [blob(S, i, mood="panic", colour=(255, 77, 94), sweat=True, dy=3 * (1 if i % 2 else -1)) for i in range(6)],
        "sleep": [blob(S, i, eyes=0.0, mood="none", sy=1 + 0.03 * math.sin(i / 8 * 2 * math.pi)) for i in range(8)],
        "poke": [blob(S, i, mood="wow", dy=-20 * math.sin(i / 7 * math.pi), sx=1.1, sy=1.1) for i in range(8)],
        "bye": [blob(S, i, wave=i * 0.9, sx=max(0.05, 1 - max(0, i - 5) / 7), sy=max(0.05, 1 - max(0, i - 5) / 7)) for i in range(13)],
        "hello": [blob(S, i, sx=min(1.0, 0.1 + i / 7), sy=min(1.0, 0.1 + i / 7), eyes=0.0 if i < 6 else 1.0) for i in range(10)],
    }
    names = list(art)
    clips = [dict(box=(0, 0, S, S), frames=[encode_baked(im, True, 1) for im in art[n]]) for n in names]
    ci = {n: i for i, n in enumerate(names)}

    def st(name, role, cond=ALWAYS, lo=0.0, hi=0.0, fps=(12, 12)):
        return dict(clip=ci[name], role=role, flags=IN_PAGE | (ONCE if role in ONCE_ROLES else 0), fpsLo=fps[0], fpsHi=fps[1],
                    sig=0, family=0, cond=cond, lo=lo, hi=hi)

    states = [st("panic", 0, REDLINE, fps=(18, 18)), st("sleep", 0, NODATA, fps=(3, 3)),
              st("idle", 0, PERMILLE, 0, 400, (6, 12)), st("bounce", 0, PERMILLE, 400, 800, (10, 24)),
              st("zoom", 0, PERMILLE, 800, 1000, (16, 30)), st("poke", 2, fps=(15, 15)),
              st("sleep", 3, fps=(4, 4)), st("bye", 4, fps=(10, 10)), st("sleep", 5, fps=(3, 3)), st("hello", 6, fps=(12, 12))]
    texts = [dict(clip=ci["sleep"], kind=1, text="z", x=40, y=-40, size=28, rot=0, jitter=0, repop=0, colour=GLOW_ACCENT, **{"from": 0, "to": 65535}),
             dict(clip=ci["panic"], kind=0, text="HOT!", x=0, y=-80, size=32, rot=-6, jitter=2, repop=6, colour=0xFF4A2A, **{"from": 0, "to": 65535}),
             dict(clip=ci["poke"], kind=0, text="!!", x=60, y=-70, size=40, rot=10, jitter=0, repop=0, colour=0xFF4A2A, **{"from": 0, "to": 65535})]
    data = build("Demo Blob", F_RLE | F_ALPHA, S, S, 1, clips, states, texts)
    out.write_bytes(data)
    describe(out, parse(data), {v: k for k, v in sig.items()})
    return out, data, "blob"


def main():
    ap = argparse.ArgumentParser(description="can2gauge sprite packs (.c2gs) - docs/SPRITES.md")
    ap.add_argument("folder", nargs="?", help="a pack folder with pack.json")
    ap.add_argument("-o", "--out", help="output .c2gs (default: <folder>/<key>.c2gs, or blob.c2gs for --demo)")
    ap.add_argument("--preview", help="also write a GIF per state into this folder")
    ap.add_argument("--accent", default="#24D1C4", help="theme accent for the previews of a layered pack")
    ap.add_argument("--demo", action="store_true", help="write a generated test pack")
    ap.add_argument("--info", help="describe a .c2gs (decodes every frame)")
    a = ap.parse_args()
    accent = hex_rgb(a.accent)
    if a.info:
        data = Path(a.info).read_bytes()
        pack = parse(data)
        describe(a.info, pack, {v: k for k, v in signal_ids().items()})
        for ci, c in enumerate(pack["clips"]):
            for k in range(c["count"]):
                render(pack, ci, k, accent)
        print("  every frame decodes")
        if a.preview:
            previews(pack, Path(a.preview), Path(a.info).stem, accent)
        return
    if a.demo:
        out, data, key = demo(Path(a.out or "blob.c2gs"))
    elif a.folder:
        out, data, key = convert(Path(a.folder), Path(a.out) if a.out else None)
    else:
        ap.print_help()
        sys.exit(1)
    if len(Path(out).stem) > KEY_LEN:
        print("warning: rename %s to %d characters or fewer before uploading" % (out, KEY_LEN))
    if a.preview:
        previews(parse(data), Path(a.preview), key, accent)


if __name__ == "__main__":
    main()
