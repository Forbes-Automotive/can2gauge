#!/usr/bin/env python3
"""Slot icons for the Custom gauge page -> LVGL A8 bitmaps.

The icons are the stroke SVGs approved on the "can2gauge Custom Layout" canvas (48 x 48 viewBox, round caps and joins).
They are kept here as the single source. This script rasterises them with PIL only (no cairo needed) and writes:

  lib/GaugeUI/src/GaugeIcons.cpp/.h  one LV_COLOR_FORMAT_A8 image per icon (tinted on the glass with image_recolor)
  tools/icons/preview.png             all icons on black, for checking by eye

Supported SVG subset: <path d> (M L H V Q T C S A Z, absolute and relative), <circle>, <rect rx>, <g transform="translate()
scale()">, and a per-element stroke-width. Run from the project root:  python tools/icons/make_icons.py [size]
"""
import math
import os
import re
import sys
import xml.etree.ElementTree as ET

# PIL is imported where the bitmaps are made, so the simulator can import ICONS with a bare Python (e.g. PlatformIO's)

SIZE = 32  # default output px (the stack rows on the glass use 32); `make_icons.py 40` overrides
SS = 8                                                 # supersampling factor
STROKE = 3.2                                           # default stroke width in viewBox units

CAN_BODY = '<path d="M10 22h22v14H10z"/><path d="M10 25H5v7h5"/><path d="M17 22v-4h8v4"/><path d="M32 26l12-6-12 12"/>'

# name -> SVG body (inside a 0 0 48 48 viewBox). Order = GaugeIcon enum order.
ICONS = [
    ("coolant", '<path d="M21 30V10a3 3 0 0 1 6 0v20"/><circle cx="24" cy="35" r="6"/><path d="M27 14h5M27 20h5M27 26h5"/>'
                '<path d="M4 44q3-3 6 0t6 0M32 44q3-3 6 0t6 0"/>'),
    ("oil_temp", '<g transform="translate(-2 8) scale(.8)">' + CAN_BODY + '</g>'
                 '<path d="M38 22V8a2.5 2.5 0 0 1 5 0v14"/><circle cx="40.5" cy="26" r="4"/>'),
    ("oil_press", CAN_BODY + '<path d="M43 29c1.5 2.2 2 3.2 2 4a2 2 0 0 1-4 0c0-.8.5-1.8 2-4z"/>'),
    ("oil_level", '<g transform="translate(0 -5)">' + CAN_BODY + '</g><path d="M6 42q3-3 6 0t6 0t6 0t6 0t6 0t6 0"/>'),
    ("battery", '<rect x="5" y="14" width="38" height="26" rx="3"/><path d="M11 14v-4h7v4M30 14v-4h7v4"/>'
                '<path d="M11 27h8M29 27h8M33 23v8"/>'),
    ("boost", '<circle cx="21" cy="27" r="14"/><circle cx="21" cy="27" r="5"/><path d="M21 13h22v9h-9"/>'
              '<path d="M21 22v-3M26 27h3M21 32v3M16 27h-3"/>'),
    ("intake", '<path d="M4 14h14M4 22h18M4 30h14"/><path d="M30 30V10a3 3 0 0 1 6 0v20"/><circle cx="33" cy="35" r="6"/>'),
    ("outside", '<path d="M13 6v18M5.2 10.5l15.6 9M5.2 19.5l15.6-9"/><path d="M30 30V10a3 3 0 0 1 6 0v20"/>'
                '<circle cx="33" cy="35" r="6"/>'),
    ("fuel", '<rect x="8" y="8" width="20" height="34" rx="2"/><path d="M12 13h12v8H12z"/><path d="M4 42h28"/>'
             '<path d="M28 18h5l5 5v13a3 3 0 0 1-6 0v-8h-4"/>'),
    ("speed", '<path d="M7 34a17 17 0 1 1 34 0"/><path d="M24 31l9-12"/><circle cx="24" cy="32" r="3"/>'
              '<path d="M11 23l3 2M24 14v3M37 23l-3 2"/>'),
    ("rpm", '<path d="M7 34a17 17 0 1 1 34 0"/><path d="M37.9 24.2A17 17 0 0 1 41 34" stroke-width="6"/>'
            '<path d="M24 31l-8-11"/><circle cx="24" cy="32" r="3"/>'),
    ("lambda", '<path d="M13 8c4 0 6 2 8 6l14 26M24 21L13 40"/>'),
    ("gearbox_temp", '<circle cx="18" cy="26" r="9"/><circle cx="18" cy="26" r="3"/>'
                     '<path d="M27 26h4M24.4 32.4l2.8 2.8M18 35v4M11.6 32.4l-2.8 2.8M9 26H5M11.6 19.6l-2.8-2.8M18 17v-4M24.4 19.6l2.8-2.8"/>'
                     '<path d="M36 28V10a2.5 2.5 0 0 1 5 0v18"/><circle cx="38.5" cy="32" r="4.5"/>'),
    ("haldex", '<path d="M12 12h24M12 36h24M24 12v24"/><rect x="5" y="6" width="7" height="12" rx="1.5"/>'
               '<rect x="36" y="6" width="7" height="12" rx="1.5"/><rect x="5" y="30" width="7" height="12" rx="1.5"/>'
               '<rect x="36" y="30" width="7" height="12" rx="1.5"/><rect x="20" y="25" width="8" height="7" rx="1"/>'),
    ("throttle", '<path d="M10 42h28"/><path d="M17 37L29 9l6 3-12 28z"/><circle cx="20" cy="40" r="2"/>'),
    ("gear", '<path d="M12 14v22M24 14v22M36 14v22M12 25h24"/><circle cx="12" cy="10" r="3"/><circle cx="24" cy="10" r="3"/>'
             '<circle cx="36" cy="10" r="3"/>'),
    # 0.31.0: generic ones, so any value (a DBC signal, OBD PID, K-line field, ECU channel, ADC sensor) gets an icon by
    # what it measures when none of the specific ones above fits
    ("temp", '<path d="M21 30V9a3 3 0 0 1 6 0v21"/><circle cx="24" cy="35" r="6"/><path d="M27 14h5M27 20h5M27 26h5"/>'),
    ("pressure", '<circle cx="24" cy="21" r="14"/><path d="M24 21l7-7"/><circle cx="24" cy="21" r="2"/>'
                 '<path d="M14 21h3M24 11v3M34 21h-3"/><path d="M21 35v8h6v-8"/>'),
    ("volts", '<path d="M27 5L13 27h11l-3 16 14-22H24z"/>'),
    ("ignition", '<path d="M20 5h8v6h-8zM18 11h12v5H18zM21 16h6v14h-6z"/><path d="M24 30v5M24 35h7"/>'
                 '<path d="M33 39l3 3M36 34h4M33 31l3-3"/>'),
    ("injector", '<path d="M19 5h10v9H19zM16 14h16v6H16zM20 20h8v14h-8z"/><path d="M24 34v3"/>'
                 '<path d="M24 40l-6 6M24 40v6M24 40l6 6"/>'),
    ("brake", '<circle cx="22" cy="27" r="15"/><circle cx="22" cy="27" r="4"/>'
              '<path d="M29 8.5a20 20 0 0 1 11.5 11.5" stroke-width="7"/>'),
    # 0.37.6: the engine warning light (EML / MIL): "Distance MIL on", "Time MIL on", "since codes cleared" ...
    ("eml", '<path d="M13 16h15v4h5l4 4h3v-3h4v14h-4v-3h-3l-4 4H17l-4-4H9v4H5V22h4v4h4z"/><path d="M16 11h10M21 11v5"/>'),
]


# --------------------------------------------------------------------------------------------------------------------
# geometry: every element becomes a list of polylines (closed flag) in viewBox units
# --------------------------------------------------------------------------------------------------------------------
def _arc_points(x0, y0, rx, ry, phi, large, sweep, x1, y1, steps=24):
    """SVG elliptical arc (endpoint form) -> points, W3C implementation notes F.6.5."""
    if rx == 0 or ry == 0:
        return [(x1, y1)]
    cp, sp = math.cos(math.radians(phi)), math.sin(math.radians(phi))
    dx, dy = (x0 - x1) / 2, (y0 - y1) / 2
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    rx, ry = abs(rx), abs(ry)
    lam = x1p ** 2 / rx ** 2 + y1p ** 2 / ry ** 2
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx ** 2 * ry ** 2 - rx ** 2 * y1p ** 2 - ry ** 2 * x1p ** 2
    den = rx ** 2 * y1p ** 2 + ry ** 2 * x1p ** 2
    co = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large == sweep:
        co = -co
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (x0 + x1) / 2
    cy = sp * cxp + cp * cyp + (y0 + y1) / 2

    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    pts = []
    for i in range(1, steps + 1):
        t = t1 + dt * i / steps
        x, y = rx * math.cos(t), ry * math.sin(t)
        pts.append((cp * x - sp * y + cx, sp * x + cp * y + cy))
    return pts


def _bez(p0, p1, p2, p3=None, steps=16):
    pts = []
    for i in range(1, steps + 1):
        t = i / steps
        if p3 is None:  # quadratic
            x = (1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t ** 2 * p2[0]
            y = (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t ** 2 * p2[1]
        else:
            x = (1 - t) ** 3 * p0[0] + 3 * (1 - t) ** 2 * t * p1[0] + 3 * (1 - t) * t ** 2 * p2[0] + t ** 3 * p3[0]
            y = (1 - t) ** 3 * p0[1] + 3 * (1 - t) ** 2 * t * p1[1] + 3 * (1 - t) * t ** 2 * p2[1] + t ** 3 * p3[1]
        pts.append((x, y))
    return pts


_TOK = re.compile(r"[MmLlHhVvQqTtCcSsAaZz]|-?(?:\d+\.?\d*|\.\d+)(?:e-?\d+)?")
_ARGS = {"M": 2, "L": 2, "H": 1, "V": 1, "Q": 4, "T": 2, "C": 6, "S": 4, "A": 7, "Z": 0}


def path_polylines(d):
    toks = _TOK.findall(d)
    i, cmd = 0, None
    x = y = sx = sy = 0.0
    lastc = None  # last control point (for T / S)
    lines, cur = [], []

    def flush(closed=False):
        nonlocal cur
        if len(cur) > 1:
            lines.append((cur, closed))
        cur = []

    while i < len(toks):
        if re.match(r"[A-Za-z]", toks[i]):
            cmd = toks[i]
            i += 1
            if cmd in "Zz":
                if cur:
                    cur.append(cur[0])
                flush(True)
                x, y = sx, sy
                lastc = None
                continue
        n = _ARGS[cmd.upper()]
        a = [float(v) for v in toks[i:i + n]]
        i += n
        rel = cmd.islower()
        C = cmd.upper()
        ox, oy = (x, y) if rel else (0.0, 0.0)
        if C == "M":
            flush()
            x, y = a[0] + ox, a[1] + oy
            sx, sy = x, y
            cur = [(x, y)]
            cmd = "l" if rel else "L"  # implicit lineto after a moveto
            lastc = None
        elif C == "L":
            x, y = a[0] + ox, a[1] + oy
            cur.append((x, y))
            lastc = None
        elif C == "H":
            x = a[0] + (x if rel else 0)
            cur.append((x, y))
            lastc = None
        elif C == "V":
            y = a[0] + (y if rel else 0)
            cur.append((x, y))
            lastc = None
        elif C in "QT":
            if C == "Q":
                c1 = (a[0] + ox, a[1] + oy)
                end = (a[2] + ox, a[3] + oy)
            else:
                c1 = (2 * x - lastc[0], 2 * y - lastc[1]) if lastc else (x, y)
                end = (a[0] + ox, a[1] + oy)
            cur += _bez((x, y), c1, end)
            lastc = c1
            x, y = end
        elif C in "CS":
            if C == "C":
                c1, c2, end = (a[0] + ox, a[1] + oy), (a[2] + ox, a[3] + oy), (a[4] + ox, a[5] + oy)
            else:
                c1 = (2 * x - lastc[0], 2 * y - lastc[1]) if lastc else (x, y)
                c2, end = (a[0] + ox, a[1] + oy), (a[2] + ox, a[3] + oy)
            cur += _bez((x, y), c1, c2, end)
            lastc = c2
            x, y = end
        elif C == "A":
            end = (a[5] + ox, a[6] + oy)
            cur += _arc_points(x, y, a[0], a[1], a[2], int(a[3]), int(a[4]), end[0], end[1])
            x, y = end
            lastc = None
    flush()
    return lines


def circle_polyline(cx, cy, r, steps=48):
    pts = [(cx + r * math.cos(2 * math.pi * i / steps), cy + r * math.sin(2 * math.pi * i / steps)) for i in range(steps + 1)]
    return [(pts, True)]


def rect_polyline(x, y, w, h, rx):
    if rx <= 0:
        return [([(x, y), (x + w, y), (x + w, y + h), (x, y + h), (x, y)], True)]
    pts = []
    for cx, cy, a0 in ((x + w - rx, y + rx, -90), (x + w - rx, y + h - rx, 0), (x + rx, y + h - rx, 90), (x + rx, y + rx, 180)):
        for i in range(9):
            a = math.radians(a0 + 90 * i / 8)
            pts.append((cx + rx * math.cos(a), cy + rx * math.sin(a)))
    pts.append(pts[0])
    return [(pts, True)]


def parse_transform(t):
    """translate(tx ty) scale(s) -> (s, tx, ty) applied as p * s + t (scale first, as SVG composes right to left)."""
    s, tx, ty = 1.0, 0.0, 0.0
    for name, args in re.findall(r"(\w+)\(([^)]*)\)", t or ""):
        v = [float(n) for n in re.split(r"[ ,]+", args.strip()) if n]
        if name == "translate":
            tx, ty = v[0], v[1] if len(v) > 1 else 0.0
        elif name == "scale":
            s = v[0]
    return s, tx, ty


def elements(svg_body):
    """-> list of (polylines, stroke_width) in viewBox units"""
    root = ET.fromstring('<svg xmlns="http://www.w3.org/2000/svg">' + svg_body + "</svg>")
    out = []

    def walk(node, xf):
        for el in node:
            tag = el.tag.split("}")[-1]
            sw = float(el.get("stroke-width", STROKE))
            if tag == "g":
                s, tx, ty = parse_transform(el.get("transform"))
                ps, ptx, pty = xf
                walk(el, (ps * s, ps * tx + ptx, ps * ty + pty))
                continue
            if tag == "path":
                pl = path_polylines(el.get("d"))
            elif tag == "circle":
                pl = circle_polyline(float(el.get("cx")), float(el.get("cy")), float(el.get("r")))
            elif tag == "rect":
                pl = rect_polyline(float(el.get("x")), float(el.get("y")), float(el.get("width")), float(el.get("height")),
                                   float(el.get("rx", 0)))
            else:
                continue
            s, tx, ty = xf
            pl = [([(px * s + tx, py * s + ty) for px, py in pts], closed) for pts, closed in pl]
            out.append((pl, sw * s))

    walk(root, (1.0, 0.0, 0.0))
    return out


def render(svg_body, size):
    from PIL import Image, ImageDraw

    big = size * SS
    k = big / 48.0
    img = Image.new("L", (big, big), 0)
    dr = ImageDraw.Draw(img)
    for polylines, sw in elements(svg_body):
        w = max(1, round(sw * k))
        r = w / 2
        for pts, _closed in polylines:
            P = [(px * k, py * k) for px, py in pts]
            dr.line(P, fill=255, width=w)
            for px, py in P:  # round caps + joins
                dr.ellipse((px - r, py - r, px + r, py + r), fill=255)
    return img.resize((size, size), Image.LANCZOS)


# --------------------------------------------------------------------------------------------------------------------
def main():
    from PIL import Image

    size = int(sys.argv[1]) if len(sys.argv) > 1 else SIZE
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, "..", ".."))
    out_dir = os.path.join(root, "lib", "GaugeUI", "src")
    imgs = [(name, render(body, size)) for name, body in ICONS]

    # preview: icons in a row on black, tinted teal, at 2x
    pv = Image.new("RGB", (len(imgs) * (size * 2 + 8) + 8, size * 2 + 16), (0, 0, 0))
    for i, (_, im) in enumerate(imgs):
        a = im.resize((size * 2, size * 2), Image.NEAREST)
        tint = Image.new("RGB", a.size, (0x24, 0xD1, 0xC4))
        pv.paste(tint, (8 + i * (size * 2 + 8), 8), a)
    pv.save(os.path.join(here, "preview.png"))

    h = ["#pragma once",
         "// generated by tools/icons/make_icons.py - do not edit (edit the SVGs there and re-run)",
         "#include <lvgl.h>", "",
         "enum gaugeIcon_t : uint8_t", "{"]
    h += ["  ICON_%s," % n.upper() for n, _ in imgs]
    h += ["  ICON_COUNT,", "  ICON_NONE = 0xFF", "};", "",
          "#define GAUGE_ICON_SIZE %d" % size,
          "// A8 bitmap (alpha only): draw with image_recolor + image_recolor_opa = LV_OPA_COVER for the colour",
          "const lv_image_dsc_t *gaugeIcon(uint8_t icon); // NULL for ICON_NONE / out of range", ""]
    c = ["// generated by tools/icons/make_icons.py - do not edit (edit the SVGs there and re-run)",
         '#include "GaugeIcons.h"', ""]
    for n, im in imgs:
        data = list(im.tobytes())
        c.append("static const uint8_t icon_%s_map[%d] = {" % (n, len(data)))
        for j in range(0, len(data), size):
            c.append("    " + ",".join("0x%02x" % v for v in data[j:j + size]) + ",")
        c.append("};")
        c.append("static const lv_image_dsc_t icon_%s = {" % n)
        c.append("    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .flags = 0, .w = %d, .h = %d, .stride = %d},"
                 % (size, size, size))
        c.append("    .data_size = %d," % len(data))
        c.append("    .data = icon_%s_map," % n)
        c.append("};")
        c.append("")
    c.append("static const lv_image_dsc_t *const icons[ICON_COUNT] = {")
    c += ["    &icon_%s," % n for n, _ in imgs]
    c.append("};")
    c.append("")
    c.append("const lv_image_dsc_t *gaugeIcon(uint8_t icon) { return icon < ICON_COUNT ? icons[icon] : NULL; }")
    with open(os.path.join(out_dir, "GaugeIcons.h"), "w", newline="\n") as f:
        f.write("\n".join(h))
    with open(os.path.join(out_dir, "GaugeIcons.cpp"), "w", newline="\n") as f:
        f.write("\n".join(c) + "\n")
    print("wrote %d icons at %d px -> lib/GaugeUI/src/GaugeIcons.{h,cpp}, tools/icons/preview.png" % (len(imgs), size))


if __name__ == "__main__":
    main()
