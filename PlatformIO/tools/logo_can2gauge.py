#!/usr/bin/env python3
"""The can2gauge logo (boot logo option 6) as SVG, the lettering as Montserrat outlines so it renders anywhere.

    python tools/logo_can2gauge.py FONT_DIR [--variant a|b|c] [--out tools/logos_src/can2gauge.svg]

FONT_DIR holds Montserrat-Medium.ttf / -Bold.ttf / -ExtraBold.ttf (Google Fonts, OFL) - only needed to regenerate;
the SVG it writes has no font dependency. Then `python tools/logo_convert.py tools/logos_src/can2gauge.svg --exact` turns it into data/logos/can2gauge.bin (--exact:
drawn 1:1, no trim / fit, so the dial sits where the gauge expects it).

The mark: a gauge dial in the can2gauge accents (teal sweeping into amber, the needle amber), over the wordmark with
the "2" in amber - CAN in, gauge out.

0.25.2: the boot logo (variant d) is drawn with the dial empty - the track and unlit ticks only. The gauge draws the
teal-to-amber sweep and lights the ticks itself, so the dial sweeps up like a cluster's needles at ignition on
(bootSweepDial() in lib/GaugeUI/src/GaugeUI.cpp). Its BOOT_DIAL_* numbers must match dial(240, 200, 122) below.
"""
import argparse
import math
import os

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

TEAL, AMBER, WHITE, MUTED, TRACK = "#24D1C4", "#FFB02E", "#F2F4F8", "#7D8EA0", "#1C2630"
A0, SPAN = 135.0, 270.0


class Font:
    def __init__(self, path):
        self.f = TTFont(path)
        self.gs = self.f.getGlyphSet()
        self.cmap = self.f.getBestCmap()
        self.upm = self.f["head"].unitsPerEm

    def width(self, text, size, track=0.0):
        return sum(self.gs[self.cmap[ord(c)]].width for c in text) * size / self.upm + track * (len(text) - 1)

    def path(self, text, x, y, size, track=0.0):
        """SVG path data of text with its baseline at y, starting at x"""
        out, s = [], size / self.upm
        for c in text:
            g = self.cmap[ord(c)]
            pen = SVGPathPen(self.gs)
            self.gs[g].draw(TransformPen(pen, (s, 0, 0, -s, x, y)))
            out.append(pen.getCommands())
            x += self.gs[g].width * s + track
        return " ".join(out)


def arc_path(cx, cy, r, a0, a1):
    p0 = (cx + r * math.cos(math.radians(a0)), cy + r * math.sin(math.radians(a0)))
    p1 = (cx + r * math.cos(math.radians(a1)), cy + r * math.sin(math.radians(a1)))
    large = 1 if (a1 - a0) % 360 > 180 else 0
    return "M %.2f %.2f A %.2f %.2f 0 %d 1 %.2f %.2f" % (p0[0], p0[1], r, r, large, p1[0], p1[1])


def dial(cx, cy, r, frac=0.72, needle=True, stroke=16, ticks=11, sweep=True):
    """the gauge mark: track, teal-to-amber sweep, ticks (lit under the sweep), needle + hub.
    sweep=False: track and unlit ticks only (the boot logo: the gauge draws the sweep)"""
    a1 = A0 + SPAN * frac
    gid = "sw%d" % int(cx * 7 + cy)
    s = ['<defs><linearGradient id="%s" gradientUnits="userSpaceOnUse" x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f">'
         '<stop offset="0" stop-color="%s"/><stop offset="0.55" stop-color="%s"/><stop offset="1" stop-color="%s"/></linearGradient></defs>'
         % (gid, cx - r, cy + r * 0.7, cx + r, cy - r * 0.2, TEAL, TEAL, AMBER)]
    s.append('<path d="%s" fill="none" stroke="%s" stroke-width="%d" stroke-linecap="round"/>' % (arc_path(cx, cy, r, A0, A0 + SPAN), TRACK, stroke))
    if sweep:
        s.append('<path d="%s" fill="none" stroke="url(#%s)" stroke-width="%d" stroke-linecap="round"/>' % (arc_path(cx, cy, r, A0, a1), gid, stroke))
    ri, ro = r - stroke * 1.55, r - stroke * 0.95
    for i in range(ticks):
        a = math.radians(A0 + SPAN * i / (ticks - 1))
        lit = sweep and i / (ticks - 1) <= frac + 1e-6
        w = 4 if i % 5 == 0 else 3
        s.append('<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f" stroke="%s" stroke-width="%d" stroke-linecap="round"/>'
                 % (cx + ri * math.cos(a), cy + ri * math.sin(a), cx + ro * math.cos(a), cy + ro * math.sin(a), WHITE if lit else "#3A4654", w))
    if needle:
        a = math.radians(a1)
        ln = r - stroke * 2.1
        s.append('<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f" stroke="%s" stroke-width="%.1f" stroke-linecap="round"/>'
                 % (cx - 0.18 * ln * math.cos(a), cy - 0.18 * ln * math.sin(a), cx + ln * math.cos(a), cy + ln * math.sin(a), AMBER, stroke * 0.42))
        s.append('<circle cx="%.2f" cy="%.2f" r="%.1f" fill="#0B0F14" stroke="%s" stroke-width="%.1f"/>' % (cx, cy, stroke * 0.7, AMBER, stroke * 0.26))
    return "\n".join(s)


def wordmark(fb, x_center, y, size, upper=False, track=0.0):
    parts = [("CAN" if upper else "can", WHITE), ("2", AMBER), ("GAUGE" if upper else "gauge", WHITE)]
    total = sum(fb.width(t, size, track) for t, _ in parts) + track * 2
    x = x_center - total / 2
    out = []
    for t, col in parts:
        out.append('<path d="%s" fill="%s"/>' % (fb.path(t, x, y, size, track), col))
        x += fb.width(t, size, track) + track
    return "\n".join(out)


def tagline(fm, text, x_center, y, size, track):
    w = fm.width(text, size, track)
    return '<path d="%s" fill="%s"/>' % (fm.path(text, x_center - w / 2, y, size, track), MUTED)


def build(fonts, variant):  # d is the boot logo; a / b / c / e are the alternatives in docs/can2gauge_logo_concepts.png
    fm, fb, fx = fonts
    body = []
    if variant == "a":  # dial with needle over the wordmark
        body += [dial(240, 196, 118), wordmark(fb, 240, 384, 60), tagline(fm, "BY FORBES AUTOMOTIVE", 240, 416, 13, 3.2)]
    elif variant == "b":  # the dial reads "2"
        body.append(dial(240, 200, 122, frac=0.72, needle=False))
        w = fx.width("2", 128)
        body.append('<path d="%s" fill="%s"/>' % (fx.path("2", 240 - w / 2, 246, 128), AMBER))
        body += [wordmark(fx, 240, 384, 40, upper=True, track=5), tagline(fm, "BY FORBES AUTOMOTIVE", 240, 414, 12, 3.2)]
    elif variant in ("d", "e"):  # the dial reads what the gauge talks to: OBD / CAN / K / ADC (d stacked, e 2 x 2)
        body.append(dial(240, 200, 122, frac=0.72, needle=False, sweep=(variant != "d")))
        if variant == "d":
            for i, t in enumerate(("OBD", "CAN", "K-LINE", "ADC")):
                w = fx.width(t, 25, 3)
                body.append('<path d="%s" fill="%s"/>' % (fx.path(t, 240 - w / 2, 200 + (i - 1.5) * 31 + 9, 25, 3), AMBER))
        else:
            for i, t in enumerate(("OBD", "CAN", "K-LINE", "ADC")):
                cx, cy = 240 + (-38 if i % 2 == 0 else 38), 200 + (-20 if i < 2 else 26)
                w = fx.width(t, 26, 1.5)
                body.append('<path d="%s" fill="%s"/>' % (fx.path(t, cx - w / 2, cy + 9, 26, 1.5), AMBER))
            body.append('<line x1="240" y1="166" x2="240" y2="240" stroke="#3A4654" stroke-width="2"/>')
            body.append('<line x1="186" y1="203" x2="294" y2="203" stroke="#3A4654" stroke-width="2"/>')
        # 0.25.2: wordmark 34 px (was 40, tracking 5) - it ran to the edge of the glass
        body += [wordmark(fx, 240, 380, 34, upper=True, track=4), tagline(fm, "BY FORBES AUTOMOTIVE", 240, 408, 11, 3)]
    else:  # lockup: small dial left, stacked wordmark right
        body.append(dial(112, 240, 58, stroke=10, ticks=9))
        body.append(wordmark(fb, 300, 254, 40))
        body.append(tagline(fm, "BY FORBES AUTOMOTIVE", 300, 284, 9.5, 2.2))
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="480" height="480" viewBox="0 0 480 480">\n'
            '<rect width="480" height="480" fill="#000"/>\n%s\n</svg>\n' % "\n".join(body))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("fonts")
    ap.add_argument("--variant", default="d")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "logos_src", "can2gauge.svg"))
    a = ap.parse_args()
    fonts = [Font(os.path.join(a.fonts, n)) for n in ("Montserrat-Medium.ttf", "Montserrat-Bold.ttf", "Montserrat-ExtraBold.ttf")]
    open(a.out, "w", encoding="utf-8").write(build(fonts, a.variant))
    print("wrote", a.out)
